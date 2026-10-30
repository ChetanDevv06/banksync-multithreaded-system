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
    bank->transaction_count = 0;
    bank->next_transaction_id = 1;

    if (pthread_mutex_init(&bank->history_mutex, NULL) != 0) {
        return BANK_ERR_MUTEX_INIT_FAILED;
    }
    bank->history_mutex_initialized = 1;

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

    if (bank->history_mutex_initialized) {
        pthread_mutex_destroy(&bank->history_mutex);
        bank->history_mutex_initialized = 0;
    }

    memset(bank, 0, sizeof(*bank));
    bank->account_count = 0;
    bank->transaction_count = 0;
    bank->next_transaction_id = 1;

    return BANK_SUCCESS;
}

/*
 * reserve_transaction — reserve a history slot before modifying account
 * balances.
 *
 * Acquires the history mutex, scans for the first FREE slot, marks it
 * RESERVED, assigns the next unique transaction ID, and releases the mutex.
 *
 * Reservation is thread-safe: the history mutex guarantees that no two
 * threads receive the same transaction ID or slot.
 *
 * A reservation must be terminated by exactly one call to
 * commit_transaction() (success) or cancel_transaction_reservation() (failure).
 *
 * If no FREE slot is available, returns BANK_ERROR_HISTORY_FULL without
 * modifying any slot state.
 *
 * Returns BANK_SUCCESS on success with *out_transaction_id and
 * *out_slot_index populated, or an appropriate BankStatus error code.
 */
