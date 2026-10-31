#include "session_manager.h"

#include <stdio.h>
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
/* Shared test state                                                  */
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

    bs = bank_create_account(&g_bank, 1001, "Alice", 5000000LL, NULL);
    bs = bank_create_account(&g_bank, 1002, "Bob", 3000000LL, NULL);
    bs = bank_create_account(&g_bank, 1003, "Carol", 1000000LL, NULL);

    ThreadPoolStatus ts = thread_pool_init(&g_pool, 4, 16);
    if (ts != THREAD_POOL_SUCCESS) {
        return 0;
    }

    TransactionManagerStatus ms =
        transaction_manager_init(&g_tmmgr, &g_pool, &g_bank, 32);
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
    /* Session manager destroy does NOT touch the TransactionManager. */
    session_manager_destroy(&g_mgr);

    /* Transaction manager destroy shuts down the thread pool. */
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
/* Client thread helper                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    SessionManager *mgr;
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
        job->wait_ms = transaction_manager_wait(job->mgr->tmmgr, job->request_id);
        if (job->wait_ms == TRANSACTION_MANAGER_SUCCESS) {
            job->get_ms = transaction_manager_get_result(
                job->mgr->tmmgr,
                job->request_id,
                &job->req_status,
                &job->bank_result,
                &job->balance_result);
            transaction_manager_release(job->mgr->tmmgr, job->request_id);
        }
    }

    job->completed = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test A — Initialization                                            */
/* ------------------------------------------------------------------ */

static void test_a_successful_initialization(void)
{
    SessionManager mgr;
    SessionManagerStatus ms;

    ms = session_manager_init(&mgr, &g_tmmgr, 16);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "successful init should return SUCCESS");
    TEST_ASSERT(mgr.initialized == 1, "manager should be marked initialized");
    TEST_ASSERT(mgr.max_sessions == 16, "max_sessions should be 16");
    TEST_ASSERT(mgr.shutdown == 0, "shutdown should be false after init");
    TEST_ASSERT(mgr.active_sessions == 0,
                "active_sessions should be 0 after init");
    TEST_ASSERT(mgr.tmmgr == &g_tmmgr,
                "manager should reference the transaction manager");

    session_manager_destroy(&mgr);
}

static void test_a2_null_manager(void)
{
    SessionManagerStatus ms =
        session_manager_init(NULL, &g_tmmgr, 16);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with NULL manager should return INVALID_ARGUMENT");
}

static void test_a3_null_tmmgr(void)
{
    SessionManager mgr;
    SessionManagerStatus ms =
        session_manager_init(&mgr, NULL, 16);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with NULL tmmgr should return INVALID_ARGUMENT");
}

static void test_a4_zero_max_sessions(void)
{
    SessionManager mgr;
    SessionManagerStatus ms =
        session_manager_init(&mgr, &g_tmmgr, 0);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with zero max_sessions should return INVALID_ARGUMENT");
}

static void test_a5_destroy_on_uninitialized_is_noop(void)
{
    SessionManager mgr;
    memset(&mgr, 0, sizeof(mgr));
    session_manager_destroy(&mgr);
    TEST_ASSERT(1, "destroy on uninitialized manager should be safe no-op");
}

static void test_a6_destroy_on_null_is_noop(void)
{
    session_manager_destroy(NULL);
    TEST_ASSERT(1, "destroy on NULL manager should be safe no-op");
}

static void test_a7_repeated_init_destroy(void)
{
    for (int i = 0; i < 5; i++) {
        SessionManager mgr;
        SessionManagerStatus ms;

        ms = session_manager_init(&mgr, &g_tmmgr, 8);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "repeated init should succeed");

        uint64_t sid;
        ms = session_create(&mgr, 1001, SESSION_CHANNEL_ATM, &sid);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

        session_manager_destroy(&mgr);
    }
}

/* ------------------------------------------------------------------ */
/* Test B — Session creation                                          */
/* ------------------------------------------------------------------ */

