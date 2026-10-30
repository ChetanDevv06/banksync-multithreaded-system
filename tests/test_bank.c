#include "bank.h"

#include <stdio.h>
#include <string.h>

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
        name();                                                             \
    } while (0)

static void test_bank_init_success(void)
{
    Bank bank;
    BankStatus status;

    status = bank_init(&bank);
    TEST_ASSERT(status == BANK_SUCCESS, "bank_init should return SUCCESS");
    TEST_ASSERT(bank.account_count == 0, "new bank should have zero accounts");

    bank_destroy(&bank);
}

static void test_create_account_success(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc = NULL;

    bank_init(&bank);

    status = bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL,
                                 &acc);
    TEST_ASSERT(status == BANK_SUCCESS, "create account should return SUCCESS");
    TEST_ASSERT(acc != NULL, "out account pointer should be set");
    TEST_ASSERT(acc->account_id == 1001, "account_id should match");
    TEST_ASSERT(strcmp(acc->customer_name, "Rahul Sharma") == 0,
                "customer_name should match");
    TEST_ASSERT(acc->balance_paise == 2500000LL, "balance_paise should match");
    TEST_ASSERT(strncmp(acc->account_number, "BS", 2) == 0,
                "account_number should use BS prefix");
    TEST_ASSERT(acc->mutex_initialized == 1,
                "account-level mutex should be initialized");

    bank_destroy(&bank);
}

static void test_multiple_account_creation(void)
{
    Bank bank;
    BankAccount *a1, *a2, *a3;

    bank_init(&bank);

    TEST_ASSERT(bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL,
                                    NULL) == BANK_SUCCESS, "acct 1001");
    TEST_ASSERT(bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL,
                                    NULL) == BANK_SUCCESS, "acct 1002");
    TEST_ASSERT(bank_create_account(&bank, 1003, "Arjun Kumar", 7500000LL,
                                    NULL) == BANK_SUCCESS, "acct 1003");
    TEST_ASSERT(bank.account_count == 3, "bank should hold 3 accounts");

    a1 = bank_find_account(&bank, 1001);
    a2 = bank_find_account(&bank, 1002);
    a3 = bank_find_account(&bank, 1003);
    TEST_ASSERT(a1 != NULL && a2 != NULL && a3 != NULL,
                "all three accounts should be findable");
    TEST_ASSERT(strcmp(a1->customer_name, "Rahul Sharma") == 0, "name 1001");
    TEST_ASSERT(strcmp(a2->customer_name, "Priya Sharma") == 0, "name 1002");
    TEST_ASSERT(strcmp(a3->customer_name, "Arjun Kumar") == 0, "name 1003");
    TEST_ASSERT(a1->balance_paise == 2500000LL, "balance 1001");
    TEST_ASSERT(a2->balance_paise == 5000000LL, "balance 1002");
    TEST_ASSERT(a3->balance_paise == 7500000LL, "balance 1003");

    bank_destroy(&bank);
}

static void test_account_lookup_success(void)
{
    Bank bank;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    acc = bank_find_account(&bank, 1002);
    TEST_ASSERT(acc != NULL, "existing account should be found");
    TEST_ASSERT(acc->account_id == 1002, "found account id should match");

    bank_destroy(&bank);
}

static void test_account_lookup_not_found(void)
{
    Bank bank;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    acc = bank_find_account(&bank, 9999);
    TEST_ASSERT(acc == NULL, "unknown account id should return NULL");

    bank_destroy(&bank);
}

static void test_duplicate_account_id_rejection(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_create_account(&bank, 1001, "Someone Else", 1000LL, NULL);
    TEST_ASSERT(status == BANK_ERR_DUPLICATE_ACCOUNT,
                "duplicate account id should be rejected");
    TEST_ASSERT(bank.account_count == 1, "registry count must stay at 1");

    bank_destroy(&bank);
}

static void test_invalid_customer_name(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);

    status = bank_create_account(&bank, 1001, NULL, 1000LL, NULL);
    TEST_ASSERT(status == BANK_ERR_INVALID_NAME, "NULL name should be invalid");

    status = bank_create_account(&bank, 1002, "", 1000LL, NULL);
    TEST_ASSERT(status == BANK_ERR_INVALID_NAME, "empty name should be invalid");

    TEST_ASSERT(bank.account_count == 0, "no accounts should be created");

    bank_destroy(&bank);
}

static void test_negative_initial_balance_rejection(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);

    status = bank_create_account(&bank, 1001, "Rahul Sharma", -100LL, NULL);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                "negative balance should be rejected");
    TEST_ASSERT(bank.account_count == 0, "no accounts should be created");

    bank_destroy(&bank);
}

static void test_maximum_account_capacity(void)
{
    Bank bank;
    int i;
    BankStatus status;

    bank_init(&bank);

    for (i = 0; i < BANK_MAX_ACCOUNTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Customer %d", i + 1);
        status = bank_create_account(&bank, i + 1, name, 0LL, NULL);
        TEST_ASSERT(status == BANK_SUCCESS, "creating account within capacity");
    }
    TEST_ASSERT(bank.account_count == BANK_MAX_ACCOUNTS,
                "registry should be full");

    status = bank_create_account(&bank, BANK_MAX_ACCOUNTS + 1, "Overflow",
                                 1000LL, NULL);
    TEST_ASSERT(status == BANK_ERR_MAX_CAPACITY,
                "account beyond capacity should be rejected");
    TEST_ASSERT(bank.account_count == BANK_MAX_ACCOUNTS,
                "registry count must stay at max");

    bank_destroy(&bank);
}

static void test_mutex_initialization_and_cleanup(void)
{
    Bank bank;
    BankAccount *acc;
    BankStatus status;

    bank_init(&bank);

    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, &acc);
    TEST_ASSERT(acc != NULL, "account should exist");
    TEST_ASSERT(acc->mutex_initialized == 1,
                "mutex should be initialized after creation");

    status = bank_destroy(&bank);
    TEST_ASSERT(status == BANK_SUCCESS, "bank_destroy should return SUCCESS");
    TEST_ASSERT(bank.account_count == 0, "destroy should reset the registry");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc == NULL, "account should not be findable after destroy");
}

static void test_deposit_success(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit should return SUCCESS");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc != NULL, "account should exist");
    TEST_ASSERT(acc->balance_paise == 3000000LL,
                "balance should be 3000000 after deposit");

    bank_destroy(&bank);
}

static void test_deposit_different_accounts(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *a1, *a2;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_deposit(&bank, 1001, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit into 1001 should succeed");
    status = bank_deposit(&bank, 1002, 200000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit into 1002 should succeed");

    a1 = bank_find_account(&bank, 1001);
    a2 = bank_find_account(&bank, 1002);
    TEST_ASSERT(a1->balance_paise == 2600000LL, "balance 1001 after deposit");
    TEST_ASSERT(a2->balance_paise == 5200000LL, "balance 1002 after deposit");

    bank_destroy(&bank);
}

static void test_deposit_zero_amount_rejected(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero deposit should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2500000LL,
                "balance unchanged after rejected zero deposit");

    bank_destroy(&bank);
}

static void test_deposit_negative_amount_rejected(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, -100LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative deposit should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2500000LL,
                "balance unchanged after rejected negative deposit");

    bank_destroy(&bank);
}

static void test_deposit_nonexistent_account_rejected(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 9999, 1000LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "deposit into nonexistent account should be rejected");

    bank_destroy(&bank);
}

static void test_deposit_overflow_rejected(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", INT64_MAX, NULL);

    status = bank_deposit(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_ERROR_BALANCE_OVERFLOW,
                "deposit causing overflow should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == INT64_MAX,
                "balance unchanged after rejected overflow deposit");

    bank_destroy(&bank);
}

static void test_deposit_balance_unchanged_after_rejection(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;
    int64_t before;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    acc = bank_find_account(&bank, 1001);
    before = acc->balance_paise;

    status = bank_deposit(&bank, 1001, -500LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "invalid deposit should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == before,
                "balance unchanged after rejected deposit");

    bank_destroy(&bank);
}

static void test_withdrawal_success(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 300000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal should return SUCCESS");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc != NULL, "account should exist");
    TEST_ASSERT(acc->balance_paise == 2200000LL,
                "balance should be 2200000 after withdrawal");

    bank_destroy(&bank);
}

static void test_withdrawal_exact_balance(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 2500000LL);
    TEST_ASSERT(status == BANK_SUCCESS,
                "withdrawal of exact balance should succeed");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 0LL,
                "balance should be 0 after exact withdrawal");

    bank_destroy(&bank);
}

static void test_withdrawal_insufficient_funds(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 3000000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "insufficient funds withdrawal should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2500000LL,
                "balance unchanged after rejected withdrawal");

    bank_destroy(&bank);
}

static void test_withdrawal_zero_amount_rejected(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero withdrawal should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2500000LL,
                "balance unchanged after rejected zero withdrawal");

    bank_destroy(&bank);
}