static BankStatus reserve_transaction(Bank *bank,
                                      int *out_transaction_id,
                                      int *out_slot_index)
{
    int i;

    if (pthread_mutex_lock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    /*
     * Scan for the first FREE slot. Canceled slots are reusable, so we do
     * not simply append at the end of the array.
     */
    for (i = 0; i < BANK_MAX_TRANSACTIONS; i++) {
        if (bank->history[i].state == TRANSACTION_STATE_FREE) {
            break;
        }
    }

    if (i >= BANK_MAX_TRANSACTIONS) {
        pthread_mutex_unlock(&bank->history_mutex);
        return BANK_ERROR_HISTORY_FULL;
    }

    bank->history[i].state = TRANSACTION_STATE_RESERVED;
    bank->history[i].transaction_id = bank->next_transaction_id++;

    if (out_transaction_id != NULL) {
        *out_transaction_id = bank->history[i].transaction_id;
    }
    if (out_slot_index != NULL) {
        *out_slot_index = i;
    }

    if (pthread_mutex_unlock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}

/*
 * cancel_transaction_reservation — release a previously reserved slot.
 *
 * Acquires the history mutex and marks the slot FREE again if it is in the
 * RESERVED state. Slots in the COMMITTED state are left untouched. This
 * ensures that canceled reservations do not invalidate records held by other
 * concurrent operations.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
static BankStatus cancel_transaction_reservation(Bank *bank, int slot_index)
{
    if (pthread_mutex_lock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (bank->history[slot_index].state == TRANSACTION_STATE_RESERVED) {
        bank->history[slot_index].state = TRANSACTION_STATE_FREE;
        bank->history[slot_index].transaction_id = 0;
    }

    if (pthread_mutex_unlock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}

/*
 * commit_transaction — commit a previously reserved transaction record.
 *
 * Acquires the history mutex, fills in the transaction details at the
 * reserved slot, marks it COMMITTED, increments the committed transaction
 * count, and releases the mutex.
 *
 * Only COMMITTED records are visible to readers via
 * bank_get_transaction_history().
 *
 * If commit fails after the account balance has already changed, the
 * caller is responsible for reversing the balance change before returning
 * the error to the caller.
 *
 * Returns BANK_SUCCESS on success, or an appropriate BankStatus error code.
 */
static BankStatus commit_transaction(Bank *bank, int slot_index,
                                     TransactionType type,
                                     int source_account_id,
                                     int destination_account_id,
                                     int64_t amount_paise)
{
    if (pthread_mutex_lock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    Transaction *txn = &bank->history[slot_index];
    txn->type = type;
    txn->source_account_id = source_account_id;
    txn->destination_account_id = destination_account_id;
    txn->amount_paise = amount_paise;
    txn->timestamp = time(NULL);
    txn->state = TRANSACTION_STATE_COMMITTED;
    bank->transaction_count++;

    if (pthread_mutex_unlock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

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
    int txn_id = 0;
    int slot_index = 0;
    BankStatus status;

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

    /*
     * Reserve a history slot before modifying the balance. If the history
     * is full, abort without touching any account balance.
     */
    status = reserve_transaction(bank, &txn_id, &slot_index);
    if (status != BANK_SUCCESS) {
        return status;
    }

    if (pthread_mutex_lock(&account->mutex) != 0) {
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (amount_paise > INT64_MAX - account->balance_paise) {
        pthread_mutex_unlock(&account->mutex);
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_BALANCE_OVERFLOW;
    }

    account->balance_paise += amount_paise;

    if (pthread_mutex_unlock(&account->mutex) != 0) {
        /*
         * Unlock failed: balance was changed but no history record was
         * committed. Reverse the balance change and cancel the reservation
         * to prevent a capacity leak and keep balances consistent.
         */
        account->balance_paise -= amount_paise;
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    /*
     * Commit the reserved transaction record. Account mutex is released
     * before acquiring the history mutex to prevent lock-order inversion.
     */
    status = commit_transaction(bank, slot_index, TRANSACTION_DEPOSIT,
                                account_id, TRANSACTION_NONE_ACCOUNT_ID,
                                amount_paise);
    if (status != BANK_SUCCESS) {
        /*
         * Balance changed but history commit failed. Reverse the balance
         * change and cancel the reservation so the operation is not
         * silently reported as success without a record.
         */
        if (pthread_mutex_lock(&account->mutex) == 0) {
            account->balance_paise -= amount_paise;
            pthread_mutex_unlock(&account->mutex);
        }
        cancel_transaction_reservation(bank, slot_index);
        return status;
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
    int txn_id = 0;
    int slot_index = 0;
    BankStatus status;

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

    /*
     * Reserve a history slot before modifying the balance. If the history
     * is full, abort without touching any account balance.
     */
    status = reserve_transaction(bank, &txn_id, &slot_index);
    if (status != BANK_SUCCESS) {
        return status;
    }

    if (pthread_mutex_lock(&account->mutex) != 0) {
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (account->balance_paise < amount_paise) {
        pthread_mutex_unlock(&account->mutex);
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_INSUFFICIENT_FUNDS;
    }

    account->balance_paise -= amount_paise;

    if (pthread_mutex_unlock(&account->mutex) != 0) {
        /*
         * Unlock failed: balance was changed but no history record was
         * committed. Reverse the balance change and cancel the reservation
         * to prevent a capacity leak and keep balances consistent.
         */
        account->balance_paise += amount_paise;
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    /*
     * Commit the reserved transaction record. Account mutex is released
     * before acquiring the history mutex to prevent lock-order inversion.
     */
    status = commit_transaction(bank, slot_index, TRANSACTION_WITHDRAWAL,
                                account_id, TRANSACTION_NONE_ACCOUNT_ID,
                                amount_paise);
    if (status != BANK_SUCCESS) {
        /*
         * Balance changed but history commit failed. Reverse the balance
         * change and cancel the reservation so the operation is not
         * silently reported as success without a record.
         */
        if (pthread_mutex_lock(&account->mutex) == 0) {
            account->balance_paise += amount_paise;
            pthread_mutex_unlock(&account->mutex);
        }
        cancel_transaction_reservation(bank, slot_index);
        return status;
    }

    return BANK_SUCCESS;
}

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
 */
BankStatus bank_get_balance(Bank *bank,
                            int account_id,
                            int64_t *balance_out)
{
    BankAccount *account;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (balance_out == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    account = bank_find_account(bank, account_id);
    if (account == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    if (pthread_mutex_lock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    *balance_out = account->balance_paise;

    if (pthread_mutex_unlock(&account->mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}

/*
 * bank_transfer — transfer funds from one account to another.
 *
 * Both accounts are locked using consistent global lock ordering (lower
 * account ID first, higher account ID second) to prevent deadlock between
 * concurrent opposite-direction transfers. The debit and credit are
 * performed atomically while both mutexes are held.
 */
BankStatus bank_transfer(Bank *bank,
                         int source_account_id,
                         int destination_account_id,
                         int64_t amount_paise)
{
    BankAccount *source;
    BankAccount *dest;
    BankAccount *first;
    BankAccount *second;
    int txn_id = 0;
    int slot_index = 0;
    BankStatus status;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (amount_paise <= 0) {
        return BANK_ERROR_INVALID_AMOUNT;
    }

    source = bank_find_account(bank, source_account_id);
    if (source == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    dest = bank_find_account(bank, destination_account_id);
    if (dest == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    if (source == dest) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    /*
     * Reserve a history slot before modifying any balance. If the history
     * is full, abort without touching any account balance.
     */
    status = reserve_transaction(bank, &txn_id, &slot_index);
    if (status != BANK_SUCCESS) {
        return status;
    }

    /*
     * Consistent lock ordering: lock the lower account ID first, then the
     * higher account ID. This ordering is independent of transfer direction
     * and prevents circular wait / deadlock.
     */
    if (source->account_id < dest->account_id) {
        first = source;
        second = dest;
    } else {
        first = dest;
        second = source;
    }

    if (pthread_mutex_lock(&first->mutex) != 0) {
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_LOCK;
    }

    if (pthread_mutex_lock(&second->mutex) != 0) {
        pthread_mutex_unlock(&first->mutex);
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_LOCK;
    }

    /*
     * Both mutexes are held. Validate and perform the transfer atomically.
     * Neither balance is modified unless all checks pass.
     */
    if (source->balance_paise < amount_paise) {
        pthread_mutex_unlock(&second->mutex);
        pthread_mutex_unlock(&first->mutex);
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_INSUFFICIENT_FUNDS;
    }

    if (amount_paise > INT64_MAX - dest->balance_paise) {
        pthread_mutex_unlock(&second->mutex);
        pthread_mutex_unlock(&first->mutex);
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_BALANCE_OVERFLOW;
    }

    source->balance_paise -= amount_paise;
    dest->balance_paise += amount_paise;

    if (pthread_mutex_unlock(&second->mutex) != 0) {
        pthread_mutex_unlock(&first->mutex);
        /*
         * Unlock failed: balances were changed but no history record was
         * committed. Reverse the balance changes and cancel the reservation
         * to prevent a capacity leak and keep balances consistent.
         */
        source->balance_paise += amount_paise;
        dest->balance_paise -= amount_paise;
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    if (pthread_mutex_unlock(&first->mutex) != 0) {
        /*
         * Unlock failed: balances were changed but no history record was
         * committed. Reverse the balance changes and cancel the reservation
         * to prevent a capacity leak and keep balances consistent.
         */
        source->balance_paise += amount_paise;
        dest->balance_paise -= amount_paise;
        cancel_transaction_reservation(bank, slot_index);
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    /*
     * Commit the reserved transaction record. Both account mutexes are
     * released before acquiring the history mutex to prevent lock-order
     * inversion.
     */
    status = commit_transaction(bank, slot_index, TRANSACTION_TRANSFER,
                                source_account_id, destination_account_id,
                                amount_paise);
    if (status != BANK_SUCCESS) {
        /*
         * Balances changed but history commit failed. Reverse both balance
         * changes and cancel the reservation so the operation is not
         * silently reported as success without a record.
         */
        if (source->account_id < dest->account_id) {
            first = source;
            second = dest;
        } else {
            first = dest;
            second = source;
        }

        if (pthread_mutex_lock(&first->mutex) == 0) {
            if (pthread_mutex_lock(&second->mutex) == 0) {
                source->balance_paise += amount_paise;
                dest->balance_paise -= amount_paise;
                pthread_mutex_unlock(&second->mutex);
            }
            pthread_mutex_unlock(&first->mutex);
        }
        cancel_transaction_reservation(bank, slot_index);
        return status;
    }

    return BANK_SUCCESS;
}

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
    size_t *count_out)
{
    size_t match_count = 0;
    size_t copied = 0;
    int i;

    if (bank == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }
    if (transactions_out == NULL || count_out == NULL) {
        return BANK_ERR_INVALID_ARGUMENT;
    }

    if (bank_find_account(bank, account_id) == NULL) {
        return BANK_ERR_ACCOUNT_NOT_FOUND;
    }

    if (pthread_mutex_lock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_LOCK;
    }

    /*
     * First pass: count matching committed records. Scan the entire array
     * because canceled slots may leave committed records at non-contiguous
     * indices.
     */
    for (i = 0; i < BANK_MAX_TRANSACTIONS; i++) {
        if (bank->history[i].state == TRANSACTION_STATE_COMMITTED &&
            (bank->history[i].source_account_id == account_id ||
             bank->history[i].destination_account_id == account_id)) {
            match_count++;
        }
    }

    if (capacity < match_count) {
        *count_out = match_count;
        pthread_mutex_unlock(&bank->history_mutex);
        return BANK_ERROR_BUFFER_TOO_SMALL;
    }

    /*
     * Second pass: copy matching committed records into caller's array.
     */
    for (i = 0; i < BANK_MAX_TRANSACTIONS; i++) {
        if (bank->history[i].state == TRANSACTION_STATE_COMMITTED &&
            (bank->history[i].source_account_id == account_id ||
             bank->history[i].destination_account_id == account_id)) {
            transactions_out[copied++] = bank->history[i];
        }
    }

    *count_out = copied;

    if (pthread_mutex_unlock(&bank->history_mutex) != 0) {
        return BANK_ERROR_MUTEX_UNLOCK;
    }

    return BANK_SUCCESS;
}
