#ifndef BANK_H
#define BANK_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/*
 * BankSync M2.1 — Account Management
 *
 * Lifecycle assumption (documented):
 *   Account creation and registry modification happen sequentially,
 *   BEFORE any worker threads are started. Registry synchronization for
 *   concurrent account creation is intentionally deferred to a later
 *   milestone and is NOT part of M2.1.
 *
 * Pointer lifetime contract:
 *   bank_find_account() returns a pointer into the Bank's internal account
 *   array. The pointer remains valid only while the Bank object stays alive
 *   and its account registry is not modified (no create/destroy that could
 *   invalidate the array). Callers must not store the pointer across such
 *   mutations.
 */

#define BANK_MAX_ACCOUNTS 100

#define BANK_ACCOUNT_NUMBER_PREFIX "BS"

/*
 * BankStatus — outcome of every public bank operation.
 *
 * M2.1 values (BANK_SUCCESS through BANK_ERR_INVALID_NAME) are preserved
 * unchanged. M2.2 appends transaction-specific error codes.
 */
typedef enum BankStatus {
    BANK_SUCCESS = 0,
    BANK_ERR_INVALID_ARGUMENT = -1,
    BANK_ERR_ACCOUNT_NOT_FOUND = -2,
    BANK_ERR_DUPLICATE_ACCOUNT = -3,
    BANK_ERR_MAX_CAPACITY = -4,
    BANK_ERR_MUTEX_INIT_FAILED = -5,
    BANK_ERR_INVALID_NAME = -6,
    /* M2.2 — deposit / withdrawal errors */
    BANK_ERROR_INVALID_AMOUNT = -7,
    BANK_ERROR_INSUFFICIENT_FUNDS = -8,
    BANK_ERROR_BALANCE_OVERFLOW = -9,
    BANK_ERROR_MUTEX_LOCK = -10,
    BANK_ERROR_MUTEX_UNLOCK = -11
} BankStatus;

/*
 * BankAccount — a single customer account.
 *
 * balance_paise stores the monetary value in integer paise to avoid
 * floating-point rounding errors.
 *
 * mutex_initialized tracks whether the pthread mutex was successfully
 * initialized so that bank_destroy() destroys only initialized mutexes.
 */
typedef struct BankAccount {
    int account_id;
    char customer_name[64];
    char account_number[20];
    int64_t balance_paise;
    pthread_mutex_t mutex;
    int mutex_initialized;
} BankAccount;

/*
 * Bank — the registry of all accounts.
 */
typedef struct Bank {
    BankAccount accounts[BANK_MAX_ACCOUNTS];
    int account_count;
} Bank;

/*
 * bank_init — zero-initialize a Bank instance.
 *
 * Returns BANK_SUCCESS on success, BANK_ERR_INVALID_ARGUMENT if bank is NULL.
 */
BankStatus bank_init(Bank *bank);

/*
 * bank_create_account — create a new account sequentially.
 *
 * Validates arguments, rejects duplicate account IDs and invalid customer
 * names, enforces maximum capacity, assigns a unique account ID, generates
 * an account number with the "BS" prefix, stores the initial balance in
 * paise, and initializes the account-level pthread mutex.
 *
 * On success stores a pointer to the new account in out_account (if not NULL)
 * and returns BANK_SUCCESS.
 */
BankStatus bank_create_account(Bank *bank,
                               int account_id,
                               const char *customer_name,
                               int64_t initial_balance_paise,
                               BankAccount **out_account);

/*
 * bank_find_account — look up an account by its ID.
 *
 * Returns a pointer to the stored account, or NULL when not found.
 * See the pointer lifetime contract at the top of this header.
 */
BankAccount *bank_find_account(Bank *bank, int account_id);

/*
 * bank_destroy — destroy every successfully initialized account mutex and
 * reset the registry. Safe to call on an already-initialized Bank.
 *
 * Returns BANK_SUCCESS on success, BANK_ERR_INVALID_ARGUMENT if bank is NULL.
 */
BankStatus bank_destroy(Bank *bank);

/*
 * bank_deposit — deposit funds into an account.
 *
 * Transaction sequence:
 *   1. Validate bank pointer and amount (> 0).
 *   2. Find the account by ID.
 *   3. Acquire the account-level pthread mutex.
 *   4. Validate int64_t balance overflow (amount <= INT64_MAX - balance).
 *   5. Increase the balance inside the critical section.
 *   6. Release the account-level mutex.
 *
 * The account balance is never modified outside the protected critical
 * section, and is left untouched when any validation fails.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
BankStatus bank_deposit(Bank *bank,
                        int account_id,
                        int64_t amount_paise);

/*
 * bank_withdraw — withdraw funds from an account.
 *
 * Transaction sequence:
 *   1. Validate bank pointer and amount (> 0).
 *   2. Find the account by ID.
 *   3. Acquire the account-level pthread mutex.
 *   4. Validate available balance (balance >= amount).
 *   5. Decrease the balance inside the critical section.
 *   6. Release the account-level mutex.
 *
 * The account balance is never modified outside the protected critical
 * section, is never allowed to become negative, and is left untouched when
 * any validation fails.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
BankStatus bank_withdraw(Bank *bank,
                         int account_id,
                         int64_t amount_paise);

#endif /* BANK_H */