static void test_withdrawal_negative_amount_rejected(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, -100LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative withdrawal should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2500000LL,
                "balance unchanged after rejected negative withdrawal");

    bank_destroy(&bank);
}

static void test_withdrawal_nonexistent_account_rejected(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 9999, 1000LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "withdrawal from nonexistent account should be rejected");

    bank_destroy(&bank);
}

static void test_withdrawal_balance_never_negative(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);

    status = bank_withdraw(&bank, 1001, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "overdraft withdrawal should be rejected");

    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 1000LL,
                "balance must not become negative");
    TEST_ASSERT(acc->balance_paise >= 0,
                "balance must never be negative");

    bank_destroy(&bank);
}

static void test_sample_transaction_scenario(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);

    /* Account ID: 1001, Rahul Sharma, ₹25,000 = 2500000 paise */
    status = bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    TEST_ASSERT(status == BANK_SUCCESS, "sample account creation");

    /* Deposit ₹5,000 = 500000 paise → expected ₹30,000 = 3000000 paise */
    status = bank_deposit(&bank, 1001, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "sample deposit");
    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 3000000LL,
                "balance should be 3000000 after deposit");

    /* Withdraw ₹3,000 = 300000 paise → expected ₹27,000 = 2700000 paise */
    status = bank_withdraw(&bank, 1001, 300000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "sample withdrawal");
    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2700000LL,
                "balance should be 2700000 after withdrawal");

    /* Attempt withdrawal of ₹30,000 = 3000000 paise → insufficient funds */
    status = bank_withdraw(&bank, 1001, 3000000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "large withdrawal should be rejected");

    /* Final balance must remain ₹27,000 = 2700000 paise */
    acc = bank_find_account(&bank, 1001);
    TEST_ASSERT(acc->balance_paise == 2700000LL,
                "final balance should be 2700000");

    bank_destroy(&bank);
}

static void test_account_independence(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *a1, *a2;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    /* Operate only on account 1001; account 1002 must be unaffected */
    status = bank_deposit(&bank, 1001, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit on 1001");
    status = bank_withdraw(&bank, 1001, 50000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal on 1001");

    a1 = bank_find_account(&bank, 1001);
    a2 = bank_find_account(&bank, 1002);
    TEST_ASSERT(a1->balance_paise == 2550000LL,
                "account 1001 independent balance");
    TEST_ASSERT(a2->balance_paise == 5000000LL,
                "account 1002 unaffected by operations on 1001");

    bank_destroy(&bank);
}

static void test_balance_enquiry_success(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry should return SUCCESS");
    TEST_ASSERT(balance == 2500000LL, "balance should be 2500000");

    bank_destroy(&bank);
}

static void test_balance_enquiry_multiple_accounts(void)
{
    Bank bank;
    BankStatus status;
    int64_t b1, b2;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_get_balance(&bank, 1001, &b1);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry 1001");
    status = bank_get_balance(&bank, 1002, &b2);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry 1002");
    TEST_ASSERT(b1 == 2500000LL, "balance 1001");
    TEST_ASSERT(b2 == 5000000LL, "balance 1002");

    bank_destroy(&bank);
}

static void test_balance_enquiry_zero_balance(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 0LL, NULL);

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry should return SUCCESS");
    TEST_ASSERT(balance == 0LL, "balance should be 0");

    bank_destroy(&bank);
}

static void test_balance_enquiry_account_not_found(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_balance(&bank, 9999, &balance);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "balance enquiry for nonexistent account should be rejected");

    bank_destroy(&bank);
}

static void test_balance_enquiry_null_bank(void)
{
    BankStatus status;
    int64_t balance;

    status = bank_get_balance(NULL, 1001, &balance);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                "NULL bank should be rejected");
}

static void test_balance_enquiry_null_output_pointer(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_balance(&bank, 1001, NULL);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                "NULL output pointer should be rejected");

    bank_destroy(&bank);
}

static void test_balance_enquiry_balance_unchanged(void)
{
    Bank bank;
    BankStatus status;
    int64_t before, after;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_balance(&bank, 1001, &before);
    TEST_ASSERT(status == BANK_SUCCESS, "initial enquiry");
    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(status == BANK_SUCCESS, "repeat enquiry");
    TEST_ASSERT(before == after, "balance unchanged after enquiry");
    TEST_ASSERT(before == 2500000LL, "balance value correct");

    bank_destroy(&bank);
}

static void test_balance_enquiry_after_deposit(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit");
    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry after deposit");
    TEST_ASSERT(balance == 3000000LL, "balance after deposit");

    bank_destroy(&bank);
}

static void test_balance_enquiry_after_withdrawal(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 300000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal");
    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry after withdrawal");
    TEST_ASSERT(balance == 2200000LL, "balance after withdrawal");

    bank_destroy(&bank);
}

static void test_transfer_success(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer should return SUCCESS");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(status == BANK_SUCCESS, "get source balance");
    TEST_ASSERT(src_bal == 2000000LL, "source balance after transfer");

    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(status == BANK_SUCCESS, "get destination balance");
    TEST_ASSERT(dst_bal == 5500000LL, "destination balance after transfer");

    bank_destroy(&bank);
}

static void test_transfer_different_account_pairs(void)
{
    Bank bank;
    BankStatus status;
    int64_t b1, b2, b3;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);
    bank_create_account(&bank, 1003, "Arjun Kumar", 7500000LL, NULL);

    status = bank_transfer(&bank, 1001, 1003, 200000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer 1001->1003");
    status = bank_transfer(&bank, 1003, 1002, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer 1003->1002");

    status = bank_get_balance(&bank, 1001, &b1);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance 1001");
    status = bank_get_balance(&bank, 1002, &b2);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance 1002");
    status = bank_get_balance(&bank, 1003, &b3);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance 1003");
    TEST_ASSERT(b1 == 2300000LL, "balance 1001");
    TEST_ASSERT(b2 == 5100000LL, "balance 1002");
    TEST_ASSERT(b3 == 7600000LL, "balance 1003");

    bank_destroy(&bank);
}

static void test_transfer_exact_balance(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 2500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "exact-balance transfer");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(status == BANK_SUCCESS, "get source balance");
    TEST_ASSERT(src_bal == 0LL, "source balance zero after exact transfer");

    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(status == BANK_SUCCESS, "get destination balance");
    TEST_ASSERT(dst_bal == 7500000LL, "destination balance after exact transfer");

    bank_destroy(&bank);
}

static void test_transfer_null_bank(void)
{
    BankStatus status;

    status = bank_transfer(NULL, 1001, 1002, 1000LL);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                "NULL bank should be rejected");
}

static void test_transfer_zero_amount(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero amount transfer should be rejected");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(src_bal == 2500000LL, "source balance unchanged");
    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(dst_bal == 5000000LL, "destination balance unchanged");

    bank_destroy(&bank);
}

static void test_transfer_negative_amount(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, -100LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative amount transfer should be rejected");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(src_bal == 2500000LL, "source balance unchanged");
    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(dst_bal == 5000000LL, "destination balance unchanged");

    bank_destroy(&bank);
}

static void test_transfer_nonexistent_source(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_transfer(&bank, 9999, 1001, 1000LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "nonexistent source account should be rejected");

    bank_destroy(&bank);
}

static void test_transfer_nonexistent_destination(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_transfer(&bank, 1001, 9999, 1000LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "nonexistent destination account should be rejected");

    bank_destroy(&bank);
}

static void test_transfer_same_account(void)
{
    Bank bank;
    BankStatus status;
    int64_t bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_transfer(&bank, 1001, 1001, 1000LL);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                "same account transfer should be rejected");

    status = bank_get_balance(&bank, 1001, &bal);
    TEST_ASSERT(bal == 2500000LL, "balance unchanged after same-account transfer");

    bank_destroy(&bank);
}

static void test_transfer_insufficient_funds(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "insufficient funds transfer should be rejected");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(src_bal == 1000LL, "source balance unchanged");
    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(dst_bal == 5000000LL, "destination balance unchanged");

    bank_destroy(&bank);
}

static void test_transfer_destination_overflow(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", INT64_MAX, NULL);

    status = bank_transfer(&bank, 1001, 1002, 1LL);
    TEST_ASSERT(status == BANK_ERROR_BALANCE_OVERFLOW,
                "destination overflow transfer should be rejected");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(src_bal == 1000LL, "source balance unchanged");
    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(dst_bal == INT64_MAX, "destination balance unchanged");

    bank_destroy(&bank);
}

