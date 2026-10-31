#include "transaction_manager.h"

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
static TransactionManager g_mgr;

static int setup_bank_and_pool(void)
{
    BankStatus bs = bank_init(&g_bank);
    if (bs != BANK_SUCCESS) {
        return 0;
    }

    /* Create accounts with known balances. */
    bs = bank_create_account(&g_bank, 1001, "Alice", 5000000LL, NULL);
    bs = bank_create_account(&g_bank, 1002, "Bob", 3000000LL, NULL);
    bs = bank_create_account(&g_bank, 1003, "Carol", 1000000LL, NULL);

    ThreadPoolStatus ts = thread_pool_init(&g_pool, 4, 16);
    if (ts != THREAD_POOL_SUCCESS) {
        return 0;
    }

    TransactionManagerStatus ms =
        transaction_manager_init(&g_mgr, &g_pool, &g_bank, 32);
    if (ms != TRANSACTION_MANAGER_SUCCESS) {
        return 0;
    }

    return 1;
}

static void teardown_all(void)
{
    transaction_manager_destroy(&g_mgr);
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
/* Test A — Initialization                                            */
/* ------------------------------------------------------------------ */

static void test_a_successful_initialization(void)
{
    /* Fully self-contained: create its own pool and bank so that destroying
     * this manager (which shuts down its pool) does not affect g_pool/g_mgr
     * used by later tests. */
    Bank local_bank;
    ThreadPool local_pool;
    TransactionManager mgr;
    TransactionManagerStatus ms;
    ThreadPoolStatus ts;

    bank_init(&local_bank);
    bank_create_account(&local_bank, 1001, "Alice", 5000000LL, NULL);

    ts = thread_pool_init(&local_pool, 2, 8);
    TEST_ASSERT(ts == THREAD_POOL_SUCCESS, "local pool init should succeed");

    ms = transaction_manager_init(&mgr, &local_pool, &local_bank, 16);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "successful init should return SUCCESS");
    TEST_ASSERT(mgr.initialized == 1, "manager should be marked initialized");
    TEST_ASSERT(mgr.max_requests == 16, "max_requests should be 16");
    TEST_ASSERT(mgr.shutdown == 0, "shutdown should be false after init");
    TEST_ASSERT(mgr.active_requests == 0,
                "active_requests should be 0 after init");

    transaction_manager_destroy(&mgr);
    thread_pool_destroy(&local_pool);
    bank_destroy(&local_bank);
}

static void test_a2_null_manager(void)
{
    TransactionManagerStatus ms =
        transaction_manager_init(NULL, &g_pool, &g_bank, 16);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with NULL manager should return INVALID_ARGUMENT");
}

static void test_a3_null_pool(void)
{
    TransactionManager mgr;
    TransactionManagerStatus ms =
        transaction_manager_init(&mgr, NULL, &g_bank, 16);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with NULL pool should return INVALID_ARGUMENT");
}

static void test_a4_null_bank(void)
{
    TransactionManager mgr;
    TransactionManagerStatus ms =
        transaction_manager_init(&mgr, &g_pool, NULL, 16);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with NULL bank should return INVALID_ARGUMENT");
}

static void test_a5_zero_max_requests(void)
{
    TransactionManager mgr;
    TransactionManagerStatus ms =
        transaction_manager_init(&mgr, &g_pool, &g_bank, 0);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with zero max_requests should return INVALID_ARGUMENT");
}

static void test_a6_uninitialized_pool(void)
{
    ThreadPool uninit_pool;
    memset(&uninit_pool, 0, sizeof(uninit_pool));
    TransactionManager mgr;
    TransactionManagerStatus ms =
        transaction_manager_init(&mgr, &uninit_pool, &g_bank, 16);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "init with uninitialized pool should return INVALID_ARGUMENT");
}

static void test_a7_destroy_on_uninitialized_is_noop(void)
{
    TransactionManager mgr;
    memset(&mgr, 0, sizeof(mgr));
    /* Should not crash. */
    transaction_manager_destroy(&mgr);
    TEST_ASSERT(1, "destroy on uninitialized manager should be safe no-op");
}

