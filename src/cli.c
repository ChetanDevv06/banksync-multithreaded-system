#include "cli.h"
#include "money_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

/*
 * CLI input buffer size. Large enough for normal input; lines longer than
 * this are truncated and the remainder discarded.
 */
#define CLI_INPUT_BUFFER_SIZE 256

/*
 * Default terminal width used when width detection is unavailable (e.g. stdout
 * is not a TTY).
 */
#define CLI_DEFAULT_WIDTH 80

/*
 * Nominal width of the centred composition. Separators and panel borders are
 * drawn at this width; all content shares a common left offset so the entire
 * layout is centred as one consistent visual composition.
 */
#define CLI_BLOCK_WIDTH CLI_SEP_WIDTH

/*
 * Width of horizontal separator lines.
 */
#define CLI_SEP_WIDTH 42

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

/*
 * Terminal colour state. Disabled by default; enabled in cli_run() only when
 * stdout is a TTY (and virtual terminal processing is enabled on Windows).
 * Tests that inject a mock IO sink never enable colour, so their output
 * remains plain text and free of escape sequences.
 */
static int cli_colour = 0;

/* ANSI escape sequences. Each is emitted as a complete, non-interleaved unit
 * so they never split visible text or interfere with alignment. */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_CYAN    "\033[36m"      /* application branding */
#define C_BRIGHT_CYAN "\033[1;36m"/* section headings */
#define C_WHITE   "\033[37m"      /* menu options */
#define C_BRIGHT_WHITE "\033[1;37m" /* important values */
#define C_GREEN   "\033[32m"      /* success messages */
#define C_RED     "\033[31m"      /* error messages */
#define C_YELLOW  "\033[33m"      /* warnings */
#define C_GRAY    "\033[2;37m"    /* secondary information */
#define C_DARK_GRAY "\033[90m"    /* borders and separators */

#if defined(_WIN32)
static void cli_enable_vt_processing(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!GetConsoleMode(h, &mode)) {
        return;
    }
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    (void)SetConsoleMode(h, mode);
}
#endif

static int cli_isatty_stream(FILE *stream)
{
#if defined(_WIN32)
    return isatty(_fileno(stream));
#else
    return isatty(fileno(stream));
#endif
}

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
/* Terminal width detection                                           */
/* ------------------------------------------------------------------ */

/*
 * cli_terminal_width — detect the current terminal width in columns.
 * Returns CLI_DEFAULT_WIDTH when stdout is not a TTY or width cannot be
 * determined. Re-detected on every call so the layout adapts to resizes.
 */
static int cli_terminal_width(void)
{
    static int cached_width = 0;
    int width;

    if (cached_width != 0) {
        return cached_width;
    }

    width = CLI_DEFAULT_WIDTH;

#if defined(_WIN32)
    if (cli_isatty_stream(stdout)) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(h, &csbi)) {
            width = (int)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
            if (width <= 0) {
                width = CLI_DEFAULT_WIDTH;
            }
        }
    }
#else
    if (cli_isatty_stream(stdout)) {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            width = (int)w.ws_col;
        }
    }
#endif

    cached_width = width;
    return width;
}

/*
 * cli_block_offset — common left offset used to centre the entire
 * composition within the terminal. Every rendered line shares this offset so
 * borders, separators, panels and contents align as one block.
 */
static int cli_block_offset(void)
{
    int term_width = cli_terminal_width();
    int offset = (term_width - CLI_BLOCK_WIDTH) / 2;
    return offset > 0 ? offset : 0;
}

/* ------------------------------------------------------------------ */
/* Presentation helpers                                               */
/* ------------------------------------------------------------------ */

static void cli_emit_code(const CliIO *io, const char *code)
{
    if (cli_colour && code != NULL && io != NULL && io->write != NULL) {
        io->write(code, io->ctx);
    }
}

static void cli_write(const CliIO *io, const char *text)
{
    if (io != NULL && io->write != NULL) {
        io->write(text, io->ctx);
    }
}