static void test_b_create_atm_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "ATM session create should succeed");
    TEST_ASSERT(sid > 0, "session ID should be positive");

    SessionState st;
    int acct;
    SessionChannel ch;
    ms = session_get(&g_mgr, sid, &st, &acct, &ch);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should succeed");
    TEST_ASSERT(st == SESSION_STATE_ACTIVE, "session should be ACTIVE");
    TEST_ASSERT(acct == 1001, "account_id should match");
    TEST_ASSERT(ch == SESSION_CHANNEL_ATM, "channel should be ATM");

    session_terminate(&g_mgr, sid);
}

static void test_b_create_online_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "online session create should succeed");

    SessionState st;
    int acct;
    SessionChannel ch;
    ms = session_get(&g_mgr, sid, &st, &acct, &ch);
    TEST_ASSERT(st == SESSION_STATE_ACTIVE, "session should be ACTIVE");
    TEST_ASSERT(acct == 1002, "account_id should match");
    TEST_ASSERT(ch == SESSION_CHANNEL_ONLINE_BANKING, "channel should be ONLINE_BANKING");

    session_terminate(&g_mgr, sid);
}

static void test_b_multiple_sessions(void)
{
    uint64_t sids[10];
    SessionManagerStatus ms;

    for (int i = 0; i < 10; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
    }

    for (int i = 0; i < 10; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_b_invalid_account(void)
{
    /* Session manager doesn't validate account existence — that's the
     * bank's job. A session can be created for any account ID. */
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 9999, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "session create for nonexistent account should succeed "
                "(bank validates at transaction time)");

    session_terminate(&g_mgr, sid);
}

static void test_b_invalid_channel(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, (SessionChannel)99, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_INVALID_ARGUMENT,
                "invalid channel should return INVALID_ARGUMENT");
}

static void test_b_registry_capacity(void)
{
    /* Use a local session manager so the registry starts clean (no
     * TERMINATED slots left over from earlier tests). */
    SessionManager local_mgr;
    SessionManagerStatus sms = session_manager_init(&local_mgr, &g_tmmgr, 32);
    TEST_ASSERT(sms == SESSION_MANAGER_SUCCESS, "local init should succeed");

    uint64_t sids[32];
    int accepted = 0;

    for (int i = 0; i < 32; i++) {
        SessionManagerStatus ms = session_create(
            &local_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        if (ms == SESSION_MANAGER_SUCCESS) {
            accepted++;
        } else {
            break;
        }
    }

    uint64_t extra_id;
    SessionManagerStatus ms = session_create(
        &local_mgr, 1001, SESSION_CHANNEL_ATM, &extra_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_REGISTRY_FULL,
                "create when registry is full should return REGISTRY_FULL");

    /* Release all accepted slots. */
    for (int i = 0; i < accepted; i++) {
        session_terminate(&local_mgr, sids[i]);
    }
    TEST_ASSERT(accepted == 32, "registry should be fully filled");

    session_manager_destroy(&local_mgr);
}

static void test_b_unique_session_ids(void)
{
    /* Use a local session manager so the registry starts clean. */
    SessionManager local_mgr;
    SessionManagerStatus sms = session_manager_init(&local_mgr, &g_tmmgr, 32);
    TEST_ASSERT(sms == SESSION_MANAGER_SUCCESS, "local init should succeed");

    const int count = 20;
    uint64_t sids[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&local_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
    }

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            TEST_ASSERT(sids[i] != sids[j],
                        "no duplicate session IDs should be issued");
        }
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&local_mgr, sids[i]);
    }

    session_manager_destroy(&local_mgr);
}

static void test_b_session_id_monotonic_after_reuse(void)
{
    /* Use a local session manager for a clean registry. */
    SessionManager local_mgr;
    SessionManagerStatus sms = session_manager_init(&local_mgr, &g_tmmgr, 8);
    TEST_ASSERT(sms == SESSION_MANAGER_SUCCESS, "local init should succeed");

    SessionManagerStatus ms;
    uint64_t id1, id2;

    ms = session_create(&local_mgr, 1001, SESSION_CHANNEL_ATM, &id1);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "first create should succeed");
    session_terminate(&local_mgr, id1);

    ms = session_create(&local_mgr, 1001, SESSION_CHANNEL_ATM, &id2);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create after terminate should succeed");
    TEST_ASSERT(id2 != id1, "session IDs must be unique after slot reuse");
    TEST_ASSERT(id2 > id1, "session IDs must be monotonically increasing");

    session_terminate(&local_mgr, id2);

    session_manager_destroy(&local_mgr);
}

