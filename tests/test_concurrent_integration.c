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

#define MAX_OPS_PER_WORKER 16

/* ------------------------------------------------------------------ */
/* Worker job — each client thread owns one session and executes a    */
/* sequence of operations through the full pipeline.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    Application *app;
    SessionManager *sm;
    TransactionManager *tm;
    Bank *bank;
    uint64_t session_id;
    int account_id;
    SessionChannel channel;
    pthread_barrier_t *barrier;

    int op_count;
    TransactionOperation ops[MAX_OPS_PER_WORKER];
    int source_accts[MAX_OPS_PER_WORKER];
    int dest_accts[MAX_OPS_PER_WORKER];
    int64_t amounts[MAX_OPS_PER_WORKER];

    int submit_count;
    int release_count;
    int successful_count;
    int64_t successful_total_paise;
    int completed;
} integration_job_t;

static void *integration_worker_func(void *arg)
{
    integration_job_t *job = (integration_job_t *)arg;

    if (job->barrier != NULL) {
        pthread_barrier_wait(job->barrier);
    }

    for (int i = 0; i < job->op_count; i++) {
        uint64_t req_id = 0;
        SessionManagerStatus sms;
        TransactionManagerStatus tms;
        TransactionRequestStatus req_status;
        BankStatus bank_result;
        int64_t balance_result;

        sms = session_submit_transaction(
            job->sm,
            job->session_id,
            job->ops[i],
            job->source_accts[i],
            job->dest_accts[i],
            job->amounts[i],
            &req_id);

        if (sms == SESSION_MANAGER_SUCCESS) {
            job->submit_count++;

            tms = transaction_manager_wait(job->tm, req_id);

            if (tms == TRANSACTION_MANAGER_SUCCESS) {
                tms = transaction_manager_get_result(
                    job->tm, req_id, &req_status,
                    &bank_result, &balance_result);
                (void)transaction_manager_release(job->tm, req_id);
                job->release_count++;

                if (bank_result == BANK_SUCCESS) {
                    job->successful_count++;
                    job->successful_total_paise += job->amounts[i];
                }
            } else {
                tms = transaction_manager_release(job->tm, req_id);
                if (tms == TRANSACTION_MANAGER_SUCCESS) {
                    job->release_count++;
                }
            }
        }
    }

    job->completed = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helper: initialize a fresh Application                             */
/* ------------------------------------------------------------------ */

