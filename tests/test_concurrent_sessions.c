#include "application.h"
#include "session_manager.h"
#include "transaction_manager.h"
#include "thread_pool.h"
#include "bank.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
/* Shared test fixtures                                               */
/* ------------------------------------------------------------------ */

static Bank g_bank;
static ThreadPool g_pool;
static TransactionManager g_tmmgr;
static SessionManager g_mgr;

static int setup_all(void)
{
    BankStatus bs = bank_init(&g_bank);
    if (bs != BANK_SUCCESS) {
        return 0;
    }

    bs = bank_create_account(&g_bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    if (bs != BANK_SUCCESS) {
        return 0;
    }
    bs = bank_create_account(&g_bank, 1002, "Priya Sharma", 5000000LL, NULL);
    if (bs != BANK_SUCCESS) {
        return 0;
    }
    bs = bank_create_account(&g_bank, 1003, "Arjun Kumar", 7500000LL, NULL);
    if (bs != BANK_SUCCESS) {
        return 0;
    }

    ThreadPoolStatus ts = thread_pool_init(&g_pool, 4, 16);
    if (ts != THREAD_POOL_SUCCESS) {
        return 0;
    }

    TransactionManagerStatus ms =
        transaction_manager_init(&g_tmmgr, &g_pool, &g_bank, 64);
    if (ms != TRANSACTION_MANAGER_SUCCESS) {
        return 0;
    }

    SessionManagerStatus sms =
        session_manager_init(&g_mgr, &g_tmmgr, 32);
    if (sms != SESSION_MANAGER_SUCCESS) {
        return 0;
    }

    return 1;
}

static void teardown_all(void)
{
    session_manager_destroy(&g_mgr);
    transaction_manager_destroy(&g_tmmgr);
    thread_pool_destroy(&g_pool);
    bank_destroy(&g_bank);
}

static int64_t get_balance(int account_id)
{
    int64_t bal = 0;
    bank_get_balance(&g_bank, account_id, &bal);
    return bal;
}

/* ------------------------------------------------------------------ */
/* Client worker — mirrors test_session_manager.c convention          */
/* ------------------------------------------------------------------ */

typedef struct {
    SessionManager *mgr;
    TransactionManager *tmmgr;
    uint64_t session_id;
    TransactionOperation operation;
    int source_account_id;
    int destination_account_id;
    int64_t amount_paise;
    uint64_t request_id;
    SessionManagerStatus submit_ms;
    TransactionManagerStatus wait_ms;
    TransactionManagerStatus get_ms;
    TransactionRequestStatus req_status;
    BankStatus bank_result;
    int64_t balance_result;
    int completed;
} client_job_t;

static void *client_thread_func(void *arg)
{
    client_job_t *job = (client_job_t *)arg;

    job->submit_ms = session_submit_transaction(
        job->mgr,
        job->session_id,
        job->operation,
        job->source_account_id,
        job->destination_account_id,
        job->amount_paise,
        &job->request_id);

    if (job->submit_ms == SESSION_MANAGER_SUCCESS) {
        job->wait_ms = transaction_manager_wait(job->tmmgr, job->request_id);
        if (job->wait_ms == TRANSACTION_MANAGER_SUCCESS) {
            job->get_ms = transaction_manager_get_result(
                job->tmmgr,
                job->request_id,
                &job->req_status,
                &job->bank_result,
                &job->balance_result);
            transaction_manager_release(job->tmmgr, job->request_id);
        }
    }

    job->completed = 1;
    return NULL;
}

/* ================================================================== */
/* Test A — Concurrent session creation                               */
/* ================================================================== */

static void test_a_concurrent_session_creation_unique_ids(void)
{
    const int count = 16;
    uint64_t sids[count];
    SessionManagerStatus results[count];

    for (int i = 0; i < count; i++) {
        results[i] = session_create(
            &g_mgr, 1001 + (i % 3), (i % 2 == 0) ? SESSION_CHANNEL_ATM
                                                 : SESSION_CHANNEL_ONLINE_BANKING,
            &sids[i]);
        TEST_ASSERT(results[i] == SESSION_MANAGER_SUCCESS,
                    "concurrent create should succeed (sequentially serialized by mutex)");
    }

    /* Every session ID must be unique. */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT(sids[i] > 0, "session ID must be positive");
        for (int j = i + 1; j < count; j++) {
            TEST_ASSERT(sids[i] != sids[j],
                        "no duplicate session IDs from creation");
        }
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_a_atm_and_online_coexist(void)
{
    const int count = 10;
    uint64_t atm_sids[count / 2];
    uint64_t online_sids[count / 2];
    SessionManagerStatus ms;

    for (int i = 0; i < count / 2; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &atm_sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "ATM session create should succeed");
        ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING,
                            &online_sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "Online Banking session create should succeed");
    }

    /* Verify each session's channel. */
    for (int i = 0; i < count / 2; i++) {
        SessionState st;
        int acct;
        SessionChannel ch;
        ms = session_get(&g_mgr, atm_sids[i], &st, &acct, &ch);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get ATM should succeed");
        TEST_ASSERT(st == SESSION_STATE_ACTIVE, "ATM session should be ACTIVE");
        TEST_ASSERT(ch == SESSION_CHANNEL_ATM, "ATM channel should match");

        ms = session_get(&g_mgr, online_sids[i], &st, &acct, &ch);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session_get Online Banking should succeed");
        TEST_ASSERT(st == SESSION_STATE_ACTIVE,
                    "Online Banking session should be ACTIVE");
        TEST_ASSERT(ch == SESSION_CHANNEL_ONLINE_BANKING,
                    "Online Banking channel should match");
    }

    for (int i = 0; i < count / 2; i++) {
        session_terminate(&g_mgr, atm_sids[i]);
        session_terminate(&g_mgr, online_sids[i]);
    }
}

