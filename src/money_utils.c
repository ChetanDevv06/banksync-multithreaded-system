#include "money_utils.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * money_parse_paise — parse a rupee amount string into integer paise without
 * using floating-point arithmetic.
 *
 * Algorithm:
 *   1. Trim leading/trailing whitespace.
 *   2. Reject negative amounts.
 *   3. Split on optional single decimal point.
 *   4. Validate integer part is all digits.
 *   5. Validate fractional part is 1 or 2 digits.
 *   6. Parse integer part with strtoll and check overflow before multiplying
 *      by 100.
 *   7. Add fractional paise (1 or 2 digits).
 */
int money_parse_paise(const char *str, int64_t *out_paise)
{
    if (str == NULL || *str == '\0' || out_paise == NULL) {
        return 0;
    }

    /* Trim leading whitespace. */
    while (isspace((unsigned char)*str)) {
        str++;
    }
    if (*str == '\0') {
        return 0;
    }

    /* Trim trailing whitespace by finding the effective end. */
    const char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    /* Reject negative amounts. */
    if (*str == '-') {
        return 0;
    }

    /* Skip optional leading plus. */
    if (*str == '+') {
        str++;
    }

    /* Locate at most one decimal point; reject any non-digit character. */
    const char *dot = NULL;
    for (const char *p = str; p <= end; p++) {
        if (*p == '.') {
            if (dot != NULL) {
                return 0; /* multiple decimal points */
            }
            dot = p;
        } else if (!isdigit((unsigned char)*p)) {
            return 0; /* non-digit character */
        }
    }

    const size_t int_len = (dot != NULL)
        ? (size_t)(dot - str)
        : (size_t)(end - str + 1);
    const char *frac_start = (dot != NULL) ? dot + 1 : NULL;
    const size_t frac_len = (dot != NULL) ? (size_t)(end - dot) : 0;

    if (int_len == 0) {
        return 0; /* no integer part (e.g. ".50") */
    }
    if (frac_len > 2) {
        return 0; /* more than two decimal places */
    }

    /* Parse the integer part into a temporary buffer. */
    char int_buf[32];
    if (int_len >= sizeof(int_buf)) {
        return 0;
    }
    memcpy(int_buf, str, int_len);
    int_buf[int_len] = '\0';

    char *int_end = NULL;
    long long int_val = strtoll(int_buf, &int_end, 10);
    if (int_end == NULL || *int_end != '\0') {
        return 0;
    }
    if (int_val < 0) {
        return 0;
    }

    /* Check for overflow before multiplying by 100. */
    if ((int64_t)int_val > (INT64_MAX - 99) / 100) {
        return 0;
    }
    int64_t paise = (int64_t)int_val * 100;

    /* Add fractional paise (1 or 2 digits). */
    if (frac_len > 0) {
        int frac_val = 0;
        for (size_t i = 0; i < frac_len; i++) {
            frac_val = frac_val * 10 + (frac_start[i] - '0');
        }
        /* Pad a single-digit fraction to two places (e.g. ".5" → 50 paise). */
        if (frac_len == 1) {
            frac_val *= 10;
        }
        paise += (int64_t)frac_val;
    }

    *out_paise = paise;
    return 1;
}

/*
 * money_format_rupees — format an integer paise value as a rupee string with
 * comma thousands separators (e.g. 125050 → "₹1,250.50").
 */
int money_format_rupees(int64_t paise, char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return -1;
    }

    const int negative = paise < 0;
    const int64_t abs_paise = negative ? -paise : paise;

    const int64_t rupees = abs_paise / 100;
    const int cents = (int)(abs_paise % 100);

    /* Build the rupees portion with comma thousands separators. */
    char rupees_buf[32];
    int rupees_len = 0;

    if (rupees == 0) {
        rupees_buf[rupees_len++] = '0';
    } else {
        char tmp[32];
        int tmp_len = 0;
        int64_t remaining = rupees;
        while (remaining > 0) {
            tmp[tmp_len++] = (char)('0' + (int)(remaining % 10));
            remaining /= 10;
        }
        /* Reverse into rupees_buf inserting commas every 3 digits. */
        for (int i = 0; i < tmp_len; i++) {
            if (i > 0 && (tmp_len - i) % 3 == 0) {
                rupees_buf[rupees_len++] = ',';
            }
            rupees_buf[rupees_len++] = tmp[tmp_len - 1 - i];
        }
    }
    rupees_buf[rupees_len] = '\0';

    int needed;
    if (negative) {
        needed = snprintf(buf, size, "-₹%s.%02d", rupees_buf, cents);
    } else {
        needed = snprintf(buf, size, "₹%s.%02d", rupees_buf, cents);
    }

    if (needed < 0) {
        return -1;
    }
    if ((size_t)needed >= size) {
        return -1;
    }
    return needed;
}