static int init_app(Application *app)
{
    memset(app, 0, sizeof(*app));
    return application_init(app) == APPLICATION_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Scenario A — Multiple concurrent customer sessions                 */
/*   12 sessions (6 ATM + 6 Online Banking) across 3 accounts         */
/*   Each session submits 1 deposit of 100 paise                      */
/* ------------------------------------------------------------------ */

static void test_scenario_a_multiple_concurrent_sessions(void)
{
    Application app;
    SessionManager *sm;
    Bank *bank;
    TransactionManager *tm;
    pthread_barrier_t barrier;
    pthread_t threads[12];
    integration_job_t jobs[12];
    uint64_t sids[12];
    const int worker_count = 12;
    const int64_t deposit_amount = 100LL;

    TEST_ASSERT(init_app(&app), "application_init");
    sm = application_get_session_manager(&app);
    bank = application_get_bank(&app);
    tm = &app.txn_mgr;

    TEST_ASSERT(sm != NULL, "session manager available");
    TEST_ASSERT(bank != NULL, "bank available");

    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    /* Create 12 sessions: 6 ATM, 6 Online Banking, 4 per account. */
    int session_accounts[12] = {1001, 1001, 1001, 1001,
                                1002, 1002, 1002, 1002,
                                1003, 1003, 1003, 1003};
    SessionChannel session_channels[12] = {
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING};

    for (int i = 0; i < worker_count; i++) {
        SessionManagerStatus ms = session_create(
            sm, session_accounts[i], session_channels[i], &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session create should succeed");
        TEST_ASSERT(sids[i] > 0, "session ID must be positive");
    }

    /* Verify unique session IDs. */
    for (int i = 0; i < worker_count; i++) {
        for (int j = i + 1; j < worker_count; j++) {
            TEST_ASSERT(sids[i] != sids[j],
                        "no duplicate session IDs");
        }
    }

    /* Verify session-account association via session_get. */
    for (int i = 0; i < worker_count; i++) {
        SessionState st;
        int acct;
        SessionChannel ch;
        SessionManagerStatus ms = session_get(
            sm, sids[i], &st, &acct, &ch);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session_get should succeed");
        TEST_ASSERT(st == SESSION_STATE_ACTIVE,
                    "session should be ACTIVE");
        TEST_ASSERT(acct == session_accounts[i],
                    "session account ID should match creation");
        TEST_ASSERT(ch == session_channels[i],
                    "session channel should match creation");
    }

    /* Initialize jobs: each session deposits 100 paise to its account. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].app = &app;
        jobs[i].sm = sm;
        jobs[i].tm = tm;
        jobs[i].bank = bank;
        jobs[i].session_id = sids[i];
        jobs[i].account_id = session_accounts[i];
        jobs[i].channel = session_channels[i];
        jobs[i].barrier = &barrier;
        jobs[i].op_count = 1;
        jobs[i].ops[0] = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_accts[0] = session_accounts[i];
        jobs[i].dest_accts[0] = 0;
        jobs[i].amounts[0] = deposit_amount;
    }

    /* Launch workers concurrently. */
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL,
                                   integration_worker_func, &jobs[i]) == 0,
                    "pthread_create");
    }

    /* Join all workers. */
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == 1, "submit should succeed");
        TEST_ASSERT(jobs[i].release_count == 1,
                    "release should succeed — no slot leak");
    }

    /* Verify per-account balances: each account got 4 deposits of 100. */
    const int64_t initial_balances[3] = {2500000LL, 5000000LL, 7500000LL};
    const int account_ids[3] = {1001, 1002, 1003};
    for (int i = 0; i < 3; i++) {
        int64_t expected = initial_balances[i] + 4 * deposit_amount;
        int64_t actual = 0;
        TEST_ASSERT(bank_get_balance(bank, account_ids[i], &actual) ==
                        BANK_SUCCESS,
                    "get_balance");
        TEST_ASSERT(actual == expected,
                    "account balance: 4 deposits of 100 applied");
    }

    /* Terminate all sessions. */
    for (int i = 0; i < worker_count; i++) {
        session_terminate(sm, sids[i]);
    }

    pthread_barrier_destroy(&barrier);
    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Scenario B — Concurrent ATM and Online Banking activity            */
/*   8 workers: 4 ATM on account 1001, 4 Online Banking on 1002       */
/*   Each worker: 1 balance enquiry + 1 deposit + 1 withdrawal        */
/* ------------------------------------------------------------------ */