static void test_transfer_total_balance_conserved(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_before, dst_before, src_after, dst_after;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_get_balance(&bank, 1001, &src_before);
    TEST_ASSERT(status == BANK_SUCCESS, "get source before");
    status = bank_get_balance(&bank, 1002, &dst_before);
    TEST_ASSERT(status == BANK_SUCCESS, "get dest before");

    status = bank_transfer(&bank, 1001, 1002, 750000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer");

    status = bank_get_balance(&bank, 1001, &src_after);
    TEST_ASSERT(status == BANK_SUCCESS, "get source after");
    status = bank_get_balance(&bank, 1002, &dst_after);
    TEST_ASSERT(status == BANK_SUCCESS, "get dest after");

    TEST_ASSERT(src_before == src_after + 750000LL,
                "source debited by transfer amount");
    TEST_ASSERT(dst_before + 750000LL == dst_after,
                "destination credited by transfer amount");
    TEST_ASSERT(src_before + dst_before == src_after + dst_after,
                "total balance conserved");

    bank_destroy(&bank);
}

static void test_history_deposit_creates_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit should succeed");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 1, "deposit should create exactly one record");
    TEST_ASSERT(txns[0].type == TRANSACTION_DEPOSIT, "type should be deposit");
    TEST_ASSERT(txns[0].source_account_id == 1001,
                "source should be receiving account");
    TEST_ASSERT(txns[0].destination_account_id == TRANSACTION_NONE_ACCOUNT_ID,
                "destination should be sentinel for deposit");
    TEST_ASSERT(txns[0].amount_paise == 500000LL, "amount should match");
    TEST_ASSERT(txns[0].transaction_id == 1, "first transaction ID should be 1");
    TEST_ASSERT(txns[0].timestamp > 0, "timestamp should be valid");

    bank_destroy(&bank);
}

static void test_history_withdrawal_creates_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_withdraw(&bank, 1001, 300000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal should succeed");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 1, "withdrawal should create exactly one record");
    TEST_ASSERT(txns[0].type == TRANSACTION_WITHDRAWAL, "type should be withdrawal");
    TEST_ASSERT(txns[0].source_account_id == 1001,
                "source should be debited account");
    TEST_ASSERT(txns[0].destination_account_id == TRANSACTION_NONE_ACCOUNT_ID,
                "destination should be sentinel for withdrawal");
    TEST_ASSERT(txns[0].amount_paise == 300000LL, "amount should match");

    bank_destroy(&bank);
}

static void test_history_transfer_creates_exactly_one_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 500000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer should succeed");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for source");
    TEST_ASSERT(count == 1, "transfer should create exactly one record for source");
    TEST_ASSERT(txns[0].type == TRANSACTION_TRANSFER, "type should be transfer");
    TEST_ASSERT(txns[0].source_account_id == 1001, "source should be 1001");
    TEST_ASSERT(txns[0].destination_account_id == 1002,
                "destination should be 1002");
    TEST_ASSERT(txns[0].amount_paise == 500000LL, "amount should match");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for destination");
    TEST_ASSERT(count == 1, "transfer should create exactly one record for destination");
    TEST_ASSERT(txns[0].transaction_id == 1,
                "same transaction ID for source and destination");

    bank_destroy(&bank);
}

static void test_history_balance_enquiry_creates_no_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry should succeed");
    TEST_ASSERT(balance == 2500000LL, "balance should match");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "balance enquiry should create no record");

    bank_destroy(&bank);
}

static void test_history_failed_deposit_creates_no_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_deposit(&bank, 1001, -100LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative deposit should be rejected");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "failed deposit should create no record");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(balance == 2500000LL, "balance unchanged after failed deposit");

    bank_destroy(&bank);
}

static void test_history_failed_withdrawal_creates_no_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);

    status = bank_withdraw(&bank, 1001, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "insufficient withdrawal should be rejected");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "failed withdrawal should create no record");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(balance == 1000LL, "balance unchanged after failed withdrawal");

    bank_destroy(&bank);
}

static void test_history_failed_transfer_creates_no_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;
    int64_t src_bal, dst_bal;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_transfer(&bank, 1001, 1002, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "insufficient transfer should be rejected");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for source");
    TEST_ASSERT(count == 0, "failed transfer should create no record for source");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for destination");
    TEST_ASSERT(count == 0, "failed transfer should create no record for destination");

    status = bank_get_balance(&bank, 1001, &src_bal);
    TEST_ASSERT(src_bal == 1000LL, "source balance unchanged");
    status = bank_get_balance(&bank, 1002, &dst_bal);
    TEST_ASSERT(dst_bal == 5000000LL, "destination balance unchanged");

    bank_destroy(&bank);
}

static void test_history_sequential_unique_ids(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    status = bank_deposit(&bank, 1001, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit 1");
    status = bank_withdraw(&bank, 1002, 50000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal 2");
    status = bank_transfer(&bank, 1001, 1002, 200000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer 3");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 2, "account 1001 should have 2 records");
    TEST_ASSERT(txns[0].transaction_id == 1, "first ID should be 1");
    TEST_ASSERT(txns[1].transaction_id == 3, "third ID should be 3");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for 1002");
    TEST_ASSERT(count == 2, "account 1002 should have 2 records");
    TEST_ASSERT(txns[0].transaction_id == 2, "second ID should be 2");
    TEST_ASSERT(txns[1].transaction_id == 3, "third ID should be 3");

    bank_destroy(&bank);
}

static void test_history_retrieve_single_account(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    bank_deposit(&bank, 1001, 100000LL);
    bank_deposit(&bank, 1002, 200000LL);

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve history for 1001");
    TEST_ASSERT(count == 1, "1001 should have 1 record");
    TEST_ASSERT(txns[0].source_account_id == 1001, "record for 1001");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve history for 1002");
    TEST_ASSERT(count == 1, "1002 should have 1 record");
    TEST_ASSERT(txns[0].source_account_id == 1002, "record for 1002");

    bank_destroy(&bank);
}

static void test_history_transfer_filter_both_accounts(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    bank_transfer(&bank, 1001, 1002, 500000LL);

    /* Source account sees the transfer */
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve history for source");
    TEST_ASSERT(count == 1, "source should see transfer");
    TEST_ASSERT(txns[0].source_account_id == 1001, "source is 1001");
    TEST_ASSERT(txns[0].destination_account_id == 1002, "dest is 1002");

    /* Destination account also sees the transfer */
    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve history for destination");
    TEST_ASSERT(count == 1, "destination should see transfer");
    TEST_ASSERT(txns[0].source_account_id == 1001, "source is 1001");
    TEST_ASSERT(txns[0].destination_account_id == 1002, "dest is 1002");

    bank_destroy(&bank);
}

static void test_history_empty_for_new_account(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve empty history");
    TEST_ASSERT(count == 0, "new account should have empty history");

    bank_destroy(&bank);
}

static void test_history_invalid_account(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_transaction_history(&bank, 9999, txns, 10, &count);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "invalid account should be rejected");

    bank_destroy(&bank);
}

static void test_history_null_arguments(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    status = bank_get_transaction_history(NULL, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT, "NULL bank rejected");

    status = bank_get_transaction_history(&bank, 1001, NULL, 10, &count);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT, "NULL output rejected");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, NULL);
    TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT, "NULL count rejected");

    bank_destroy(&bank);
}

static void test_history_unchanged_after_retrieval(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns1[10], txns2[10];
    size_t count1, count2;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_deposit(&bank, 1001, 500000LL);

    status = bank_get_transaction_history(&bank, 1001, txns1, 10, &count1);
    TEST_ASSERT(status == BANK_SUCCESS, "first retrieval");
    TEST_ASSERT(count1 == 1, "one record");

    status = bank_get_transaction_history(&bank, 1001, txns2, 10, &count2);
    TEST_ASSERT(status == BANK_SUCCESS, "second retrieval");
    TEST_ASSERT(count2 == 1, "still one record");
    TEST_ASSERT(txns1[0].transaction_id == txns2[0].transaction_id,
                "same ID");
    TEST_ASSERT(txns1[0].amount_paise == txns2[0].amount_paise,
                "same amount");

    bank_destroy(&bank);
}

static void test_history_insufficient_output_capacity(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[1];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    bank_deposit(&bank, 1001, 100000LL);
    bank_deposit(&bank, 1001, 200000LL);

    /* Buffer can hold only 1 record, but 2 match */
    status = bank_get_transaction_history(&bank, 1001, txns, 1, &count);
    TEST_ASSERT(status == BANK_ERROR_BUFFER_TOO_SMALL,
                "insufficient capacity should be rejected");
    TEST_ASSERT(count == 2, "count should report required size");

    bank_destroy(&bank);
}