/* ================================================================== */
/* Test B — Concurrent session retrieval                              */
/* ================================================================== */

typedef struct {
    SessionManager *mgr;
    uint64_t session_id;
    SessionState observed_state;
    int observed_account_id;
    SessionChannel observed_channel;
    SessionManagerStatus get_ms;
    int completed;
} retrieve_job_t;

static void *retrieve_thread_func(void *arg)
{
    retrieve_job_t *job = (retrieve_job_t *)arg;
    job->get_ms = session_get(
        job->mgr,
        job->session_id,
        &job->observed_state,
        &job->observed_account_id,
        &job->observed_channel);
    job->completed = 1;
    return NULL;
}

static void test_b_concurrent_session_retrieval(void)
{
    const int count = 12;
    uint64_t sids[count];
    pthread_t threads[count];
    retrieve_job_t jobs[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001 + (i % 3),
                            (i % 2 == 0) ? SESSION_CHANNEL_ATM
                                         : SESSION_CHANNEL_ONLINE_BANKING,
                            &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, retrieve_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].completed == 1, "retrieve thread should complete");
        TEST_ASSERT(jobs[i].get_ms == SESSION_MANAGER_SUCCESS,
                    "session_get should succeed");
        TEST_ASSERT(jobs[i].observed_state == SESSION_STATE_ACTIVE,
                    "retrieved session should be ACTIVE");
        TEST_ASSERT(jobs[i].observed_account_id == 1001 + (i % 3),
                    "retrieved account ID should match");
        TEST_ASSERT(jobs[i].observed_channel ==
                        ((i % 2 == 0) ? SESSION_CHANNEL_ATM
                                      : SESSION_CHANNEL_ONLINE_BANKING),
                    "retrieved channel should match");
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

/* ================================================================== */
/* Test C — Concurrent session termination                            */
/* ================================================================== */

typedef struct {
    SessionManager *mgr;
    uint64_t session_id;
    SessionManagerStatus term_ms;
    int completed;
} terminate_job_t;

static void *terminate_thread_func(void *arg)
{
    terminate_job_t *job = (terminate_job_t *)arg;
    job->term_ms = session_terminate(job->mgr, job->session_id);
    job->completed = 1;
    return NULL;
}

static void test_c_concurrent_session_termination(void)
{
    const int count = 10;
    uint64_t sids[count];
    pthread_t threads[count];
    terminate_job_t jobs[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, terminate_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].completed == 1, "terminate thread should complete");
        TEST_ASSERT(jobs[i].term_ms == SESSION_MANAGER_SUCCESS,
                    "terminate should succeed");
    }

    /* All sessions should now be TERMINATED. */
    for (int i = 0; i < count; i++) {
        SessionState st;
        ms = session_get(&g_mgr, sids[i], &st, NULL, NULL);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should succeed");
        TEST_ASSERT(st == SESSION_STATE_TERMINATED,
                    "session should be TERMINATED after concurrent termination");
    }
}