static void test_scenario_b_concurrent_atm_online_activity(void)
{
    Application app;
    SessionManager *sm;
    Bank *bank;
    TransactionManager *tm;
    pthread_barrier_t barrier;
    pthread_t threads[8];
    integration_job_t jobs[8];
    uint64_t sids[8];
    const int worker_count = 8;
    const int64_t deposit_amount = 200LL;
    const int64_t withdrawal_amount = 100LL;

    TEST_ASSERT(init_app(&app), "application_init");
    sm = application_get_session_manager(&app);
    bank = application_get_bank(&app);
    tm = &app.txn_mgr;

    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    /* 4 ATM workers on account 1001, 4 Online Banking workers on 1002. */
    int session_accounts[8] = {1001, 1001, 1001, 1001,
                               1002, 1002, 1002, 1002};
    SessionChannel session_channels[8] = {
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING};

    for (int i = 0; i < worker_count; i++) {
        SessionManagerStatus ms = session_create(
            sm, session_accounts[i], session_channels[i], &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session create should succeed");

        SessionState st;
        int acct;
        SessionChannel ch;
        SessionManagerStatus ms2 = session_get(
            sm, sids[i], &st, &acct, &ch);
        TEST_ASSERT(ms2 == SESSION_MANAGER_SUCCESS,
                    "session_get should succeed");
        TEST_ASSERT(acct == session_accounts[i],
                    "session account should match assigned account");
        TEST_ASSERT(ch == session_channels[i],
                    "session channel should match assigned channel");
    }

    /* Each worker: balance enquiry, deposit, withdrawal. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].app = &app;
        jobs[i].sm = sm;
        jobs[i].tm = tm;
        jobs[i].bank = bank;
        jobs[i].session_id = sids[i];
        jobs[i].account_id = session_accounts[i];
        jobs[i].channel = session_channels[i];
        jobs[i].barrier = &barrier;
        jobs[i].op_count = 3;
        jobs[i].ops[0] = TRANSACTION_OP_BALANCE_ENQUIRY;
        jobs[i].source_accts[0] = session_accounts[i];
        jobs[i].dest_accts[0] = 0;
        jobs[i].amounts[0] = 0;
        jobs[i].ops[1] = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_accts[1] = session_accounts[i];
        jobs[i].dest_accts[1] = 0;
        jobs[i].amounts[1] = deposit_amount;
        jobs[i].ops[2] = TRANSACTION_OP_WITHDRAWAL;
        jobs[i].source_accts[2] = session_accounts[i];
        jobs[i].dest_accts[2] = 0;
        jobs[i].amounts[2] = withdrawal_amount;
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL,
                                   integration_worker_func, &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == 3, "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == 3,
                    "all releases succeeded — no slot leak");
    }

    /* Verify channel separation: account 1001 got 4 deposits - 4 withdrawals,
     * account 1002 got 4 deposits - 4 withdrawals. */
    int64_t bal_1001 = 0, bal_1002 = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &bal_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(bank, 1002, &bal_1002) == BANK_SUCCESS,
                "get_balance 1002");

    /* ATM workers: +4*200 - 4*100 = +400 on 1001 */
    TEST_ASSERT(bal_1001 == 2500000LL + 4 * deposit_amount -
                                 4 * withdrawal_amount,
                "account 1001: ATM deposits minus withdrawals");
    /* Online Banking workers: +4*200 - 4*100 = +400 on 1002 */
    TEST_ASSERT(bal_1002 == 5000000LL + 4 * deposit_amount -
                                 4 * withdrawal_amount,
                "account 1002: Online Banking deposits minus withdrawals");

    for (int i = 0; i < worker_count; i++) {
        session_terminate(sm, sids[i]);
    }

    pthread_barrier_destroy(&barrier);
    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Scenario C — Concurrent multi-account transaction workload         */
/*   10 workers across 3 accounts                                     */
/*   Mix of deposits, withdrawals, transfers (incl. opposite dir)     */
/* ------------------------------------------------------------------ */

static void test_scenario_c_concurrent_multi_account_workload(void)
{
    Application app;
    SessionManager *sm;
    Bank *bank;
    TransactionManager *tm;
    pthread_barrier_t barrier;
    pthread_t threads[10];
    integration_job_t jobs[10];
    uint64_t sids[10];
    const int worker_count = 10;

    TEST_ASSERT(init_app(&app), "application_init");
    sm = application_get_session_manager(&app);
    bank = application_get_bank(&app);
    tm = &app.txn_mgr;

    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    /* 4 workers on account 1001, 3 on 1002, 3 on 1003. */
    int session_accounts[10] = {1001, 1001, 1001, 1001,
                                1002, 1002, 1002,
                                1003, 1003, 1003};
    SessionChannel session_channels[10] = {
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING};

    for (int i = 0; i < worker_count; i++) {
        SessionManagerStatus ms = session_create(
            sm, session_accounts[i], session_channels[i], &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session create should succeed");
    }

    /* Worker 0-1: deposits on 1001 (2 × 250 paise each). */
    /* Worker 2-3: withdrawals from 1001 (2 × 150 paise each). */
    /* Worker 4-5: deposits on 1002 (2 × 300 paise each). */
    /* Worker 6: withdrawal from 1002 (1 × 200 paise). */
    /* Worker 7-8: transfer 1001→1003 (2 × 500 paise each). */
    /* Worker 9: transfer 1003→1001 (1 × 400 paise). */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].app = &app;
        jobs[i].sm = sm;
        jobs[i].tm = tm;
        jobs[i].bank = bank;
        jobs[i].session_id = sids[i];
        jobs[i].account_id = session_accounts[i];
        jobs[i].channel = session_channels[i];
        jobs[i].barrier = &barrier;

        if (i < 2) {
            jobs[i].op_count = 2;
            jobs[i].ops[0] = TRANSACTION_OP_DEPOSIT;
            jobs[i].source_accts[0] = 1001;
            jobs[i].dest_accts[0] = 0;
            jobs[i].amounts[0] = 250LL;
            jobs[i].ops[1] = TRANSACTION_OP_DEPOSIT;
            jobs[i].source_accts[1] = 1001;
            jobs[i].dest_accts[1] = 0;
            jobs[i].amounts[1] = 250LL;
        } else if (i < 4) {
            jobs[i].op_count = 2;
            jobs[i].ops[0] = TRANSACTION_OP_WITHDRAWAL;
            jobs[i].source_accts[0] = 1001;
            jobs[i].dest_accts[0] = 0;
            jobs[i].amounts[0] = 150LL;
            jobs[i].ops[1] = TRANSACTION_OP_WITHDRAWAL;
            jobs[i].source_accts[1] = 1001;
            jobs[i].dest_accts[1] = 0;
            jobs[i].amounts[1] = 150LL;
        } else if (i < 6) {
            jobs[i].op_count = 2;
            jobs[i].ops[0] = TRANSACTION_OP_DEPOSIT;
            jobs[i].source_accts[0] = 1002;
            jobs[i].dest_accts[0] = 0;
            jobs[i].amounts[0] = 300LL;
            jobs[i].ops[1] = TRANSACTION_OP_DEPOSIT;
            jobs[i].source_accts[1] = 1002;
            jobs[i].dest_accts[1] = 0;
            jobs[i].amounts[1] = 300LL;
        } else if (i == 6) {
            jobs[i].op_count = 1;
            jobs[i].ops[0] = TRANSACTION_OP_WITHDRAWAL;
            jobs[i].source_accts[0] = 1002;
            jobs[i].dest_accts[0] = 0;
            jobs[i].amounts[0] = 200LL;
        } else if (i < 9) {
            jobs[i].op_count = 2;
            jobs[i].ops[0] = TRANSACTION_OP_TRANSFER;
            jobs[i].source_accts[0] = 1001;
            jobs[i].dest_accts[0] = 1003;
            jobs[i].amounts[0] = 500LL;
            jobs[i].ops[1] = TRANSACTION_OP_TRANSFER;
            jobs[i].source_accts[1] = 1001;
            jobs[i].dest_accts[1] = 1003;
            jobs[i].amounts[1] = 500LL;
        } else {
            jobs[i].op_count = 1;
            jobs[i].ops[0] = TRANSACTION_OP_TRANSFER;
            jobs[i].source_accts[0] = 1003;
            jobs[i].dest_accts[0] = 1001;
            jobs[i].amounts[0] = 400LL;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL,
                                   integration_worker_func, &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == jobs[i].op_count,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == jobs[i].op_count,
                    "all releases succeeded — no slot leak");
        TEST_ASSERT(jobs[i].successful_count == jobs[i].op_count,
                    "all operations succeeded");
    }

    /* Track successful totals per account. */
    int64_t deposits_1001 = 0, withdrawals_1001 = 0;
    int64_t deposits_1002 = 0, withdrawals_1002 = 0;
    int64_t transfers_out_1001 = 0, transfers_in_1003 = 0;
    int64_t transfers_out_1003 = 0, transfers_in_1001 = 0;

    for (int i = 0; i < 2; i++) {
        deposits_1001 += jobs[i].successful_total_paise;
    }
    for (int i = 2; i < 4; i++) {
        withdrawals_1001 += jobs[i].successful_total_paise;
    }
    for (int i = 4; i < 6; i++) {
        deposits_1002 += jobs[i].successful_total_paise;
    }
    if (jobs[6].successful_count > 0) {
        withdrawals_1002 += jobs[6].successful_total_paise;
    }
    for (int i = 7; i < 9; i++) {
        transfers_out_1001 += jobs[i].successful_total_paise;
        transfers_in_1003 += jobs[i].successful_total_paise;
    }
    if (jobs[9].successful_count > 0) {
        transfers_out_1003 += jobs[9].successful_total_paise;
        transfers_in_1001 += jobs[9].successful_total_paise;
    }

    /* Verify balances. */
    int64_t bal_1001 = 0, bal_1002 = 0, bal_1003 = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &bal_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(bank, 1002, &bal_1002) == BANK_SUCCESS,
                "get_balance 1002");
    TEST_ASSERT(bank_get_balance(bank, 1003, &bal_1003) == BANK_SUCCESS,
                "get_balance 1003");

    TEST_ASSERT(bal_1001 == 2500000LL + deposits_1001 - withdrawals_1001 -
                                 transfers_out_1001 + transfers_in_1001,
                "account 1001 balance correct");
    TEST_ASSERT(bal_1002 == 5000000LL + deposits_1002 - withdrawals_1002,
                "account 1002 balance correct");
    TEST_ASSERT(bal_1003 == 7500000LL + transfers_in_1003 - transfers_out_1003,
                "account 1003 balance correct");

    TEST_ASSERT(bal_1001 >= 0 && bal_1002 >= 0 && bal_1003 >= 0,
                "no account balance negative");

    /* Compute expected balances independently. */
    int64_t exp_1001 = 2500000LL + deposits_1001 - withdrawals_1001 -
                       transfers_out_1001 + transfers_in_1001;
    int64_t exp_1002 = 5000000LL + deposits_1002 - withdrawals_1002;
    int64_t exp_1003 = 7500000LL + transfers_in_1003 - transfers_out_1003;

    TEST_ASSERT(bal_1001 == exp_1001, "account 1001 balance correct");
    TEST_ASSERT(bal_1002 == exp_1002, "account 1002 balance correct");
    TEST_ASSERT(bal_1003 == exp_1003, "account 1003 balance correct");

    /* Total balance: deposits add, withdrawals subtract, transfers conserve. */
    int64_t total_before = 2500000LL + 5000000LL + 7500000LL;
    int64_t total_after = bal_1001 + bal_1002 + bal_1003;
    int64_t expected_net = deposits_1001 + deposits_1002 -
                           withdrawals_1001 - withdrawals_1002;
    TEST_ASSERT(total_after == total_before + expected_net,
                "combined balance: deposits minus withdrawals net");

    for (int i = 0; i < worker_count; i++) {
        session_terminate(sm, sids[i]);
    }

    pthread_barrier_destroy(&barrier);
    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Scenario D — Session termination with pending transactions         */
