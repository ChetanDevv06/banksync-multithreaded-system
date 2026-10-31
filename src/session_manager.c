#include "session_manager.h"

#include <stdlib.h>
#include <string.h>

/*
 * find_session_by_id — scan the registry for an allocated slot with the
 * given session ID. Returns the pointer, or NULL if not found.
 * Caller must hold manager_mutex.
 */
static Session *find_session_by_id(SessionManager *mgr, uint64_t session_id)
{
    size_t i;
    /* Scan all slots (not just allocated ones) so that a terminated session
     * whose slot has been freed can still be looked up by its ID. A stale
     * ID can never match a reused slot because session IDs are
     * monotonically increasing and never repeated. */
    for (i = 0; i < mgr->max_sessions; i++) {
        if (mgr->sessions[i].session_id == session_id) {
            return &mgr->sessions[i];
        }
    }
    return NULL;
}

SessionManagerStatus session_manager_init(
    SessionManager *manager,
    TransactionManager *tmmgr,
    size_t max_sessions)
{
    if (manager == NULL) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (tmmgr == NULL) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (max_sessions == 0) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    memset(manager, 0, sizeof(*manager));

    manager->tmmgr = tmmgr;
    manager->max_sessions = max_sessions;
    manager->next_session_id = 1;

    manager->sessions = (Session *)calloc(max_sessions, sizeof(Session));
    if (manager->sessions == NULL) {
        return SESSION_MANAGER_ERR_ALLOC_FAILED;
    }

    if (pthread_mutex_init(&manager->manager_mutex, NULL) != 0) {
        free(manager->sessions);
        manager->sessions = NULL;
        return SESSION_MANAGER_ERR_MUTEX_INIT_FAILED;
    }
    manager->manager_mutex_initialized = 1;

    manager->initialized = 1;
    return SESSION_MANAGER_SUCCESS;
}

SessionManagerStatus session_create(
    SessionManager *manager,
    int account_id,
    SessionChannel channel,
    uint64_t *out_session_id)
{
    if (manager == NULL || !manager->initialized) {
        return SESSION_MANAGER_ERR_NOT_INITIALIZED;
    }
    if (out_session_id == NULL) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }
    if (channel != SESSION_CHANNEL_ATM &&
        channel != SESSION_CHANNEL_ONLINE_BANKING) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SHUTDOWN;
    }

    /* Find a FREE slot in the registry. */
    size_t slot_index = manager->max_sessions;
    for (size_t i = 0; i < manager->max_sessions; i++) {
        if (!manager->sessions[i].allocated) {
            slot_index = i;
            break;
        }
    }

    if (slot_index == manager->max_sessions) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_REGISTRY_FULL;
    }

    Session *session = &manager->sessions[slot_index];

    /* Handle uint64_t wraparound: skip ID 0. */
    uint64_t new_id = manager->next_session_id++;
    if (new_id == 0) {
        new_id = manager->next_session_id++;
    }
    session->session_id = new_id;

    session->account_id = account_id;
    session->channel = channel;
    session->state = SESSION_STATE_ACTIVE;
    session->allocated = 1;

    manager->active_sessions++;
    uint64_t session_id = session->session_id;

    pthread_mutex_unlock(&manager->manager_mutex);

    *out_session_id = session_id;
    return SESSION_MANAGER_SUCCESS;
}

SessionManagerStatus session_get(
    SessionManager *manager,
    uint64_t session_id,
    SessionState *out_state,
    int *out_account_id,
    SessionChannel *out_channel)
{
    if (manager == NULL || !manager->initialized) {
        return SESSION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    Session *found = find_session_by_id(manager, session_id);
    if (found == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SESSION_NOT_FOUND;
    }

    if (out_state != NULL) {
        *out_state = found->state;
    }
    if (out_account_id != NULL) {
        *out_account_id = found->account_id;
    }
    if (out_channel != NULL) {
        *out_channel = found->channel;
    }

    pthread_mutex_unlock(&manager->manager_mutex);
    return SESSION_MANAGER_SUCCESS;
}

SessionManagerStatus session_submit_transaction(
    SessionManager *manager,
    uint64_t session_id,
    TransactionOperation operation,
    int source_account_id,
    int destination_account_id,
    int64_t amount_paise,
    uint64_t *out_request_id)
{
    if (manager == NULL || !manager->initialized) {
        return SESSION_MANAGER_ERR_NOT_INITIALIZED;
    }
    if (out_request_id == NULL) {
        return SESSION_MANAGER_ERR_INVALID_ARGUMENT;
    }

    /* Validate session state under the session mutex, then release it
     * before delegating to the transaction manager. Never hold the session
     * mutex while calling transaction_manager_submit(). */
    pthread_mutex_lock(&manager->manager_mutex);

    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SHUTDOWN;
    }

    Session *session = find_session_by_id(manager, session_id);
    if (session == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SESSION_NOT_FOUND;
    }

    if (session->state != SESSION_STATE_ACTIVE) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SESSION_TERMINATED;
    }

    /* Release session mutex before TM submission to avoid lock coupling. */
    pthread_mutex_unlock(&manager->manager_mutex);

    TransactionManagerStatus ms = transaction_manager_submit(
        manager->tmmgr,
        operation,
        source_account_id,
        destination_account_id,
        amount_paise,
        out_request_id);

    if (ms != TRANSACTION_MANAGER_SUCCESS) {
        return SESSION_MANAGER_ERR_TRANSACTION_SUBMIT_FAILED;
    }

    return SESSION_MANAGER_SUCCESS;
}

SessionManagerStatus session_terminate(
    SessionManager *manager,
    uint64_t session_id)
{
    if (manager == NULL || !manager->initialized) {
        return SESSION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    Session *session = find_session_by_id(manager, session_id);
    if (session == NULL) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_ERR_SESSION_NOT_FOUND;
    }

    if (session->state == SESSION_STATE_TERMINATED) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_SUCCESS; /* idempotent */
    }

    session->state = SESSION_STATE_TERMINATED;
    session->allocated = 0;
    manager->active_sessions--;

    pthread_mutex_unlock(&manager->manager_mutex);
    return SESSION_MANAGER_SUCCESS;
}

SessionManagerStatus session_manager_shutdown(
    SessionManager *manager)
{
    if (manager == NULL || !manager->initialized) {
        return SESSION_MANAGER_ERR_NOT_INITIALIZED;
    }

    pthread_mutex_lock(&manager->manager_mutex);

    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->manager_mutex);
        return SESSION_MANAGER_SUCCESS;
    }

    manager->shutdown = 1;
    pthread_mutex_unlock(&manager->manager_mutex);
    return SESSION_MANAGER_SUCCESS;
}

void session_manager_destroy(SessionManager *manager)
{
    if (manager == NULL || !manager->initialized) {
        return;
    }

    /* Do NOT destroy the referenced TransactionManager — the caller owns it.
     * Only release this manager's own resources. */

    if (manager->manager_mutex_initialized) {
        pthread_mutex_destroy(&manager->manager_mutex);
    }

    free(manager->sessions);
    manager->sessions = NULL;

    manager->initialized = 0;
}
