#include "transaction_manager.h"

#include <stdlib.h>
#include <string.h>

/*
 * transaction_worker — worker callback executed by the ThreadPool for each
 * accepted transaction request.
 *
 * Runs entirely outside the manager mutex except for brief critical sections
 * that update request status and stored results. The arg pointer is a
 * TransactionRequest* into the manager's registry; the slot's address is
 * stable for the lifetime of the manager.
 *
 * Sequence:
 *   1. Lock manager_mutex, mark status PROCESSING, unlock.
 *   2. Execute the banking operation via the existing Bank API (outside
 *      any manager lock).
 *   3. Lock manager_mutex, store bank result and balance result, transition
 *      to COMPLETED or FAILED (terminal), decrement active_requests,
 *      signal completion_cond and idle_cond as needed, unlock.
 *
 * The worker never accesses the slot after setting its terminal state, so
 * the slot can safely be released by another thread once the worker returns.
 */
static void transaction_worker(void *arg)
{
    TransactionRequest *req = (TransactionRequest *)arg;
    TransactionManager *mgr = req->manager;
    Bank *bank = mgr->bank;
    BankStatus bs;

    /* Mark request as processing. */
    pthread_mutex_lock(&mgr->manager_mutex);
    req->status = TRANSACTION_REQUEST_PROCESSING;
    pthread_mutex_unlock(&mgr->manager_mutex);

    /* Execute the banking operation via the existing Bank API.
     * No manager locks are held here. */
    switch (req->operation) {
    case TRANSACTION_OP_DEPOSIT:
        bs = bank_deposit(bank, req->source_account_id, req->amount_paise);
        break;
    case TRANSACTION_OP_WITHDRAWAL:
        bs = bank_withdraw(bank, req->source_account_id, req->amount_paise);
        break;
    case TRANSACTION_OP_TRANSFER:
        bs = bank_transfer(bank, req->source_account_id,
                           req->destination_account_id, req->amount_paise);
        break;
    case TRANSACTION_OP_BALANCE_ENQUIRY: {
        int64_t balance = 0;
        bs = bank_get_balance(bank, req->source_account_id, &balance);
        if (bs == BANK_SUCCESS) {
            req->balance_result = balance;
        }
        break;
    }
    default:
        bs = BANK_ERR_INVALID_ARGUMENT;
        break;
    }

    /* Store results and transition to terminal state. */
    pthread_mutex_lock(&mgr->manager_mutex);
    req->bank_result = bs;
    req->status = (bs == BANK_SUCCESS)
        ? TRANSACTION_REQUEST_COMPLETED
        : TRANSACTION_REQUEST_FAILED;
    mgr->active_requests--;
    if (mgr->active_requests == 0) {
        pthread_cond_broadcast(&mgr->idle_cond);
    }
    /* Wake any waiter for this specific request. Broadcast is safe:
     * waiters re-check their own request's status under the manager mutex. */
    pthread_cond_broadcast(&mgr->completion_cond);
    pthread_mutex_unlock(&mgr->manager_mutex);
}

/*
 * find_request_by_id — scan the registry for an allocated slot with the
 * given request ID. Returns the pointer, or NULL if not found.
 * Caller must hold manager_mutex.
 */
static TransactionRequest *find_request_by_id(TransactionManager *mgr,
                                              uint64_t request_id)
{
    size_t i;
    for (i = 0; i < mgr->max_requests; i++) {
        if (mgr->requests[i].allocated &&
            mgr->requests[i].request_id == request_id) {
            return &mgr->requests[i];
        }
    }
    return NULL;
}