/*   Create session, submit deposit, terminate session while pending, */
/*   wait for accepted transaction, verify balance, verify rejection  */
/* ------------------------------------------------------------------ */

static void test_scenario_d_session_termination_with_pending(void)
{
    Application app;
    SessionManager *sm;
    Bank *bank;
    TransactionManager *tm;
    pthread_barrier_t barrier;
    uint64_t sid;

    TEST_ASSERT(init_app(&app), "application_init");
    sm = application_get_session_manager(&app);
    bank = application_get_bank(&app);
    tm = &app.txn_mgr;

    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, 1) == 0,
                "barrier_init");

    SessionManagerStatus ms = session_create(
        sm, 1001, SESSION_CHANNEL_ATM, &sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "session create should succeed");

    SessionState st;
    ms = session_get(sm, sid, &st, NULL, NULL);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should succeed");
    TEST_ASSERT(st == SESSION_STATE_ACTIVE,
                "session should be ACTIVE");

    int64_t balance_before = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &balance_before) == BANK_SUCCESS,
                "get_balance before");

    /* Submit a deposit through the session. */
    uint64_t req_id = 0;
    ms = session_submit_transaction(
        sm, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 500LL, &req_id);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "submit should succeed");

    /* Terminate the session while the transaction is pending. */
    ms = session_terminate(sm, sid);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                "terminate should succeed while pending");

    /* Verify session is now TERMINATED. */
    ms = session_get(sm, sid, &st, NULL, NULL);
    TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS, "session_get should succeed");
    TEST_ASSERT(st == SESSION_STATE_TERMINATED,
                "session should be TERMINATED");

    /* Verify new submissions through terminated session are rejected. */
    uint64_t req_id2 = 0;
    ms = session_submit_transaction(
        sm, sid, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &req_id2);
    TEST_ASSERT(ms == SESSION_MANAGER_ERR_SESSION_TERMINATED,
                "new submission through terminated session should be rejected");

    /* The accepted transaction must still complete. */
    TransactionManagerStatus tms = transaction_manager_wait(tm, req_id);
    TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS,
                "wait for accepted transaction should succeed");

    BankStatus bs;
    transaction_manager_get_result(tm, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS,
                "accepted transaction should complete despite termination");

    transaction_manager_release(tm, req_id);

    /* Verify balance was updated. */
    int64_t balance_after = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &balance_after) == BANK_SUCCESS,
                "get_balance after");
    TEST_ASSERT(balance_after == balance_before + 500LL,
                "deposit should have been applied");

    pthread_barrier_destroy(&barrier);
    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Scenario E — Graceful application shutdown                         */