static void cli_write_spaces(const CliIO *io, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        cli_write(io, " ");
    }
}

static void cli_write_line(const CliIO *io, const char *text)
{
    cli_write(io, text);
    cli_write(io, "\n");
}

static void cli_write_text(const CliIO *io, const char *text)
{
    cli_write(io, text);
}

/* Write a line with the common centring offset and a trailing newline. */
static void cli_write_centered_line(const CliIO *io, const char *text)
{
    cli_write_spaces(io, cli_block_offset());
    cli_write_line(io, text);
}
/* Write text with the common centring offset, no trailing newline (prompts). */
static void cli_write_centered_text(const CliIO *io, const char *text)
{
    cli_write_spaces(io, cli_block_offset());
    cli_write(io, text);
}

/* Write a colour-wrapped line with the common centring offset. */
static void cli_color_line_centered(const CliIO *io, const char *color,
                                    const char *text)
{
    cli_write_spaces(io, cli_block_offset());
    cli_emit_code(io, color);
    cli_write_line(io, text);
    cli_emit_code(io, C_RESET);
}

/* Message renderers. Core phrases stay contiguous so substring-based tests
 * pass regardless of colour state or leading centring spaces. */
static void cli_success_centered(const CliIO *io, const char *text)
{
    cli_write_spaces(io, cli_block_offset());
    cli_emit_code(io, C_GREEN);
    cli_write_text(io, "[OK] ");
    cli_emit_code(io, C_RESET);
    cli_write_line(io, text);
}

static void cli_error_centered(const CliIO *io, const char *text)
{
    cli_write_spaces(io, cli_block_offset());
    cli_emit_code(io, C_RED);
    cli_write_text(io, "[!] ");
    cli_emit_code(io, C_RESET);
    cli_write_line(io, text);
}

static const char *cli_bank_status_message(BankStatus bs)
{
    switch (bs) {
    case BANK_SUCCESS:
        return "Success";
    case BANK_ERROR_INSUFFICIENT_FUNDS:
        return "Insufficient funds";
    case BANK_ERROR_INVALID_AMOUNT:
        return "Invalid amount";
    case BANK_ERROR_BALANCE_OVERFLOW:
        return "Balance overflow";
    case BANK_ERR_ACCOUNT_NOT_FOUND:
        return "Account not found";
    case BANK_ERR_INVALID_ARGUMENT:
        return "Invalid argument";
    case BANK_ERR_DUPLICATE_ACCOUNT:
        return "Duplicate account";
    case BANK_ERR_MAX_CAPACITY:
        return "Maximum capacity reached";
    case BANK_ERR_MUTEX_INIT_FAILED:
        return "Mutex initialization failed";
    case BANK_ERR_INVALID_NAME:
        return "Invalid customer name";
    case BANK_ERROR_MUTEX_LOCK:
        return "Mutex lock failed";
    case BANK_ERROR_MUTEX_UNLOCK:
        return "Mutex unlock failed";
    case BANK_ERROR_HISTORY_FULL:
        return "Transaction history full";
    case BANK_ERROR_BUFFER_TOO_SMALL:
        return "Buffer too small";
    default:
        return "Unknown bank error";
    }
}

static void cli_menu_item(const CliIO *io, int number, const char *label)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "  %d. %s", number, label);
    cli_write_centered_line(io, buf);
}

static void cli_prompt(const CliIO *io, const char *text)
{
    cli_emit_code(io, C_BRIGHT_CYAN);
    cli_write_centered_text(io, text);
    cli_emit_code(io, C_RESET);
}

static int cli_format_amount(int64_t amount_paise, char *buf, size_t size)
{
    return money_format_rupees(amount_paise, buf, size);
}