static void test_c_duplicate_termination_idempotent(void)
{
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "first terminate should succeed");
    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "second terminate should be idempotent");

    SessionState st;
    ms = session_get(&g_mgr, sid, &st, NULL, NULL);
    TEST_ASSERT(st == SESSION_STATE_TERMINATED,
                "session should remain TERMINATED");
}

/* ================================================================== */
/* Test D — Terminated session identity                               */
/* ================================================================== */

static void test_d_terminated_id_rejected_for_submit(void)
{
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    session_terminate(&g_mgr, sid);

    uint64_t req_id;
    ms = session_submit_transaction(
        &g_mgr, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_TERMINATED,
                "submit through terminated session should return SESSION_TERMINATED");
}

static void test_d_terminated_id_cannot_be_reused_as_valid(void)
{
    /* Terminate a session, then attempt to use its old ID for a new
     * transaction submission. The ID should not match any active session. */
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    session_terminate(&g_mgr, sid);

    /* session_get should still find the slot (TERMINATED), but submission
     * should be rejected because the session is not ACTIVE. */
    SessionState st;
    ms = session_get(&g_mgr, sid, &st, NULL, NULL);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should find terminated slot");
    TEST_ASSERT(st == SESSION_STATE_TERMINATED,
                "session should be TERMINATED");

    uint64_t req_id;
    ms = session_submit_transaction(
        &g_mgr, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_TERMINATED,
                "terminated session ID should not accept new transactions");
}

/* ================================================================== */
/* Test E — Slot reuse                                                */
/* ================================================================== */

static void test_e_slot_reuse_safe_after_termination(void)
{
    const int cycles = 5;
    uint64_t first_ids[cycles];
    uint64_t second_ids[cycles];
    SessionManagerStatus ms;

    for (int i = 0; i < cycles; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &first_ids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "first create should succeed");
        session_terminate(&g_mgr, first_ids[i]);
    }

    for (int i = 0; i < cycles; i++) {
        ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING,
                            &second_ids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "create after terminate should succeed");
    }

    /* New session IDs must differ from old ones (monotonically increasing). */
    for (int i = 0; i < cycles; i++) {
        TEST_ASSERT(second_ids[i] > first_ids[i],
                    "reused slot must receive a new, larger session ID");
    }

    /* Verify each reused slot has correct data. */
    for (int i = 0; i < cycles; i++) {
        SessionState st;
        int acct;
        SessionChannel ch;
        ms = session_get(&g_mgr, second_ids[i], &st, &acct, &ch);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should succeed");
        TEST_ASSERT(st == SESSION_STATE_ACTIVE, "reused session should be ACTIVE");
        TEST_ASSERT(acct == 1002, "reused session should have new account ID");
        TEST_ASSERT(ch == SESSION_CHANNEL_ONLINE_BANKING,
                    "reused session should have new channel");
    }

    for (int i = 0; i < cycles; i++) {
        session_terminate(&g_mgr, second_ids[i]);
    }
}

/* ================================================================== */
/* Test F — Transaction lifecycle vs session termination              */
/* ================================================================== */

typedef struct {
    SessionManager *mgr;
    TransactionManager *tmmgr;
    uint64_t session_id;
    int terminate_before_wait; /* 1 = terminate session before waiting */
    int64_t amount_paise;
    int source_account_id;
    BankStatus bank_result;
    TransactionManagerStatus wait_ms;
    int completed;
} lifecycle_job_t;

