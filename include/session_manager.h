#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include <stdint.h>
#include <stddef.h>

#include "transaction_manager.h"

/*
 * SessionManager — M3.3 Concurrent Customer Session Management.
 *
 * Integration layer that simulates multiple independent customers accessing
 * the banking system concurrently through ATM and online banking channels.
 *
 * Each session represents a logical customer interaction and is identified
 * by a unique, monotonically increasing session ID. A session is not a
 * worker thread; client threads drive sessions by submitting banking
 * operations through the existing TransactionManager.
 *
 * Ownership model:
 *   - The session manager holds a non-owning pointer to an existing
 *     TransactionManager. It does NOT own the TransactionManager, the
 *     ThreadPool, or the Bank, and it does NOT destroy them.
 *   - Callers must ensure the TransactionManager (and thus the ThreadPool
 *     and Bank) outlive the SessionManager.
 *   - The session manager owns its fixed-capacity session registry.
 *
 * Lock ordering:
 *   1. session_mutex protects: session registry, session IDs, lifecycle
 *      state, active session count, shutdown state.
 *   2. Never hold session_mutex while calling transaction_manager_submit()
 *      or transaction_manager_wait() — this avoids lock coupling between
 *      the session manager and the transaction manager, and prevents
 *      deadlock (transaction manager workers need the TM mutex to update
 *      request status).
 *   3. Session state is checked under session_mutex, then the mutex is
 *      released before delegating to the transaction manager.
 */

/*
 * Maximum number of sessions the manager can track.
 * Fixed-capacity session registry.
 */
#define SESSION_MANAGER_MAX_SESSIONS 256

/*
 * SessionChannel — banking channel through which a customer accesses the
 * system.
 */
typedef enum SessionChannel {
    SESSION_CHANNEL_ATM = 0,
    SESSION_CHANNEL_ONLINE_BANKING = 1
} SessionChannel;

/*
 * SessionState — lifecycle state of a session slot.
 *
 * ACTIVE    -> session is accepting new transaction submissions
 * TERMINATED -> session has been terminated; rejects new submissions but
 *               does not cancel or corrupt already-accepted transactions
 */
typedef enum SessionState {
    SESSION_STATE_ACTIVE = 0,
    SESSION_STATE_TERMINATED = 1
} SessionState;

/*
 * SessionManagerStatus — outcome of every session manager operation.
 */
typedef enum SessionManagerStatus {
    SESSION_MANAGER_SUCCESS = 0,
    SESSION_MANAGER_ERR_INVALID_ARGUMENT = -1,
    SESSION_MANAGER_ERR_NOT_INITIALIZED = -2,
    SESSION_MANAGER_ERR_ALREADY_INITIALIZED = -3,
    SESSION_MANAGER_ERR_REGISTRY_FULL = -4,
    SESSION_MANAGER_ERR_ALLOC_FAILED = -5,
    SESSION_MANAGER_ERR_MUTEX_INIT_FAILED = -6,
    SESSION_MANAGER_ERR_SESSION_NOT_FOUND = -7,
    SESSION_MANAGER_ERR_SESSION_TERMINATED = -8,
    SESSION_MANAGER_ERR_SHUTDOWN = -9,
    SESSION_MANAGER_ERR_TRANSACTION_SUBMIT_FAILED = -10
} SessionManagerStatus;

/* Forward declaration so Session can exist before the full SessionManager
 * definition. */
struct SessionManager;
typedef struct SessionManager SessionManager;

/*
 * Session — a single tracked customer session stored in the fixed-capacity
 * registry. Slot addresses are stable for the lifetime of the manager.
 */
typedef struct Session {
    uint64_t session_id;
    int account_id;
    SessionChannel channel;
    SessionState state;
    int allocated;
} Session;

/*
 * SessionManager — owns the session registry and coordinates session
 * lifecycle with the existing TransactionManager.
 */
struct SessionManager {
    TransactionManager *tmmgr; /* non-owning reference */

    Session *sessions;
    size_t max_sessions;

    uint64_t next_session_id;
    int shutdown;
    int initialized;

    pthread_mutex_t manager_mutex;
    int manager_mutex_initialized;

    size_t active_sessions; /* count of ACTIVE sessions */
};

/*
 * session_manager_init — initialize the manager with an existing
 * TransactionManager and a fixed session registry capacity.
 *
 * The manager holds a non-owning reference to tmmgr; it does NOT destroy
 * the TransactionManager, ThreadPool, or Bank on destruction. Callers must
 * ensure tmmgr (and the pool/bank it references) outlives this manager.
 *
 * Returns SESSION_MANAGER_SUCCESS on success, or an error code.
 * On failure, all acquired resources are released and the manager is left
 * uninitialized.
 */
SessionManagerStatus session_manager_init(
    SessionManager *manager,
    TransactionManager *tmmgr,
    size_t max_sessions);

/*
 * session_create — allocate a new session for a customer on the given
 * channel and return a unique session ID.
 *
 * Returns SESSION_MANAGER_SUCCESS with *out_session_id set, or an error
 * code.
 */
SessionManagerStatus session_create(
    SessionManager *manager,
    int account_id,
    SessionChannel channel,
    uint64_t *out_session_id);

/*
 * session_get — retrieve the lifecycle state and metadata for a session.
 * Works on sessions in any state (ACTIVE or TERMINATED).
 *
 * Output pointers may be NULL to skip that field.
 *
 * Returns SESSION_MANAGER_SUCCESS, or SESSION_MANAGER_ERR_SESSION_NOT_FOUND.
 */
SessionManagerStatus session_get(
    SessionManager *manager,
    uint64_t session_id,
    SessionState *out_state,
    int *out_account_id,
    SessionChannel *out_channel);

/*
 * session_submit_transaction — validate that the session is active and the
 * manager is not shut down, then submit the banking operation through the
 * existing TransactionManager.
 *
 * Does NOT hold session_mutex while calling transaction_manager_submit().
 *
 * Returns SESSION_MANAGER_SUCCESS with *out_request_id set, or an error
 * code.
 */
SessionManagerStatus session_submit_transaction(
    SessionManager *manager,
    uint64_t session_id,
    TransactionOperation operation,
    int source_account_id,
    int destination_account_id,
    int64_t amount_paise,
    uint64_t *out_request_id);

/*
 * session_terminate — transition an ACTIVE session to TERMINATED.
 * Rejects new transaction submissions for the session but does NOT cancel
 * or corrupt transactions already accepted by the TransactionManager.
 *
 * Returns SESSION_MANAGER_SUCCESS, or an error code.
 */
SessionManagerStatus session_terminate(
    SessionManager *manager,
    uint64_t session_id);

/*
 * session_manager_shutdown — reject new session creation and new
 * transaction submissions. Idempotent. Already-created sessions remain in
 * their current state; ACTIVE sessions can still have their transactions
 * processed to completion.
 *
 * Returns SESSION_MANAGER_SUCCESS.
 */
SessionManagerStatus session_manager_shutdown(
    SessionManager *manager);

/*
 * session_manager_destroy — release all session manager resources.
 * Does NOT destroy the referenced TransactionManager (caller owns it).
 * Safe to call on an uninitialized manager (no-op).
 *
 * Callers must ensure no client thread is accessing session-manager memory
 * before destroying.
 */
void session_manager_destroy(SessionManager *manager);

#endif /* SESSION_MANAGER_H */
