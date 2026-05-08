/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef UNIWIDTH_H
#define UNIWIDTH_H

#include <wchar.h>

/* Set to 2 if the terminal renders EAW=Ambiguous characters as 2 columns.
 * Call terminal_probe_eaw_width() before initscr() to detect automatically. */
extern int g_eaw_ambiguous_width;

/* Probe the terminal to detect EAW=Ambiguous rendering width (1 or 2).
 * Must be called before initscr() — uses raw terminal I/O directly.
 * Sets g_eaw_ambiguous_width and returns the detected value. */
int terminal_probe_eaw_width(void);

/* Display width of a single Unicode codepoint. */
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