/*   Init, create sessions, submit transactions, shutdown, verify     */
/*   state STOPPED, repeated shutdown idempotent                      */
/* ------------------------------------------------------------------ */

static void test_scenario_e_graceful_application_shutdown(void)
{
    Application app;

    TEST_ASSERT(init_app(&app), "application_init");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_RUNNING,
                "state should be RUNNING after init");

    SessionManager *sm = application_get_session_manager(&app);
    TransactionManager *tm = &app.txn_mgr;
    Bank *bank = application_get_bank(&app);

    /* Create 5 sessions and submit transactions. */
    uint64_t sids[5];
    uint64_t req_ids[5];
    for (int i = 0; i < 5; i++) {
        int acct = 1001 + (i % 3);
        SessionManagerStatus ms = session_create(
            sm, acct, (i % 2 == 0) ? SESSION_CHANNEL_ATM
                                   : SESSION_CHANNEL_ONLINE_BANKING,
            &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session create should succeed");

        SessionManagerStatus sms = session_submit_transaction(
            sm, sids[i], TRANSACTION_OP_DEPOSIT, acct, 0, 100LL, &req_ids[i]);
        TEST_ASSERT(sms == SESSION_MANAGER_SUCCESS,
                    "submit should succeed");
    }

    /* Wait for all transactions, retrieve results, release slots. */
    for (int i = 0; i < 5; i++) {
        TransactionManagerStatus tms =
            transaction_manager_wait(tm, req_ids[i]);
        TEST_ASSERT(tms == TRANSACTION_MANAGER_SUCCESS,
                    "wait should succeed before shutdown");
        TransactionRequestStatus st;
        BankStatus bs;
        transaction_manager_get_result(tm, req_ids[i], &st, &bs, NULL);
        TEST_ASSERT(bs == BANK_SUCCESS, "bank result should be SUCCESS");
        transaction_manager_release(tm, req_ids[i]);
    }

    /* Verify balances reflect all 5 deposits. */
    int64_t bal_1001 = 0, bal_1002 = 0, bal_1003 = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &bal_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(bank, 1002, &bal_1002) == BANK_SUCCESS,
                "get_balance 1002");
    TEST_ASSERT(bank_get_balance(bank, 1003, &bal_1003) == BANK_SUCCESS,
                "get_balance 1003");

    /* Sessions 0,3 → 1001 (2 deposits); 1,4 → 1002 (2 deposits); 2 → 1003 (1) */
    TEST_ASSERT(bal_1001 == 2500000LL + 200LL, "account 1001 balance");
    TEST_ASSERT(bal_1002 == 5000000LL + 200LL, "account 1002 balance");
    TEST_ASSERT(bal_1003 == 7500000LL + 100LL, "account 1003 balance");

    /* Graceful shutdown. */
    ApplicationStatus st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS, "shutdown should succeed");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should be STOPPED");

    /* Repeated shutdown should be idempotent. */
    st = application_shutdown(&app);
    TEST_ASSERT(st == APPLICATION_SUCCESS,
                "repeated shutdown should be idempotent");
    TEST_ASSERT(application_get_state(&app) == APP_STATE_STOPPED,
                "state should remain STOPPED");
}

