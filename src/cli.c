#include "cli.h"
#include "money_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * CLI input buffer size. Large enough for normal input; lines longer than
 * this are truncated and the remainder discarded.
 */
#define CLI_INPUT_BUFFER_SIZE 256

/*
 * Main menu option constants.
 */
#define CLI_OPTION_ATM 1
#define CLI_OPTION_ONLINE_BANKING 2
#define CLI_OPTION_EXIT 3

/*
 * Dashboard option constants.
 */
#define CLI_DASHBOARD_END_SESSION 1
#define CLI_DASHBOARD_RETURN_TO_MAIN 2

/*
 * ATM operation menu option constants.
 */
#define CLI_ATM_OPTION_BALANCE_ENQUIRY 1
#define CLI_ATM_OPTION_WITHDRAWAL 2
#define CLI_ATM_OPTION_DEPOSIT 3
#define CLI_ATM_OPTION_TRANSFER 4
#define CLI_ATM_OPTION_END_SESSION 5

/*
 * Online Banking operation menu option constants.
 */
#define CLI_ONLINE_BANKING_OPTION_BALANCE_ENQUIRY 1
#define CLI_ONLINE_BANKING_OPTION_DEPOSIT 2
#define CLI_ONLINE_BANKING_OPTION_WITHDRAWAL 3
#define CLI_ONLINE_BANKING_OPTION_TRANSFER 4
#define CLI_ONLINE_BANKING_OPTION_LOGOUT 5

/*
 * CLI state machine states.
 */
typedef enum CliState {
    CLI_STATE_MAIN_MENU = 0,
    CLI_STATE_ACCOUNT_SELECTION = 1,
    CLI_STATE_SESSION_DASHBOARD = 2,
    CLI_STATE_ATM_OPERATIONS = 3,
    CLI_STATE_ONLINE_BANKING_OPERATIONS = 4,
    CLI_STATE_EXIT = 5
} CliState;

/*
 * Active session context. Tracks the session created during the current
 * customer interaction.
 */
typedef struct CliSessionContext {
    int has_session;
    uint64_t session_id;
    int account_id;
    SessionChannel channel;
    char customer_name[64];
    char account_number[20];
} CliSessionContext;

/* ------------------------------------------------------------------ */
/* Production IO sinks                                                */
/* ------------------------------------------------------------------ */

static int cli_read_stdin(char *buf, size_t size, void *ctx)
{
    (void)ctx;
    if (fgets(buf, (int)size, stdin) == NULL) {
        return -1;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[len - 1] = '\0';
        len--;
    }
    return (int)len;
}