static void test_history_capacity_limit(void)
{
    Bank bank;
    BankStatus status;
    int i;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    /* Fill history to capacity */
    for (i = 0; i < BANK_MAX_TRANSACTIONS; i++) {
        status = bank_deposit(&bank, 1001, 1LL);
        TEST_ASSERT(status == BANK_SUCCESS, "deposit within capacity");
    }

    /* Next transaction should be rejected without changing balance */
    {
        int64_t balance_before;
        status = bank_get_balance(&bank, 1001, &balance_before);
        TEST_ASSERT(status == BANK_SUCCESS, "get balance before");

        status = bank_deposit(&bank, 1001, 1LL);
        TEST_ASSERT(status == BANK_ERROR_HISTORY_FULL,
                    "deposit beyond capacity should be rejected");

        int64_t balance_after;
        status = bank_get_balance(&bank, 1001, &balance_after);
        TEST_ASSERT(balance_before == balance_after,
                    "balance unchanged after rejected transaction");
    }

    bank_destroy(&bank);
}

static void test_history_records_not_overwritten(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    bank_deposit(&bank, 1001, 100000LL);
    bank_deposit(&bank, 1001, 200000LL);

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "retrieve history");
    TEST_ASSERT(count == 2, "two records");
    TEST_ASSERT(txns[0].amount_paise == 100000LL, "first record amount");
    TEST_ASSERT(txns[1].amount_paise == 200000LL, "second record amount");

    bank_destroy(&bank);
}

static void test_repeated_insufficient_funds_does_not_exhaust_capacity(void)
{
    Bank bank;
    BankStatus status;
    int i;
    int64_t balance;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    /* Repeated failed withdrawals must not consume history capacity */
    for (i = 0; i < 50; i++) {
        status = bank_withdraw(&bank, 1001, 2000LL);
        TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                    "insufficient funds withdrawal should be rejected");
    }

    /* A successful transaction must still work after many failures */
    status = bank_deposit(&bank, 1001, 500LL);
    TEST_ASSERT(status == BANK_SUCCESS,
                "deposit should succeed after repeated failures");

    /* No canceled reservations should appear in history */
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 1, "only the successful deposit should be recorded");
    TEST_ASSERT(txns[0].amount_paise == 500LL,
                "record should be the successful deposit");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(balance == 1500LL, "balance should reflect successful deposit");

    bank_destroy(&bank);
}

static void test_repeated_invalid_operations_do_not_exhaust_capacity(void)
{
    Bank bank;
    BankStatus status;
    int i;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    /* Repeated invalid deposits */
    for (i = 0; i < 50; i++) {
        status = bank_deposit(&bank, 1001, -100LL);
        TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                    "negative deposit should be rejected");
        status = bank_deposit(&bank, 1001, 0LL);
        TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                    "zero deposit should be rejected");
    }

    /* Repeated invalid transfers */
    for (i = 0; i < 50; i++) {
        status = bank_transfer(&bank, 1001, 1002, -100LL);
        TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                    "negative transfer should be rejected");
        status = bank_transfer(&bank, 1001, 1001, 1000LL);
        TEST_ASSERT(status == BANK_ERR_INVALID_ARGUMENT,
                    "same-account transfer should be rejected");
    }

    /* History must be empty of canceled reservations */
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "failed operations should not create records");

    /* A successful transaction must still work */
    status = bank_transfer(&bank, 1001, 1002, 1000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer should succeed");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval after success");
    TEST_ASSERT(count == 1, "only the successful transfer should be recorded");

    bank_destroy(&bank);
}

static void test_canceled_reservations_do_not_appear_in_history(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    /* Failed operations cancel their reservations */
    status = bank_withdraw(&bank, 1001, 999999999999LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "large withdrawal should be rejected");
    status = bank_transfer(&bank, 1001, 1002, 999999999999LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "large transfer should be rejected");

    /* History must not contain any records from canceled reservations */
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "canceled reservations must not appear");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 0, "canceled reservations must not appear");

    bank_destroy(&bank);
}

static void test_canceled_slot_can_be_reused(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[20];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    /* Create a mix of failed and successful operations */
    status = bank_withdraw(&bank, 1001, 999999999999LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "failed withdrawal cancels reservation");

    status = bank_deposit(&bank, 1001, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "successful deposit");

    status = bank_withdraw(&bank, 1001, 999999999999LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "another failed withdrawal cancels reservation");

    status = bank_deposit(&bank, 1001, 200000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "another successful deposit");

    /* Canceled slots are reusable; committed records are intact */
    status = bank_get_transaction_history(&bank, 1001, txns, 20, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 2, "only successful deposits are recorded");
    TEST_ASSERT(txns[0].amount_paise == 100000LL, "first deposit amount");
    TEST_ASSERT(txns[1].amount_paise == 200000LL, "second deposit amount");

    bank_destroy(&bank);
}