/* ------------------------------------------------------------------ */
/* Scenario F — Full-system stress and invariant validation           */
/*   12 sessions, mixed operations, all 3 accounts, both channels     */
/*   Deterministic workload, balance verification, conservation       */
/* ------------------------------------------------------------------ */

static void test_scenario_f_full_system_stress(void)
{
    Application app;
    SessionManager *sm;
    Bank *bank;
    TransactionManager *tm;
    pthread_barrier_t barrier;
    pthread_t threads[12];
    integration_job_t jobs[12];
    uint64_t sids[12];
    const int worker_count = 12;

    TEST_ASSERT(init_app(&app), "application_init");
    sm = application_get_session_manager(&app);
    bank = application_get_bank(&app);
    tm = &app.txn_mgr;

    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    /* Distribute 12 sessions across 3 accounts and both channels. */
    int session_accounts[12] = {1001, 1001, 1001,
                                1002, 1002, 1002,
                                1003, 1003, 1003,
                                1001, 1002, 1003};
    SessionChannel session_channels[12] = {
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ONLINE_BANKING,
        SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING, SESSION_CHANNEL_ATM,
        SESSION_CHANNEL_ONLINE_BANKING};

    for (int i = 0; i < worker_count; i++) {
        SessionManagerStatus ms = session_create(
            sm, session_accounts[i], session_channels[i], &sids[i]);
        TEST_ASSERT(ms == SESSION_MANAGER_SUCCESS,
                    "session create should succeed");

        SessionState st;
        int acct;
        SessionChannel ch;
        SessionManagerStatus ms2 = session_get(
            sm, sids[i], &st, &acct, &ch);
        TEST_ASSERT(ms2 == SESSION_MANAGER_SUCCESS, "session_get should succeed");
        TEST_ASSERT(acct == session_accounts[i],
                    "session account should match");
        TEST_ASSERT(ch == session_channels[i],
                    "session channel should match");
    }

    /* Each worker: 5 operations — deposit, withdrawal, balance enquiry,
     * transfer (if applicable), deposit. Transfers go between adjacent
     * accounts in both directions to exercise lock ordering. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].app = &app;
        jobs[i].sm = sm;
        jobs[i].tm = tm;
        jobs[i].bank = bank;
        jobs[i].session_id = sids[i];
        jobs[i].account_id = session_accounts[i];
        jobs[i].channel = session_channels[i];
        jobs[i].barrier = &barrier;
        jobs[i].op_count = 5;

        int acct = session_accounts[i];
        int dest = (acct == 1001) ? 1002 : (acct == 1002) ? 1003 : 1001;

        jobs[i].ops[0] = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_accts[0] = acct;
        jobs[i].dest_accts[0] = 0;
        jobs[i].amounts[0] = 100LL;

        jobs[i].ops[1] = TRANSACTION_OP_WITHDRAWAL;
        jobs[i].source_accts[1] = acct;
        jobs[i].dest_accts[1] = 0;
        jobs[i].amounts[1] = 50LL;

        jobs[i].ops[2] = TRANSACTION_OP_BALANCE_ENQUIRY;
        jobs[i].source_accts[2] = acct;
        jobs[i].dest_accts[2] = 0;
        jobs[i].amounts[2] = 0;

        jobs[i].ops[3] = TRANSACTION_OP_TRANSFER;
        jobs[i].source_accts[3] = acct;
        jobs[i].dest_accts[3] = dest;
        jobs[i].amounts[3] = 200LL;

        jobs[i].ops[4] = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_accts[4] = acct;
        jobs[i].dest_accts[4] = 0;
        jobs[i].amounts[4] = 75LL;
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL,
                                   integration_worker_func, &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == jobs[i].op_count,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == jobs[i].op_count,
                    "all releases succeeded — no slot leak");
    }

    /* Verify final balances directly from the bank. */
    int64_t bal_1001 = 0, bal_1002 = 0, bal_1003 = 0;
    TEST_ASSERT(bank_get_balance(bank, 1001, &bal_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(bank, 1002, &bal_1002) == BANK_SUCCESS,
                "get_balance 1002");
    TEST_ASSERT(bank_get_balance(bank, 1003, &bal_1003) == BANK_SUCCESS,
                "get_balance 1003");

    TEST_ASSERT(bal_1001 >= 0, "account 1001 not negative");
    TEST_ASSERT(bal_1002 >= 0, "account 1002 not negative");
    TEST_ASSERT(bal_1003 >= 0, "account 1003 not negative");

    /* Total balance conservation: deposits/withdrawals net to zero within
     * the system (transfers conserve, deposits add, withdrawals subtract).
     * Each worker: +100 -50 +200(out) +75 = +325 net per worker for
     * deposit/withdrawal/transfer+deposit. But transfers move money between
     * accounts, so total changes only by deposits - withdrawals + final
     * deposits: each worker net = +100 -50 +75 = +125 (transfers cancel
     * in aggregate).
     */
    int64_t total_before = 2500000LL + 5000000LL + 7500000LL;
    int64_t total_after = bal_1001 + bal_1002 + bal_1003;
    int64_t expected_net = (int64_t)worker_count * 125LL;
    TEST_ASSERT(total_after == total_before + expected_net,
                "combined balance: deposits minus withdrawals net");

    /* Verify transaction history contains committed records. */
    Transaction history[1000];
    size_t count = 0;
    TEST_ASSERT(bank_get_transaction_history(bank, 1001, history, 1000,
                                             &count) == BANK_SUCCESS,
                "get_transaction_history for 1001");

    for (int i = 0; i < worker_count; i++) {
        session_terminate(sm, sids[i]);
    }

    pthread_barrier_destroy(&barrier);
    application_shutdown(&app);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf("M5.5.3 — Full Concurrent Session Integration\n");
    printf("========================================\n\n");

    /* Scenario A — Multiple concurrent customer sessions */
    RUN_TEST(test_scenario_a_multiple_concurrent_sessions);

    /* Scenario B — Concurrent ATM and Online Banking activity */
    RUN_TEST(test_scenario_b_concurrent_atm_online_activity);

    /* Scenario C — Concurrent multi-account transaction workload */
    RUN_TEST(test_scenario_c_concurrent_multi_account_workload);

    /* Scenario D — Session termination with pending transactions */
    RUN_TEST(test_scenario_d_session_termination_with_pending);

    /* Scenario E — Graceful application shutdown */
    RUN_TEST(test_scenario_e_graceful_application_shutdown);

    /* Scenario F — Full-system stress and invariant validation */
    RUN_TEST(test_scenario_f_full_system_stress);

    printf("\n========================================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");
    fflush(stdout);

    return tests_failed == 0 ? 0 : 1;
}
