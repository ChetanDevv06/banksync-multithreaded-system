#include "application.h"

#include <stdio.h>
#include <string.h>

/*
 * Sample account counts and configuration for deterministic initialization.
 */
#define APPLICATION_SAMPLE_ACCOUNT_COUNT 3
#define APPLICATION_THREAD_POOL_WORKERS 4
#define APPLICATION_THREAD_POOL_QUEUE_CAPACITY 16
#define APPLICATION_MAX_TRANSACTION_REQUESTS 256
#define APPLICATION_MAX_SESSIONS 256

/* ------------------------------------------------------------------ */
/* Internal cleanup helper                                            */
/* ------------------------------------------------------------------ */

/*
 * application_cleanup — release all initialized components in reverse
 * initialization order (SessionManager, TransactionManager, ThreadPool,
 * Bank). Safe to call on any state; only initialized components are
 * destroyed. Resets all initialization flags afterward.
 */
static void application_cleanup(Application *app)
{
    if (app->sm_initialized) {
        session_manager_destroy(&app->session_mgr);
        app->sm_initialized = 0;
    }
    if (app->tm_initialized) {
        transaction_manager_destroy(&app->txn_mgr);
        app->tm_initialized = 0;
    }
    if (app->pool_initialized) {
        thread_pool_destroy(&app->pool);
        app->pool_initialized = 0;
    }
    if (app->bank_initialized) {
        bank_destroy(&app->bank);
        app->bank_initialized = 0;
    }

    app->account_count = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

ApplicationStatus application_init(Application *app)
{
    BankStatus bs;
    ThreadPoolStatus ts;
    TransactionManagerStatus tms;
    SessionManagerStatus sms;
    int i;

    if (app == NULL) {
        return APPLICATION_ERR_INVALID_ARGUMENT;
    }

    /* Re-initialization without destruction is not permitted. */
    if (app->state != APP_STATE_UNINITIALIZED &&
        app->state != APP_STATE_STOPPED &&
        app->state != APP_STATE_FAILED) {
        return APPLICATION_ERR_ALREADY_INITIALIZED;
    }

    /* Zero-initialize so cleanup tracks only what was actually set. */
    memset(app, 0, sizeof(*app));
    app->state = APP_STATE_INITIALIZING;

    /* --- 1. Initialize Bank --- */
    bs = bank_init(&app->bank);
    if (bs != BANK_SUCCESS) {
        app->state = APP_STATE_FAILED;
        return APPLICATION_ERR_BANK_INIT_FAILED;
    }
    app->bank_initialized = 1;

    /* --- 2. Create initial sample accounts --- */
    {
        const int sample_ids[] = {1001, 1002, 1003};
        const char *sample_names[] = {
            "Rahul Sharma",
            "Priya Sharma",
            "Arjun Kumar",
        };
        const int64_t sample_balances[] = {2500000LL, 5000000LL, 7500000LL};

        for (i = 0; i < APPLICATION_SAMPLE_ACCOUNT_COUNT; i++) {
            bs = bank_create_account(
                &app->bank,
                sample_ids[i],
                sample_names[i],
                sample_balances[i],
                NULL);
            if (bs != BANK_SUCCESS) {
                app->state = APP_STATE_FAILED;
                application_cleanup(app);
                return APPLICATION_ERR_ACCOUNT_CREATION_FAILED;
            }
        }
        app->account_count = APPLICATION_SAMPLE_ACCOUNT_COUNT;
    }

    /* --- 3. Initialize ThreadPool --- */
    ts = thread_pool_init(
        &app->pool,
        APPLICATION_THREAD_POOL_WORKERS,
        APPLICATION_THREAD_POOL_QUEUE_CAPACITY);
    if (ts != THREAD_POOL_SUCCESS) {
        app->state = APP_STATE_FAILED;
        application_cleanup(app);
        return APPLICATION_ERR_THREAD_POOL_INIT_FAILED;
    }
    app->pool_initialized = 1;

    /* --- 4. Initialize TransactionManager (Bank + ThreadPool) --- */
    tms = transaction_manager_init(
        &app->txn_mgr,
        &app->pool,
        &app->bank,
        APPLICATION_MAX_TRANSACTION_REQUESTS);
    if (tms != TRANSACTION_MANAGER_SUCCESS) {
        app->state = APP_STATE_FAILED;
        application_cleanup(app);
        return APPLICATION_ERR_TRANSACTION_MANAGER_INIT_FAILED;
    }
    app->tm_initialized = 1;

    /* --- 5. Initialize SessionManager (TransactionManager) --- */
    sms = session_manager_init(
        &app->session_mgr,
        &app->txn_mgr,
        APPLICATION_MAX_SESSIONS);
    if (sms != SESSION_MANAGER_SUCCESS) {
        app->state = APP_STATE_FAILED;
        application_cleanup(app);
        return APPLICATION_ERR_SESSION_MANAGER_INIT_FAILED;
    }
    app->sm_initialized = 1;

    app->state = APP_STATE_RUNNING;
    return APPLICATION_SUCCESS;
}

ApplicationStatus application_shutdown(Application *app)
{
    if (app == NULL) {
        return APPLICATION_ERR_INVALID_ARGUMENT;
    }

    /* Nothing to do on an uninitialized or already-stopped application. */
    if (app->state == APP_STATE_UNINITIALIZED ||
        app->state == APP_STATE_STOPPED) {
        return APPLICATION_SUCCESS;
    }

    app->state = APP_STATE_SHUTTING_DOWN;

    /* --- 1. Stop accepting new customer sessions --- */
    if (app->sm_initialized) {
        (void)session_manager_shutdown(&app->session_mgr);
    }

    /* --- 2. Terminate existing sessions (idempotent per-session) ---
     * No sessions exist at this milestone, but the shutdown call above
     * prevents new submissions. Session slots are released by
     * session_manager_destroy in the cleanup step. --- */

    /* --- 3-4. Shut down TransactionManager (drains pending requests via
     * ThreadPool graceful shutdown). --- */
    if (app->tm_initialized) {
        if (transaction_manager_shutdown(&app->txn_mgr) !=
            TRANSACTION_MANAGER_SUCCESS) {
            app->state = APP_STATE_FAILED;
            application_cleanup(app);
            return APPLICATION_ERR_SHUTDOWN_FAILED;
        }
    }

    /* --- 5-7. Destroy all components in reverse initialization order. ---
     * TransactionManager.destroy already invoked ThreadPool.shutdown
     * (idempotent), so ThreadPool.destroy simply releases pool memory.
     * Bank is destroyed last, after no worker can access it. --- */
    application_cleanup(app);

    app->state = APP_STATE_STOPPED;
    return APPLICATION_SUCCESS;
}

ApplicationState application_get_state(const Application *app)
{
    if (app == NULL) {
        return APP_STATE_UNINITIALIZED;
    }
    return app->state;
}

int application_get_account_count(const Application *app)
{
    if (app == NULL) {
        return 0;
    }
    return app->account_count;
}

Bank *application_get_bank(Application *app)
{
    if (app == NULL) {
        return NULL;
    }
    return &app->bank;
}

SessionManager *application_get_session_manager(Application *app)
{
    if (app == NULL) {
        return NULL;
    }
    return &app->session_mgr;
}

void application_destroy(Application *app)
{
    if (app == NULL) {
        return;
    }
    application_cleanup(app);
    app->state = APP_STATE_STOPPED;
}