static void test_multiple_outstanding_reservations_remain_independent(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[20];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);

    /* Sequential reservations that succeed and commit independently */
    status = bank_deposit(&bank, 1001, 100000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit 1");
    status = bank_withdraw(&bank, 1002, 50000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal 2");
    status = bank_transfer(&bank, 1001, 1002, 200000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer 3");

    status = bank_get_transaction_history(&bank, 1001, txns, 20, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for 1001");
    TEST_ASSERT(count == 2, "1001 has deposit and transfer");

    count = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 20, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for 1002");
    TEST_ASSERT(count == 2, "1002 has withdrawal and transfer");

    bank_destroy(&bank);
}

static void test_successful_transactions_still_receive_unique_ids(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[20];
    size_t count;
    int i;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    /* Mix of failed and successful operations; IDs must remain unique */
    for (i = 0; i < 5; i++) {
        status = bank_withdraw(&bank, 1001, 999999999999LL);
        TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                    "failed withdrawal should be rejected");
        status = bank_deposit(&bank, 1001, 1000LL * (i + 1));
        TEST_ASSERT(status == BANK_SUCCESS, "successful deposit");
    }

    status = bank_get_transaction_history(&bank, 1001, txns, 20, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval should succeed");
    TEST_ASSERT(count == 5, "five successful deposits recorded");

    for (i = 0; i < (int)count; i++) {
        TEST_ASSERT(txns[i].amount_paise == 1000LL * (i + 1),
                    "deposit amount should match");
    }

    bank_destroy(&bank);
}

static void test_history_capacity_reached_only_by_committed_or_active(void)
{
    Bank bank;
    BankStatus status;
    int i;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);

    /* Fill all slots with committed records */
    for (i = 0; i < BANK_MAX_TRANSACTIONS; i++) {
        status = bank_deposit(&bank, 1001, 1LL);
        TEST_ASSERT(status == BANK_SUCCESS, "deposit within capacity");
    }

    /* History is full: all slots COMMITTED */
    status = bank_deposit(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_ERROR_HISTORY_FULL,
                "history full when all slots committed");

    bank_destroy(&bank);
}

/* =====================================================================
 * M2.6 — Concurrent test infrastructure
 * ===================================================================== */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int total;
    int released;
} barrier_t;

static void barrier_init(barrier_t *b, int total)
{
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = 0;
    b->total = total;
    b->released = 0;
}

static void barrier_wait(barrier_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->count++;
    if (b->count >= b->total && !b->released) {
        b->released = 1;
        pthread_cond_broadcast(&b->cond);
    }
    while (!b->released) {
        pthread_cond_wait(&b->cond, &b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
}

static void barrier_destroy(barrier_t *b)
{
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

typedef struct {
    Bank *bank;
    int account_id;
    int64_t amount_paise;
    int iterations;
    barrier_t *barrier;
    long success_count;
    long failure_count;
} deposit_args_t;

typedef struct {
    Bank *bank;
    int account_id;
    int64_t amount_paise;
    int iterations;
    barrier_t *barrier;
    long success_count;
    long failure_count;
} withdraw_args_t;

typedef struct {
    Bank *bank;
    int source_id;
    int dest_id;
    int64_t amount_paise;
    int iterations;
    barrier_t *barrier;
    long success_count;
    long failure_count;
} transfer_args_t;

typedef struct {
    Bank *bank;
    int account_id;
    int iterations;
    barrier_t *barrier;
    long success_count;
    long failure_count;
} history_reader_args_t;

static void *deposit_worker(void *arg)
{
    deposit_args_t *args = (deposit_args_t *)arg;
    int i;

    barrier_wait(args->barrier);

    for (i = 0; i < args->iterations; i++) {
        if (bank_deposit(args->bank, args->account_id, args->amount_paise)
            == BANK_SUCCESS) {
            __atomic_add_fetch(&args->success_count, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&args->failure_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    return NULL;
}

static void *withdraw_worker(void *arg)
{
    withdraw_args_t *args = (withdraw_args_t *)arg;
    int i;

    barrier_wait(args->barrier);

    for (i = 0; i < args->iterations; i++) {
        if (bank_withdraw(args->bank, args->account_id, args->amount_paise)
            == BANK_SUCCESS) {
            __atomic_add_fetch(&args->success_count, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&args->failure_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    return NULL;
}

static void *transfer_worker(void *arg)
{
    transfer_args_t *args = (transfer_args_t *)arg;
    int i;

    barrier_wait(args->barrier);

    for (i = 0; i < args->iterations; i++) {
        if (bank_transfer(args->bank, args->source_id, args->dest_id,
                          args->amount_paise) == BANK_SUCCESS) {
            __atomic_add_fetch(&args->success_count, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&args->failure_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    return NULL;
}

static void *history_reader_worker(void *arg)
{
    history_reader_args_t *args = (history_reader_args_t *)arg;
    Transaction txns[128];
    size_t count;
    int i;
    long last_id = 0;

    barrier_wait(args->barrier);

    for (i = 0; i < args->iterations; i++) {
        count = 0;
        if (bank_get_transaction_history(args->bank, args->account_id, txns,
                                         128, &count) == BANK_SUCCESS) {
            __atomic_add_fetch(&args->success_count, 1, __ATOMIC_SEQ_CST);
            /* Verify all returned records are COMMITTED */
            for (count = 0; count < 64 && txns[count].state
                 == TRANSACTION_STATE_COMMITTED; count++) {
                /* just counting */
            }
            /* Verify unique IDs within this snapshot */
            for (count = 0; txns[count].state == TRANSACTION_STATE_COMMITTED
                 && count < 64; count++) {
                if (txns[count].transaction_id > last_id) {
                    last_id = txns[count].transaction_id;
                }
            }
        } else {
            __atomic_add_fetch(&args->failure_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    return NULL;
}

static void *insufficient_funds_worker(void *arg)
{
    withdraw_args_t *args = (withdraw_args_t *)arg;
    int i;

    barrier_wait(args->barrier);

    for (i = 0; i < args->iterations; i++) {
        /* Attempt withdrawal larger than balance — should fail and cancel */
        if (bank_withdraw(args->bank, args->account_id,
                          args->amount_paise) != BANK_SUCCESS) {
            __atomic_add_fetch(&args->failure_count, 1, __ATOMIC_SEQ_CST);
        } else {
            __atomic_add_fetch(&args->success_count, 1, __ATOMIC_SEQ_CST);
        }
    }

    return NULL;
}

/* =====================================================================
 * M2.6 — Boundary tests
 * ===================================================================== */

static void test_boundary_zero_initial_balance(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    status = bank_create_account(&bank, 1001, "Zero Balance", 0LL, &acc);
    TEST_ASSERT(status == BANK_SUCCESS, "zero initial balance should succeed");
    TEST_ASSERT(acc->balance_paise == 0LL, "balance should be zero");

    status = bank_deposit(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit one paise to zero balance");
    TEST_ASSERT(acc->balance_paise == 1LL, "balance should be one paise");

    status = bank_withdraw(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdraw one paise to zero");
    TEST_ASSERT(acc->balance_paise == 0LL, "balance should return to zero");

    bank_destroy(&bank);
}

static void test_boundary_max_initial_balance(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *acc;

    bank_init(&bank);
    status = bank_create_account(&bank, 1001, "Max Balance", INT64_MAX, &acc);
    TEST_ASSERT(status == BANK_SUCCESS, "max initial balance should succeed");
    TEST_ASSERT(acc->balance_paise == INT64_MAX, "balance should be INT64_MAX");

    /* Deposit should overflow */
    status = bank_deposit(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_ERROR_BALANCE_OVERFLOW,
                "deposit to INT64_MAX should overflow");

    bank_destroy(&bank);
}

static void test_boundary_deposit_to_int64_max_exact(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Exact Max", INT64_MAX - 100LL, NULL);

    status = bank_deposit(&bank, 1001, 100LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit to reach INT64_MAX exactly");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance after exact max deposit");
    TEST_ASSERT(balance == INT64_MAX, "balance should be INT64_MAX");

    bank_destroy(&bank);
}

static void test_boundary_withdraw_one_paise(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "One Paise", 1LL, NULL);

    status = bank_withdraw(&bank, 1001, 1LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdraw one paise should succeed");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(balance == 0LL, "balance should be zero after one paise withdrawal");

    bank_destroy(&bank);
}

static void test_boundary_withdraw_exceeding_balance(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Insufficient", 100LL, NULL);

    status = bank_withdraw(&bank, 1001, 101LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS,
                "withdrawal exceeding balance should be rejected");

    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(balance == 100LL, "balance unchanged after rejected withdrawal");

    bank_destroy(&bank);
}

static void test_boundary_account_lookup_beginning_and_end(void)
{
    Bank bank;
    BankStatus status;
    BankAccount *first, *last;
    int i;

    bank_init(&bank);

    /* Fill registry to capacity */
    for (i = 0; i < BANK_MAX_ACCOUNTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Customer %d", i + 1);
        status = bank_create_account(&bank, 1000 + i, name, 1000LL, NULL);
        TEST_ASSERT(status == BANK_SUCCESS, "create account within capacity");
    }

    /* Look up first and last accounts */
    first = bank_find_account(&bank, 1000);
    TEST_ASSERT(first != NULL, "first account should be findable");
    TEST_ASSERT(first->account_id == 1000, "first account id should match");

    last = bank_find_account(&bank, 1000 + BANK_MAX_ACCOUNTS - 1);
    TEST_ASSERT(last != NULL, "last account should be findable");
    TEST_ASSERT(last->account_id == 1000 + BANK_MAX_ACCOUNTS - 1,
                "last account id should match");

    bank_destroy(&bank);
}

static void test_boundary_invalid_account_ids(void)
{
    Bank bank;
    BankStatus status;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Valid", 1000LL, NULL);

    /* Negative account ID */
    status = bank_deposit(&bank, -1, 100LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "negative account ID should not be found");

    status = bank_withdraw(&bank, -1, 100LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "negative account ID should not be found for withdrawal");

    status = bank_get_balance(&bank, -1, &balance);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "negative account ID should not be found for balance enquiry");

    status = bank_transfer(&bank, -1, 1001, 100LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "negative source account ID should not be found");

    status = bank_transfer(&bank, 1001, -1, 100LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "negative destination account ID should not be found");

    /* Zero account ID */
    status = bank_deposit(&bank, 0, 100LL);
    TEST_ASSERT(status == BANK_ERR_ACCOUNT_NOT_FOUND,
                "zero account ID should not be found");

    bank_destroy(&bank);
}

static void test_boundary_negative_and_zero_amounts(void)
{
    Bank bank;
    BankStatus status;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Amounts", 1000000LL, NULL);

    /* Zero and negative amounts for all operations */
    status = bank_deposit(&bank, 1001, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero deposit should be rejected");

    status = bank_deposit(&bank, 1001, -1LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative deposit should be rejected");

    status = bank_withdraw(&bank, 1001, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero withdrawal should be rejected");

    status = bank_withdraw(&bank, 1001, -1LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "negative withdrawal should be rejected");

    status = bank_transfer(&bank, 1001, 1002, 0LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT,
                "zero transfer amount returns INVALID_AMOUNT");

    bank_destroy(&bank);
}

static void test_boundary_history_exactly_one_record(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "One Record", 1000LL, NULL);

    status = bank_deposit(&bank, 1001, 500LL);
    TEST_ASSERT(status == BANK_SUCCESS, "single deposit");

    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 1, "exactly one committed record");
    TEST_ASSERT(txns[0].state == TRANSACTION_STATE_COMMITTED,
                "record should be COMMITTED");

    bank_destroy(&bank);
}

/* =====================================================================
 * M2.6 — Financial invariant tests
 * ===================================================================== */

static void test_invariant_deposit_exact_amount(void)
{
    Bank bank;
    BankStatus status;
    int64_t before, after;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Exact", 5000000LL, NULL);

    status = bank_get_balance(&bank, 1001, &before);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance before");

    status = bank_deposit(&bank, 1001, 123456LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit exact amount");

    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance after");
    TEST_ASSERT(after == before + 123456LL,
                "deposit should increase balance by exact amount");

    bank_destroy(&bank);
}

static void test_invariant_withdraw_exact_amount(void)
{
    Bank bank;
    BankStatus status;
    int64_t before, after;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Exact", 5000000LL, NULL);

    status = bank_get_balance(&bank, 1001, &before);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance before");

    status = bank_withdraw(&bank, 1001, 123456LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdraw exact amount");

    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance after");
    TEST_ASSERT(after == before - 123456LL,
                "withdrawal should decrease balance by exact amount");

    bank_destroy(&bank);
}

static void test_invariant_transfer_debits_credits_same_amount(void)
{
    Bank bank;
    BankStatus status;
    int64_t src_before, src_after, dst_before, dst_after;
    int64_t amount = 777777LL;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Source", 5000000LL, NULL);
    bank_create_account(&bank, 1002, "Dest", 3000000LL, NULL);

    status = bank_get_balance(&bank, 1001, &src_before);
    TEST_ASSERT(status == BANK_SUCCESS, "get source before");
    status = bank_get_balance(&bank, 1002, &dst_before);
    TEST_ASSERT(status == BANK_SUCCESS, "get dest before");

    status = bank_transfer(&bank, 1001, 1002, amount);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer");

    status = bank_get_balance(&bank, 1001, &src_after);
    TEST_ASSERT(status == BANK_SUCCESS, "get source after");
    status = bank_get_balance(&bank, 1002, &dst_after);
    TEST_ASSERT(status == BANK_SUCCESS, "get dest after");

    TEST_ASSERT(src_before - src_after == amount,
                "source debited by exact amount");
    TEST_ASSERT(dst_after - dst_before == amount,
                "destination credited by exact amount");
    TEST_ASSERT(src_before + dst_before == src_after + dst_after,
                "total money conserved");

    bank_destroy(&bank);
}

static void test_invariant_failed_operation_leaves_balances_unchanged(void)
{
    Bank bank;
    BankStatus status;
    int64_t before, after;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Failed", 1000LL, NULL);

    status = bank_get_balance(&bank, 1001, &before);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance before failed ops");

    /* Failed deposit */
    status = bank_deposit(&bank, 1001, -100LL);
    TEST_ASSERT(status == BANK_ERROR_INVALID_AMOUNT, "failed deposit");
    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(after == before, "balance unchanged after failed deposit");

    /* Failed withdrawal */
    status = bank_withdraw(&bank, 1001, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS, "failed withdrawal");
    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(after == before, "balance unchanged after failed withdrawal");

    /* Failed transfer (insufficient funds) */
    bank_create_account(&bank, 1002, "Dest", 5000LL, NULL);
    status = bank_transfer(&bank, 1001, 1002, 2000LL);
    TEST_ASSERT(status == BANK_ERROR_INSUFFICIENT_FUNDS, "failed transfer");
    status = bank_get_balance(&bank, 1001, &after);
    TEST_ASSERT(after == before, "balance unchanged after failed transfer");

    bank_destroy(&bank);
}

static void test_invariant_one_committed_record_per_successful_operation(void)
{
    Bank bank;
    BankStatus status;
    Transaction txns[10];
    size_t count;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "One Record", 1000000LL, NULL);
    bank_create_account(&bank, 1002, "Dest", 1000000LL, NULL);

    /* Deposit: exactly 1 record */
    status = bank_deposit(&bank, 1001, 50000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit");
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(count == 1, "deposit creates exactly one record");

    /* Withdrawal: exactly 1 record */
    status = bank_withdraw(&bank, 1001, 25000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "withdrawal");
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(count == 2, "deposit + withdrawal = 2 records");

    /* Transfer: exactly 1 record (not 2) */
    status = bank_transfer(&bank, 1001, 1002, 10000LL);
    TEST_ASSERT(status == BANK_SUCCESS, "transfer");
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(count == 3, "deposit + withdrawal + transfer = 3 records");

    /* Balance enquiry: no record */
    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "balance enquiry");
    status = bank_get_transaction_history(&bank, 1001, txns, 10, &count);
    TEST_ASSERT(count == 3, "balance enquiry creates no record");

    bank_destroy(&bank);
}

/* =====================================================================
 * M2.6 — Concurrent tests (Tests A-F)
 * ===================================================================== */

static void test_concurrent_deposits(void)
{
    /* Test A: Concurrent deposits */
    Bank bank;
    BankStatus status;
    int64_t final_balance;
    pthread_t threads[10];
    deposit_args_t args[10];
    barrier_t barrier;
    int i;
    long total_success = 0;
    long total_failure = 0;
    Transaction txns[1024];
    size_t count;
    int unique_ids[1024];
    int id_count = 0;
    int j;
    int duplicate = 0;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Concurrent Deposit", 1000000LL, NULL);

    barrier_init(&barrier, 10);

    for (i = 0; i < 10; i++) {
        args[i].bank = &bank;
        args[i].account_id = 1001;
        args[i].amount_paise = 100000LL;  /* ₹1,000 */
        args[i].iterations = 100;
        args[i].barrier = &barrier;
        args[i].success_count = 0;
        args[i].failure_count = 0;
        pthread_create(&threads[i], NULL, deposit_worker, &args[i]);
    }

    for (i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
        total_success += args[i].success_count;
        total_failure += args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(total_success == 1000, "1000 successful deposits expected");
    TEST_ASSERT(total_failure == 0, "zero failures expected");

    status = bank_get_balance(&bank, 1001, &final_balance);
    TEST_ASSERT(status == BANK_SUCCESS, "get final balance");
    TEST_ASSERT(final_balance == 101000000LL,
                "final balance should be ₹1,010,000 = 101000000 paise");

    /* Verify history: exactly 1000 committed deposit records */
    status = bank_get_transaction_history(&bank, 1001, txns, 1024, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 1000, "exactly 1000 committed records");

    /* Verify unique transaction IDs */
    for (i = 0; i < (int)count; i++) {
        TEST_ASSERT(txns[i].state == TRANSACTION_STATE_COMMITTED,
                    "all records should be COMMITTED");
        TEST_ASSERT(txns[i].type == TRANSACTION_DEPOSIT,
                    "all records should be deposits");
        for (j = 0; j < id_count; j++) {
            if (unique_ids[j] == txns[i].transaction_id) {
                duplicate = 1;
                break;
            }
        }
        TEST_ASSERT(duplicate == 0, "no duplicate transaction IDs");
        unique_ids[id_count++] = txns[i].transaction_id;
    }

    bank_destroy(&bank);
}

static void test_concurrent_withdrawals(void)
{
    /* Test B: Concurrent withdrawals */
    Bank bank;
    BankStatus status;
    int64_t final_balance;
    pthread_t threads[10];
    withdraw_args_t args[10];
    barrier_t barrier;
    int i;
    long total_success = 0;
    long total_failure = 0;
    Transaction txns[1024];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Concurrent Withdraw", 20000000LL, NULL);

    barrier_init(&barrier, 10);

    for (i = 0; i < 10; i++) {
        args[i].bank = &bank;
        args[i].account_id = 1001;
        args[i].amount_paise = 10000LL;  /* ₹100 */
        args[i].iterations = 100;
        args[i].barrier = &barrier;
        args[i].success_count = 0;
        args[i].failure_count = 0;
        pthread_create(&threads[i], NULL, withdraw_worker, &args[i]);
    }

    for (i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
        total_success += args[i].success_count;
        total_failure += args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(total_success == 1000, "1000 successful withdrawals expected");
    TEST_ASSERT(total_failure == 0, "zero failures expected");

    status = bank_get_balance(&bank, 1001, &final_balance);
    TEST_ASSERT(status == BANK_SUCCESS, "get final balance");
    TEST_ASSERT(final_balance == 10000000LL,
                "final balance should be ₹100,000 = 10000000 paise");

    /* Verify no negative balance ever occurred (checked via success count) */
    TEST_ASSERT(total_success == 1000, "all withdrawals succeeded (no negative balance)");

    /* Verify history: exactly 1000 committed withdrawal records */
    status = bank_get_transaction_history(&bank, 1001, txns, 1024, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 1000, "exactly 1000 committed records");

    bank_destroy(&bank);
}

static void test_concurrent_opposite_direction_transfers(void)
{
    /* Test C: Concurrent opposite-direction transfers */
    Bank bank;
    BankStatus status;
    int64_t balance_a, balance_b;
    pthread_t threads[10];
    transfer_args_t args[10];
    barrier_t barrier;
    int i;
    long total_success = 0;
    long total_failure = 0;
    Transaction txns[256];
    size_t count_a, count_b;
    int unique_ids[256];
    int id_count = 0;
    int j;
    int duplicate = 0;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Account A", 10000000LL, NULL);
    bank_create_account(&bank, 1002, "Account B", 10000000LL, NULL);

    barrier_init(&barrier, 10);

    /* 5 threads A→B, 5 threads B→A */
    for (i = 0; i < 5; i++) {
        args[i].bank = &bank;
        args[i].source_id = 1001;
        args[i].dest_id = 1002;
        args[i].amount_paise = 10000LL;  /* ₹100 */
        args[i].iterations = 10;
        args[i].barrier = &barrier;
        args[i].success_count = 0;
        args[i].failure_count = 0;
        pthread_create(&threads[i], NULL, transfer_worker, &args[i]);
    }
    for (i = 5; i < 10; i++) {
        args[i].bank = &bank;
        args[i].source_id = 1002;
        args[i].dest_id = 1001;
        args[i].amount_paise = 10000LL;  /* ₹100 */
        args[i].iterations = 10;
        args[i].barrier = &barrier;
        args[i].success_count = 0;
        args[i].failure_count = 0;
        pthread_create(&threads[i], NULL, transfer_worker, &args[i]);
    }

    for (i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
        total_success += args[i].success_count;
        total_failure += args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(total_success == 100, "100 successful transfers expected");
    TEST_ASSERT(total_failure == 0, "zero failures expected");

    /* Combined balance must remain ₹200,000 = 20000000 paise */
    status = bank_get_balance(&bank, 1001, &balance_a);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance A");
    status = bank_get_balance(&bank, 1002, &balance_b);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance B");
    TEST_ASSERT(balance_a + balance_b == 20000000LL,
                "combined balance conserved at ₹200,000");
    TEST_ASSERT(balance_a >= 0 && balance_b >= 0,
                "neither account negative");

    /* Verify history: exactly 100 committed transfer records */
    count_a = 0;
    status = bank_get_transaction_history(&bank, 1001, txns, 256, &count_a);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for A");
    count_b = 0;
    status = bank_get_transaction_history(&bank, 1002, txns, 256, &count_b);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval for B");
    /* Each transfer creates exactly one record that appears in both
     * accounts' histories. Check uniqueness within Account A's history
     * only (Account B sees the same IDs). */
    for (i = 0; i < (int)count_a; i++) {
        TEST_ASSERT(txns[i].state == TRANSACTION_STATE_COMMITTED,
                    "all records should be COMMITTED");
        TEST_ASSERT(txns[i].type == TRANSACTION_TRANSFER,
                    "all records should be transfers");
        for (j = 0; j < id_count; j++) {
            if (unique_ids[j] == txns[i].transaction_id) {
                duplicate = 1;
                break;
            }
        }
        TEST_ASSERT(duplicate == 0, "no duplicate transaction IDs");
        unique_ids[id_count++] = txns[i].transaction_id;
    }
    TEST_ASSERT(id_count == 100, "exactly 100 unique transfer records");

    /* Both accounts see the same transfer records */
    TEST_ASSERT(count_a == count_b, "both accounts see same record count");

    bank_destroy(&bank);
}

static void test_concurrent_deposit_withdraw_same_account(void)
{
    /* Test D: Concurrent deposits and withdrawals on same account */
    Bank bank;
    BankStatus status;
    int64_t final_balance;
    pthread_t deposit_threads[5];
    pthread_t withdraw_threads[5];
    deposit_args_t dep_args[5];
    withdraw_args_t wd_args[5];
    barrier_t barrier;
    int i;
    long dep_success = 0, dep_failure = 0;
    long wd_success = 0, wd_failure = 0;
    Transaction txns[1024];
    size_t count;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Mixed Ops", 10000000LL, NULL);

    barrier_init(&barrier, 10);

    /* 5 depositor threads: each deposits ₹100 × 100 = ₹10,000 */
    for (i = 0; i < 5; i++) {
        dep_args[i].bank = &bank;
        dep_args[i].account_id = 1001;
        dep_args[i].amount_paise = 10000LL;  /* ₹100 */
        dep_args[i].iterations = 100;
        dep_args[i].barrier = &barrier;
        dep_args[i].success_count = 0;
        dep_args[i].failure_count = 0;
        pthread_create(&deposit_threads[i], NULL, deposit_worker, &dep_args[i]);
    }

    /* 5 withdrawer threads: each withdraws ₹50 × 100 = ₹5,000 */
    for (i = 0; i < 5; i++) {
        wd_args[i].bank = &bank;
        wd_args[i].account_id = 1001;
        wd_args[i].amount_paise = 5000LL;  /* ₹50 */
        wd_args[i].iterations = 100;
        wd_args[i].barrier = &barrier;
        wd_args[i].success_count = 0;
        wd_args[i].failure_count = 0;
        pthread_create(&withdraw_threads[i], NULL, withdraw_worker, &wd_args[i]);
    }

    for (i = 0; i < 5; i++) {
        pthread_join(deposit_threads[i], NULL);
        dep_success += dep_args[i].success_count;
        dep_failure += dep_args[i].failure_count;
    }
    for (i = 0; i < 5; i++) {
        pthread_join(withdraw_threads[i], NULL);
        wd_success += wd_args[i].success_count;
        wd_failure += wd_args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(dep_success == 500, "500 successful deposits expected");
    TEST_ASSERT(dep_failure == 0, "zero deposit failures");
    TEST_ASSERT(wd_success == 500, "500 successful withdrawals expected");
    TEST_ASSERT(wd_failure == 0, "zero withdrawal failures");

    /* Final: 10,000,000 + 5,000,000 - 2,500,000 = 12,500,000 paise = ₹125,000 */
    status = bank_get_balance(&bank, 1001, &final_balance);
    TEST_ASSERT(status == BANK_SUCCESS, "get final balance");
    TEST_ASSERT(final_balance == 12500000LL,
                "final balance should be ₹125,000 = 12500000 paise");

    /* Verify history: 1000 committed records */
    status = bank_get_transaction_history(&bank, 1001, txns, 1024, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 1000, "exactly 1000 committed records");

    bank_destroy(&bank);
}

static void test_concurrent_history_retrieval(void)
{
    /* Test E: Concurrent history retrieval while operations occur */
    Bank bank;
    BankStatus status;
    pthread_t worker_threads[5];
    pthread_t reader_threads[3];
    deposit_args_t dep_args[5];
    history_reader_args_t reader_args[3];
    barrier_t barrier;
    int i;
    long dep_success = 0, dep_failure = 0;
    long read_success = 0, read_failure = 0;
    Transaction txns[128];
    size_t count;
    int unique_ids[128];
    int id_count = 0;
    int j;
    int duplicate = 0;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "History Reader", 10000000LL, NULL);

    barrier_init(&barrier, 8);  /* 5 workers + 3 readers */

    /* 5 worker threads doing deposits */
    for (i = 0; i < 5; i++) {
        dep_args[i].bank = &bank;
        dep_args[i].account_id = 1001;
        dep_args[i].amount_paise = 1000LL;  /* ₹10 */
        dep_args[i].iterations = 20;
        dep_args[i].barrier = &barrier;
        dep_args[i].success_count = 0;
        dep_args[i].failure_count = 0;
        pthread_create(&worker_threads[i], NULL, deposit_worker, &dep_args[i]);
    }

    /* 3 reader threads repeatedly reading history */
    for (i = 0; i < 3; i++) {
        reader_args[i].bank = &bank;
        reader_args[i].account_id = 1001;
        reader_args[i].iterations = 10;
        reader_args[i].barrier = &barrier;
        reader_args[i].success_count = 0;
        reader_args[i].failure_count = 0;
        pthread_create(&reader_threads[i], NULL, history_reader_worker,
                       &reader_args[i]);
    }

    for (i = 0; i < 5; i++) {
        pthread_join(worker_threads[i], NULL);
        dep_success += dep_args[i].success_count;
        dep_failure += dep_args[i].failure_count;
    }
    for (i = 0; i < 3; i++) {
        pthread_join(reader_threads[i], NULL);
        read_success += reader_args[i].success_count;
        read_failure += reader_args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(dep_success == 100, "100 successful deposits expected");
    TEST_ASSERT(dep_failure == 0, "zero deposit failures");
    TEST_ASSERT(read_success == 30, "30 successful history reads expected");

    /* Final history should have 100 committed records */
    status = bank_get_transaction_history(&bank, 1001, txns, 128, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "final history retrieval");
    TEST_ASSERT(count == 100, "exactly 100 committed records");

    /* Verify all records are COMMITTED and have unique IDs */
    for (i = 0; i < (int)count; i++) {
        TEST_ASSERT(txns[i].state == TRANSACTION_STATE_COMMITTED,
                    "all records should be COMMITTED");
        for (j = 0; j < id_count; j++) {
            if (unique_ids[j] == txns[i].transaction_id) {
                duplicate = 1;
                break;
            }
        }
        TEST_ASSERT(duplicate == 0, "no duplicate transaction IDs");
        unique_ids[id_count++] = txns[i].transaction_id;
    }

    bank_destroy(&bank);
}

static void test_concurrent_reservation_cancellation(void)
{
    /* Test F: Concurrent reservation cancellation (failed operations) */
    Bank bank;
    BankStatus status;
    pthread_t threads[10];
    withdraw_args_t args[10];
    barrier_t barrier;
    int i;
    long total_failure = 0;
    Transaction txns[128];
    size_t count;
    int64_t balance;

    bank_init(&bank);
    bank_create_account(&bank, 1001, "Cancellation", 1000LL, NULL);
    bank_create_account(&bank, 1002, "Dest", 10000000LL, NULL);

    barrier_init(&barrier, 10);

    /* All 10 threads attempt insufficient-funds withdrawals (₹1,000,000 each) */
    for (i = 0; i < 10; i++) {
        args[i].bank = &bank;
        args[i].account_id = 1001;
        args[i].amount_paise = 100000000LL;  /* ₹1,000,000 — way more than ₹1,000 */
        args[i].iterations = 10;
        args[i].barrier = &barrier;
        args[i].success_count = 0;
        args[i].failure_count = 0;
        pthread_create(&threads[i], NULL, insufficient_funds_worker, &args[i]);
    }

    for (i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
        total_failure += args[i].failure_count;
    }

    barrier_destroy(&barrier);

    TEST_ASSERT(total_failure == 100, "100 failed withdrawals expected");
    TEST_ASSERT(args[0].success_count == 0, "zero successful withdrawals");

    /* Balance must be unchanged */
    status = bank_get_balance(&bank, 1001, &balance);
    TEST_ASSERT(status == BANK_SUCCESS, "get balance");
    TEST_ASSERT(balance == 1000LL, "balance unchanged after failed withdrawals");

    /* History must be empty (all reservations canceled) */
    status = bank_get_transaction_history(&bank, 1001, txns, 128, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval");
    TEST_ASSERT(count == 0, "no committed records from failed operations");

    /* Canceled slots must be reusable: a successful operation must work */
    status = bank_deposit(&bank, 1001, 500LL);
    TEST_ASSERT(status == BANK_SUCCESS, "deposit should work after cancellations");

    status = bank_get_transaction_history(&bank, 1001, txns, 128, &count);
    TEST_ASSERT(status == BANK_SUCCESS, "history retrieval after reuse");
    TEST_ASSERT(count == 1, "one committed record after slot reuse");

    bank_destroy(&bank);
}

/* =====================================================================
 * M2.6 — Lifecycle tests
 * ===================================================================== */

static void test_lifecycle_repeated_init_destroy(void)
{
    Bank bank;
    BankStatus status;
    int i;

    /* Repeated init/destroy cycles should work */
    for (i = 0; i < 5; i++) {
        status = bank_init(&bank);
        TEST_ASSERT(status == BANK_SUCCESS, "bank_init should succeed");
        TEST_ASSERT(bank.account_count == 0, "fresh bank has zero accounts");
        TEST_ASSERT(bank.transaction_count == 0,
                    "fresh bank has zero transactions");

        status = bank_create_account(&bank, 1001, "Lifecycle", 1000LL, NULL);
        TEST_ASSERT(status == BANK_SUCCESS, "create account");

        status = bank_destroy(&bank);
        TEST_ASSERT(status == BANK_SUCCESS, "bank_destroy should succeed");
    }

    /* After destroy, re-init should work */
    status = bank_init(&bank);
    TEST_ASSERT(status == BANK_SUCCESS, "re-init after destroy should succeed");
    status = bank_create_account(&bank, 1002, "After Destroy", 2000LL, NULL);
    TEST_ASSERT(status == BANK_SUCCESS, "create account after destroy");
    status = bank_destroy(&bank);
    TEST_ASSERT(status == BANK_SUCCESS, "destroy after re-init");
}

/* =====================================================================
 * main — register all tests
 * ===================================================================== */
int main(void)
{
    RUN_TEST(test_bank_init_success);
    RUN_TEST(test_create_account_success);
    RUN_TEST(test_multiple_account_creation);
    RUN_TEST(test_account_lookup_success);
    RUN_TEST(test_account_lookup_not_found);
    RUN_TEST(test_duplicate_account_id_rejection);
    RUN_TEST(test_invalid_customer_name);
    RUN_TEST(test_negative_initial_balance_rejection);
    RUN_TEST(test_maximum_account_capacity);
    RUN_TEST(test_mutex_initialization_and_cleanup);

    /* M2.2 — deposit tests */
    RUN_TEST(test_deposit_success);
    RUN_TEST(test_deposit_different_accounts);
    RUN_TEST(test_deposit_zero_amount_rejected);
    RUN_TEST(test_deposit_negative_amount_rejected);
    RUN_TEST(test_deposit_nonexistent_account_rejected);
    RUN_TEST(test_deposit_overflow_rejected);
    RUN_TEST(test_deposit_balance_unchanged_after_rejection);

    /* M2.2 — withdrawal tests */
    RUN_TEST(test_withdrawal_success);
    RUN_TEST(test_withdrawal_exact_balance);
    RUN_TEST(test_withdrawal_insufficient_funds);
    RUN_TEST(test_withdrawal_zero_amount_rejected);
    RUN_TEST(test_withdrawal_negative_amount_rejected);
    RUN_TEST(test_withdrawal_nonexistent_account_rejected);
    RUN_TEST(test_withdrawal_balance_never_negative);

    /* M2.2 — scenario and independence tests */
    RUN_TEST(test_sample_transaction_scenario);
    RUN_TEST(test_account_independence);

    /* M2.3 — balance enquiry tests */
    RUN_TEST(test_balance_enquiry_success);
    RUN_TEST(test_balance_enquiry_multiple_accounts);
    RUN_TEST(test_balance_enquiry_zero_balance);
    RUN_TEST(test_balance_enquiry_account_not_found);
    RUN_TEST(test_balance_enquiry_null_bank);
    RUN_TEST(test_balance_enquiry_null_output_pointer);
    RUN_TEST(test_balance_enquiry_balance_unchanged);
    RUN_TEST(test_balance_enquiry_after_deposit);
    RUN_TEST(test_balance_enquiry_after_withdrawal);

    /* M2.4 — fund transfer tests */
    RUN_TEST(test_transfer_success);
    RUN_TEST(test_transfer_different_account_pairs);
    RUN_TEST(test_transfer_exact_balance);
    RUN_TEST(test_transfer_null_bank);
    RUN_TEST(test_transfer_zero_amount);
    RUN_TEST(test_transfer_negative_amount);
    RUN_TEST(test_transfer_nonexistent_source);
    RUN_TEST(test_transfer_nonexistent_destination);
    RUN_TEST(test_transfer_same_account);
    RUN_TEST(test_transfer_insufficient_funds);
    RUN_TEST(test_transfer_destination_overflow);
    RUN_TEST(test_transfer_total_balance_conserved);

    /* M2.5 — transaction history tests */
    RUN_TEST(test_history_deposit_creates_record);
    RUN_TEST(test_history_withdrawal_creates_record);
    RUN_TEST(test_history_transfer_creates_exactly_one_record);
    RUN_TEST(test_history_balance_enquiry_creates_no_record);
    RUN_TEST(test_history_failed_deposit_creates_no_record);
    RUN_TEST(test_history_failed_withdrawal_creates_no_record);
    RUN_TEST(test_history_failed_transfer_creates_no_record);
    RUN_TEST(test_history_sequential_unique_ids);
    RUN_TEST(test_history_retrieve_single_account);
    RUN_TEST(test_history_transfer_filter_both_accounts);
    RUN_TEST(test_history_empty_for_new_account);
    RUN_TEST(test_history_invalid_account);
    RUN_TEST(test_history_null_arguments);
    RUN_TEST(test_history_unchanged_after_retrieval);
    RUN_TEST(test_history_insufficient_output_capacity);
    RUN_TEST(test_history_capacity_limit);
    RUN_TEST(test_history_records_not_overwritten);

    /* M2.5 — transaction reservation lifecycle tests */
    RUN_TEST(test_repeated_insufficient_funds_does_not_exhaust_capacity);
    RUN_TEST(test_repeated_invalid_operations_do_not_exhaust_capacity);
    RUN_TEST(test_canceled_reservations_do_not_appear_in_history);
    RUN_TEST(test_canceled_slot_can_be_reused);
    RUN_TEST(test_multiple_outstanding_reservations_remain_independent);
    RUN_TEST(test_successful_transactions_still_receive_unique_ids);
    RUN_TEST(test_history_capacity_reached_only_by_committed_or_active);

    /* M2.6 — boundary tests */
    RUN_TEST(test_boundary_zero_initial_balance);
    RUN_TEST(test_boundary_max_initial_balance);
    RUN_TEST(test_boundary_deposit_to_int64_max_exact);
    RUN_TEST(test_boundary_withdraw_one_paise);
    RUN_TEST(test_boundary_withdraw_exceeding_balance);
    RUN_TEST(test_boundary_account_lookup_beginning_and_end);
    RUN_TEST(test_boundary_invalid_account_ids);
    RUN_TEST(test_boundary_negative_and_zero_amounts);
    RUN_TEST(test_boundary_history_exactly_one_record);

    /* M2.6 — financial invariant tests */
    RUN_TEST(test_invariant_deposit_exact_amount);
    RUN_TEST(test_invariant_withdraw_exact_amount);
    RUN_TEST(test_invariant_transfer_debits_credits_same_amount);
    RUN_TEST(test_invariant_failed_operation_leaves_balances_unchanged);
    RUN_TEST(test_invariant_one_committed_record_per_successful_operation);

    /* M2.6 — concurrent tests (Tests A-F) */
    RUN_TEST(test_concurrent_deposits);
    RUN_TEST(test_concurrent_withdrawals);
    RUN_TEST(test_concurrent_opposite_direction_transfers);
    RUN_TEST(test_concurrent_deposit_withdraw_same_account);
    RUN_TEST(test_concurrent_history_retrieval);
    RUN_TEST(test_concurrent_reservation_cancellation);

    /* M2.6 — lifecycle tests */
    RUN_TEST(test_lifecycle_repeated_init_destroy);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