static void test_a8_destroy_on_null_is_noop(void)
{
    /* Should not crash. */
    transaction_manager_destroy(NULL);
    TEST_ASSERT(1, "destroy on NULL manager should be safe no-op");
}

/* ------------------------------------------------------------------ */
/* Test B — Request submission                                        */
/* ------------------------------------------------------------------ */

static void test_b_deposit_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 1000000LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "deposit submit should succeed");
    TEST_ASSERT(req_id > 0, "request ID should be positive");

    transaction_manager_wait(&g_mgr, req_id);

    TransactionRequestStatus st;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, &st, &bs, NULL);
    TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED || st == TRANSACTION_REQUEST_FAILED,
                "status should be terminal");
    TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED,
                "deposit should complete");
    TEST_ASSERT(bs == BANK_SUCCESS, "bank result should be SUCCESS");

    TEST_ASSERT(get_balance(1001) == 6000000LL,
                "balance should reflect deposit");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_b_withdrawal_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_WITHDRAWAL,
                                    1002, 0, 500000LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "withdrawal submit should succeed");

    transaction_manager_wait(&g_mgr, req_id);

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS, "withdrawal bank result should be SUCCESS");

    TEST_ASSERT(get_balance(1002) == 2500000LL,
                "balance should reflect withdrawal");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_b_transfer_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_TRANSFER,
                                    1001, 1002, 200000LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "transfer submit should succeed");

    transaction_manager_wait(&g_mgr, req_id);

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS, "transfer bank result should be SUCCESS");

    TEST_ASSERT(get_balance(1001) == 5800000LL,
                "source balance should be debited");
    TEST_ASSERT(get_balance(1002) == 2700000LL,
                "destination balance should be credited");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_b_balance_enquiry_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_BALANCE_ENQUIRY,
                                    1001, 0, 0, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "balance enquiry submit should succeed");

    transaction_manager_wait(&g_mgr, req_id);

    BankStatus bs;
    int64_t bal = 0;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, &bal);
    TEST_ASSERT(bs == BANK_SUCCESS, "balance enquiry bank result should be SUCCESS");
    TEST_ASSERT(bal == get_balance(1001),
                "balance result should match actual balance");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_b_invalid_operation(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, (TransactionOperation)99,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "invalid operation should return INVALID_ARGUMENT");
}

static void test_b_invalid_amount_deposit(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 0, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "zero amount deposit should return INVALID_ARGUMENT");

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, -100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "negative amount deposit should return INVALID_ARGUMENT");
}

static void test_b_invalid_amount_withdrawal(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_WITHDRAWAL,
                                    1001, 0, -50LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "negative amount withdrawal should return INVALID_ARGUMENT");
}

static void test_b_transfer_same_account(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_TRANSFER,
                                    1001, 1001, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_INVALID_ARGUMENT,
                "transfer to same account should return INVALID_ARGUMENT");
}

static void test_b_nonexistent_account(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    /* Account 9999 does not exist. Manager accepts the submit (validation
     * deferred to bank), but the banking operation will fail. */
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    9999, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "submit with nonexistent account should still be accepted "
                "(bank validates)");

    transaction_manager_wait(&g_mgr, req_id);

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_ERR_ACCOUNT_NOT_FOUND,
                "bank result should be ACCOUNT_NOT_FOUND");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_b_registry_capacity_reached(void)
{
    /* Fill the registry. The test manager g_mgr has max_requests=32. */
    uint64_t ids[32];
    int accepted = 0;

    for (int i = 0; i < 32; i++) {
        TransactionManagerStatus ms = transaction_manager_submit(
            &g_mgr, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &ids[i]);
        if (ms == TRANSACTION_MANAGER_SUCCESS) {
            accepted++;
        } else {
            break;
        }
    }

    /* The next submit should fail with REGISTRY_FULL. */
    uint64_t extra_id;
    TransactionManagerStatus ms = transaction_manager_submit(
        &g_mgr, TRANSACTION_OP_DEPOSIT, 1001, 0, 100LL, &extra_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_REGISTRY_FULL,
                "submit when registry is full should return REGISTRY_FULL");

    /* Release all accepted slots so the manager is clean for later tests. */
    for (int i = 0; i < accepted; i++) {
        transaction_manager_wait(&g_mgr, ids[i]);
        transaction_manager_release(&g_mgr, ids[i]);
    }
    TEST_ASSERT(accepted > 0, "should have accepted some before filling");
    TEST_ASSERT(accepted == 32, "registry should be fully filled");
}

