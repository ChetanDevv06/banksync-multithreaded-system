#ifndef TRANSACTION_MANAGER_H
#define TRANSACTION_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#include "bank.h"
#include "thread_pool.h"

/*
 * TransactionManager — M3.2 Transaction Request Manager.
 *
 * Integration layer between the banking engine (Bank) and the generic
 * thread pool (ThreadPool). Submits banking operations asynchronously,
 * tracks their lifecycle through a fixed-capacity registry, and stores
 * results for retrieval.
 *
 * The registry is for tracking request lifecycle and results only — it is
 * NOT a scheduling queue. Scheduling is delegated entirely to the ThreadPool.
 *
 * The manager does not own the ThreadPool or the Bank. Callers must ensure
 * both outlive the TransactionManager.
 *
 * Lock ordering:
 *   1. manager_mutex protects: registry, request IDs, lifecycle status,
 *      stored results, active_requests count, shutdown state.
 *   2. Never hold manager_mutex while calling thread_pool_submit() —
 *      submission may block when the queue is full, and workers need the
 *      manager mutex to update request status, which would deadlock.
 *   3. Never hold manager_mutex while executing banking API functions.
 *   4. Banking operations execute outside the manager mutex; only result
 *      storage happens inside it.
 */

/*
 * Maximum number of transaction requests the manager can track.
 * Fixed-capacity registry, not a scheduling queue.
 */
#define TRANSACTION_MANAGER_MAX_REQUESTS 256

/*
 * TransactionOperation — kind of banking operation requested.
 */
typedef enum TransactionOperation {
    TRANSACTION_OP_DEPOSIT = 0,
    TRANSACTION_OP_WITHDRAWAL = 1,
    TRANSACTION_OP_TRANSFER = 2,
    TRANSACTION_OP_BALANCE_ENQUIRY = 3
} TransactionOperation;

/*
 * TransactionRequestStatus — lifecycle status of a request slot.
 *
 * FREE      -> slot is available for allocation
 * PENDING   -> slot allocated, task submitted to thread pool, not yet started
 * PROCESSING -> worker is executing the banking operation
 * COMPLETED -> worker finished; banking result stored (may be success or error)
 * FAILED    -> worker finished; banking operation returned an error status
 *
 * COMPLETED and FAILED are both terminal lifecycle states. A request at a
 * terminal state may be released; its slot is then reusable.
 *
 * Note: COMPLETED refers to the request-manager lifecycle, not the banking
 * outcome. A request can be COMPLETED at the manager level while its
 * BankStatus indicates insufficient funds or another banking error.
 */
typedef enum TransactionRequestStatus {
    TRANSACTION_REQUEST_FREE = 0,
    TRANSACTION_REQUEST_PENDING = 1,
    TRANSACTION_REQUEST_PROCESSING = 2,
    TRANSACTION_REQUEST_COMPLETED = 3,
    TRANSACTION_REQUEST_FAILED = 4
} TransactionRequestStatus;

/*
 * TransactionManagerStatus — outcome of every manager operation.
 */
typedef enum TransactionManagerStatus {
    TRANSACTION_MANAGER_SUCCESS = 0,
    TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT = -1,
    TRANSACTION_MANAGER_ERR_NOT_INITIALIZED = -2,
    TRANSACTION_MANAGER_ERR_ALREADY_INITIALIZED = -3,
    TRANSACTION_MANAGER_ERR_REGISTRY_FULL = -4,
    TRANSACTION_MANAGER_ERR_ALLOC_FAILED = -5,
    TRANSACTION_MANAGER_ERR_MUTEX_INIT_FAILED = -6,
    TRANSACTION_MANAGER_ERR_COND_INIT_FAILED = -7,
    TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND = -8,
    TRANSACTION_MANAGER_ERR_SHUTDOWN = -9,
    TRANSACTION_MANAGER_ERR_THREAD_POOL_SUBMIT_FAILED = -10,
    TRANSACTION_MANAGER_ERR_REQUEST_NOT_TERMINAL = -11
} TransactionManagerStatus;

/* Forward declaration so TransactionRequest can reference TransactionManager. */
struct TransactionManager;
typedef struct TransactionManager TransactionManager;

/*
 * TransactionRequest — a single tracked request stored in the fixed-capacity
 * registry. Slot addresses are stable for the lifetime of the manager.
 *
 * The manager pointer (back-pointer) lets the worker callback access the
 * Bank via the manager without needing a separate context allocation.
 */