TransactionManagerStatus transaction_manager_init(
    TransactionManager *manager,
    ThreadPool *pool,
    Bank *bank,
    size_t max_requests)
{
    if (manager == NULL) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (pool == NULL || !pool->initialized) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (bank == NULL) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (max_requests == 0) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    memset(manager, 0, sizeof(*manager));

    manager->pool = pool;
    manager->bank = bank;
    manager->max_requests = max_requests;
    manager->next_request_id = 1;

    manager->requests = (TransactionRequest *)calloc(max_requests,
                                                     sizeof(TransactionRequest));
    if (manager->requests == NULL) {
        return TRANSACTION_MANAGER_ERR_ALLOC_FAILED;
    }

    if (pthread_mutex_init(&manager->manager_mutex, NULL) != 0) {
        free(manager->requests);
        manager->requests = NULL;
        return TRANSACTION_MANAGER_ERR_MUTEX_INIT_FAILED;
    }
    manager->manager_mutex_initialized = 1;

    if (pthread_cond_init(&manager->completion_cond, NULL) != 0) {
        pthread_mutex_destroy(&manager->manager_mutex);
        manager->manager_mutex_initialized = 0;
        free(manager->requests);
        manager->requests = NULL;
        return TRANSACTION_MANAGER_ERR_COND_INIT_FAILED;
    }
    manager->completion_cond_initialized = 1;

    if (pthread_cond_init(&manager->idle_cond, NULL) != 0) {
        pthread_cond_destroy(&manager->completion_cond);
        manager->completion_cond_initialized = 0;
        pthread_mutex_destroy(&manager->manager_mutex);
        manager->manager_mutex_initialized = 0;
        free(manager->requests);
        manager->requests = NULL;
        return TRANSACTION_MANAGER_ERR_COND_INIT_FAILED;
    }
    manager->idle_cond_initialized = 1;

    manager->initialized = 1;
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_submit(
    TransactionManager *manager,
    TransactionOperation operation,
    int source_account_id,
    int destination_account_id,
    int64_t amount_paise,
    uint64_t *out_request_id)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }
    if (out_request_id == NULL) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (operation < TRANSACTION_OP_DEPOSIT ||
        operation > TRANSACTION_OP_BALANCE_ENQUIRY) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    /* Amount must be positive for mutating operations. */
    if (operation != TRANSACTION_OP_BALANCE_ENQUIRY && amount_paise <= 0) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    /* Transfer source and destination must differ. */
    if (operation == TRANSACTION_OP_TRANSFER &&
        source_account_id == destination_account_id) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_SHUTDOWN;
    }

    /* Find a FREE slot in the registry. */
    size_t slot_index = manager->max_requests;
    for (size_t i = 0; i < manager->max_requests; i++) {
        if (!manager->requests[i].allocated) {
            slot_index = i;
            break;
        }
    }

    if (slot_index == manager->max_requests) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_REGISTRY_FULL;
    }

    TransactionRequest *req = &manager->requests[slot_index];

    /* Allocate the slot. Stable address; manager back-pointer set here. */
    req->allocated = 1;

    /* Handle uint64_t wraparound: skip ID 0. With uint64_t this is
     * practically unreachable, but handled for correctness. */
    uint64_t new_id = manager->next_request_id++;
    if (new_id == 0) {
        new_id = manager->next_request_id++;
    }
    req->request_id = new_id;

    req->operation = operation;
    req->source_account_id = source_account_id;
    req->destination_account_id = destination_account_id;
    req->amount_paise = amount_paise;
    req->status = TRANSACTION_REQUEST_PENDING;
    req->bank_result = BANK_SUCCESS;
    req->balance_result = 0;
    req->manager = manager;

    manager->active_requests++;

    uint64_t request_id = req->request_id;

    /* Unlock before submitting to the thread pool. thread_pool_submit may
     * block when the queue is full, and workers need manager_mutex to
     * update request status — holding it here would deadlock. */
    pthread_mutex_unlock(&manager->manager_mutex);

    ThreadTask task;
    task.function = transaction_worker;
    task.arg = req;

    ThreadPoolStatus ts = thread_pool_submit(manager->pool,
                                             task.function, task.arg);
    if (ts != THREAD_POOL_SUCCESS) {
        /* Submission failed: return the slot to FREE. */
        pthread_mutex_lock(&manager->manager_mutex);
        req->allocated = 0;
        manager->active_requests--;
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_THREAD_POOL_SUBMIT_FAILED;
    }

    *out_request_id = request_id;
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_get_result(
    TransactionManager *manager,
    uint64_t request_id,
    TransactionRequestStatus *out_status,
    BankStatus *out_bank_result,
    int64_t *out_balance_result)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    TransactionRequest *found = find_request_by_id(manager, request_id);
    if (found == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND;
    }

    if (out_status != NULL) {
        *out_status = found->status;
    }
    if (out_bank_result != NULL) {
        *out_bank_result = found->bank_result;
    }
    if (out_balance_result != NULL) {
        *out_balance_result = found->balance_result;
    }

    pthread_mutex_unlock(&manager->manager_mutex);
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_wait(
    TransactionManager *manager,
    uint64_t request_id)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    TransactionRequest *found = find_request_by_id(manager, request_id);
    if (found == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND;
    }

    /* Predicate loop handles spurious wakeups. The worker signals
     * completion_cond when it transitions the request to a terminal state. */
    while (found->status != TRANSACTION_REQUEST_COMPLETED &&
           found->status != TRANSACTION_REQUEST_FAILED) {
        pthread_cond_wait(&manager->completion_cond,
                          &manager->manager_mutex);
    }

    pthread_mutex_unlock(&manager->manager_mutex);
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_wait_idle(
    TransactionManager *manager)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    while (manager->active_requests > 0) {
        pthread_cond_wait(&manager->idle_cond, &manager->manager_mutex);
    }

    pthread_mutex_unlock(&manager->manager_mutex);
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_release(
    TransactionManager *manager,
    uint64_t request_id)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    TransactionRequest *found = find_request_by_id(manager, request_id);
    if (found == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND;
    }

    if (found->status != TRANSACTION_REQUEST_COMPLETED &&
        found->status != TRANSACTION_REQUEST_FAILED) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_ERR_REQUEST_NOT_TERMINAL;
    }

    /* Return slot to FREE for reuse. */
    found->allocated = 0;

    pthread_mutex_unlock(&manager->manager_mutex);
    return TRANSACTION_MANAGER_SUCCESS;
}