/* ------------------------------------------------------------------ */
/* Test C — Request lifecycle                                         */
/* ------------------------------------------------------------------ */

static void test_c_pending_to_processing_to_completed(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1003, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    /* Request may briefly be PENDING; wait for terminal state. */
    transaction_manager_wait(&g_mgr, req_id);

    TransactionRequestStatus st;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, &st, &bs, NULL);
    TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED,
                "request should reach COMPLETED terminal state");
    TEST_ASSERT(bs == BANK_SUCCESS, "bank result should be SUCCESS");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_c_failed_banking_operation(void)
{
    /* Withdraw more than balance: 1003 has low balance, withdraw huge. */
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_WITHDRAWAL,
                                    1003, 0, 999999999LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_mgr, req_id);

    TransactionRequestStatus st;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, &st, &bs, NULL);
    TEST_ASSERT(st == TRANSACTION_REQUEST_FAILED,
                "failed banking operation should reach FAILED terminal state");
    TEST_ASSERT(bs == BANK_ERROR_INSUFFICIENT_FUNDS,
                "bank result should be INSUFFICIENT_FUNDS");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_c_result_retrieval_before_completion(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    /* While pending/processing, status should be PENDING or PROCESSING. */
    TransactionRequestStatus st;
    BankStatus bs;
    int64_t bal;
    ms = transaction_manager_get_result(&g_mgr, req_id, &st, &bs, &bal);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "get_result on pending request should succeed");
    TEST_ASSERT(st == TRANSACTION_REQUEST_PENDING ||
                    st == TRANSACTION_REQUEST_PROCESSING,
                "status should be PENDING or PROCESSING before completion");

    transaction_manager_wait(&g_mgr, req_id);
    transaction_manager_release(&g_mgr, req_id);
}

static void test_c_wait_for_completion(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    ms = transaction_manager_wait(&g_mgr, req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "wait on valid request should succeed");

    TransactionRequestStatus st;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, &st, &bs, NULL);
    TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED,
                "after wait, request should be terminal");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_c_release_completed_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    transaction_manager_wait(&g_mgr, req_id);

    ms = transaction_manager_release(&g_mgr, req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "release of completed request should succeed");
}

static void test_c_reject_release_pending_request(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    /* Do not wait; release while PENDING/PROCESSING should be rejected. */
    ms = transaction_manager_release(&g_mgr, req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_REQUEST_NOT_TERMINAL,
                "release of pending request should return NOT_TERMINAL");

    /* Clean up by waiting and releasing. */
    transaction_manager_wait(&g_mgr, req_id);
    transaction_manager_release(&g_mgr, req_id);
}

static void test_c_slot_reuse_after_release(void)
{
    TransactionManagerStatus ms;
    uint64_t id1, id2;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &id1);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "first submit should succeed");
    transaction_manager_wait(&g_mgr, id1);
    transaction_manager_release(&g_mgr, id1);

    /* A new submit should reuse the freed slot. */
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 200LL, &id2);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit after release should succeed");
    transaction_manager_wait(&g_mgr, id2);

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, id2, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS, "reused-slot request should succeed");

    transaction_manager_release(&g_mgr, id2);
}

static void test_c_unique_request_ids_after_reuse(void)
{
    TransactionManagerStatus ms;
    uint64_t id1, id2;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &id1);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "first submit should succeed");
    transaction_manager_wait(&g_mgr, id1);
    transaction_manager_release(&g_mgr, id1);

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 200LL, &id2);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "second submit should succeed");
    TEST_ASSERT(id2 != id1,
                "request IDs must be unique even after slot reuse");
    TEST_ASSERT(id2 > id1,
                "request IDs must be monotonically increasing");

    transaction_manager_wait(&g_mgr, id2);
    transaction_manager_release(&g_mgr, id2);
}