static void cli_write_stdout(const char *text, void *ctx)
{
    (void)ctx;
    fputs(text, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Input parsing helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * cli_read_line — read a line of input through the IO sink. Returns -1 on
 * EOF/error, 0 on empty line, or the number of characters read.
 */
static int cli_read_line(const CliIO *io, char *buf, size_t size)
{
    if (io == NULL || io->read_line == NULL || buf == NULL || size == 0) {
        return -1;
    }
    return io->read_line(buf, size, io->ctx);
}

static void cli_write(const CliIO *io, const char *text)
{
    if (io != NULL && io->write != NULL) {
        io->write(text, io->ctx);
    }
}

/*
 * cli_parse_uint — parse a null-terminated string as a non-negative integer.
 * Returns 1 on success with *out set, 0 on failure.
 */
static int cli_parse_uint(const char *str, int *out)
{
    if (str == NULL || *str == '\0') {
        return 0;
    }
    char *end = NULL;
    long value = strtol(str, &end, 10);
    if (end == str || *end != '\0') {
        return 0;
    }
    if (value < 0 || value > INT32_MAX) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

/*
 * cli_is_all_whitespace — return 1 if the string contains only whitespace
 * or is empty.
 */
static int cli_is_all_whitespace(const char *str)
{
    if (str == NULL) {
        return 1;
    }
    while (*str != '\0') {
        if (!isspace((unsigned char)*str)) {
            return 0;
        }
        str++;
    }
    return 1;
}

/*
 * cli_read_amount — read a rupee amount from the IO sink and parse it into
 * paise. Returns 1 on success, 0 on parse failure or empty input, -1 on EOF.
 */
static int cli_read_amount(const CliIO *io, int64_t *out_paise)
{
    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return -1;
    }
    if (cli_is_all_whitespace(buf)) {
        return 0;
    }
    if (!money_parse_paise(buf, out_paise)) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Transaction submit / wait / get / release helper                   */
/* ------------------------------------------------------------------ */

/*
 * cli_submit_transaction — submit a banking operation through the
 * session/transaction infrastructure, wait for completion, retrieve the
 * result, and release the request slot.
 *
 * Returns 0 on success (infrastructure completed), negative on
 * infrastructure failure (*out_bank_result holds the BankStatus when
 * retrievable).
 */
static int cli_submit_transaction(SessionManager *sm,
                                  TransactionManager *tm,
                                  uint64_t session_id,
                                  TransactionOperation op,
                                  int source_account_id,
                                  int dest_account_id,
                                  int64_t amount_paise,
                                  BankStatus *out_bank_result,
                                  int64_t *out_balance_result)
{
    uint64_t request_id = 0;
    SessionManagerStatus sms = session_submit_transaction(
        sm, session_id, op, source_account_id, dest_account_id,
        amount_paise, &request_id);
    if (sms != SESSION_MANAGER_SUCCESS) {
        return -1;
    }

    TransactionManagerStatus tms = transaction_manager_wait(tm, request_id);
    if (tms != TRANSACTION_MANAGER_SUCCESS) {
        return -2;
    }

    TransactionRequestStatus req_status;
    BankStatus bs;
    int64_t balance = 0;
    tms = transaction_manager_get_result(
        tm, request_id, &req_status, &bs, &balance);
    if (tms != TRANSACTION_MANAGER_SUCCESS) {
        return -3;
    }

    /* Release the request slot now that we have the result. */
    (void)transaction_manager_release(tm, request_id);

    *out_bank_result = bs;
    if (out_balance_result != NULL) {
        *out_balance_result = balance;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Screen rendering                                                   */
/* ------------------------------------------------------------------ */

static void cli_render_banner(const CliIO *io)
{
    cli_write(io, "========================================\n");
    cli_write(io, "              BANKSYNC\n");
    cli_write(io, "       MULTITHREADED BANKING SYSTEM\n");
    cli_write(io, "========================================\n");
}

static void cli_render_main_menu(const CliIO *io)
{
    cli_write(io, "\n");
    cli_write(io, "1. ATM Banking\n");
    cli_write(io, "2. Online Banking\n");
    cli_write(io, "3. Exit\n");
    cli_write(io, "Select an option: ");
}

static void cli_render_account_selection(const CliIO *io, const Bank *bank)
{
    char buf[128];

    cli_write(io, "\n========================================\n");
    cli_write(io, "          SELECT CUSTOMER ACCOUNT\n");
    cli_write(io, "========================================\n");

    for (int i = 0; i < bank->account_count; i++) {
        const BankAccount *acc = &bank->accounts[i];
        snprintf(buf, sizeof(buf), "Account ID: %d\n", acc->account_id);
        cli_write(io, buf);
        snprintf(buf, sizeof(buf), "Customer: %s\n", acc->customer_name);
        cli_write(io, buf);
        snprintf(buf, sizeof(buf), "Account Number: %s\n", acc->account_number);
        cli_write(io, buf);
        cli_write(io, "----------------------------------------\n");
    }

    cli_write(io, "Enter Account ID: ");
}

static void cli_render_session_created(const CliIO *io, const CliSessionContext *ctx)
{
    char buf[256];

    cli_write(io, "\n========================================\n");
    cli_write(io, "          SESSION CREATED\n");
    cli_write(io, "========================================\n");

    snprintf(buf, sizeof(buf), "Customer: %s\n", ctx->customer_name);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Account ID: %d\n", ctx->account_id);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Account Number: %s\n", ctx->account_number);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Channel: %s\n",
             ctx->channel == SESSION_CHANNEL_ATM ? "ATM" : "Online Banking");
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Session ID: %llu\n",
             (unsigned long long)ctx->session_id);
    cli_write(io, buf);
    cli_write(io, "Status: ACTIVE\n");
}

static void cli_render_atm_menu(const CliIO *io, const CliSessionContext *ctx)
{
    char buf[256];

    cli_write(io, "\n========================================\n");
    cli_write(io, "             BANKSYNC ATM\n");
    cli_write(io, "========================================\n");

    snprintf(buf, sizeof(buf), "Welcome, %s!\n", ctx->customer_name);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Account ID: %d\n", ctx->account_id);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Session ID: %llu\n",
             (unsigned long long)ctx->session_id);
    cli_write(io, buf);

    cli_write(io, "1. Balance Enquiry\n");
    cli_write(io, "2. Cash Withdrawal\n");
    cli_write(io, "3. Cash Deposit\n");
    cli_write(io, "4. Fund Transfer\n");
    cli_write(io, "5. End Session\n");
    cli_write(io, "Select an option: ");
}

static void cli_render_online_banking_menu(const CliIO *io,
                                           const CliSessionContext *ctx)
{
    char buf[256];

    cli_write(io, "\n========================================\n");
    cli_write(io, "         BANKSYNC ONLINE BANKING\n");
    cli_write(io, "========================================\n");

    snprintf(buf, sizeof(buf), "Welcome, %s!\n", ctx->customer_name);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Account ID: %d\n", ctx->account_id);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Session ID: %llu\n",
             (unsigned long long)ctx->session_id);
    cli_write(io, buf);

    cli_write(io, "1. Balance Enquiry\n");
    cli_write(io, "2. Deposit Money\n");
    cli_write(io, "3. Withdraw Money\n");
    cli_write(io, "4. Fund Transfer\n");
    cli_write(io, "5. Logout\n");
    cli_write(io, "Select an option: ");
}

