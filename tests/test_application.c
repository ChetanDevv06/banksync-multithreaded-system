#include "application.h"

#include <stdio.h>
#include <string.h>

#include "bank.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
            tests_failed++;                                                 \
        }                                                                   \
    } while (0)

#define RUN_TEST(name)                                                      \
    do {                                                                    \
        printf("TEST: %s\n", #name);                                        \
        fflush(stdout);                                                     \
        name();                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test 1 — Application context initialization                        */
/* ------------------------------------------------------------------ */

static void test_1_app_context_zero_init(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    TEST_ASSERT(app.state == APP_STATE_UNINITIALIZED,
                "default state should be UNINITIALIZED");
    TEST_ASSERT(app.bank_initialized == 0, "bank should not be initialized");
    TEST_ASSERT(app.pool_initialized == 0, "pool should not be initialized");
    TEST_ASSERT(app.tm_initialized == 0, "tm should not be initialized");
    TEST_ASSERT(app.sm_initialized == 0, "sm should not be initialized");
    TEST_ASSERT(app.account_count == 0, "account count should be 0");
}

static void test_1_init_null_app(void)
{
    ApplicationStatus st = application_init(NULL);
    TEST_ASSERT(st == APPLICATION_ERR_INVALID_ARGUMENT,
                "init with NULL app should be rejected");
}

static void test_1_shutdown_null_app(void)
{
    ApplicationStatus st = application_shutdown(NULL);
    TEST_ASSERT(st == APPLICATION_ERR_INVALID_ARGUMENT,
                "shutdown with NULL app should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 2 — Successful complete initialization                        */
/* ------------------------------------------------------------------ */

static void test_2_successful_init(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "application_init should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_RUNNING,
                "state should be RUNNING after init");
    TEST_ASSERT(app.bank_initialized == 1, "bank should be initialized");
    TEST_ASSERT(app.pool_initialized == 1, "thread pool should be initialized");
    TEST_ASSERT(app.tm_initialized == 1,
                "transaction manager should be initialized");
    TEST_ASSERT(app.sm_initialized == 1,
                "session manager should be initialized");

    application_shutdown(&app);
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED after shutdown");
}

/* ------------------------------------------------------------------ */
/* Test 3 — Correct initial account count                             */
/* ------------------------------------------------------------------ */

static void test_3_account_count(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");
    TEST_ASSERT(application_get_account_count(&app) == 3,
                "should have 3 sample accounts");
    TEST_ASSERT(app.account_count == 3, "app account_count should be 3");

    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Test 4 — Correct initial account data                              */
/* ------------------------------------------------------------------ */

static void test_4_account_data(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    BankAccount *a1 = bank_find_account(&app.bank, 1001);
    TEST_ASSERT(a1 != NULL, "account 1001 should exist");
    if (a1 != NULL) {
        TEST_ASSERT(a1->account_id == 1001, "account ID should match");
        TEST_ASSERT(strcmp(a1->customer_name, "Rahul Sharma") == 0,
                    "customer name should match");
        TEST_ASSERT(strcmp(a1->account_number, "BS1001") == 0,
                    "account number should match");
        TEST_ASSERT(a1->balance_paise == 2500000LL,
                    "balance should match");
        TEST_ASSERT(a1->mutex_initialized == 1, "mutex should be initialized");
    }

    BankAccount *a2 = bank_find_account(&app.bank, 1002);
    TEST_ASSERT(a2 != NULL, "account 1002 should exist");
    if (a2 != NULL) {
        TEST_ASSERT(a2->account_id == 1002, "account ID should match");
        TEST_ASSERT(strcmp(a2->customer_name, "Priya Sharma") == 0,
                    "customer name should match");
        TEST_ASSERT(strcmp(a2->account_number, "BS1002") == 0,
                    "account number should match");
        TEST_ASSERT(a2->balance_paise == 5000000LL,
                    "balance should match");
        TEST_ASSERT(a2->mutex_initialized == 1, "mutex should be initialized");
    }

    BankAccount *a3 = bank_find_account(&app.bank, 1003);
    TEST_ASSERT(a3 != NULL, "account 1003 should exist");
    if (a3 != NULL) {
        TEST_ASSERT(a3->account_id == 1003, "account ID should match");
        TEST_ASSERT(strcmp(a3->customer_name, "Arjun Kumar") == 0,
                    "customer name should match");
        TEST_ASSERT(strcmp(a3->account_number, "BS1003") == 0,
                    "account number should match");
        TEST_ASSERT(a3->balance_paise == 7500000LL,
                    "balance should match");
        TEST_ASSERT(a3->mutex_initialized == 1, "mutex should be initialized");
    }

    BankAccount *missing = bank_find_account(&app.bank, 9999);
    TEST_ASSERT(missing == NULL, "nonexistent account should not be found");

    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Test 5 — Application state transitions                             */
/* ------------------------------------------------------------------ */

static void test_5_state_transitions(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    TEST_ASSERT(application_get_state(&app) == APP_STATE_UNINITIALIZED,
                "initial state should be UNINITIALIZED");

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_RUNNING,
                "state should be RUNNING after init");

    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED after shutdown");
}

static void test_5_reject_init_on_running(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "first init should succeed");

    st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_ERR_ALREADY_INITIALIZED,
                "re-init while running should be rejected");

    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Test 6 — Shutdown after successful initialization                  */
/* ------------------------------------------------------------------ */

static void test_6_shutdown_after_init(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED");
    TEST_ASSERT(app.bank_initialized == 0, "bank should be cleaned up");
    TEST_ASSERT(app.pool_initialized == 0, "pool should be cleaned up");
    TEST_ASSERT(app.tm_initialized == 0, "tm should be cleaned up");
    TEST_ASSERT(app.sm_initialized == 0, "sm should be cleaned up");
}

/* ------------------------------------------------------------------ */
/* Test 7 — Safe repeated shutdown                                    */
/* ------------------------------------------------------------------ */

static void test_7_repeated_shutdown(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "first shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED");

    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS,
                "repeated shutdown should be idempotent");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should remain STOPPED");
}

/* ------------------------------------------------------------------ */
/* Test 8 — Cleanup after partial initialization failure              */
/* ------------------------------------------------------------------ */

static void test_8_partial_init_cleanup(void)
{
    /*
     * Simulate a partially-initialized application: only the Bank and
     * ThreadPool are initialized. TransactionManager and SessionManager
     * are not. This mimics a failure at the TransactionManager init step.
     * We then call application_destroy (cleanup-only) and verify that
     * only the initialized components are released.
     */
    Application app;
    memset(&app, 0, sizeof(app));

    /* Manually initialize the Bank to simulate partial init success. */
    BankStatus bs = bank_init(&app.bank);
    TEST_ASSERT(bs == BANK_SUCCESS, "manual bank init should succeed");
    app.bank_initialized = 1;

    /* Manually initialize the ThreadPool. */
    ThreadPoolStatus ts = thread_pool_init(&app.pool, 2, 8);
    TEST_ASSERT(ts == THREAD_POOL_SUCCESS, "manual thread pool init should succeed");
    app.pool_initialized = 1;

    /* tm and sm remain uninitialized. */
    app.tm_initialized = 0;
    app.sm_initialized = 0;
    app.account_count = 0;
    app.state = APP_STATE_FAILED;

    /* Cleanup should release bank and pool, and skip tm/sm. */
    application_destroy(&app);
    TEST_ASSERT(app.bank_initialized == 0, "bank should be cleaned up");
    TEST_ASSERT(app.pool_initialized == 0, "pool should be cleaned up");
    TEST_ASSERT(app.tm_initialized == 0, "tm should not be touched");
    TEST_ASSERT(app.sm_initialized == 0, "sm should not be touched");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED after cleanup");
}

/* ------------------------------------------------------------------ */
/* Test 9 — Invalid application pointers                              */
/* ------------------------------------------------------------------ */

static void test_9_invalid_pointers(void)
{
    TEST_ASSERT(application_init(NULL) == APPLICATION_ERR_INVALID_ARGUMENT,
                "init NULL should be rejected");
    TEST_ASSERT(application_shutdown(NULL) == APPLICATION_ERR_INVALID_ARGUMENT,
                "shutdown NULL should be rejected");
    TEST_ASSERT(application_get_state(NULL) == APP_STATE_UNINITIALIZED,
                "get_state NULL should return UNINITIALIZED");
    TEST_ASSERT(application_get_account_count(NULL) == 0,
                "get_account_count NULL should return 0");
    application_destroy(NULL); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Test 10 — No access to destroyed components                        */
/* ------------------------------------------------------------------ */

static void test_10_no_access_after_destroy(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    application_shutdown(&app);

    /* After shutdown, the bank should be destroyed; find_account on a
     * destroyed bank returns NULL (bank_destroy memsets the struct). */
    BankAccount *acc = bank_find_account(&app.bank, 1001);
    TEST_ASSERT(acc == NULL, "destroyed bank should not find accounts");
}

/* ------------------------------------------------------------------ */
/* Test 11 — No leaked worker threads                                 */
/* ------------------------------------------------------------------ */

static void test_11_no_leaked_worker_threads(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    /* The ThreadPool is initialized with 4 workers. After shutdown,
     * thread_pool_shutdown (invoked via transaction_manager_shutdown and
     * thread_pool_destroy) joins all workers, so none are leaked. */
    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED");
    TEST_ASSERT(app.pool_initialized == 0, "pool should be destroyed");
    TEST_ASSERT(app.pool.workers == NULL, "pool workers should be freed");
    TEST_ASSERT(app.pool.queue == NULL, "pool queue should be freed");
}

/* ------------------------------------------------------------------ */
/* Test 12 — No outstanding requests after shutdown                   */
/* ------------------------------------------------------------------ */

static void test_12_no_outstanding_requests(void)
{
    Application app;
    memset(&app, 0, sizeof(app));

    ApplicationStatus st = application_init(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "init should succeed");

    /* No session or transaction requests are created at this milestone,
     * so there should be nothing outstanding after shutdown. */
    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED");
    TEST_ASSERT(app.tm_initialized == 0, "tm should be destroyed");
    TEST_ASSERT(app.sm_initialized == 0, "sm should be destroyed");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Test 1 — context initialization */
    RUN_TEST(test_1_app_context_zero_init);
    RUN_TEST(test_1_init_null_app);
    RUN_TEST(test_1_shutdown_null_app);

    /* Test 2 — successful complete initialization */
    RUN_TEST(test_2_successful_init);

    /* Test 3 — account count */
    RUN_TEST(test_3_account_count);

    /* Test 4 — account data */
    RUN_TEST(test_4_account_data);

    /* Test 5 — state transitions */
    RUN_TEST(test_5_state_transitions);
    RUN_TEST(test_5_reject_init_on_running);

    /* Test 6 — shutdown after init */
    RUN_TEST(test_6_shutdown_after_init);

    /* Test 7 — repeated shutdown */
    RUN_TEST(test_7_repeated_shutdown);

    /* Test 8 — partial init failure cleanup */
    RUN_TEST(test_8_partial_init_cleanup);

    /* Test 9 — invalid pointers */
    RUN_TEST(test_9_invalid_pointers);

    /* Test 10 — no access after destroy */
    RUN_TEST(test_10_no_access_after_destroy);

    /* Test 11 — no leaked worker threads */
    RUN_TEST(test_11_no_leaked_worker_threads);

    /* Test 12 — no outstanding requests after shutdown */
    RUN_TEST(test_12_no_outstanding_requests);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