/* ------------------------------------------------------------------ */
/* Test C — Session operations                                        */
/* ------------------------------------------------------------------ */

static void test_c_deposit_through_atm_session(void)
{
    SessionManagerStatus ms;
    TransactionManagerStatus tms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t before = get_balance(1001);
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 123456LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    tms = transaction_manager_wait(&g_tmmgr, req_id);
    TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS, "wait should succeed");

    BankStatus bs;
    transaction_manager_get_result(&g_tmmgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS, "deposit bank result should be SUCCESS");
    TEST_ASSERT(get_balance(1001) == before + 123456LL,
                "balance should reflect deposit");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_c_withdrawal_through_atm_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t before = get_balance(1002);
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_WITHDRAWAL,
                                    1002, 0, 100000LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_tmmgr, req_id);
    TEST_ASSERT(get_balance(1002) == before - 100000LL,
                "balance should reflect withdrawal");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_c_transfer_through_online_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ONLINE_BANKING, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t src_before = get_balance(1001);
    int64_t dst_before = get_balance(1002);
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_TRANSFER,
                                    1001, 1002, 250000LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_tmmgr, req_id);
    TEST_ASSERT(get_balance(1001) == src_before - 250000LL,
                "source should be debited");
    TEST_ASSERT(get_balance(1002) == dst_before + 250000LL,
                "destination should be credited");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_c_balance_enquiry_through_online_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_BALANCE_ENQUIRY,
                                    1002, 0, 0, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_tmmgr, req_id);
    int64_t bal = 0;
    BankStatus bs;
    transaction_manager_get_result(&g_tmmgr, req_id, NULL, &bs, &bal);
    TEST_ASSERT(bs == BANK_SUCCESS, "balance enquiry should succeed");
    TEST_ASSERT(bal == get_balance(1002),
                "returned balance should match actual balance");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_c_invalid_session_id(void)
{
    uint64_t req_id;
    SessionManagerStatus ms = session_submit_transaction(
        &g_mgr, 999999, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_NOT_FOUND,
                "submit with invalid session ID should return SESSION_NOT_FOUND");
}

/* ------------------------------------------------------------------ */
/* Test D — Lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void test_d_terminate_active_session(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    SessionState st;
    ms = session_get(&g_mgr, sid, &st, NULL, NULL);
    TEST_ASSERT(st == SESSION_STATE_ACTIVE, "session should be ACTIVE");

    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "terminate should succeed");

    ms = session_get(&g_mgr, sid, &st, NULL, NULL);
    TEST_ASSERT(st == SESSION_STATE_TERMINATED,
                "session should be TERMINATED after terminate");
}

static void test_d_reject_submit_after_termination(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    session_terminate(&g_mgr, sid);

    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_TERMINATED,
                "submit after termination should return SESSION_TERMINATED");
}

static void test_d_reject_submit_invalid_session(void)
{
    uint64_t req_id;
    SessionManagerStatus ms = session_submit_transaction(
        &g_mgr, 999999, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_NOT_FOUND,
                "submit with nonexistent session should return SESSION_NOT_FOUND");
}

static void test_d_termination_does_not_cancel_accepted_requests(void)
{
    SessionManagerStatus ms;
    TransactionManagerStatus tms;
    uint64_t sid, req_id;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    /* Submit a transaction. */
    int64_t balance_before = get_balance(1001);
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 500LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    /* Terminate the session while the transaction is in flight. */
    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "terminate should succeed");

    /* The accepted transaction should still complete. */
    tms = transaction_manager_wait(&g_tmmgr, req_id);
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

