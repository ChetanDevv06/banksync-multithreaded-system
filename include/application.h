#ifndef APPLICATION_H
#define APPLICATION_H

#include "bank.h"
#include "thread_pool.h"
#include "transaction_manager.h"
#include "session_manager.h"

/*
 * ApplicationState — lifecycle state of the BankSync application.
 *
 * UNINITIALIZED:  no initialization has been attempted.
 * INITIALIZING:   initialization is in progress (partial state possible).
 * RUNNING:        all components initialized and ready.
 * SHUTTING_DOWN:  graceful shutdown is in progress.
 * STOPPED:        shutdown complete, all resources released.
 * FAILED:         initialization failed, cleanup performed.
 */
typedef enum ApplicationState {
    APP_STATE_UNINITIALIZED = 0,
    APP_STATE_INITIALIZING = 1,
    APP_STATE_RUNNING = 2,
    APP_STATE_SHUTTING_DOWN = 3,
    APP_STATE_STOPPED = 4,
    APP_STATE_FAILED = 5
} ApplicationState;

/*
 * ApplicationStatus — outcome of every public application operation.
 */
typedef enum ApplicationStatus {
    APPLICATION_SUCCESS = 0,
    APPLICATION_ERR_INVALID_ARGUMENT = -1,
    APPLICATION_ERR_INVALID_STATE = -2,
    APPLICATION_ERR_ALREADY_INITIALIZED = -3,
    APPLICATION_ERR_BANK_INIT_FAILED = -4,
    APPLICATION_ERR_ACCOUNT_CREATION_FAILED = -5,
    APPLICATION_ERR_THREAD_POOL_INIT_FAILED = -6,
    APPLICATION_ERR_TRANSACTION_MANAGER_INIT_FAILED = -7,
    APPLICATION_ERR_SESSION_MANAGER_INIT_FAILED = -8,
    APPLICATION_ERR_SHUTDOWN_FAILED = -9,
} ApplicationStatus;

/*
 * Application — central application context owning all BankSync subsystems.
 *
 * Ownership model:
 *   - The Application owns the Bank, ThreadPool, TransactionManager and
 *     SessionManager instances directly (embedded by value).
 *   - TransactionManager holds non-owning references to Bank and ThreadPool;
 *     both must outlive the TransactionManager.
 *   - SessionManager holds a non-owning reference to TransactionManager;
 *     the TransactionManager must outlive the SessionManager.
 *   - Destruction order (reverse of initialization): SessionManager,
 *     TransactionManager, ThreadPool, Bank.
 */
typedef struct Application {
    Bank bank;
    ThreadPool pool;
    TransactionManager txn_mgr;
    SessionManager session_mgr;

    int bank_initialized;
    int pool_initialized;
    int tm_initialized;
    int sm_initialized;

    int account_count;
    ApplicationState state;
} Application;

/*
 * application_init — initialize all subsystems in dependency order:
 *   1. Bank
 *   2. Sample accounts
 *   3. ThreadPool
 *   4. TransactionManager (Bank + ThreadPool)
 *   5. SessionManager (TransactionManager)
 *
 * On partial failure, already-initialized components are cleaned up in
 * reverse order and the state is set to APP_STATE_FAILED.
 *
 * Returns APPLICATION_SUCCESS on success, or an error code.
 */
ApplicationStatus application_init(Application *app);

/*
 * application_shutdown — gracefully shut down all subsystems and release
 * resources. Idempotent: safe to call on an uninitialized or already-stopped
 * application.
 *
 * Sequence:
 *   1. Stop accepting new customer sessions.
 *   2. Terminate existing sessions.
 *   3. Shut down TransactionManager (drains pending requests via ThreadPool).
 *   4. Destroy SessionManager, TransactionManager, ThreadPool and Bank.
 *
 * Returns APPLICATION_SUCCESS on success.
 */
ApplicationStatus application_shutdown(Application *app);

/*
 * application_get_state — return the current lifecycle state.
 */
ApplicationState application_get_state(const Application *app);

/*
 * application_get_account_count — return the number of sample accounts.
 */
int application_get_account_count(const Application *app);

/*
 * application_get_bank — return a non-owning pointer to the embedded Bank.
 */
Bank *application_get_bank(Application *app);

/*
 * application_get_session_manager — return a non-owning pointer to the
 * embedded SessionManager.
 */
SessionManager *application_get_session_manager(Application *app);

/*
 * application_destroy — release all resources without the graceful shutdown
 * steps. Used for partial-initialization failure cleanup. Safe on any state.
 */
void application_destroy(Application *app);

#endif /* APPLICATION_H */