static void cli_render_session_dashboard(const CliIO *io,
                                         const CliSessionContext *ctx)
{
    char buf[256];

    cli_write(io, "\n========================================\n");
    cli_write(io, "             CUSTOMER DASHBOARD\n");
    cli_write(io, "========================================\n");

    snprintf(buf, sizeof(buf), "Welcome, %s!\n", ctx->customer_name);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Account ID: %d\n", ctx->account_id);
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Channel: %s\n",
             ctx->channel == SESSION_CHANNEL_ATM ? "ATM" : "Online Banking");
    cli_write(io, buf);
    snprintf(buf, sizeof(buf), "Session ID: %llu\n",
             (unsigned long long)ctx->session_id);
    cli_write(io, buf);
}

/* ------------------------------------------------------------------ */
/* Balance display helper                                             */
/* ------------------------------------------------------------------ */

static void cli_display_balance(const CliIO *io, int64_t balance_paise)
{
    char buf[64];
    int n = money_format_rupees(balance_paise, buf, sizeof(buf));
    if (n < 0) {
        cli_write(io, "Balance: <error>\n");
    } else {
        cli_write(io, "Balance: ");
        cli_write(io, buf);
        cli_write(io, "\n");
    }
}

/* ------------------------------------------------------------------ */
/* Session validation helpers                                         */
/* ------------------------------------------------------------------ */

/*
 * cli_validate_session_channel — verify the session exists, is ACTIVE, and
 * matches the expected channel. Returns 1 on success, 0 on failure.
 *
 * Syncs the authoritative account ID and channel from the session record.
 */
static int cli_validate_session_channel(SessionManager *sm,
                                        CliSessionContext *ctx,
                                        SessionChannel expected_channel)
{
    if (!ctx->has_session) {
        return 0;
    }

    SessionState state;
    int account_id = 0;
    SessionChannel channel;
    SessionManagerStatus sms = session_get(
        sm, ctx->session_id, &state, &account_id, &channel);
    if (sms != SESSION_MANAGER_SUCCESS) {
        return 0;
    }

    if (state != SESSION_STATE_ACTIVE) {
        return 0;
    }
    if (channel != expected_channel) {
        return 0;
    }

    /* Sync the authoritative account ID and channel from the session
     * record. */
    ctx->account_id = account_id;
    ctx->channel = channel;
    return 1;
}

