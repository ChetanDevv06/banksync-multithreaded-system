#include "bank.h"

#include <stdio.h>

static void print_account(const BankAccount *acc)
{
    printf("Account ID : %d\n", acc->account_id);
    printf("Customer   : %s\n", acc->customer_name);
    printf("Number     : %s\n", acc->account_number);
    printf("Balance    : %lld paise\n", (long long)acc->balance_paise);
    printf("Mutex      : %s\n",
           acc->mutex_initialized ? "initialized" : "not initialized");
    printf("-----------------------------\n");
}

int main(void)
{
    Bank bank;
    BankStatus status;

    status = bank_init(&bank);
    if (status != BANK_SUCCESS) {
        fprintf(stderr, "bank_init failed: %d\n", (int)status);
        return 1;
    }

    status = bank_create_account(&bank, 1001, "Rahul Sharma", 2500000LL, NULL);
    status = bank_create_account(&bank, 1002, "Priya Sharma", 5000000LL, NULL);
    status = bank_create_account(&bank, 1003, "Arjun Kumar", 7500000LL, NULL);

    BankAccount *acc = bank_find_account(&bank, 1002);
    if (acc != NULL) {
        print_account(acc);
    }

    status = bank_destroy(&bank);
    if (status != BANK_SUCCESS) {
        fprintf(stderr, "bank_destroy failed: %d\n", (int)status);
        return 1;
    }

    return 0;
}