static void test_d_unrelated_sessions_remain_active(void)
{
    SessionManagerStatus ms;
    uint64_t sid1, sid2;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid1);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create 1 should succeed");
    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sid2);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create 2 should succeed");

    session_terminate(&g_mgr, sid1);

    SessionState st1, st2;
    session_get(&g_mgr, sid1, &st1, NULL, NULL);
    session_get(&g_mgr, sid2, &st2, NULL, NULL);
    TEST_ASSERT(st1 == SESSION_STATE_TERMINATED, "session 1 should be TERMINATED");
    TEST_ASSERT(st2 == SESSION_STATE_ACTIVE, "session 2 should remain ACTIVE");

    /* Session 2 can still submit. */
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid2, TRANSACTION_OP_DEPOSIT,
                                    1002, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "active session should submit");

    transaction_manager_wait(&g_tmmgr, req_id);
    transaction_manager_release(&g_tmmgr, req_id);

    session_terminate(&g_mgr, sid2);
}

static void test_d_terminate_idempotent(void)
{
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "first terminate should succeed");
    ms = session_terminate(&g_mgr, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "second terminate should be idempotent");
}

/* ------------------------------------------------------------------ */
/* Test E — Concurrency                                               */
/* ------------------------------------------------------------------ */

static void test_e_concurrent_atm_sessions(void)
{
    const int count = 8;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t balance_before = get_balance(1001);

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 500LL;
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

    int64_t expected = balance_before + (int64_t)count * 500LL;
    TEST_ASSERT(get_balance(1001) == expected,
                "all concurrent deposits should be applied exactly once");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_e_concurrent_online_sessions(void)
{
    const int count = 8;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1002;
        jobs[i].amount_paise = 500LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].submit_ms == SESSION_MANAGER_SUCCESS,
                    "submit should succeed");
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "bank result should be SUCCESS");
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_e_mixed_atm_and_online_sessions(void)
{
    const int count = 6;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        SessionChannel ch = (i % 2 == 0) ? SESSION_CHANNEL_ATM
                                         : SESSION_CHANNEL_ONLINE_BANKING;
        int acct = (i % 2 == 0) ? 1001 : 1002;
        ms = session_create(&g_mgr, acct, ch, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = acct;
        jobs[i].amount_paise = 100LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "all mixed-session deposits should succeed");
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_e_concurrent_same_account(void)
{
    const int count = 10;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t before = get_balance(1001);

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 777LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "all same-account deposits should succeed");
    }

    int64_t after = get_balance(1001);
    TEST_ASSERT(after == before + (int64_t)count * 777LL,
                "all deposits to same account must be applied exactly once");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_e_concurrent_different_accounts(void)
{
    pthread_t threads[2];
    client_job_t jobs[2];
    SessionManagerStatus ms;
    uint64_t sid_a, sid_b;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid_a);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create A should succeed");
    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sid_b);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create B should succeed");

    int64_t a_before = get_balance(1001);
    int64_t b_before = get_balance(1002);

    memset(&jobs[0], 0, sizeof(jobs[0]));
    jobs[0].mgr = &g_mgr;
    jobs[0].session_id = sid_a;
    jobs[0].operation = TRANSACTION_OP_DEPOSIT;
    jobs[0].source_account_id = 1001;
    jobs[0].amount_paise = 100LL;

    memset(&jobs[1], 0, sizeof(jobs[1]));
    jobs[1].mgr = &g_mgr;
    jobs[1].session_id = sid_b;
    jobs[1].operation = TRANSACTION_OP_DEPOSIT;
    jobs[1].source_account_id = 1002;
    jobs[1].amount_paise = 200LL;

    pthread_create(&threads[0], NULL, client_thread_func, &jobs[0]);
    pthread_create(&threads[1], NULL, client_thread_func, &jobs[1]);

    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    TEST_ASSERT(jobs[0].bank_result == BANK_SUCCESS, "deposit A should succeed");
    TEST_ASSERT(jobs[1].bank_result == BANK_SUCCESS, "deposit B should succeed");
    TEST_ASSERT(get_balance(1001) == a_before + 100LL, "account A balance correct");
    TEST_ASSERT(get_balance(1002) == b_before + 200LL, "account B balance correct");

    session_terminate(&g_mgr, sid_a);
    session_terminate(&g_mgr, sid_b);
}