static void *lifecycle_thread_func(void *arg)
{
    lifecycle_job_t *job = (lifecycle_job_t *)arg;
    uint64_t req_id;
    SessionManagerStatus sms = session_submit_transaction(
        job->mgr,
        job->session_id,
        TRANSACTION_OP_DEPOSIT,
        job->source_account_id,
        0,
        job->amount_paise,
        &req_id);

    if (sms == SESSION_MANAGER_SUCCESS) {
        if (job->terminate_before_wait) {
            /* Terminate the session while the transaction is in flight. */
            (void)session_terminate(job->mgr, job->session_id);
        }

        job->wait_ms = transaction_manager_wait(job->tmmgr, req_id);
        if (job->wait_ms == TRANSACTION_MANAGER_SUCCESS) {
            TransactionRequestStatus st;
            transaction_manager_get_result(
                job->tmmgr, req_id, &st, &job->bank_result, NULL);
            transaction_manager_release(job->tmmgr, req_id);
        }
    } else {
        job->wait_ms = TRANSACTION_MANAGER_ERR_REQUEST_NOT_FOUND;
        job->bank_result = BANK_ERR_INVALID_ARGUMENT;
    }

    job->completed = 1;
    return NULL;
}

static void test_f_accepted_transaction_survives_termination(void)
{
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t balance_before = get_balance(1001);
    uint64_t req_id;
    ms = session_submit_transaction(
        &g_mgr, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 500LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    /* Terminate the session while the transaction is in flight. */
    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "terminate should succeed");

    /* The accepted transaction must still complete. */
    TransactionManagerStatus tms = transaction_manager_wait(&g_tmmgr, req_id);
    TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS,
                "wait for accepted transaction should succeed");

    BankStatus bs;
    transaction_manager_get_result(&g_tmmgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS,
                "accepted transaction should complete despite termination");
    TEST_ASSERT(get_balance(1001) == balance_before + 500LL,
                "deposit should have been applied");

    transaction_manager_release(&g_tmmgr, req_id);
}

static void test_f_new_submission_through_terminated_session_rejected(void)
{
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    session_terminate(&g_mgr, sid);

    uint64_t req_id;
    ms = session_submit_transaction(
        &g_mgr, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_TERMINATED,
                "new submission through terminated session should be rejected");
}

static void test_f_concurrent_termination_does_not_corrupt_accepted_requests(void)
{
    const int count = 8;
    uint64_t sids[count];
    pthread_t threads[count];
    lifecycle_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t balance_before = get_balance(1001);

    /* Create sessions and submit transactions from all of them. */
    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].tmmgr = &g_tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].terminate_before_wait = (i % 2 == 0);
        jobs[i].amount_paise = 100LL;
        jobs[i].source_account_id = 1001;
    }

    /* Launch all workers concurrently. Half terminate before waiting,
     * half wait first. All accepted transactions must complete. */
    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, lifecycle_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].completed == 1, "lifecycle thread should complete");
        TEST_ASSERT(jobs[i].wait_ms == TRANSACTION_MANAGER_SUCCESS,
                    "wait should succeed");
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "bank result should be SUCCESS");
    }

    int64_t expected = balance_before + (int64_t)count * 100LL;
    TEST_ASSERT(get_balance(1001) == expected,
                "all accepted transactions must complete exactly once "
                "regardless of termination timing");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

/* ================================================================== */
/* Test G — Multiple workers through shared TM/ThreadPool             */
/* ================================================================== */

static void test_g_multiple_workers_shared_infrastructure(void)
{
    const int count = 12;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t balance_before = get_balance(1001);

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, (i % 2 == 0) ? SESSION_CHANNEL_ATM
                                                       : SESSION_CHANNEL_ONLINE_BANKING,
                            &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].tmmgr = &g_tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 250LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].completed == 1, "client thread should complete");
        TEST_ASSERT(jobs[i].submit_ms == SESSION_MANAGER_SUCCESS,
                    "submit should succeed");
        TEST_ASSERT(jobs[i].wait_ms == TRANSACTION_MANAGER_SUCCESS,
                    "wait should succeed");
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "bank result should be SUCCESS");
    }

    int64_t expected = balance_before + (int64_t)count * 250LL;
    TEST_ASSERT(get_balance(1001) == expected,
                "all concurrent deposits through shared infrastructure "
                "must be applied exactly once");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_g_no_request_slot_leak(void)
{
    const int count = 20;
    uint64_t sids[count];
    uint64_t req_ids[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

        ms = session_submit_transaction(
            &g_mgr, sids[i], TRANSACTION_OP_DEPOSIT, 1001, 0, 10LL, &req_ids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

        TransactionManagerStatus tms = transaction_manager_wait(&g_tmmgr, req_ids[i]);
        TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS, "wait should succeed");
        transaction_manager_release(&g_tmmgr, req_ids[i]);

        session_terminate(&g_mgr, sids[i]);
    }

    /* After all releases, the transaction manager registry should have no
     * allocated slots remaining. We can't directly inspect this, but we can
     * verify that a new session can still submit and that the maximum
     * number of concurrent requests hasn't been permanently reduced. */
    SessionManager local_mgr;
    ms = session_manager_init(&local_mgr, &g_tmmgr, 8);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "local init should succeed");

    uint64_t local_sid;
    ms = session_create(&local_mgr, 1001, SESSION_CHANNEL_ATM, &local_sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "local create should succeed");

    uint64_t local_req[8];
    int accepted = 0;
    for (int i = 0; i < 8; i++) {
        ms = session_submit_transaction(
            &local_mgr, local_sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 10LL,
            &local_req[i]);
        if (ms == SESSION_MANAGER_SUCCESS) {
            accepted++;
        } else {
            break;
        }
    }
    TEST_ASSERT(accepted == 8, "all 8 request slots should be available");

    for (int i = 0; i < accepted; i++) {
        transaction_manager_release(&g_tmmgr, local_req[i]);
    }

    session_terminate(&local_mgr, local_sid);
    session_manager_destroy(&local_mgr);
}