typedef struct TransactionRequest {
    uint64_t request_id;
    TransactionOperation operation;
    int source_account_id;
    int destination_account_id;
    int64_t amount_paise;

    TransactionRequestStatus status;
    BankStatus bank_result;
    int64_t balance_result;

    int allocated; /* 1 = slot in use, 0 = free */

    TransactionManager *manager; /* back-pointer for the worker callback */
} TransactionRequest;

/*
 * TransactionManager — owns the request registry and coordinates with the
 * ThreadPool and Bank.
 */
struct TransactionManager {
    ThreadPool *pool;
    Bank *bank;

    TransactionRequest *requests;
    size_t max_requests;

    uint64_t next_request_id;
    int shutdown;
    int initialized;

    pthread_mutex_t manager_mutex;
    pthread_cond_t completion_cond;  /* signaled when any request reaches terminal state */
    pthread_cond_t idle_cond;        /* signaled when active_requests reaches 0 */
    int manager_mutex_initialized;
    int completion_cond_initialized;
    int idle_cond_initialized;

    size_t active_requests; /* count of PENDING + PROCESSING requests */
};

/*
 * transaction_manager_init — initialize the manager with an existing
 * ThreadPool and Bank, and a fixed request registry capacity.
 *
 * Returns TRANSACTION_MANAGER_SUCCESS on success, or an error code.
 * On failure, all acquired resources are released and the manager is left
 * uninitialized.
 */
TransactionManagerStatus transaction_manager_init(
    TransactionManager *manager,
    ThreadPool *pool,
    Bank *bank,
    size_t max_requests);

/*
 * transaction_manager_submit — validate and register a banking request,
 * then submit it to the thread pool for asynchronous execution.
 *
 * The caller receives a unique, monotonically increasing request ID. The
 * request remains owned by the manager until transaction_manager_release()
 * is called on a terminal-state request.
 *
 * Does NOT hold manager_mutex while calling thread_pool_submit().
 *
 * Returns TRANSACTION_MANAGER_SUCCESS with *out_request_id set, or an error
 * code. On thread-pool submission failure, the reserved slot is returned to
 * FREE before returning.
 */
TransactionManagerStatus transaction_manager_submit(
    TransactionManager *manager,
    TransactionOperation operation,
    int source_account_id,
    int destination_account_id,
    int64_t amount_paise,
    uint64_t *out_request_id);

/*
 * transaction_manager_get_result — retrieve the lifecycle status and stored
 * banking result for a request. Works on requests in any state (including
 * PENDING and PROCESSING).
 *
 * Output pointers may be NULL to skip that field.
 *
 * Returns TRANSACTION_MANAGER_SUCCESS, or TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND.
 */
TransactionManagerStatus transaction_manager_get_result(
    TransactionManager *manager,
    uint64_t request_id,
    TransactionRequestStatus *out_status,
    BankStatus *out_bank_result,
    int64_t *out_balance_result);

/*
 * transaction_manager_wait — block until the request reaches a terminal
 * lifecycle state (COMPLETED or FAILED). Uses a predicate loop to handle
 * spurious wakeups.
 *
 * Returns TRANSACTION_MANAGER_SUCCESS, or TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND.
 */
TransactionManagerStatus transaction_manager_wait(
    TransactionManager *manager,
    uint64_t request_id);

/*
 * transaction_manager_wait_idle — block until all accepted requests have
 * reached terminal state (active_requests == 0).
 *
 * Returns TRANSACTION_MANAGER_SUCCESS.
 */
TransactionManagerStatus transaction_manager_wait_idle(
    TransactionManager *manager);

/*
 * transaction_manager_release — release a terminal-state request, returning
 * its registry slot to FREE for reuse. Rejects requests that are PENDING or
 * PROCESSING.
 *
 * Returns TRANSACTION_MANAGER_SUCCESS, or an error code.
 */
TransactionManagerStatus transaction_manager_release(
    TransactionManager *manager,
    uint64_t request_id);

/*
 * transaction_manager_shutdown — reject new submissions and allow accepted
 * requests to finish by delegating to the thread pool's graceful shutdown.
 * Idempotent.
 *
 * Returns TRANSACTION_MANAGER_SUCCESS.
 */
TransactionManagerStatus transaction_manager_shutdown(
    TransactionManager *manager);

/*
 * transaction_manager_destroy — release all manager resources. Initiates
 * shutdown (joining all worker threads via the thread pool) if not already
 * shut down, then destroys mutexes and condition variables and frees the
 * request registry. Safe to call on an uninitialized manager (no-op).
 *
 * Callers must not concurrently submit requests while destroying.
 */
void transaction_manager_destroy(TransactionManager *manager);

#endif /* TRANSACTION_MANAGER_H */