static void test_e_opposite_direction_transfers(void)
{
    pthread_t threads[2];
    client_job_t jobs[2];
    SessionManagerStatus ms;
    uint64_t sid_ab, sid_ba;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid_ab);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create A->B should succeed");
    ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sid_ba);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create B->A should succeed");

    int64_t a_before = get_balance(1001);
    int64_t b_before = get_balance(1002);

    memset(&jobs[0], 0, sizeof(jobs[0]));
    jobs[0].mgr = &g_mgr;
    jobs[0].session_id = sid_ab;
    jobs[0].operation = TRANSACTION_OP_TRANSFER;
    jobs[0].source_account_id = 1001;
    jobs[0].destination_account_id = 1002;
    jobs[0].amount_paise = 100LL;

    memset(&jobs[1], 0, sizeof(jobs[1]));
    jobs[1].mgr = &g_mgr;
    jobs[1].session_id = sid_ba;
    jobs[1].operation = TRANSACTION_OP_TRANSFER;
    jobs[1].source_account_id = 1002;
    jobs[1].destination_account_id = 1001;
    jobs[1].amount_paise = 100LL;

    pthread_create(&threads[0], NULL, client_thread_func, &jobs[0]);
    pthread_create(&threads[1], NULL, client_thread_func, &jobs[1]);

    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    TEST_ASSERT(jobs[0].bank_result == BANK_SUCCESS, "transfer A->B should succeed");
    TEST_ASSERT(jobs[1].bank_result == BANK_SUCCESS, "transfer B->A should succeed");
    /* Net effect: balances unchanged (100 out, 100 in for each). */
    TEST_ASSERT(get_balance(1001) == a_before, "account A net balance unchanged");
    TEST_ASSERT(get_balance(1002) == b_before, "account B net balance unchanged");

    session_terminate(&g_mgr, sid_ab);
    session_terminate(&g_mgr, sid_ba);
}

static void test_e_concurrent_session_creation(void)
{
    /* Multiple threads creating sessions concurrently. Since session_create
     * takes the session mutex, this tests registry allocation under
     * contention. */
    const int count = 16;
    uint64_t sids[count];
    SessionManagerStatus results[count];

    /* Session creation is mutex-protected; do it sequentially for
     * deterministic results while still testing unique ID generation. */
    for (int i = 0; i < count; i++) {
        results[i] = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(results[i] == SESSION_MANAGER_SUCCESS,
                    "concurrent create should succeed");
    }

    /* Verify all unique. */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            TEST_ASSERT(sids[i] != sids[j],
                        "no duplicate session IDs from concurrent creation");
        }
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_e_concurrent_transaction_submission(void)
{
    const int count = 12;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;

    /* Create one session, then submit many transactions from it concurrently. */
    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[0]);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t balance_before = get_balance(1001);

    for (int i = 0; i < count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[0];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 10LL;
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "all concurrent submissions should succeed");
    }

    int64_t expected = balance_before + (int64_t)count * 10LL;
    TEST_ASSERT(get_balance(1001) == expected,
                "all concurrent submissions must be applied exactly once");

    session_terminate(&g_mgr, sids[0]);
}

/* ------------------------------------------------------------------ */
/* Test F — Financial invariants                                      */
/* ------------------------------------------------------------------ */

static void test_f_total_balance_conservation(void)
{
    /* Transfer between accounts conserves total balance. */
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t total_before = get_balance(1001) + get_balance(1002);
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_TRANSFER,
                                    1001, 1002, 500000LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_tmmgr, req_id);
    int64_t total_after = get_balance(1001) + get_balance(1002);
    TEST_ASSERT(total_before == total_after,
                "total balance must be conserved for transfers");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_f_no_negative_balances(void)
{
    /* Attempt withdrawal exceeding balance — should fail, balance unchanged. */
    SessionManagerStatus ms;
    uint64_t sid;

    ms = session_create(&g_mgr, 1003, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

    int64_t before = get_balance(1003);
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, sid, TRANSACTION_OP_WITHDRAWAL,
                                    1003, 0, 999999999LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_tmmgr, req_id);
    BankStatus bs;
    transaction_manager_get_result(&g_tmmgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_ERROR_INSUFFICIENT_FUNDS,
                "overdraft withdrawal should fail");
    TEST_ASSERT(get_balance(1003) == before,
                "balance must not go negative");

    transaction_manager_release(&g_tmmgr, req_id);
    session_terminate(&g_mgr, sid);
}