/* Display an account balance using the existing integer-paise formatter. */
static void cli_display_balance(const CliIO *io, int64_t balance_paise)
{
    char buf[64];
    int n = money_format_rupees(balance_paise, buf, sizeof(buf));
    if (n < 0) {
        cli_write_centered_line(io, "Balance: <error>");
    } else {
        char line[128];
        snprintf(line, sizeof(line), "Balance: %s", buf);
        cli_write_centered_line(io, line);
    }
}

/* Fetch and display the live balance for an account after a mutating
 * operation. Read-only; does not alter balances or semantics. */
static void cli_show_updated_balance(const CliIO *io, Application *app,
                                     int account_id)
{
    Bank *bank = application_get_bank(app);
    int64_t balance = 0;
    BankStatus bs;

    if (bank == NULL) {
        cli_write_centered_line(io, "Updated balance: <unavailable>");
        return;
    }
    bs = bank_get_balance(bank, account_id, &balance);
    if (bs != BANK_SUCCESS) {
        cli_write_centered_line(io, "Updated balance: <unavailable>");
        return;
    }
    {
        char line[128];
        char amt[64];
        if (money_format_rupees(balance, amt, sizeof(amt)) < 0) {
            cli_write_centered_line(io, "Updated balance: <error>");
        } else {
            snprintf(line, sizeof(line), "Updated balance: %s", amt);
            cli_write_centered_line(io, line);
        }
    }
}

/* Horizontal separator frame line (fixed width, centred with the block). */
static void cli_sep(const CliIO *io, char ch)
{
    char buf[CLI_SEP_WIDTH + 1];
    int i;

    for (i = 0; i < CLI_SEP_WIDTH; i++) {
        buf[i] = ch;
    }
    buf[CLI_SEP_WIDTH] = '\0';
    cli_emit_code(io, C_DARK_GRAY);
    cli_write_centered_line(io, buf);
    cli_emit_code(io, C_RESET);
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
    cli_write_line(io, "");
    cli_sep(io, '=');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "              BANKSYNC");
    cli_color_line_centered(io, C_CYAN, "     MULTITHREADED BANKING SYSTEM");
    cli_sep(io, '=');
    cli_write_line(io, "");
}

static void cli_render_main_menu(const CliIO *io)
{
    cli_sep(io, '-');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "             MAIN MENU");
    cli_sep(io, '-');
    cli_write_line(io, "");
    cli_menu_item(io, CLI_OPTION_ATM, "ATM Banking");
    cli_menu_item(io, CLI_OPTION_ONLINE_BANKING, "Online Banking");
    cli_menu_item(io, CLI_OPTION_EXIT, "Exit");
    cli_write_line(io, "");
    cli_color_line_centered(io, C_GRAY, "  Tip: Enter the number of your choice (1-3).");
    cli_prompt(io, "Select an option: ");
}

static void cli_render_account_selection(const CliIO *io, const Bank *bank)
{
    char buf[256];
    int i;

    cli_write_line(io, "");
    cli_sep(io, '=');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "          SELECT CUSTOMER ACCOUNT");
    cli_sep(io, '=');
    cli_write_line(io, "");
    cli_write_centered_line(io, "  Available accounts:");
    for (i = 0; i < bank->account_count; i++) {
        const BankAccount *acc = &bank->accounts[i];
        cli_sep(io, '-');
        snprintf(buf, sizeof(buf), "  Account ID: %d", acc->account_id);
        cli_write_centered_line(io, buf);
        snprintf(buf, sizeof(buf), "  Customer: %s", acc->customer_name);
        cli_write_centered_line(io, buf);
        snprintf(buf, sizeof(buf), "  Account Number: %s", acc->account_number);
        cli_write_centered_line(io, buf);
    }
    if (bank->account_count > 0) {
        cli_sep(io, '-');
    }
    cli_write_line(io, "");
    cli_prompt(io, "Enter Account ID: ");
}