/*
 * cli_validate_atm_session — validate the session for ATM operations.
 * Returns 1 on success, 0 on failure.
 */
static int cli_validate_atm_session(SessionManager *sm,
                                    CliSessionContext *ctx)
{
    return cli_validate_session_channel(sm, ctx, SESSION_CHANNEL_ATM);
}

/*
 * cli_validate_online_banking_session — validate the session for Online
 * Banking operations. Returns 1 on success, 0 on failure.
 */
static int cli_validate_online_banking_session(SessionManager *sm,
                                               CliSessionContext *ctx)
{
    return cli_validate_session_channel(sm, ctx, SESSION_CHANNEL_ONLINE_BANKING);
}

/* ------------------------------------------------------------------ */
/* ATM operation handlers                                             */
/* ------------------------------------------------------------------ */

/*
 * cli_handle_balance_enquiry — submit a balance enquiry and display the
 * result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_balance_enquiry(const CliIO *io, Application *app,
                                      CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    BankStatus bs;
    int64_t balance = 0;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_BALANCE_ENQUIRY,
        ctx->account_id, 0, 0, &bs, &balance);

    if (result != 0) {
        cli_write(io, "Balance enquiry request failed.\n");
        return 0;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Balance enquiry failed.\n");
        return 0;
    }

    cli_write(io, "Balance enquiry successful.\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    cli_display_balance(io, balance);
    return 1;
}

/*
 * cli_handle_cash_withdrawal — prompt for amount, submit withdrawal, display
 * result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_cash_withdrawal(const CliIO *io, Application *app,
                                      CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter withdrawal amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing withdrawal...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_WITHDRAWAL,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Withdrawal request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Withdrawal failed.\n");
        return 1;
    }

    cli_write(io, "Withdrawal successful.\n");
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    return 1;
}

/*
 * cli_handle_cash_deposit — prompt for amount, submit deposit, display
 * result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_cash_deposit(const CliIO *io, Application *app,
                                   CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter deposit amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing deposit...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_DEPOSIT,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Deposit request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Deposit failed.\n");
        return 1;
    }

    cli_write(io, "Deposit successful.\n");
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    return 1;
}

/*
 * cli_handle_fund_transfer — prompt for destination account and amount,
 * submit transfer, display result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_fund_transfer(const CliIO *io, Application *app,
                                    CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;
    Bank *bank = application_get_bank(app);

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter destination account ID: ");
    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return 0; /* EOF */
    }
    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid input. Please enter a valid Account ID.\n");
        return 1;
    }

    int dest_account_id = 0;
    if (!cli_parse_uint(buf, &dest_account_id)) {
        cli_write(io, "Invalid input. Account ID must be a number.\n");
        return 1;
    }

    if (dest_account_id == ctx->account_id) {
        cli_write(io, "Cannot transfer to the same account.\n");
        return 1;
    }

    BankAccount *dest_acc = bank_find_account(bank, dest_account_id);
    if (dest_acc == NULL) {
        cli_write(io, "Unknown destination account ID.\n");
        return 1;
    }

    cli_write(io, "Enter transfer amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing transfer...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_TRANSFER,
        ctx->account_id, dest_account_id, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Transfer request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Transfer failed.\n");
        return 1;
    }

    cli_write(io, "Transfer successful.\n");
    cli_write(io, "From Account: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    cli_write(io, "To Account: ");
    snprintf(id_buf, sizeof(id_buf), "%d\n", dest_account_id);
    cli_write(io, id_buf);
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Online Banking operation handlers                                  */
/* ------------------------------------------------------------------ */

/*
 * cli_handle_online_balance_enquiry — submit a balance enquiry for an
 * Online Banking session and display the result. Returns 1 on success, 0
 * on failure.
 */
static int cli_handle_online_balance_enquiry(const CliIO *io, Application *app,
                                             CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    BankStatus bs;
    int64_t balance = 0;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_BALANCE_ENQUIRY,
        ctx->account_id, 0, 0, &bs, &balance);

    if (result != 0) {
        cli_write(io, "Balance enquiry request failed.\n");
        return 0;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Balance enquiry failed.\n");
        return 0;
    }

    cli_write(io, "Balance enquiry successful.\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    cli_display_balance(io, balance);
    return 1;
}

/*
 * cli_handle_online_deposit — prompt for amount, submit deposit, display
 * result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_online_deposit(const CliIO *io, Application *app,
                                     CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter deposit amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing deposit...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_DEPOSIT,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Deposit request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Deposit failed.\n");
        return 1;
    }

    cli_write(io, "Deposit successful.\n");
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    return 1;
}

/*
 * cli_handle_online_withdrawal — prompt for amount, submit withdrawal,
 * display result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_online_withdrawal(const CliIO *io, Application *app,
                                        CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter withdrawal amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing withdrawal...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_WITHDRAWAL,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Withdrawal request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Withdrawal failed.\n");
        return 1;
    }

    cli_write(io, "Withdrawal successful.\n");
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    cli_write(io, "Account ID: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    return 1;
}

/*
 * cli_handle_online_transfer — prompt for destination account and amount,
 * submit transfer, display result. Returns 1 on success, 0 on failure.
 */
static int cli_handle_online_transfer(const CliIO *io, Application *app,
                                      CliSessionContext *ctx)
{
    SessionManager *sm = application_get_session_manager(app);
    TransactionManager *tm = &app->txn_mgr;
    Bank *bank = application_get_bank(app);

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_write(io, "Session is not active. Returning to main menu.\n");
        return 0;
    }

    cli_write(io, "Enter destination account ID: ");
    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return 0; /* EOF */
    }
    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid input. Please enter a valid Account ID.\n");
        return 1;
    }

    int dest_account_id = 0;
    if (!cli_parse_uint(buf, &dest_account_id)) {
        cli_write(io, "Invalid input. Account ID must be a number.\n");
        return 1;
    }

    if (dest_account_id == ctx->account_id) {
        cli_write(io, "Cannot transfer to the same account.\n");
        return 1;
    }

    BankAccount *dest_acc = bank_find_account(bank, dest_account_id);
    if (dest_acc == NULL) {
        cli_write(io, "Unknown destination account ID.\n");
        return 1;
    }

    cli_write(io, "Enter transfer amount: ");
    int64_t amount_paise = 0;
    int read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_write(io, "Invalid amount. Please try again.\n");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_write(io, "Amount must be greater than zero.\n");
        return 1;
    }

    cli_write(io, "Processing transfer...\n");

    BankStatus bs;
    int result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_TRANSFER,
        ctx->account_id, dest_account_id, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_write(io, "Transfer request failed.\n");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write(io, "Transfer failed.\n");
        return 1;
    }

    cli_write(io, "Transfer successful.\n");
    cli_write(io, "From Account: ");
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%d\n", ctx->account_id);
    cli_write(io, id_buf);
    cli_write(io, "To Account: ");
    snprintf(id_buf, sizeof(id_buf), "%d\n", dest_account_id);
    cli_write(io, id_buf);
    cli_write(io, "Amount: ");
    char amt_buf[64];
    money_format_rupees(amount_paise, amt_buf, sizeof(amt_buf));
    cli_write(io, amt_buf);
    cli_write(io, "\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* State handlers                                                     */
/* ------------------------------------------------------------------ */

/*
 * cli_handle_main_menu — display the main menu and read a choice.
 * Returns the next state.
 */
static CliState cli_handle_main_menu(const CliIO *io, Application *app,
                                     SessionChannel *selected_channel)
{
    (void)app;
    cli_render_banner(io);
    cli_render_main_menu(io);

    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return CLI_STATE_EXIT;
    }

    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid selection. Please enter 1, 2, or 3.\n");
        return CLI_STATE_MAIN_MENU;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_write(io, "Invalid selection. Please enter 1, 2, or 3.\n");
        return CLI_STATE_MAIN_MENU;
    }

    switch (choice) {
    case CLI_OPTION_ATM:
        *selected_channel = SESSION_CHANNEL_ATM;
        return CLI_STATE_ACCOUNT_SELECTION;
    case CLI_OPTION_ONLINE_BANKING:
        *selected_channel = SESSION_CHANNEL_ONLINE_BANKING;
        return CLI_STATE_ACCOUNT_SELECTION;
    case CLI_OPTION_EXIT:
        return CLI_STATE_EXIT;
    default:
        cli_write(io, "Invalid selection. Please enter 1, 2, or 3.\n");
        return CLI_STATE_MAIN_MENU;
    }
}