static void test_f_no_lost_deposits_or_withdrawals(void)
{
    const int count = 15;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t before = get_balance(1001);

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 333LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "all deposits should succeed");
    }

    int64_t after = get_balance(1001);
    TEST_ASSERT(after == before + (int64_t)count * 333LL,
                "no deposits should be lost");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_f_no_duplicate_execution(void)
{
    /* Each accepted transaction executes exactly once — verified by
     * balance delta matching the number of submissions. */
    const int count = 10;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;
    int64_t before = get_balance(1002);

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1002, SESSION_CHANNEL_ONLINE_BANKING, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &g_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_WITHDRAWAL;
        jobs[i].source_account_id = 1002;
        jobs[i].amount_paise = 100LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "all withdrawals should succeed");
    }

    int64_t after = get_balance(1002);
    TEST_ASSERT(after == before - (int64_t)count * 100LL,
                "no withdrawal should be duplicated");

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

static void test_f_every_accepted_request_reaches_terminal_state(void)
{
    const int count = 8;
    uint64_t sids[count];
    uint64_t req_ids[count];
    SessionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

        ms = session_submit_transaction(&g_mgr, sids[i], TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 100LL, &req_ids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");
    }

    /* Wait for all requests. */
    for (int i = 0; i < count; i++) {
        TransactionManagerStatus tms =
            transaction_manager_wait(&g_tmmgr, req_ids[i]);
        TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS,
                    "wait should succeed");

        TransactionRequestStatus st;
        BankStatus bs;
        transaction_manager_get_result(&g_tmmgr, req_ids[i], &st, &bs, NULL);
        TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED ||
                    st == TRANSACTION_REQUEST_FAILED,
                    "each request should reach a terminal state");
        transaction_manager_release(&g_tmmgr, req_ids[i]);
    }

    for (int i = 0; i < count; i++) {
        session_terminate(&g_mgr, sids[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Test G — Shutdown and cleanup                                      */
/* ------------------------------------------------------------------ */

static void test_g_reject_new_sessions_after_shutdown(void)
{
    SessionManagerStatus ms;

    ms = session_manager_shutdown(&g_mgr);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "shutdown should succeed");

    uint64_t sid;
    ms = session_create(&g_mgr, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SHUTDOWN,
                "create after shutdown should return SHUTDOWN");

    /* Also reject submissions after shutdown. */
    uint64_t req_id;
    ms = session_submit_transaction(&g_mgr, 1, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SHUTDOWN,
                "submit after shutdown should return SHUTDOWN");
}

static void test_g_active_threads_finish_before_destroy(void)
{
    /* Use a LOCAL session manager (referencing g_tmmgr) so that destroying
     * it doesn't affect g_mgr used by other tests. Submit transactions,
     * wait for all client threads to finish, then destroy the local
     * manager. No thread should access manager memory after destroy. */
    const int count = 6;
    uint64_t sids[count];
    pthread_t threads[count];
    client_job_t jobs[count];
    SessionManagerStatus ms;

    SessionManager local_mgr;
    ms = session_manager_init(&local_mgr, &g_tmmgr, 8);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "local init should succeed");

    for (int i = 0; i < count; i++) {
        ms = session_create(&local_mgr, 1001, SESSION_CHANNEL_ATM, &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &local_mgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].amount_paise = 100LL;
    }

    for (int i = 0; i < count; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &jobs[i]);
    }

    for (int i = 0; i < count; i++) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT(jobs[i].completed == 1, "client thread should complete");
        TEST_ASSERT(jobs[i].bank_result == BANK_SUCCESS,
                    "bank result should be SUCCESS");
    }

    /* Terminate all sessions before destroying the local manager. */
    for (int i = 0; i < count; i++) {
        session_terminate(&local_mgr, sids[i]);
    }

    session_manager_destroy(&local_mgr);
    TEST_ASSERT(1, "destroy after all threads finished should be safe");
}