static void cli_render_session_created(const CliIO *io, const CliSessionContext *ctx)
{
    char buf[256];
    const char *channel_text =
        ctx->channel == SESSION_CHANNEL_ATM ? "ATM" : "Online Banking";

    cli_write_line(io, "");
    cli_sep(io, '=');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "          SESSION CREATED");
    cli_sep(io, '=');
    cli_write_line(io, "");
    snprintf(buf, sizeof(buf), "  Status: ACTIVE");
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Customer: %s", ctx->customer_name);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account ID: %d", ctx->account_id);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account Number: %s", ctx->account_number);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Channel: %s", channel_text);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Session ID: %llu",
             (unsigned long long)ctx->session_id);
    cli_write_centered_line(io, buf);
    cli_write_line(io, "");
}

static void cli_render_atm_menu(const CliIO *io, const CliSessionContext *ctx)
{
    char buf[256];

    cli_write_line(io, "");
    cli_sep(io, '=');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "             BANKSYNC ATM");
    cli_sep(io, '=');
    cli_write_line(io, "");
    snprintf(buf, sizeof(buf), "  Welcome, %s!", ctx->customer_name);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account ID: %d", ctx->account_id);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account Number: %s", ctx->account_number);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Channel: ATM");
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Session ID: %llu",
             (unsigned long long)ctx->session_id);
    cli_write_centered_line(io, buf);
    cli_write_line(io, "");
    cli_menu_item(io, CLI_ATM_OPTION_BALANCE_ENQUIRY, "Balance Enquiry");
    cli_menu_item(io, CLI_ATM_OPTION_WITHDRAWAL, "Cash Withdrawal");
    cli_menu_item(io, CLI_ATM_OPTION_DEPOSIT, "Cash Deposit");
    cli_menu_item(io, CLI_ATM_OPTION_TRANSFER, "Fund Transfer");
    cli_menu_item(io, CLI_ATM_OPTION_END_SESSION, "Logout / Return");
    cli_write_line(io, "");
    cli_prompt(io, "Select an option: ");
}

static void cli_render_online_banking_menu(const CliIO *io,
                                           const CliSessionContext *ctx)
{
    char buf[256];

    cli_write_line(io, "");
    cli_sep(io, '=');
    cli_color_line_centered(io, C_BRIGHT_CYAN, "        BANKSYNC ONLINE BANKING");
    cli_sep(io, '=');
    cli_write_line(io, "");
    snprintf(buf, sizeof(buf), "  Welcome, %s!", ctx->customer_name);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account ID: %d", ctx->account_id);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Account Number: %s", ctx->account_number);
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Channel: Online Banking");
    cli_write_centered_line(io, buf);
    snprintf(buf, sizeof(buf), "  Session ID: %llu",
             (unsigned long long)ctx->session_id);
    cli_write_centered_line(io, buf);
    cli_write_line(io, "");
    cli_menu_item(io, CLI_ONLINE_BANKING_OPTION_BALANCE_ENQUIRY,
                  "Balance Enquiry");
    cli_menu_item(io, CLI_ONLINE_BANKING_OPTION_DEPOSIT, "Deposit");
    cli_menu_item(io, CLI_ONLINE_BANKING_OPTION_WITHDRAWAL, "Withdraw");
    cli_menu_item(io, CLI_ONLINE_BANKING_OPTION_TRANSFER, "Fund Transfer");
    cli_menu_item(io, CLI_ONLINE_BANKING_OPTION_LOGOUT, "Logout / Return");
    cli_write_line(io, "");
    cli_prompt(io, "Select an option: ");
}

/* ------------------------------------------------------------------ */
/* Balance display helper                                             */
/* ------------------------------------------------------------------ */

