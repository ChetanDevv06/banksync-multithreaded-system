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

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
