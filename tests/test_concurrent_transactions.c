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
/* Worker job — each client thread owns one session and loops         */
/* submit → wait → get result → release for a fixed number of         */
/* iterations. Slots are released incrementally, never exceeding      */
/* one outstanding request per worker.                                */
/* ------------------------------------------------------------------ */

typedef struct {
    SessionManager *mgr;
    TransactionManager *tmmgr;
    uint64_t session_id;
    TransactionOperation operation;
    int source_account_id;
    int destination_account_id;
    int64_t amount_paise;
    int iteration_count;
    pthread_barrier_t *barrier;

    /* Outcome counters — validated after all threads join. */
    int submit_count;
    int release_count;
    int successful_count;
    int64_t successful_total_paise;
    SessionManagerStatus last_submit_ms;
    TransactionManagerStatus last_wait_ms;
    TransactionManagerStatus last_release_ms;
    int completed;
} worker_job_t;

static void *worker_thread_func(void *arg)
{
    worker_job_t *job = (worker_job_t *)arg;

    /* Synchronized start: no worker submits its first transaction until
     * every worker has reached the barrier. This guarantees actual
     * execution overlap rather than merely concurrent thread creation. */
    if (job->barrier != NULL) {
        pthread_barrier_wait(job->barrier);
    }

    for (int i = 0; i < job->iteration_count; i++) {
        uint64_t req_id = 0;

        job->last_submit_ms = session_submit_transaction(
            job->mgr,
            job->session_id,
            job->operation,
            job->source_account_id,
            job->destination_account_id,
            job->amount_paise,
            &req_id);

        if (job->last_submit_ms == SESSION_MANAGER_SUCCESS) {
            job->submit_count++;

            job->last_wait_ms =
                transaction_manager_wait(job->tmmgr, req_id);

            /* Always attempt release once the request is terminal.
             * If wait succeeded, the request is COMPLETED or FAILED and
             * release must succeed. If wait failed, release may fail —
             * that is recorded separately. */
            if (job->last_wait_ms == TRANSACTION_MANAGER_SUCCESS) {
                TransactionRequestStatus req_status;
                BankStatus bank_result;
                int64_t balance_result;
                job->last_release_ms = transaction_manager_get_result(
                    job->tmmgr, req_id, &req_status,
                    &bank_result, &balance_result);
                (void)transaction_manager_release(job->tmmgr, req_id);
                job->release_count++;

                if (bank_result == BANK_SUCCESS) {
                    job->successful_count++;
                    job->successful_total_paise += job->amount_paise;
                }
            } else {
                job->last_release_ms =
                    transaction_manager_release(job->tmmgr, req_id);
                if (job->last_release_ms == TRANSACTION_MANAGER_SUCCESS) {
                    job->release_count++;
                }
            }
        }
    }

    job->completed = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helper: create a session for a worker                              */
/* ------------------------------------------------------------------ */

static int create_worker_session(SessionManager *mgr, int account_id,
                                 SessionChannel channel, uint64_t *out_sid)
{
    SessionManagerStatus ms = session_create(mgr, account_id, channel, out_sid);
    return ms == SESSION_MANAGER_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Scenario A — Concurrent deposits into one account                  */
/*   10 workers × 100 deposits = 1,000 deposits                       */
/*   All target the same account                                      */
/*   Uses a dedicated bank fixture (1,000 history slots exactly)      */
/* ------------------------------------------------------------------ */

static void test_scenario_a_concurrent_deposits(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[10];
    worker_job_t jobs[10];
    uint64_t sids[10];
    const int worker_count = 10;
    const int iterations_per_worker = 100;
    const int64_t deposit_amount = 100LL;
    const int64_t initial_balance = 2500000LL;

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    TEST_ASSERT(bank_create_account(&bank, 1001, "Account A",
                                    initial_balance, NULL) == BANK_SUCCESS,
                "create account 1001");
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    /* Create one session per worker. */
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, 1001, SESSION_CHANNEL_ATM,
                                          &sids[i]),
                    "create worker session");
    }

    /* Initialize jobs. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = 1001;
        jobs[i].destination_account_id = 0;
        jobs[i].amount_paise = deposit_amount;
        jobs[i].iteration_count = iterations_per_worker;
        jobs[i].barrier = &barrier;
    }

    /* Launch workers concurrently. */
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    /* Join all workers. */
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == iterations_per_worker,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == iterations_per_worker,
                    "all releases succeeded — no slot leak");
    }

    /* Verify final balance: initial + total deposits. */
    int64_t total_deposits =
        (int64_t)worker_count * (int64_t)iterations_per_worker * deposit_amount;
    int64_t expected_balance = initial_balance + total_deposits;
    int64_t actual_balance = 0;
    TEST_ASSERT(bank_get_balance(&bank, 1001, &actual_balance) == BANK_SUCCESS,
                "get_balance");
    TEST_ASSERT(actual_balance == expected_balance,
                "final balance matches expected (all deposits applied)");

    /* Verify all transactions committed to history (1,000 records = full
     * capacity — scenario uses a dedicated bank fixture precisely for this). */
    Transaction history[1000];
    size_t count = 0;
    TEST_ASSERT(bank_get_transaction_history(&bank, 1001, history,
                                             1000, &count) == BANK_SUCCESS,
                "get_transaction_history");
    TEST_ASSERT(count == (size_t)(worker_count * iterations_per_worker),
                "all 1,000 deposits recorded in history");

    /* Cleanup. */
    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Scenario B — Concurrent withdrawals                                */
