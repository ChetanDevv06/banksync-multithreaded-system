#ifndef MONEY_UTILS_H
#define MONEY_UTILS_H

#include <stdint.h>
#include <stddef.h>

/*
 * money_parse_paise — parse a user-entered rupee amount into integer paise.
 *
 * Never uses floating-point arithmetic. Accepts decimal amounts with at most
 * two decimal places (e.g. "1250.50" → 125050 paise, "1250" → 125000 paise).
 *
 * Returns 1 on success with *out_paise set, or 0 on failure for:
 *   - NULL or empty input
 *   - Negative amounts
 *   - Non-numeric characters
 *   - Multiple decimal points
 *   - More than two decimal digits
 *   - Integer overflow (value exceeds INT64_MAX paise)
 *
 * Whole-rupee inputs such as "1250" are accepted and treated as 125000 paise.
 * Zero is accepted by the parser; callers may reject zero where applicable.
 */
int money_parse_paise(const char *str, int64_t *out_paise);

/*
 * money_format_rupees — format an integer paise value as a rupee display
 * string (e.g. 125050 → "₹1,250.50").
 *
 * Returns the number of characters written (excluding NUL), or a negative
 * value on failure (e.g. buffer too small).
 */
int money_format_rupees(int64_t paise, char *buf, size_t size);

#endif /* MONEY_UTILS_H */
