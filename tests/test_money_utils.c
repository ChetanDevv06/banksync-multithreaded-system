#include "money_utils.h"

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
        fflush(stdout);                                                     \
        name();                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test 1 — Whole-rupee amount                                        */
/* ------------------------------------------------------------------ */

static void test_1_whole_rupee(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("1250", &paise) == 1, "whole rupee should parse");
    TEST_ASSERT(paise == 125000LL, "1250 rupees should be 125000 paise");
}

/* ------------------------------------------------------------------ */
/* Test 2 — Amount with one decimal digit                             */
/* ------------------------------------------------------------------ */

static void test_2_one_decimal_digit(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("1250.5", &paise) == 1, "one decimal digit should parse");
    TEST_ASSERT(paise == 125050LL, "1250.5 should be 125050 paise");
}

/* ------------------------------------------------------------------ */
/* Test 3 — Amount with two decimal digits                            */
/* ------------------------------------------------------------------ */

static void test_3_two_decimal_digits(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("1250.50", &paise) == 1, "two decimal digits should parse");
    TEST_ASSERT(paise == 125050LL, "1250.50 should be 125050 paise");
}

/* ------------------------------------------------------------------ */
/* Test 4 — Zero                                                      */
/* ------------------------------------------------------------------ */

static void test_4_zero(void)
{
    int64_t paise = -1;
    TEST_ASSERT(money_parse_paise("0", &paise) == 1, "zero should parse");
    TEST_ASSERT(paise == 0LL, "zero should be 0 paise");
}

/* ------------------------------------------------------------------ */
/* Test 5 — Negative amount                                           */
/* ------------------------------------------------------------------ */

static void test_5_negative_amount(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("-100", &paise) == 0, "negative should be rejected");
    TEST_ASSERT(money_parse_paise("-0.01", &paise) == 0, "negative decimal should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 6 — Empty input                                               */
/* ------------------------------------------------------------------ */

static void test_6_empty_input(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("", &paise) == 0, "empty should be rejected");
    TEST_ASSERT(money_parse_paise("   ", &paise) == 0, "whitespace-only should be rejected");
    TEST_ASSERT(money_parse_paise(NULL, &paise) == 0, "NULL should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 7 — Non-numeric input                                         */
/* ------------------------------------------------------------------ */

static void test_7_non_numeric_input(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("abc", &paise) == 0, "non-numeric should be rejected");
    TEST_ASSERT(money_parse_paise("12.34.56", &paise) == 0, "mixed should be rejected");
    TEST_ASSERT(money_parse_paise("12a50", &paise) == 0, "embedded letters should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 8 — Multiple decimal points                                   */
/* ------------------------------------------------------------------ */

static void test_8_multiple_decimal_points(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("1.2.3", &paise) == 0, "multiple dots should be rejected");
    TEST_ASSERT(money_parse_paise("10..50", &paise) == 0, "double dot should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 9 — More than two decimal digits                              */
/* ------------------------------------------------------------------ */

static void test_9_more_than_two_decimals(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("1250.500", &paise) == 0, "three decimals should be rejected");
    TEST_ASSERT(money_parse_paise("0.123", &paise) == 0, "three decimals should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 10 — Extremely large amount                                   */
/* ------------------------------------------------------------------ */

static void test_10_extremely_large(void)
{
    int64_t paise = 0;
    TEST_ASSERT(money_parse_paise("9999999999999999", &paise) == 1,
                "large amount should parse");
    TEST_ASSERT(paise == 999999999999999900LL,
                "large amount should be correct paise");
}

/* ------------------------------------------------------------------ */
/* Test 11 — Integer overflow                                         */
/* ------------------------------------------------------------------ */

static void test_11_integer_overflow(void)
{
    int64_t paise = 0;
    /* INT64_MAX / 100 rupees is about 9.2e17, so this exceeds it. */
    TEST_ASSERT(money_parse_paise("99999999999999999999", &paise) == 0,
                "overflow should be rejected");
}

/* ------------------------------------------------------------------ */
/* Test 12 — Excessively long input                                   */
/* ------------------------------------------------------------------ */

static void test_12_excessively_long_input(void)
{
    int64_t paise = 0;
    char long_input[1024];
    memset(long_input, '9', sizeof(long_input) - 1);
    long_input[sizeof(long_input) - 1] = '\0';
    TEST_ASSERT(money_parse_paise(long_input, &paise) == 0,
                "excessively long input should be rejected");
}

/* ------------------------------------------------------------------ */
/* Formatting tests                                                   */
/* ------------------------------------------------------------------ */

static void test_format_standard(void)
{
    char buf[64];
    int n = money_format_rupees(125050, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "format should succeed");
    TEST_ASSERT(strcmp(buf, "₹1,250.50") == 0, "format should match ₹1,250.50");
}

static void test_format_whole_rupees(void)
{
    char buf[64];
    int n = money_format_rupees(125000, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "format should succeed");
    TEST_ASSERT(strcmp(buf, "₹1,250.00") == 0, "format should match ₹1,250.00");
}

static void test_format_sub_rupee(void)
{
    char buf[64];
    int n = money_format_rupees(50, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "format should succeed");
    TEST_ASSERT(strcmp(buf, "₹0.50") == 0, "format should match ₹0.50");
}

static void test_format_large(void)
{
    char buf[64];
    int n = money_format_rupees(123456789LL, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "format should succeed");
    TEST_ASSERT(strcmp(buf, "₹1,234,567.89") == 0, "large format should match");
}

static void test_format_buffer_too_small(void)
{
    char buf[4];
    int n = money_format_rupees(125050, buf, sizeof(buf));
    TEST_ASSERT(n < 0, "small buffer should fail");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Parsing tests (1-12) */
    RUN_TEST(test_1_whole_rupee);
    RUN_TEST(test_2_one_decimal_digit);
    RUN_TEST(test_3_two_decimal_digits);
    RUN_TEST(test_4_zero);
    RUN_TEST(test_5_negative_amount);
    RUN_TEST(test_6_empty_input);
    RUN_TEST(test_7_non_numeric_input);
    RUN_TEST(test_8_multiple_decimal_points);
    RUN_TEST(test_9_more_than_two_decimals);
    RUN_TEST(test_10_extremely_large);
    RUN_TEST(test_11_integer_overflow);
    RUN_TEST(test_12_excessively_long_input);

    /* Formatting tests */
    RUN_TEST(test_format_standard);
    RUN_TEST(test_format_whole_rupees);
    RUN_TEST(test_format_sub_rupee);
    RUN_TEST(test_format_large);
    RUN_TEST(test_format_buffer_too_small);

    printf("\n==============================\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_run - tests_failed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==============================\n");

    return tests_failed == 0 ? 0 : 1;
}
