#include "cli.h"
#include "application.h"
#include "money_utils.h"
#include "bank.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
/* Mock IO                                                            */
/* ------------------------------------------------------------------ */

typedef struct MockIO {
    const char **inputs;
    int input_count;
    int input_index;
    char *output_buffer;
    size_t output_size;
    size_t output_len;
} MockIO;

static int mock_read_line(char *buf, size_t size, void *ctx)
{
    MockIO *mock = (MockIO *)ctx;
    if (mock == NULL || mock->input_index >= mock->input_count) {
        return -1;
    }
    const char *input = mock->inputs[mock->input_index++];
    size_t len = strlen(input);
    if (len >= size) {
        len = size - 1;
    }
    memcpy(buf, input, len);
    buf[len] = '\0';
    return (int)len;
}

static void mock_write(const char *text, void *ctx)
{
    MockIO *mock = (MockIO *)ctx;
    if (mock == NULL || text == NULL) {
        return;
    }
    size_t len = strlen(text);
    if (mock->output_len + len + 1 > mock->output_size) {
        return;
    }
    memcpy(mock->output_buffer + mock->output_len, text, len);
    mock->output_len += len;
    mock->output_buffer[mock->output_len] = '\0';
}

static int output_contains(const MockIO *mock, const char *needle)
{
    return strstr(mock->output_buffer, needle) != NULL;
}

/* ------------------------------------------------------------------ */
/* Test harness                                                       */
/* ------------------------------------------------------------------ */

static Application *create_test_app(void)
{
    Application *app = (Application *)malloc(sizeof(Application));
    if (app == NULL) {
        return NULL;
    }
    memset(app, 0, sizeof(*app));
    ApplicationStatus st = application_init(app);
    if (st != APPLICATION_SUCCESS) {
        free(app);
        return NULL;
    }
    return app;
}

static void destroy_test_app(Application *app)
{
    if (app != NULL) {
        application_shutdown(app);
        free(app);
    }
}

/* ------------------------------------------------------------------ */
/* Test 1 — Valid main-menu selection                                 */
/* ------------------------------------------------------------------ */

static void test_1_valid_main_menu_selection(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");
    TEST_ASSERT(output_contains(&mock, "ATM"),
                "output should contain ATM channel");
    TEST_ASSERT(output_contains(&mock, "Session terminated"),
                "output should contain Session terminated");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 2 — Invalid main-menu selection                               */
/* ------------------------------------------------------------------ */

static void test_2_invalid_main_menu_selection(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"99", "1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Invalid selection"),
                "output should contain Invalid selection for bad menu choice");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should still contain SESSION CREATED after recovery");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 3 — ATM channel selection                                     */
/* ------------------------------------------------------------------ */

static void test_3_atm_channel_selection(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Channel: ATM"),
                "output should show Channel: ATM");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 4 — Online Banking channel selection                          */
/* ------------------------------------------------------------------ */

static void test_4_online_banking_channel_selection(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Channel: Online Banking"),
                "output should show Channel: Online Banking");
    TEST_ASSERT(output_contains(&mock, "BANKSYNC ONLINE BANKING"),
                "output should show Online Banking menu");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 5 — Valid account selection                                   */
/* ------------------------------------------------------------------ */

static void test_5_valid_account_selection(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Account ID: 1001"),
                "output should show Account ID: 1001");
    TEST_ASSERT(output_contains(&mock, "Rahul Sharma"),
                "output should show customer name Rahul Sharma");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 6 — Invalid account ID                                        */
/* ------------------------------------------------------------------ */

static void test_6_invalid_account_id(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "9999", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Unknown account ID"),
                "output should contain Unknown account ID for 9999");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED after recovery");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 7 — Non-numeric account input                                 */
/* ------------------------------------------------------------------ */

static void test_7_non_numeric_account_input(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "abc", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "must be a number"),
                "output should contain must be a number for abc");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED after recovery");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 8 — Empty input                                               */
/* ------------------------------------------------------------------ */

static void test_8_empty_input(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Invalid input"),
                "output should contain Invalid input for empty line");
    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED after recovery");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 9 — Excessively long input                                    */
/* ------------------------------------------------------------------ */