/*
 * cli_handle_account_selection — display accounts, read an account ID,
 * validate it, and create a session. Returns the next state.
 */
static CliState cli_handle_account_selection(const CliIO *io, Application *app,
                                             SessionChannel selected_channel,
                                             CliSessionContext *ctx)
{
    Bank *bank = application_get_bank(app);
    SessionManager *sm = application_get_session_manager(app);

    cli_render_account_selection(io, bank);

    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return CLI_STATE_EXIT;
    }

    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid input. Please enter a valid Account ID.\n");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    int account_id = 0;
    if (!cli_parse_uint(buf, &account_id)) {
        cli_write(io, "Invalid input. Account ID must be a number.\n");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    BankAccount *acc = bank_find_account(bank, account_id);
    if (acc == NULL) {
        cli_write(io, "Unknown account ID. Please try again.\n");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    /* Create the session through SessionManager. */
    uint64_t session_id = 0;
    SessionManagerStatus sms = session_create(
        sm, account_id, selected_channel, &session_id);
    if (sms != SESSION_MANAGER_SUCCESS) {
        cli_write(io, "Failed to create session. Please try again.\n");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    /* Populate session context. */
    ctx->has_session = 1;
    ctx->session_id = session_id;
    ctx->account_id = account_id;
    ctx->channel = selected_channel;
    snprintf(ctx->customer_name, sizeof(ctx->customer_name), "%s", acc->customer_name);
    snprintf(ctx->account_number, sizeof(ctx->account_number), "%s", acc->account_number);

    cli_render_session_created(io, ctx);
    return CLI_STATE_SESSION_DASHBOARD;
}

/*
 * cli_handle_session_dashboard — display the dashboard and route based on
 * channel. ATM sessions proceed to ATM operations; Online Banking sessions
 * proceed to Online Banking operations.
 * Returns the next state.
 */
static CliState cli_handle_session_dashboard(const CliIO *io, Application *app,
                                             CliSessionContext *ctx)
{
    (void)app;
    cli_render_session_dashboard(io, ctx);

    if (ctx->channel == SESSION_CHANNEL_ATM) {
        return CLI_STATE_ATM_OPERATIONS;
    }

    return CLI_STATE_ONLINE_BANKING_OPERATIONS;
}

/*
 * cli_handle_atm_operations — display the ATM menu and process the selected
 * operation. After each operation, return to the ATM menu.
 * Returns the next state.
 */
static CliState cli_handle_atm_operations(const CliIO *io, Application *app,
                                          CliSessionContext *ctx)
{
    cli_render_atm_menu(io, ctx);

    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return CLI_STATE_EXIT;
    }

    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ATM_OPERATIONS;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ATM_OPERATIONS;
    }

    switch (choice) {
    case CLI_ATM_OPTION_BALANCE_ENQUIRY:
        (void)cli_handle_balance_enquiry(io, app, ctx);
        return CLI_STATE_ATM_OPERATIONS;
    case CLI_ATM_OPTION_WITHDRAWAL:
        (void)cli_handle_cash_withdrawal(io, app, ctx);
        return CLI_STATE_ATM_OPERATIONS;
    case CLI_ATM_OPTION_DEPOSIT:
        (void)cli_handle_cash_deposit(io, app, ctx);
        return CLI_STATE_ATM_OPERATIONS;
    case CLI_ATM_OPTION_TRANSFER:
        (void)cli_handle_fund_transfer(io, app, ctx);
        return CLI_STATE_ATM_OPERATIONS;
    case CLI_ATM_OPTION_END_SESSION: {
        SessionManager *sm = application_get_session_manager(app);
        SessionManagerStatus sms = session_terminate(sm, ctx->session_id);
        if (sms != SESSION_MANAGER_SUCCESS) {
            cli_write(io, "Failed to terminate session.\n");
        } else {
            cli_write(io, "\nSession terminated.\n");
        }
        ctx->has_session = 0;
        ctx->session_id = 0;
        ctx->account_id = 0;
        ctx->channel = SESSION_CHANNEL_ATM;
        ctx->customer_name[0] = '\0';
        ctx->account_number[0] = '\0';
        return CLI_STATE_MAIN_MENU;
    }
    default:
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ATM_OPERATIONS;
    }
}