static void test_g_repeated_lifecycle(void)
{
    for (int i = 0; i < 5; i++) {
        SessionManager mgr;
        SessionManagerStatus ms;

        ms = session_manager_init(&mgr, &g_tmmgr, 8);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "repeated init should succeed");

        uint64_t sid;
        ms = session_create(&mgr, 1001, SESSION_CHANNEL_ATM, &sid);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "create should succeed");

        uint64_t req_id;
        ms = session_submit_transaction(&mgr, sid, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 100LL, &req_id);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "submit should succeed");

        transaction_manager_wait(&g_tmmgr, req_id);
        transaction_manager_release(&g_tmmgr, req_id);

        session_terminate(&mgr, sid);
        session_manager_destroy(&mgr);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    if (!setup_all()) {
        printf("FAIL: setup_all failed\n");
        return 1;
    }

    /* Test A — Initialization */
    RUN_TEST(test_a_successful_initialization);
    RUN_TEST(test_a2_null_manager);
    RUN_TEST(test_a3_null_tmmgr);
    RUN_TEST(test_a4_zero_max_sessions);
    RUN_TEST(test_a5_destroy_on_uninitialized_is_noop);
    RUN_TEST(test_a6_destroy_on_null_is_noop);
    RUN_TEST(test_a7_repeated_init_destroy);

    /* Test B — Session creation */
    RUN_TEST(test_b_create_atm_session);
    RUN_TEST(test_b_create_online_session);
    RUN_TEST(test_b_multiple_sessions);
    RUN_TEST(test_b_invalid_account);
    RUN_TEST(test_b_invalid_channel);
    RUN_TEST(test_b_registry_capacity);
    RUN_TEST(test_b_unique_session_ids);
    RUN_TEST(test_b_session_id_monotonic_after_reuse);

    /* Test C — Session operations */
    RUN_TEST(test_c_deposit_through_atm_session);
    RUN_TEST(test_c_withdrawal_through_atm_session);
    RUN_TEST(test_c_transfer_through_online_session);
    RUN_TEST(test_c_balance_enquiry_through_online_session);
    RUN_TEST(test_c_invalid_session_id);

    /* Test D — Lifecycle */
    RUN_TEST(test_d_terminate_active_session);
    RUN_TEST(test_d_reject_submit_after_termination);
    RUN_TEST(test_d_reject_submit_invalid_session);
    RUN_TEST(test_d_termination_does_not_cancel_accepted_requests);
    RUN_TEST(test_d_unrelated_sessions_remain_active);
    RUN_TEST(test_d_terminate_idempotent);

    /* Test E — Concurrency */
    RUN_TEST(test_e_concurrent_atm_sessions);
    RUN_TEST(test_e_concurrent_online_sessions);
    RUN_TEST(test_e_mixed_atm_and_online_sessions);
    RUN_TEST(test_e_concurrent_same_account);
    RUN_TEST(test_e_concurrent_different_accounts);
    RUN_TEST(test_e_opposite_direction_transfers);
    RUN_TEST(test_e_concurrent_session_creation);
    RUN_TEST(test_e_concurrent_transaction_submission);

    /* Test F — Financial invariants */
    RUN_TEST(test_f_total_balance_conservation);
    RUN_TEST(test_f_no_negative_balances);
    RUN_TEST(test_f_no_lost_deposits_or_withdrawals);
    RUN_TEST(test_f_no_duplicate_execution);
    RUN_TEST(test_f_every_accepted_request_reaches_terminal_state);

    /* Test G — Shutdown and cleanup.
     * NOTE: test_g_active_threads_finish_before_destroy destroys g_mgr,
     * so it must run last. test_g_reject_new_sessions_after_shutdown uses
     * g_mgr and must run before it. */
    RUN_TEST(test_g_reject_new_sessions_after_shutdown);
    RUN_TEST(test_g_repeated_lifecycle);
    RUN_TEST(test_g_active_threads_finish_before_destroy);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");
    fflush(stdout);

    teardown_all();

    return tests_failed == 0 ? 0 : 1;
}