static void test_9_excessively_long_input(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    char long_input[600];
    memset(long_input, 'A', sizeof(long_input) - 1);
    long_input[sizeof(long_input) - 1] = '\0';

    const char *inputs[] = {"1", long_input, "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED after long input");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 10 — Successful session creation                              */
/* ------------------------------------------------------------------ */

static void test_10_successful_session_creation(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");
    TEST_ASSERT(output_contains(&mock, "Status: ACTIVE"),
                "output should show Status: ACTIVE");
    TEST_ASSERT(output_contains(&mock, "Session ID:"),
                "output should show Session ID");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 11 — Correct channel stored in session                        */
/* ------------------------------------------------------------------ */

static void test_11_correct_channel_stored_in_session(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1003", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Channel: Online Banking"),
                "output should show Channel: Online Banking");
    TEST_ASSERT(output_contains(&mock, "Arjun Kumar"),
                "output should show customer Arjun Kumar");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 12 — Session termination                                      */
/* ------------------------------------------------------------------ */

static void test_12_session_termination(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Session terminated"),
                "output should contain Session terminated");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 13 — Returning to main menu                                   */
/* ------------------------------------------------------------------ */

static void test_13_return_to_main_menu(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "SESSION CREATED"),
                "output should contain SESSION CREATED");
    TEST_ASSERT(output_contains(&mock, "Select an option:"),
                "output should return to main menu after end session");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 14 — EOF handling                                             */
/* ------------------------------------------------------------------ */