/*
 * cli_handle_online_banking_operations — display the Online Banking menu
 * and process the selected operation. After each operation, return to the
 * Online Banking menu.
 * Returns the next state.
 */
static CliState cli_handle_online_banking_operations(const CliIO *io,
                                                     Application *app,
                                                     CliSessionContext *ctx)
{
    cli_render_online_banking_menu(io, ctx);

    char buf[CLI_INPUT_BUFFER_SIZE];
    int n = cli_read_line(io, buf, sizeof(buf));
    if (n < 0) {
        return CLI_STATE_EXIT;
    }

    if (cli_is_all_whitespace(buf)) {
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    }

    switch (choice) {
    case CLI_ONLINE_BANKING_OPTION_BALANCE_ENQUIRY:
        (void)cli_handle_online_balance_enquiry(io, app, ctx);
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    case CLI_ONLINE_BANKING_OPTION_DEPOSIT:
        (void)cli_handle_online_deposit(io, app, ctx);
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    case CLI_ONLINE_BANKING_OPTION_WITHDRAWAL:
        (void)cli_handle_online_withdrawal(io, app, ctx);
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    case CLI_ONLINE_BANKING_OPTION_TRANSFER:
        (void)cli_handle_online_transfer(io, app, ctx);
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    case CLI_ONLINE_BANKING_OPTION_LOGOUT: {
        SessionManager *sm = application_get_session_manager(app);
        SessionManagerStatus sms = session_terminate(sm, ctx->session_id);
        if (sms != SESSION_MANAGER_SUCCESS) {
            cli_write(io, "Failed to terminate session.\n");
        } else {
            cli_write(io, "\nLogged out.\n");
        }
        ctx->has_session = 0;
        ctx->session_id = 0;
        ctx->account_id = 0;
        ctx->channel = SESSION_CHANNEL_ATM;
        ctx->customer_name[0] = '\0';
        ctx->account_number[0] = '\0';
        return CLI_STATE_MAIN_MENU;
    }
    default:
        cli_write(io, "Invalid selection. Please enter 1 through 5.\n");
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void cli_run_with_io(Application *app, const CliIO *io)
{
    if (app == NULL || io == NULL) {
        return;
    }

    CliState state = CLI_STATE_MAIN_MENU;
    SessionChannel selected_channel = SESSION_CHANNEL_ATM;
    CliSessionContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    while (state != CLI_STATE_EXIT) {
        switch (state) {
        case CLI_STATE_MAIN_MENU:
            state = cli_handle_main_menu(io, app, &selected_channel);
            break;
        case CLI_STATE_ACCOUNT_SELECTION:
            state = cli_handle_account_selection(io, app, selected_channel, &ctx);
            break;
        case CLI_STATE_SESSION_DASHBOARD:
            state = cli_handle_session_dashboard(io, app, &ctx);
            break;
        case CLI_STATE_ATM_OPERATIONS:
            state = cli_handle_atm_operations(io, app, &ctx);
            break;
        case CLI_STATE_ONLINE_BANKING_OPERATIONS:
            state = cli_handle_online_banking_operations(io, app, &ctx);
            break;
        case CLI_STATE_EXIT:
            break;
        }
    }
}

void cli_run(Application *app)
{
    CliIO io;
    io.read_line = cli_read_stdin;
    io.write = cli_write_stdout;
    io.ctx = NULL;
    cli_run_with_io(app, &io);
}