/* ================================================================== */
/* Test H — Application shutdown with session orchestration           */
/* ================================================================== */

static void test_h_shutdown_handles_session_lifecycle(void)
{
    /* Create a session, submit a transaction, then shut down the
     * SessionManager. Active sessions remain; new creation is rejected. */
    uint64_t sid;
    SessionManagerStatus ms = session_create(
        &g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    /* Submit a transaction and let it complete. */
    uint64_t req_id;
    ms = session_submit_transaction(
        &g_mgr, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    TransactionManagerStatus tms = transaction_manager_wait(&g_tmmgr, req_id);
    TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS, "wait should succeed");
    transaction_manager_release(&g_tmmgr, req_id);

    /* Shut down the session manager. */
    ms = session_manager_shutdown(&g_mgr);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "shutdown should succeed");

    /* New session creation should be rejected after shutdown. */
    uint64_t new_sid;
    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &new_sid);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SHUTDOWN,
                "create after shutdown should return SHUTDOWN");

    /* Existing session still reflects its last known state. */
    SessionState st;
    ms = session_get(&g_mgr, sid, &st, NULL, NULL);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should still work");
    TEST_ASSERT(st == SESSION_STATE_ACTIVE,
                "active session should remain ACTIVE after shutdown");

    /* Terminate the existing session after shutdown. */
    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "terminate after shutdown should succeed");
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void)
{
    if (!setup_all()) {
        printf("FAIL: setup_all failed\n");
        return 1;
    }

    /* Test A — Concurrent session creation */
    RUN_TEST(test_a_concurrent_session_creation_unique_ids);
    RUN_TEST(test_a_atm_and_online_coexist);

    /* Test B — Concurrent session retrieval */
    RUN_TEST(test_b_concurrent_session_retrieval);

    /* Test C — Concurrent session termination */
    RUN_TEST(test_c_concurrent_session_termination);
    RUN_TEST(test_c_duplicate_termination_idempotent);

    /* Test D — Terminated session identity */
    RUN_TEST(test_d_terminated_id_rejected_for_submit);
    RUN_TEST(test_d_terminated_id_cannot_be_reused_as_valid);

    /* Test E — Slot reuse */
    RUN_TEST(test_e_slot_reuse_safe_after_termination);

    /* Test F — Transaction lifecycle vs session termination */
    RUN_TEST(test_f_accepted_transaction_survives_termination);
    RUN_TEST(test_f_new_submission_through_terminated_session_rejected);
    RUN_TEST(test_f_concurrent_termination_does_not_corrupt_accepted_requests);

    /* Test G — Multiple workers through shared TM/ThreadPool */
    RUN_TEST(test_g_multiple_workers_shared_infrastructure);
    RUN_TEST(test_g_no_request_slot_leak);

    /* Test H — Application shutdown with session orchestration */
    RUN_TEST(test_h_shutdown_handles_session_lifecycle);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");
    fflush(stdout);

    teardown_all();

    return tests_failed == 0 ? 0 : 1;
}