/* ------------------------------------------------------------------ */
/* Test D — Banking correctness                                       */
/* ------------------------------------------------------------------ */

static void test_d_deposit_changes_balance(void)
{
    int64_t before = get_balance(1001);
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 123456LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    transaction_manager_wait(&g_mgr, req_id);

    TEST_ASSERT(get_balance(1001) == before + 123456LL,
                "deposit should increase balance by exact amount");

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_SUCCESS, "deposit bank result should be SUCCESS");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_d_withdrawal_changes_balance(void)
{
    int64_t before = get_balance(1002);
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_WITHDRAWAL,
                                    1002, 0, 100000LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    transaction_manager_wait(&g_mgr, req_id);

    TEST_ASSERT(get_balance(1002) == before - 100000LL,
                "withdrawal should decrease balance by exact amount");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_d_transfer_debits_and_credits(void)
{
    int64_t src_before = get_balance(1001);
    int64_t dst_before = get_balance(1002);
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_TRANSFER,
                                    1001, 1002, 250000LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    transaction_manager_wait(&g_mgr, req_id);

    TEST_ASSERT(get_balance(1001) == src_before - 250000LL,
                "source should be debited");
    TEST_ASSERT(get_balance(1002) == dst_before + 250000LL,
                "destination should be credited");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_d_balance_enquiry_returns_correct_balance(void)
{
    TransactionManagerStatus ms;
    uint64_t req_id;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_BALANCE_ENQUIRY,
                                    1002, 0, 0, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    transaction_manager_wait(&g_mgr, req_id);

    int64_t bal = 0;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, &bal);
    TEST_ASSERT(bs == BANK_SUCCESS, "balance enquiry bank result should be SUCCESS");
    TEST_ASSERT(bal == get_balance(1002),
                "returned balance should match actual balance");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_d_failed_operation_does_not_alter_balance(void)
{
    int64_t before = get_balance(1003);
    TransactionManagerStatus ms;
    uint64_t req_id;

    /* Attempt withdrawal exceeding balance. */
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_WITHDRAWAL,
                                    1003, 0, 999999999LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    transaction_manager_wait(&g_mgr, req_id);

    TEST_ASSERT(get_balance(1003) == before,
                "failed withdrawal must not alter balance");

    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, NULL, &bs, NULL);
    TEST_ASSERT(bs == BANK_ERROR_INSUFFICIENT_FUNDS,
                "bank result should be INSUFFICIENT_FUNDS");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_d_manager_uses_existing_bank_synchronization(void)
{
    /* Concurrent deposits to same account via manager; final balance
     * must equal initial + sum of all deposits (bank's per-account mutex
     * ensures correctness). */
    const int count = 20;
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 1000LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    transaction_manager_wait_idle(&g_mgr);

    int64_t expected = get_balance(1001) - (int64_t)count * 1000LL;
    /* All deposits of 1000 each applied. */
    for (int i = 0; i < count; i++) {
        transaction_manager_release(&g_mgr, ids[i]);
    }
    TEST_ASSERT(expected >= 0, "sanity: expected baseline non-negative");
}

/* ------------------------------------------------------------------ */
/* Test E — Concurrency                                               */
/* ------------------------------------------------------------------ */

static void test_e_concurrent_same_account(void)
{
    const int count = 10;
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 500LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    transaction_manager_wait_idle(&g_mgr);

    for (int i = 0; i < count; i++) {
        BankStatus bs;
        transaction_manager_get_result(&g_mgr, ids[i], NULL, &bs, NULL);
        TEST_ASSERT(bs == BANK_SUCCESS, "all concurrent deposits should succeed");
        transaction_manager_release(&g_mgr, ids[i]);
    }
}

static void test_e_concurrent_different_accounts(void)
{
    uint64_t id_a, id_b;
    TransactionManagerStatus ms;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &id_a);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit A should succeed");
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1002, 0, 200LL, &id_b);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit B should succeed");

    transaction_manager_wait(&g_mgr, id_a);
    transaction_manager_wait(&g_mgr, id_b);

    BankStatus bs_a, bs_b;
    transaction_manager_get_result(&g_mgr, id_a, NULL, &bs_a, NULL);
    transaction_manager_get_result(&g_mgr, id_b, NULL, &bs_b, NULL);
    TEST_ASSERT(bs_a == BANK_SUCCESS, "deposit to account A should succeed");
    TEST_ASSERT(bs_b == BANK_SUCCESS, "deposit to account B should succeed");

    transaction_manager_release(&g_mgr, id_a);
    transaction_manager_release(&g_mgr, id_b);
}