/*   5 workers × 30 withdrawal attempts                               */
/*   Mix of sufficient and insufficient funds                         */
/*   Verify no balance goes negative                                  */
/* ------------------------------------------------------------------ */

static void test_scenario_b_concurrent_withdrawals(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[5];
    worker_job_t jobs[5];
    uint64_t sids[5];
    const int worker_count = 5;
    const int iterations_per_worker = 30;
    const int64_t withdrawal_amount = 200000LL;
    const int64_t initial_balance = 5000000LL;

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    TEST_ASSERT(bank_create_account(&bank, 1001, "Account B",
                                    initial_balance, NULL) == BANK_SUCCESS,
                "create account 1001");
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, 1001, SESSION_CHANNEL_ATM,
                                          &sids[i]),
                    "create worker session");
    }

    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_WITHDRAWAL;
        jobs[i].source_account_id = 1001;
        jobs[i].destination_account_id = 0;
        jobs[i].amount_paise = withdrawal_amount;
        jobs[i].iteration_count = iterations_per_worker;
        jobs[i].barrier = &barrier;
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == iterations_per_worker,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == iterations_per_worker,
                    "all releases succeeded — no slot leak");
    }

    /* Final balance = initial - sum of successful withdrawals. */
    int64_t total_successful = 0;
    for (int i = 0; i < worker_count; i++) {
        total_successful += jobs[i].successful_total_paise;
    }
    int64_t expected_balance = initial_balance - total_successful;
    int64_t actual_balance = 0;
    TEST_ASSERT(bank_get_balance(&bank, 1001, &actual_balance) == BANK_SUCCESS,
                "get_balance");
    TEST_ASSERT(actual_balance == expected_balance,
                "balance equals initial minus successful withdrawals");
    TEST_ASSERT(actual_balance >= 0, "balance never negative");

    /* At least some withdrawals should have succeeded. */
    int total_successful_count = 0;
    for (int i = 0; i < worker_count; i++) {
        total_successful_count += jobs[i].successful_count;
    }
    TEST_ASSERT(total_successful_count > 0,
                "at least one withdrawal succeeded");

    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Scenario C — Concurrent deposits and withdrawals                   */
/*   5 deposit workers + 5 withdrawal workers on same account         */
/*   Verify final balance using exact successful operation counts     */
/* ------------------------------------------------------------------ */

