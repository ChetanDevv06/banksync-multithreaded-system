#include "bank.h"

#include <string.h>
#include <stdio.h>

/*
 * generate_account_number — render a "BS"-prefixed account number into a
 * fixed-width buffer. Format: BS<account_id>, e.g. account 1001 → "BS1001".
 * Returns 0 on success, non-zero if the output would be truncated.
 */
static int generate_account_number(int account_id, char *out, size_t out_size)
{
    int written = snprintf(out, out_size, "%s%d", BANK_ACCOUNT_NUMBER_PREFIX,
                           account_id);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return 0;
}

/*
 * valid_customer_name — check that a customer name is non-NULL, non-empty and
 * fits within the fixed customer_name buffer (leaving room for NUL).
 */
static int valid_customer_name(const char *name)
{
    size_t len;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len == 0 || len >= sizeof(((BankAccount *)0)->customer_name)) {
        return 0;
    }
    return 1;
}

/*
 * find_account_index_by_id — linear scan of the live registry for an ID.
 * Returns the index, or -1 when no account with the given ID exists.
 * Only the first `account_count` entries are considered live.
 */
static int find_account_index_by_id(const Bank *bank, int account_id)
{
    int i;

    for (i = 0; i < bank->account_count; i++) {
        if (bank->accounts[i].account_id == account_id) {
            return i;
        }
    }
    return -1;
}

BankStatus bank_init(Bank *bank)
{
    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    memset(bank, 0, sizeof(*bank));
    bank->account_count = 0;

    return BANK_SUCCESS;
}

BankStatus bank_create_account(Bank *bank,
                               int account_id,
                               const char *customer_name,
                               int64_t initial_balance_paise,
                               BankAccount **out_account)
{
    BankAccount *account = NULL;
    int index;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (!valid_customer_name(customer_name)) {
        return BANK_ERR_INVALID_NAME;
    }
    if (initial_balance_paise < 0) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    index = find_account_index_by_id(bank, account_id);
    if (index != -1) {
        return BANK_ERR_DUPLICATE_ACCOUNT;
    }

    if (bank->account_count >= BANK_MAX_ACCOUNTS) {
        return BANK_ERR_MAX_CAPACITY;
    }

    index = bank->account_count;
    account = &bank->accounts[index];

    account->account_id = account_id;
    strncpy(account->customer_name, customer_name,
            sizeof(account->customer_name) - 1);
    account->customer_name[sizeof(account->customer_name) - 1] = '\0';

    if (generate_account_number(account_id, account->account_number,
                                sizeof(account->account_number)) != 0) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    account->balance_paise = initial_balance_paise;
    account->mutex_initialized = 0;

    if (pthread_mutex_init(&account->mutex, NULL) != 0) {
        memset(account, 0, sizeof(*account));
        return BANK_ERR_MUTEX_INIT_FAILED;
    }
    account->mutex_initialized = 1;

    bank->account_count++;

    if (out_account != NULL) {
        *out_account = account;
    }

    return BANK_SUCCESS;
}

BankAccount *bank_find_account(Bank *bank, int account_id)
{
    int index;

    if (bank == NULL) {
        return NULL;
    }

    index = find_account_index_by_id(bank, account_id);
    if (index == -1) {
        return NULL;
    }

    return &bank->accounts[index];
}

BankStatus bank_destroy(Bank *bank)
{
    int i;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < bank->account_count; i++) {
        if (bank->accounts[i].mutex_initialized) {
            pthread_mutex_destroy(&bank->accounts[i].mutex);
            bank->accounts[i].mutex_initialized = 0;
        }
    }

    memset(bank, 0, sizeof(*bank));
    bank->account_count = 0;

    return BANK_SUCCESS;
}

/*
 * bank_deposit — deposit funds into an account.
 *
 * Sequence:
 *   1. Validate bank pointer and amount (> 0).
 *   2. Find the account by ID.
 *   3. Acquire the account-level pthread mutex.
 *   4. Validate int64_t balance overflow (amount <= INT64_MAX - balance).
 *   5. Increase the balance inside the critical section.
 *   6. Release the account-level mutex.
 *
 * The balance is never modified outside the protected critical section, and
 * is left untouched when any validation fails.
 */
BankStatus bank_deposit(Bank *bank,
                        int account_id,
                        int64_t amount_paise)
{
    BankAccount *account;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (amount_paise <= 0) {
        return BANK_ERROR_INVALID_AMOUNT;
    }

    account = bank_find_account(bank, account_id);
    if (account == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    if (pthread_mutex_lock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (amount_paise > INT64_MAX - account->balance_paise) {
        pthread_mutex_unlock(&account->mutex);
        return BANK_ERROR_BALANCE_OVERFLOW;
    }

    account->balance_paise += amount_paise;

    if (pthread_mutex_unlock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}

/*
 * bank_withdraw — withdraw funds from an account.
 *
 * Sequence:
 *   1. Validate bank pointer and amount (> 0).
 *   2. Find the account by ID.
 *   3. Acquire the account-level pthread mutex.
 *   4. Validate available balance (balance >= amount).
 *   5. Decrease the balance inside the critical section.
 *   6. Release the account-level mutex.
 *
 * The balance is never modified outside the protected critical section, is
 * never allowed to become negative, and is left untouched when any
 * validation fails.
 */
BankStatus bank_withdraw(Bank *bank,
                         int account_id,
                         int64_t amount_paise)
{
    BankAccount *account;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (amount_paise <= 0) {
        return BANK_ERROR_INVALID_AMOUNT;
    }

    account = bank_find_account(bank, account_id);
    if (account == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    if (pthread_mutex_lock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (account->balance_paise < amount_paise) {
        pthread_mutex_unlock(&account->mutex);
        return BANK_ERROR_INSUFFICIENT_FUNDS;
    }

    account->balance_paise -= amount_paise;

    if (pthread_mutex_unlock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}