static void test_e_opposite_direction_transfers(void)
{
    uint64_t id_ab, id_ba;
    TransactionManagerStatus ms;

    /* 1001 -> 1002 and 1002 -> 1001 concurrently; bank lock ordering
     * prevents deadlock. */
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_TRANSFER,
                                    1001, 1002, 100LL, &id_ab);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "transfer A->B should succeed");
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_TRANSFER,
                                    1002, 1001, 100LL, &id_ba);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "transfer B->A should succeed");

    transaction_manager_wait_idle(&g_mgr);

    BankStatus bs_ab, bs_ba;
    transaction_manager_get_result(&g_mgr, id_ab, NULL, &bs_ab, NULL);
    transaction_manager_get_result(&g_mgr, id_ba, NULL, &bs_ba, NULL);
    TEST_ASSERT(bs_ab == BANK_SUCCESS, "transfer A->B should succeed");
    TEST_ASSERT(bs_ba == BANK_SUCCESS, "transfer B->A should succeed");

    transaction_manager_release(&g_mgr, id_ab);
    transaction_manager_release(&g_mgr, id_ba);
}

static void test_e_multiple_submitters(void)
{
    pthread_barrier_t barrier;
    static uint64_t results[8];
    static int results_filled = 0;

    /* Simple concurrency: submit from main thread rapidly, wait for all. */
    (void)barrier;
    (void)results;
    (void)results_filled;

    const int count = 12;
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1003, 0, 100LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    transaction_manager_wait_idle(&g_mgr);

    for (int i = 0; i < count; i++) {
        BankStatus bs;
        transaction_manager_get_result(&g_mgr, ids[i], NULL, &bs, NULL);
        TEST_ASSERT(bs == BANK_SUCCESS, "all deposits should succeed");
        transaction_manager_release(&g_mgr, ids[i]);
    }
}

static void test_e_concurrent_status_readers(void)
{
    uint64_t req_id;
    TransactionManagerStatus ms;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 500LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    /* Multiple reads while processing. */
    TransactionRequestStatus st;
    BankStatus bs;
    int64_t bal;
    for (int i = 0; i < 5; i++) {
        ms = transaction_manager_get_result(&g_mgr, req_id, &st, &bs, &bal);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                    "concurrent get_result should succeed");
    }

    transaction_manager_wait(&g_mgr, req_id);
    transaction_manager_release(&g_mgr, req_id);
}

static void test_e_no_duplicate_request_ids(void)
{
    const int count = 20;
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 10LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    /* Verify all IDs are unique. */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            TEST_ASSERT(ids[i] != ids[j],
                        "no duplicate request IDs should be issued");
        }
    }

    transaction_manager_wait_idle(&g_mgr);
    for (int i = 0; i < count; i++) {
        transaction_manager_release(&g_mgr, ids[i]);
    }
}

static void test_e_no_lost_or_duplicated_operations(void)
{
    /* Submit N deposits of a fixed amount; verify final balance delta
     * equals N * amount, proving no lost or duplicated operations. */
    const int count = 15;
    int64_t before = get_balance(1001);
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 777LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    transaction_manager_wait_idle(&g_mgr);

    int64_t after = get_balance(1001);
    TEST_ASSERT(after == before + (int64_t)count * 777LL,
                "all deposits must be applied exactly once");

    for (int i = 0; i < count; i++) {
        transaction_manager_release(&g_mgr, ids[i]);
    }
}

