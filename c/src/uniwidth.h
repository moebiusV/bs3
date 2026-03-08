#ifndef UNIWIDTH_H
#define UNIWIDTH_H

#include <wchar.h>

/* Display width of a single Unicode codepoint.
 *
 * Uses wcwidth() as baseline, with overrides:
 *   - Emoji symbols (So category) at U+2600+ forced to 2
 *   - Variation selectors U+FE00-FE0F forced to 1
 *   - Control chars and invalid return 0
 */
int char_display_width(wchar_t ch);

/* Display width of a UTF-8 string */
int str_display_width(const char *utf8);

/* Truncate UTF-8 string to fit within max_w display columns.
 * Returns a newly allocated string. */
char *str_truncate_to_width(const char *utf8, int max_w);

/* Return pointer into utf8 after skipping cols display columns.
 * Returns pointer into the original string (not a copy). */
const char *str_skip_display_cols(const char *utf8, int cols);

#endif
