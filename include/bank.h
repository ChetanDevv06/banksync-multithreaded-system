#ifndef BANK_H
#define BANK_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

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

#define BANK_MAX_TRANSACTIONS 1000

/*
 * Sentinel value used for the unused account ID field in deposit and
 * withdrawal records. Real account IDs are always positive.
 */
#define TRANSACTION_NONE_ACCOUNT_ID -1

/*
 * TransactionType — kind of financial transaction recorded in history.
 */
typedef enum TransactionType {
    TRANSACTION_DEPOSIT = 0,
    TRANSACTION_WITHDRAWAL = 1,
    TRANSACTION_TRANSFER = 2
} TransactionType;

/*
 * TransactionState — lifecycle state of a history slot.
 *
 * FREE: slot is available for reservation.
 * RESERVED: slot has been reserved but not yet committed; invisible to
 *           readers and does not count toward committed transaction totals.
 * COMMITTED: slot holds a fully written transaction record visible to readers.
 */
typedef enum TransactionState {
    TRANSACTION_STATE_FREE = 0,
    TRANSACTION_STATE_RESERVED = 1,
    TRANSACTION_STATE_COMMITTED = 2
} TransactionState;

/*
 * Transaction — an immutable record of a successful financial transaction.
 *
 * For deposits: source_account_id is the receiving account;
 *   destination_account_id is TRANSACTION_NONE_ACCOUNT_ID.
 *
 * For withdrawals: source_account_id is the debited account;
 *   destination_account_id is TRANSACTION_NONE_ACCOUNT_ID.
 *
 * For transfers: source and destination account IDs are the actual accounts.
 *
 * state tracks the lifecycle of the slot. Only COMMITTED records are
 * visible to readers via bank_get_transaction_history().
 */
typedef struct Transaction {
    int transaction_id;
    TransactionType type;
    int source_account_id;
    int destination_account_id;
    int64_t amount_paise;
    time_t timestamp;
    TransactionState state;
} Transaction;

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
    BANK_ERROR_MUTEX_UNLOCK = -11,
    /* M2.5 — transaction history errors */
    BANK_ERROR_HISTORY_FULL = -12,
    BANK_ERROR_BUFFER_TOO_SMALL = -13
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
 * Bank — the registry of all accounts plus transaction history.
 *
 * history_mutex protects the transaction history array, transaction_count,
 * and next_transaction_id. It is a dedicated lock separate from the
 * per-account mutexes to avoid deadlock: account mutexes are always released
 * before the history mutex is acquired.
 *
 * transaction_count is the number of COMMITTED records. RESERVED (pending)
 * slots do not count toward this total and do not consume capacity.
 */
typedef struct Bank {
    BankAccount accounts[BANK_MAX_ACCOUNTS];
    int account_count;

    Transaction history[BANK_MAX_TRANSACTIONS];
    int transaction_count;
    int next_transaction_id;
    pthread_mutex_t history_mutex;
    int history_mutex_initialized;
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

/*
 * bank_get_balance — enquire the current balance of an account.
 *
 * Sequence:
 *   1. Validate bank and balance_out pointers.
 *   2. Find the account by ID.
 *   3. Acquire the account-level pthread mutex.
 *   4. Copy balance_paise into *balance_out inside the critical section.
 *   5. Release the account-level mutex.
 *
 * The balance is only read while holding the account mutex, providing a
 * consistent snapshot. The account balance is never modified by this
 * operation.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
BankStatus bank_get_balance(Bank *bank,
                            int account_id,
                            int64_t *balance_out);

/*
 * bank_transfer — transfer funds from one account to another.
 *
 * Both accounts are locked using consistent global lock ordering (lower
 * account ID first, higher account ID second) to prevent deadlock between
 * concurrent opposite-direction transfers. The debit and credit are
 * performed atomically while both mutexes are held.
 *
 * Validation:
 *   - Bank pointer must not be NULL.
 *   - Amount must be strictly greater than zero.
 *   - Source and destination accounts must exist.
 *   - Source and destination must be different accounts.
 *   - Source must have sufficient funds.
 *   - Destination must not overflow INT64_MAX.
 *
 * Either both balances change, or neither changes. Failed validation leaves
 * both balances unchanged.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
BankStatus bank_transfer(Bank *bank,
                         int source_account_id,
                         int destination_account_id,
                         int64_t amount_paise);

/*
 * bank_get_transaction_history — retrieve committed transaction records
 * for a given account.
 *
 * Validates the bank pointer, output pointers, and account existence.
 * Acquires the history mutex, copies matching committed records into the
 * caller-provided output array, and releases the mutex.
 *
 * Matching rules:
 *   - Deposit: account appears as source_account_id (receiving account).
 *   - Withdrawal: account appears as source_account_id (debited account).
 *   - Transfer: account appears as either source or destination.
 *
 * If capacity is insufficient for all matching records, returns
 * BANK_ERROR_BUFFER_TOO_SMALL and sets *count_out to the required count
 * without copying any records.
 *
 * Internal history-array pointers are never exposed; stored records are
 * never modified during retrieval.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
BankStatus bank_get_transaction_history(
    Bank *bank,
    int account_id,
    Transaction *transactions_out,
    size_t capacity,
    size_t *count_out
);

#endif /* BANK_H */