static void test_14_eof_handling(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {NULL};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 0;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 15 — Multiple sequential customer sessions                    */
/* ------------------------------------------------------------------ */

static void test_15_multiple_sequential_sessions(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "5", "1", "1002", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    int count = 0;
    const char *p = output;
    while ((p = strstr(p, "SESSION CREATED")) != NULL) {
        count++;
        p++;
    }
    TEST_ASSERT(count == 2,
                "output should contain two SESSION CREATED messages");

    count = 0;
    p = output;
    while ((p = strstr(p, "Session terminated")) != NULL) {
        count++;
        p++;
    }
    TEST_ASSERT(count == 2,
                "output should contain two Session terminated messages");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 16 — Successful balance enquiry (ATM)                         */
/* ------------------------------------------------------------------ */

static void test_16_balance_enquiry(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Balance enquiry successful"),
                "output should contain Balance enquiry successful");
    TEST_ASSERT(output_contains(&mock, "₹25,000.00"),
                "output should show balance ₹25,000.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 17 — Successful deposit (ATM)                                 */
/* ------------------------------------------------------------------ */

static void test_17_successful_deposit(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "3", "500.00", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Deposit successful"),
                "output should contain Deposit successful");
    TEST_ASSERT(output_contains(&mock, "₹500.00"),
                "output should show deposit amount ₹500.00");
    TEST_ASSERT(output_contains(&mock, "₹25,500.00"),
                "output should show updated balance ₹25,500.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 18 — Successful withdrawal (ATM)                              */
/* ------------------------------------------------------------------ */

static void test_18_successful_withdrawal(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "2", "200.00", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Withdrawal successful"),
                "output should contain Withdrawal successful");
    TEST_ASSERT(output_contains(&mock, "₹200.00"),
                "output should show withdrawal amount ₹200.00");
    TEST_ASSERT(output_contains(&mock, "₹24,800.00"),
                "output should show updated balance ₹24,800.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 19 — Successful transfer (ATM)                                */
/* ------------------------------------------------------------------ */

static void test_19_successful_transfer(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "4", "1002", "300.00", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Transfer successful"),
                "output should contain Transfer successful");
    TEST_ASSERT(output_contains(&mock, "From Account: 1001"),
                "output should show From Account: 1001");
    TEST_ASSERT(output_contains(&mock, "To Account: 1002"),
                "output should show To Account: 1002");
    TEST_ASSERT(output_contains(&mock, "₹300.00"),
                "output should show transfer amount ₹300.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 20 — Withdrawal with insufficient funds (ATM)                 */
/* ------------------------------------------------------------------ */

static void test_20_insufficient_funds_withdrawal(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "2", "999999999.99", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Withdrawal failed"),
                "output should contain Withdrawal failed for insufficient funds");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 21 — Transfer with insufficient funds (ATM)                   */
/* ------------------------------------------------------------------ */

static void test_21_insufficient_funds_transfer(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "4", "1002", "999999999.99", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Transfer failed"),
                "output should contain Transfer failed for insufficient funds");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 22 — Invalid destination account (ATM)                        */
/* ------------------------------------------------------------------ */

static void test_22_invalid_destination_account(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "4", "9999", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Unknown destination account ID"),
                "output should contain Unknown destination account ID");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 23 — Self-transfer (ATM)                                      */
/* ------------------------------------------------------------------ */

static void test_23_self_transfer(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "4", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "same account"),
                "output should contain same account for self-transfer");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 24 — Zero-value operation (ATM)                               */
/* ------------------------------------------------------------------ */

static void test_24_zero_value_operation(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "3", "0", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "greater than zero"),
                "output should contain greater than zero for zero deposit");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 25 — Invalid amount (ATM)                                     */
/* ------------------------------------------------------------------ */

static void test_25_invalid_amount(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "3", "abc", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Invalid amount"),
                "output should contain Invalid amount for non-numeric input");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 26 — Repeated operations in one session (ATM)                 */
/* ------------------------------------------------------------------ */

static void test_26_repeated_operations(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"1", "1001", "1", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    int count = 0;
    const char *p = output;
    while ((p = strstr(p, "Balance enquiry successful")) != NULL) {
        count++;
        p++;
    }
    TEST_ASSERT(count == 2,
                "output should contain two Balance enquiry successful messages");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 27 — ATM session cannot access Online Banking operations      */
/* ------------------------------------------------------------------ */

static void test_27_atm_session_rejected_by_online_banking(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    /* ATM session → should NOT contain Online Banking menu */
    const char *inputs[] = {"1", "1001", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(strstr(output, "BANKSYNC ONLINE BANKING") == NULL,
                "output should NOT contain Online Banking menu for ATM session");
    TEST_ASSERT(output_contains(&mock, "BANKSYNC ATM"),
                "output should contain ATM menu for ATM session");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 28 — Successful balance enquiry (Online Banking)              */
/* ------------------------------------------------------------------ */

static void test_28_online_balance_enquiry(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 5;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Balance enquiry successful"),
                "output should contain Balance enquiry successful");
    TEST_ASSERT(output_contains(&mock, "₹50,000.00"),
                "output should show balance ₹50,000.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 29 — Successful deposit (Online Banking)                      */
/* ------------------------------------------------------------------ */

static void test_29_online_deposit(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "2", "500.00", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Deposit successful"),
                "output should contain Deposit successful");
    TEST_ASSERT(output_contains(&mock, "₹500.00"),
                "output should show deposit amount ₹500.00");
    TEST_ASSERT(output_contains(&mock, "₹50,500.00"),
                "output should show updated balance ₹50,500.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 30 — Successful withdrawal (Online Banking)                   */
/* ------------------------------------------------------------------ */

static void test_30_online_withdrawal(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "3", "200.00", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Withdrawal successful"),
                "output should contain Withdrawal successful");
    TEST_ASSERT(output_contains(&mock, "₹200.00"),
                "output should show withdrawal amount ₹200.00");
    TEST_ASSERT(output_contains(&mock, "₹49,800.00"),
                "output should show updated balance ₹49,800.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 31 — Successful transfer (Online Banking)                     */
/* ------------------------------------------------------------------ */

static void test_31_online_transfer(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "4", "1001", "300.00", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 7;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Transfer successful"),
                "output should contain Transfer successful");
    TEST_ASSERT(output_contains(&mock, "From Account: 1002"),
                "output should show From Account: 1002");
    TEST_ASSERT(output_contains(&mock, "To Account: 1001"),
                "output should show To Account: 1001");
    TEST_ASSERT(output_contains(&mock, "₹300.00"),
                "output should show transfer amount ₹300.00");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 32 — Insufficient funds withdrawal (Online Banking)           */
/* ------------------------------------------------------------------ */

static void test_32_online_insufficient_funds_withdrawal(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "3", "999999999.99", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Withdrawal failed"),
                "output should contain Withdrawal failed for insufficient funds");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 33 — Invalid destination account (Online Banking)             */
/* ------------------------------------------------------------------ */

static void test_33_online_invalid_destination(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "4", "9999", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Unknown destination account ID"),
                "output should contain Unknown destination account ID");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 34 — Self-transfer rejection (Online Banking)                 */
/* ------------------------------------------------------------------ */

static void test_34_online_self_transfer(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "4", "1002", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "same account"),
                "output should contain same account for self-transfer");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 35 — Invalid amount (Online Banking)                          */
/* ------------------------------------------------------------------ */

static void test_35_online_invalid_amount(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "2", "abc", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Invalid amount"),
                "output should contain Invalid amount for non-numeric input");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 36 — Zero amount rejection (Online Banking)                   */
/* ------------------------------------------------------------------ */

static void test_36_online_zero_amount(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "2", "0", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "greater than zero"),
                "output should contain greater than zero for zero deposit");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 37 — Repeated operations in one session (Online Banking)      */
/* ------------------------------------------------------------------ */

static void test_37_online_repeated_operations(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "1", "1", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 6;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    int count = 0;
    const char *p = output;
    while ((p = strstr(p, "Balance enquiry successful")) != NULL) {
        count++;
        p++;
    }
    TEST_ASSERT(count == 2,
                "output should contain two Balance enquiry successful messages");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 38 — Online Banking session cannot access ATM operations      */
/* ------------------------------------------------------------------ */

static void test_38_online_session_rejected_by_atm(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    /* Online Banking session → should NOT contain ATM menu */
    const char *inputs[] = {"2", "1002", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(strstr(output, "BANKSYNC ATM") == NULL,
                "output should NOT contain ATM menu for Online Banking session");
    TEST_ASSERT(output_contains(&mock, "BANKSYNC ONLINE BANKING"),
                "output should contain Online Banking menu");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */
/* Test 39 — Logout returns to main menu (Online Banking)             */
/* ------------------------------------------------------------------ */

static void test_39_online_logout_returns_to_main(void)
{
    Application *app = create_test_app();
    TEST_ASSERT(app != NULL, "app should be created");

    const char *inputs[] = {"2", "1002", "5", "3"};
    char output[8192];
    memset(output, 0, sizeof(output));

    MockIO mock;
    memset(&mock, 0, sizeof(mock));
    mock.inputs = inputs;
    mock.input_count = 4;
    mock.output_buffer = output;
    mock.output_size = sizeof(output);

    CliIO io;
    io.read_line = mock_read_line;
    io.write = mock_write;
    io.ctx = &mock;

    cli_run_with_io(app, &io);

    TEST_ASSERT(output_contains(&mock, "Logged out"),
                "output should contain Logged out");
    TEST_ASSERT(output_contains(&mock, "Select an option:"),
                "output should return to main menu after logout");

    destroy_test_app(app);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Tests 1-15: existing M5.2 tests (updated for new menu) */
    RUN_TEST(test_1_valid_main_menu_selection);
    RUN_TEST(test_2_invalid_main_menu_selection);
    RUN_TEST(test_3_atm_channel_selection);
    RUN_TEST(test_4_online_banking_channel_selection);
    RUN_TEST(test_5_valid_account_selection);
    RUN_TEST(test_6_invalid_account_id);
    RUN_TEST(test_7_non_numeric_account_input);
    RUN_TEST(test_8_empty_input);
    RUN_TEST(test_9_excessively_long_input);
    RUN_TEST(test_10_successful_session_creation);
    RUN_TEST(test_11_correct_channel_stored_in_session);
    RUN_TEST(test_12_session_termination);
    RUN_TEST(test_13_return_to_main_menu);
    RUN_TEST(test_14_eof_handling);
    RUN_TEST(test_15_multiple_sequential_sessions);

    /* Tests 16-27: ATM functional tests */
    RUN_TEST(test_16_balance_enquiry);
    RUN_TEST(test_17_successful_deposit);
    RUN_TEST(test_18_successful_withdrawal);
    RUN_TEST(test_19_successful_transfer);
    RUN_TEST(test_20_insufficient_funds_withdrawal);
    RUN_TEST(test_21_insufficient_funds_transfer);
    RUN_TEST(test_22_invalid_destination_account);
    RUN_TEST(test_23_self_transfer);
    RUN_TEST(test_24_zero_value_operation);
    RUN_TEST(test_25_invalid_amount);
    RUN_TEST(test_26_repeated_operations);
    RUN_TEST(test_27_atm_session_rejected_by_online_banking);

    /* Tests 28-39: Online Banking functional tests */
    RUN_TEST(test_28_online_balance_enquiry);
    RUN_TEST(test_29_online_deposit);
    RUN_TEST(test_30_online_withdrawal);
    RUN_TEST(test_31_online_transfer);
    RUN_TEST(test_32_online_insufficient_funds_withdrawal);
    RUN_TEST(test_33_online_invalid_destination);
    RUN_TEST(test_34_online_self_transfer);
    RUN_TEST(test_35_online_invalid_amount);
    RUN_TEST(test_36_online_zero_amount);
    RUN_TEST(test_37_online_repeated_operations);
    RUN_TEST(test_38_online_session_rejected_by_atm);
    RUN_TEST(test_39_online_logout_returns_to_main);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