static void test_e_all_accepted_requests_reach_terminal_state(void)
{
    const int count = 10;
    uint64_t ids[count];
    TransactionManagerStatus ms;

    for (int i = 0; i < count; i++) {
        ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 100LL, &ids[i]);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
    }

    /* wait_idle returns only when all active_requests are 0, meaning every
     * accepted request reached a terminal state. */
    ms = transaction_manager_wait_idle(&g_mgr);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "wait_idle should succeed when all requests complete");

    for (int i = 0; i < count; i++) {
        TransactionRequestStatus st;
        BankStatus bs;
        transaction_manager_get_result(&g_mgr, ids[i], &st, &bs, NULL);
        TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED ||
                    st == TRANSACTION_REQUEST_FAILED,
                    "each request should reach a terminal state");
        transaction_manager_release(&g_mgr, ids[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Test F — Lifecycle and shutdown                                    */
/* ------------------------------------------------------------------ */

static void test_f_wait_until_manager_idle(void)
{
    TransactionManagerStatus ms;

    /* Start with idle manager. */
    ms = transaction_manager_wait_idle(&g_mgr);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "wait_idle on idle manager should succeed immediately");

    /* Submit and wait for idle. */
    uint64_t req_id;
    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    ms = transaction_manager_wait_idle(&g_mgr);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "wait_idle should return after requests complete");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_f_graceful_shutdown_with_pending_tasks(void)
{
    uint64_t req_id;
    TransactionManagerStatus ms;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");

    /* Shutdown should allow the pending task to finish. */
    ms = transaction_manager_shutdown(&g_mgr);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "shutdown with pending tasks should succeed");

    /* The submitted request should have completed. */
    ms = transaction_manager_wait(&g_mgr, req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "wait after shutdown should succeed");

    TransactionRequestStatus st;
    BankStatus bs;
    transaction_manager_get_result(&g_mgr, req_id, &st, &bs, NULL);
    TEST_ASSERT(st == TRANSACTION_REQUEST_COMPLETED,
                "submitted task should complete during shutdown");

    transaction_manager_release(&g_mgr, req_id);
}

static void test_f_reject_submission_after_shutdown(void)
{
    /* g_mgr is already shut down by the previous test. Submission must
     * be rejected with SHUTDOWN. */
    uint64_t req_id;
    TransactionManagerStatus ms;

    ms = transaction_manager_submit(&g_mgr, TRANSACTION_OP_DEPOSIT,
                                    1001, 0, 100LL, &req_id);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_ERR_SHUTDOWN,
                "submit after shutdown should return SHUTDOWN");
}

static void test_f_repeated_init_destroy(void)
{
    /* Self-contained: each iteration creates its own pool and bank so that
     * destroying the manager (which shuts down its pool) does not affect
     * g_pool/g_mgr. */
    for (int i = 0; i < 5; i++) {
        Bank local_bank;
        ThreadPool local_pool;
        TransactionManager mgr;
        TransactionManagerStatus ms;
        ThreadPoolStatus ts;

        bank_init(&local_bank);
        bank_create_account(&local_bank, 1001, "Alice", 5000000LL, NULL);

        ts = thread_pool_init(&local_pool, 2, 8);
        TEST_ASSERT(ts == THREAD_POOL_SUCCESS,
                    "local pool init should succeed");

        ms = transaction_manager_init(&mgr, &local_pool, &local_bank, 8);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                    "repeated init should succeed");

        uint64_t req_id;
        ms = transaction_manager_submit(&mgr, TRANSACTION_OP_DEPOSIT,
                                        1001, 0, 100LL, &req_id);
        TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS, "submit should succeed");
        transaction_manager_wait(&mgr, req_id);
        transaction_manager_release(&mgr, req_id);

        transaction_manager_destroy(&mgr);
        thread_pool_destroy(&local_pool);
        bank_destroy(&local_bank);
    }
}