/* (cli_display_balance defined above with the presentation helpers.) */

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
    BankStatus bs;
    int64_t balance = 0;
    int result;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_BALANCE_ENQUIRY,
        ctx->account_id, 0, 0, &bs, &balance);

    if (result != 0) {
        cli_error_centered(io, "Balance enquiry request failed.");
        return 0;
    }

    if (bs != BANK_SUCCESS) {
        cli_error_centered(io, "Balance enquiry failed.");
        cli_write_line(io, "");
        cli_sep(io, '-');
        return 0;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Balance enquiry successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    cli_display_balance(io, balance);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int read_result;
    int result;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter withdrawal amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing withdrawal...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_WITHDRAWAL,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Withdrawal request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Withdrawal failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Withdrawal successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int read_result;
    int result;

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter deposit amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing deposit...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_DEPOSIT,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Deposit request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Deposit failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Deposit successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int dest_account_id = 0;
    int read_result;
    int result;
    char buf[CLI_INPUT_BUFFER_SIZE];

    if (!cli_validate_atm_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter destination account ID: ");
    read_result = cli_read_line(io, buf, sizeof(buf));
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (cli_is_all_whitespace(buf)) {
        cli_error_centered(io, "Invalid input. Please enter a valid Account ID.");
        return 1;
    }

    if (!cli_parse_uint(buf, &dest_account_id)) {
        cli_error_centered(io, "Invalid input. Account ID must be a number.");
        return 1;
    }

    if (dest_account_id == ctx->account_id) {
        cli_error_centered(io, "Cannot transfer to the same account.");
        return 1;
    }

    if (bank_find_account(bank, dest_account_id) == NULL) {
        cli_error_centered(io, "Unknown destination account ID.");
        return 1;
    }

    cli_prompt(io, "Enter transfer amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing transfer...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_TRANSFER,
        ctx->account_id, dest_account_id, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Transfer request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Transfer failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Transfer successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "From Account: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
        snprintf(id_buf, sizeof(id_buf), "To Account: %d", dest_account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t balance = 0;
    int result;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_BALANCE_ENQUIRY,
        ctx->account_id, 0, 0, &bs, &balance);

    if (result != 0) {
        cli_error_centered(io, "Balance enquiry request failed.");
        return 0;
    }

    if (bs != BANK_SUCCESS) {
        cli_error_centered(io, "Balance enquiry failed.");
        return 0;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Balance enquiry successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    cli_display_balance(io, balance);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int read_result;
    int result;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter deposit amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing deposit...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_DEPOSIT,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Deposit request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Deposit failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Deposit successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int read_result;
    int result;

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter withdrawal amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing withdrawal...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_WITHDRAWAL,
        ctx->account_id, 0, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Withdrawal request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Withdrawal failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Withdrawal successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "Account ID: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
    BankStatus bs;
    int64_t amount_paise = 0;
    int dest_account_id = 0;
    int read_result;
    int result;
    char buf[CLI_INPUT_BUFFER_SIZE];

    if (!cli_validate_online_banking_session(sm, ctx)) {
        cli_error_centered(io, "Session is not active. Returning to main menu.");
        return 0;
    }

    cli_prompt(io, "Enter destination account ID: ");
    read_result = cli_read_line(io, buf, sizeof(buf));
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (cli_is_all_whitespace(buf)) {
        cli_error_centered(io, "Invalid input. Please enter a valid Account ID.");
        return 1;
    }

    if (!cli_parse_uint(buf, &dest_account_id)) {
        cli_error_centered(io, "Invalid input. Account ID must be a number.");
        return 1;
    }

    if (dest_account_id == ctx->account_id) {
        cli_error_centered(io, "Cannot transfer to the same account.");
        return 1;
    }

    if (bank_find_account(bank, dest_account_id) == NULL) {
        cli_error_centered(io, "Unknown destination account ID.");
        return 1;
    }

    cli_prompt(io, "Enter transfer amount: ");
    read_result = cli_read_amount(io, &amount_paise);
    if (read_result < 0) {
        return 0; /* EOF */
    }
    if (read_result == 0) {
        cli_error_centered(io, "Invalid amount. Please try again.");
        return 1;
    }
    if (amount_paise <= 0) {
        cli_error_centered(io, "Amount must be greater than zero.");
        return 1;
    }

    cli_write_centered_line(io, "Processing transfer...");

    result = cli_submit_transaction(
        sm, tm, ctx->session_id, TRANSACTION_OP_TRANSFER,
        ctx->account_id, dest_account_id, amount_paise, &bs, NULL);

    if (result != 0) {
        cli_error_centered(io, "Transfer request failed.");
        return 1;
    }

    if (bs != BANK_SUCCESS) {
        cli_write_line(io, "");
        cli_sep(io, '-');
        cli_error_centered(io, "Transfer failed.");
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Reason: %s",
                     cli_bank_status_message(bs));
            cli_write_centered_line(io, msg);
        }
        cli_sep(io, '-');
        cli_write_line(io, "");
        return 1;
    }

    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_success_centered(io, "Transfer successful.");
    {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "From Account: %d", ctx->account_id);
        cli_write_centered_line(io, id_buf);
        snprintf(id_buf, sizeof(id_buf), "To Account: %d", dest_account_id);
        cli_write_centered_line(io, id_buf);
    }
    {
        char amt_buf[64];
        cli_format_amount(amount_paise, amt_buf, sizeof(amt_buf));
        char line[128];
        snprintf(line, sizeof(line), "Amount: %s", amt_buf);
        cli_write_centered_line(io, line);
    }
    cli_show_updated_balance(io, app, ctx->account_id);
    cli_sep(io, '-');
    cli_write_line(io, "");
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
        cli_error_centered(io, "Invalid selection. Please enter 1, 2, or 3.");
        return CLI_STATE_MAIN_MENU;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_error_centered(io, "Invalid selection. Please enter 1, 2, or 3.");
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
        cli_error_centered(io, "Invalid selection. Please enter 1, 2, or 3.");
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
        cli_error_centered(io, "Invalid input. Please enter a valid Account ID.");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    int account_id = 0;
    if (!cli_parse_uint(buf, &account_id)) {
        cli_error_centered(io, "Invalid input. Account ID must be a number.");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    BankAccount *acc = bank_find_account(bank, account_id);
    if (acc == NULL) {
        cli_error_centered(io, "Unknown account ID. Please try again.");
        return CLI_STATE_ACCOUNT_SELECTION;
    }

    /* Create the session through SessionManager. */
    uint64_t session_id = 0;
    SessionManagerStatus sms = session_create(
        sm, account_id, selected_channel, &session_id);
    if (sms != SESSION_MANAGER_SUCCESS) {
        cli_error_centered(io, "Failed to create session. Please try again.");
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
 * cli_handle_session_dashboard — confirm the active session and route to the
 * channel-specific operation menu. Returns the next state.
 */
static CliState cli_handle_session_dashboard(const CliIO *io, Application *app,
                                             CliSessionContext *ctx)
{
    (void)app;
    cli_write_line(io, "");
    cli_sep(io, '-');
    cli_write_centered_line(io, "  Session active. Proceed to operations.");
    cli_sep(io, '-');
    cli_write_line(io, "");

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
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
        return CLI_STATE_ATM_OPERATIONS;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
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
            cli_error_centered(io, "Failed to terminate session.");
        } else {
            cli_write_line(io, "");
            cli_success_centered(io, "Session terminated.");
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
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
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
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
        return CLI_STATE_ONLINE_BANKING_OPERATIONS;
    }

    int choice = 0;
    if (!cli_parse_uint(buf, &choice)) {
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
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
            cli_error_centered(io, "Failed to terminate session.");
        } else {
            cli_write_line(io, "");
            cli_success_centered(io, "Logged out.");
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
        cli_error_centered(io, "Invalid selection. Please enter 1 through 5.");
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

    cli_colour = 0;
#if defined(_WIN32)
    cli_enable_vt_processing();
    /* Configure the Windows console to interpret output as UTF-8 so that
     * Unicode currency symbols (e.g. ₹) render correctly instead of being
     * misread as legacy code-page characters. */
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (cli_isatty_stream(stdout)) {
        cli_colour = 1;
    }

    cli_run_with_io(app, &io);
}