TransactionManagerStatus transaction_manager_shutdown(
    TransactionManager *manager)
{
    if (manager == NULL || !manager->initialized) {
        return TRANSACTION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return TRANSACTION_MANAGER_SUCCESS;
    }

    manager->shutdown = 1;
    pthread_mutex_unlock(&manager->manager_mutex);

    /* Delegates to the thread pool's graceful shutdown: pending tasks
     * finish, workers exit, then all are joined. After this returns, no
     * worker accesses manager memory, so destruction is safe. */
    ThreadPoolStatus ts = thread_pool_shutdown(manager->pool);
    if (ts != THREAD_POOL_SUCCESS) {
        return TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    return TRANSACTION_MANAGER_SUCCESS;
}

void transaction_manager_destroy(TransactionManager *manager)
{
    if (manager == NULL || !manager->initialized) {
        return;
    }

    /* Initiate shutdown if not already done so worker threads finish
     * before we release synchronization resources. */
    if (!manager->shutdown) {
        pthread_mutex_lock(&manager->manager_mutex);
        manager->shutdown = 1;
        pthread_mutex_unlock(&manager->manager_mutex);
        thread_pool_shutdown(manager->pool);
    }

    /* Destroy synchronization primitives that were successfully initialized. */
    if (manager->idle_cond_initialized) {
        pthread_cond_destroy(&manager->idle_cond);
    }
    if (manager->completion_cond_initialized) {
        pthread_cond_destroy(&manager->completion_cond);
    }
    if (manager->manager_mutex_initialized) {
        pthread_mutex_destroy(&manager->manager_mutex);
    }

    /* Free the request registry. */
    free(manager->requests);
    manager->requests = NULL;

    manager->initialized = 0;
}