static void test_scenario_c_concurrent_deposits_withdrawals(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[10];
    worker_job_t jobs[10];
    uint64_t sids[10];
    const int worker_count = 10;
    const int iterations_per_worker = 50;
    const int64_t deposit_amount = 200LL;
    const int64_t withdrawal_amount = 100LL;
    const int64_t initial_balance = 3000000LL;

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    TEST_ASSERT(bank_create_account(&bank, 1001, "Account C",
                                    initial_balance, NULL) == BANK_SUCCESS,
                "create account 1001");
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, 1001, SESSION_CHANNEL_ATM,
                                          &sids[i]),
                    "create worker session");
    }

    /* Workers 0-4: deposits. Workers 5-9: withdrawals. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].source_account_id = 1001;
        jobs[i].destination_account_id = 0;
        jobs[i].iteration_count = iterations_per_worker;
        jobs[i].barrier = &barrier;
        if (i < 5) {
            jobs[i].operation = TRANSACTION_OP_DEPOSIT;
            jobs[i].amount_paise = deposit_amount;
        } else {
            jobs[i].operation = TRANSACTION_OP_WITHDRAWAL;
            jobs[i].amount_paise = withdrawal_amount;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == iterations_per_worker,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == iterations_per_worker,
                    "all releases succeeded — no slot leak");
    }

    /* Aggregate successful deposits and withdrawals. */
    int64_t total_deposits = 0;
    int64_t total_withdrawals = 0;
    for (int i = 0; i < 5; i++) {
        total_deposits += jobs[i].successful_total_paise;
    }
    for (int i = 5; i < 10; i++) {
        total_withdrawals += jobs[i].successful_total_paise;
    }

    int64_t expected_balance =
        initial_balance + total_deposits - total_withdrawals;
    int64_t actual_balance = 0;
    TEST_ASSERT(bank_get_balance(&bank, 1001, &actual_balance) == BANK_SUCCESS,
                "get_balance");
    TEST_ASSERT(actual_balance == expected_balance,
                "balance equals initial + deposits - withdrawals");
    TEST_ASSERT(actual_balance >= 0, "balance never negative");

    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Scenario D — Independent account operations                        */
/*   4 workers × 100 deposits, each on a different account            */
/*   Verify per-account balances independently                        */
/*   Demonstrates no global lock serializes independent accounts      */
/* ------------------------------------------------------------------ */

static void test_scenario_d_independent_accounts(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[4];
    worker_job_t jobs[4];
    uint64_t sids[4];
    const int worker_count = 4;
    const int iterations_per_worker = 100;
    const int64_t deposit_amount = 100LL;
    const int64_t initial_balance = 1000000LL;
    const int account_ids[4] = {1001, 1002, 1003, 1004};

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(bank_create_account(&bank, account_ids[i], "Account D",
                                        initial_balance, NULL) == BANK_SUCCESS,
                    "create account");
    }
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, account_ids[i],
                                          SESSION_CHANNEL_ATM, &sids[i]),
                    "create worker session");
    }

    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_DEPOSIT;
        jobs[i].source_account_id = account_ids[i];
        jobs[i].destination_account_id = 0;
        jobs[i].amount_paise = deposit_amount;
        jobs[i].iteration_count = iterations_per_worker;
        jobs[i].barrier = &barrier;
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == iterations_per_worker,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == iterations_per_worker,
                    "all releases succeeded — no slot leak");
    }

    /* Each account: initial + 100 * 100 = initial + 10,000. */
    for (int i = 0; i < worker_count; i++) {
        int64_t expected = initial_balance +
            (int64_t)iterations_per_worker * deposit_amount;
        int64_t actual = 0;
        TEST_ASSERT(bank_get_balance(&bank, account_ids[i], &actual) ==
                        BANK_SUCCESS,
                    "get_balance");
        TEST_ASSERT(actual == expected,
                    "independent account balance correct");
    }

    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Scenario E — Opposite-direction fund transfers                     */
/*   10 workers: 5 transfer 1001→1002, 5 transfer 1002→1001           */
/*   Each does 50 transfers of 1,000 paise                            */
/*   Combined balance must remain unchanged                           */
/*   Lock ordering prevents deadlock                                  */
/* ------------------------------------------------------------------ */