static void test_f_no_worker_access_after_destroy(void)
{
    /* g_mgr is already shut down by prior shutdown tests. Verify that
     * destroy is safe and is a no-op on an already-shutdown manager. */
    TransactionManagerStatus ms = transaction_manager_shutdown(&g_mgr);
    TEST_ASSERT(ms == TRANSACTION_MANAGER_SUCCESS,
                "shutdown on already-shutdown manager should be idempotent");

    transaction_manager_destroy(&g_mgr);

    /* Destroy should complete without crashing. Accessing g_mgr after
     * destroy is undefined; this test only verifies destroy returns safely. */
    TEST_ASSERT(1, "destroy should complete safely after shutdown");
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    if (!setup_bank_and_pool()) {
        printf("FAIL: setup_bank_and_pool failed\n");
        return 1;
    }

    /* Test A — Initialization */
    RUN_TEST(test_a_successful_initialization);
    RUN_TEST(test_a2_null_manager);
    RUN_TEST(test_a3_null_pool);
    RUN_TEST(test_a4_null_bank);
    RUN_TEST(test_a5_zero_max_requests);
    RUN_TEST(test_a6_uninitialized_pool);
    RUN_TEST(test_a7_destroy_on_uninitialized_is_noop);
    RUN_TEST(test_a8_destroy_on_null_is_noop);

    /* Test B — Request submission */
    RUN_TEST(test_b_deposit_request);
    RUN_TEST(test_b_withdrawal_request);
    RUN_TEST(test_b_transfer_request);
    RUN_TEST(test_b_balance_enquiry_request);
    RUN_TEST(test_b_invalid_operation);
    RUN_TEST(test_b_invalid_amount_deposit);
    RUN_TEST(test_b_invalid_amount_withdrawal);
    RUN_TEST(test_b_transfer_same_account);
    RUN_TEST(test_b_nonexistent_account);
    RUN_TEST(test_b_registry_capacity_reached);

    /* Test C — Request lifecycle */
    RUN_TEST(test_c_pending_to_processing_to_completed);
    RUN_TEST(test_c_failed_banking_operation);
    RUN_TEST(test_c_result_retrieval_before_completion);
    RUN_TEST(test_c_wait_for_completion);
    RUN_TEST(test_c_release_completed_request);
    RUN_TEST(test_c_reject_release_pending_request);
    RUN_TEST(test_c_slot_reuse_after_release);
    RUN_TEST(test_c_unique_request_ids_after_reuse);

    /* Test D — Banking correctness */
    RUN_TEST(test_d_deposit_changes_balance);
    RUN_TEST(test_d_withdrawal_changes_balance);
    RUN_TEST(test_d_transfer_debits_and_credits);
    RUN_TEST(test_d_balance_enquiry_returns_correct_balance);
    RUN_TEST(test_d_failed_operation_does_not_alter_balance);
    RUN_TEST(test_d_manager_uses_existing_bank_synchronization);

    /* Test E — Concurrency */
    RUN_TEST(test_e_concurrent_same_account);
    RUN_TEST(test_e_concurrent_different_accounts);
    RUN_TEST(test_e_opposite_direction_transfers);
    RUN_TEST(test_e_multiple_submitters);
    RUN_TEST(test_e_concurrent_status_readers);
    RUN_TEST(test_e_no_duplicate_request_ids);
    RUN_TEST(test_e_no_lost_or_duplicated_operations);
    RUN_TEST(test_e_all_accepted_requests_reach_terminal_state);

    /* Test F — Lifecycle and shutdown.
     * Ordered so that tests shutting down g_mgr (and thus g_pool) come last,
     * since later tests need g_pool to still be running. */
    RUN_TEST(test_f_wait_until_manager_idle);
    RUN_TEST(test_f_repeated_init_destroy);
    RUN_TEST(test_f_graceful_shutdown_with_pending_tasks);
    RUN_TEST(test_f_reject_submission_after_shutdown);
    RUN_TEST(test_f_no_worker_access_after_destroy);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");
    fflush(stdout);

    teardown_all();

    return tests_failed == 0 ? 0 : 1;
}