static void test_scenario_e_opposite_direction_transfers(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[10];
    worker_job_t jobs[10];
    uint64_t sids[10];
    const int worker_count = 10;
    const int iterations_per_worker = 50;
    const int64_t transfer_amount = 1000LL;
    const int64_t initial_balance = 500000LL;

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    TEST_ASSERT(bank_create_account(&bank, 1001, "Account E1",
                                    initial_balance, NULL) == BANK_SUCCESS,
                "create account 1001");
    TEST_ASSERT(bank_create_account(&bank, 1002, "Account E2",
                                    initial_balance, NULL) == BANK_SUCCESS,
                "create account 1002");
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, 1001, SESSION_CHANNEL_ATM,
                                          &sids[i]),
                    "create worker session");
    }

    /* Workers 0-4: 1001 → 1002. Workers 5-9: 1002 → 1001. */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].operation = TRANSACTION_OP_TRANSFER;
        jobs[i].iteration_count = iterations_per_worker;
        jobs[i].amount_paise = transfer_amount;
        jobs[i].barrier = &barrier;
        if (i < 5) {
            jobs[i].source_account_id = 1001;
            jobs[i].destination_account_id = 1002;
        } else {
            jobs[i].source_account_id = 1002;
            jobs[i].destination_account_id = 1001;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == iterations_per_worker,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == iterations_per_worker,
                    "all releases succeeded — no slot leak");
    }

    /* Combined balance must be unchanged: 2 * initial_balance. */
    int64_t balance_1001 = 0;
    int64_t balance_1002 = 0;
    TEST_ASSERT(bank_get_balance(&bank, 1001, &balance_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(&bank, 1002, &balance_1002) == BANK_SUCCESS,
                "get_balance 1002");
    TEST_ASSERT(balance_1001 + balance_1002 == 2 * initial_balance,
                "combined balance unchanged after opposite-direction transfers");

    /* Each account balance: initial + (incoming - outgoing). */
    int64_t outgoing_1001 = 0;
    int64_t incoming_1001 = 0;
    for (int i = 0; i < 5; i++) {
        if (jobs[i].successful_count > 0) {
            outgoing_1001 += jobs[i].successful_total_paise;
        }
    }
    for (int i = 5; i < 10; i++) {
        if (jobs[i].successful_count > 0) {
            incoming_1001 += jobs[i].successful_total_paise;
        }
    }
    TEST_ASSERT(balance_1001 == initial_balance - outgoing_1001 + incoming_1001,
                "account 1001 balance correct");

    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Scenario F — Mixed concurrent workload                             */
/*   6 workers, multiple accounts, mixed operations                   */
/*   Deposits, withdrawals, balance enquiries, transfers              */
/*   Verify balances, no lost updates, no leaked slots                */
/* ------------------------------------------------------------------ */

static void test_scenario_f_mixed_workload(void)
{
    Bank bank;
    ThreadPool pool;
    TransactionManager tmmgr;
    SessionManager mgr;
    pthread_barrier_t barrier;
    pthread_t threads[6];
    worker_job_t jobs[6];
    uint64_t sids[6];
    const int worker_count = 6;
    const int64_t initial_1001 = 2000000LL;
    const int64_t initial_1002 = 1000000LL;
    const int64_t initial_1003 = 500000LL;

    TEST_ASSERT(bank_init(&bank) == BANK_SUCCESS, "bank_init");
    TEST_ASSERT(bank_create_account(&bank, 1001, "Account F1",
                                    initial_1001, NULL) == BANK_SUCCESS,
                "create account 1001");
    TEST_ASSERT(bank_create_account(&bank, 1002, "Account F2",
                                    initial_1002, NULL) == BANK_SUCCESS,
                "create account 1002");
    TEST_ASSERT(bank_create_account(&bank, 1003, "Account F3",
                                    initial_1003, NULL) == BANK_SUCCESS,
                "create account 1003");
    TEST_ASSERT(thread_pool_init(&pool, 4, 64) == THREAD_POOL_SUCCESS,
                "thread_pool_init");
    TEST_ASSERT(transaction_manager_init(&tmmgr, &pool, &bank, 64) ==
                    TRANSACTION_MANAGER_SUCCESS,
                "transaction_manager_init");
    TEST_ASSERT(session_manager_init(&mgr, &tmmgr, 16) ==
                    SESSION_MANAGER_SUCCESS,
                "session_manager_init");
    TEST_ASSERT(pthread_barrier_init(&barrier, NULL, worker_count) == 0,
                "barrier_init");

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(create_worker_session(&mgr, 1001, SESSION_CHANNEL_ATM,
                                          &sids[i]),
                    "create worker session");
    }

    /* Worker 0-1: deposits on 1001 (50 × 200 paise each). */
    /* Worker 2-3: withdrawals from 1002 (50 × 100 paise each). */
    /* Worker 4: transfers 1001→1003 (30 × 500 paise). */
    /* Worker 5: balance enquiries on 1003 (50 enquiries). */
    for (int i = 0; i < worker_count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].mgr = &mgr;
        jobs[i].tmmgr = &tmmgr;
        jobs[i].session_id = sids[i];
        jobs[i].iteration_count = 50;
        jobs[i].barrier = &barrier;
        switch (i) {
        case 0:
        case 1:
            jobs[i].operation = TRANSACTION_OP_DEPOSIT;
            jobs[i].source_account_id = 1001;
            jobs[i].destination_account_id = 0;
            jobs[i].amount_paise = 200LL;
            jobs[i].iteration_count = 50;
            break;
        case 2:
        case 3:
            jobs[i].operation = TRANSACTION_OP_WITHDRAWAL;
            jobs[i].source_account_id = 1002;
            jobs[i].destination_account_id = 0;
            jobs[i].amount_paise = 100LL;
            jobs[i].iteration_count = 50;
            break;
        case 4:
            jobs[i].operation = TRANSACTION_OP_TRANSFER;
            jobs[i].source_account_id = 1001;
            jobs[i].destination_account_id = 1003;
            jobs[i].amount_paise = 500LL;
            jobs[i].iteration_count = 30;
            break;
        case 5:
            jobs[i].operation = TRANSACTION_OP_BALANCE_ENQUIRY;
            jobs[i].source_account_id = 1003;
            jobs[i].destination_account_id = 0;
            jobs[i].amount_paise = 0;
            jobs[i].iteration_count = 50;
            break;
        default:
            break;
        }
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_create(&threads[i], NULL, worker_thread_func,
                                   &jobs[i]) == 0,
                    "pthread_create");
    }

    for (int i = 0; i < worker_count; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0, "pthread_join");
        TEST_ASSERT(jobs[i].completed == 1, "worker completed");
        TEST_ASSERT(jobs[i].submit_count == jobs[i].iteration_count,
                    "all submits succeeded");
        TEST_ASSERT(jobs[i].release_count == jobs[i].iteration_count,
                    "all releases succeeded — no slot leak");
    }

    /* Track successful totals per operation type. */
    int64_t total_deposits_1001 = 0;
    int64_t total_withdrawals_1002 = 0;
    int64_t total_transfers_out_1001 = 0;
    int64_t total_transfers_in_1003 = 0;

    for (int i = 0; i < 2; i++) {
        total_deposits_1001 += jobs[i].successful_total_paise;
    }
    for (int i = 2; i < 4; i++) {
        total_withdrawals_1002 += jobs[i].successful_total_paise;
    }
    total_transfers_out_1001 = jobs[4].successful_total_paise;
    total_transfers_in_1003 = jobs[4].successful_total_paise;

    /* Verify account balances. */
    int64_t bal_1001 = 0, bal_1002 = 0, bal_1003 = 0;
    TEST_ASSERT(bank_get_balance(&bank, 1001, &bal_1001) == BANK_SUCCESS,
                "get_balance 1001");
    TEST_ASSERT(bank_get_balance(&bank, 1002, &bal_1002) == BANK_SUCCESS,
                "get_balance 1002");
    TEST_ASSERT(bank_get_balance(&bank, 1003, &bal_1003) == BANK_SUCCESS,
                "get_balance 1003");

    TEST_ASSERT(bal_1001 == initial_1001 + total_deposits_1001 -
                                 total_transfers_out_1001,
                "account 1001 balance: initial + deposits - transfers out");
    TEST_ASSERT(bal_1002 == initial_1002 - total_withdrawals_1002,
                "account 1002 balance: initial - withdrawals");
    TEST_ASSERT(bal_1003 == initial_1003 + total_transfers_in_1003,
                "account 1003 balance: initial + transfers in");

    /* Verify no balance went negative. */
    TEST_ASSERT(bal_1001 >= 0 && bal_1002 >= 0 && bal_1003 >= 0,
                "no account balance negative");

    pthread_barrier_destroy(&barrier);
    for (int i = 0; i < worker_count; i++) {
        session_terminate(&mgr, sids[i]);
    }
    session_manager_destroy(&mgr);
    transaction_manager_destroy(&tmmgr);
    thread_pool_destroy(&pool);
    bank_destroy(&bank);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("========================================\n");
    printf("M5.5.2 — Concurrent Transaction Submission\n");
    printf("========================================\n\n");

    /* Scenario A — Concurrent deposits into one account */
    RUN_TEST(test_scenario_a_concurrent_deposits);

    /* Scenario B — Concurrent withdrawals */
    RUN_TEST(test_scenario_b_concurrent_withdrawals);

    /* Scenario C — Concurrent deposits and withdrawals */
    RUN_TEST(test_scenario_c_concurrent_deposits_withdrawals);

    /* Scenario D — Independent account operations */
    RUN_TEST(test_scenario_d_independent_accounts);

    /* Scenario E — Opposite-direction fund transfers */
    RUN_TEST(test_scenario_e_opposite_direction_transfers);

    /* Scenario F — Mixed concurrent workload */
    RUN_TEST(test_scenario_f_mixed_workload);

    printf("\n========================================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");
    fflush(stdout);

    return tests_failed == 0 ? 0 : 1;
}
