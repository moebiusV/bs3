/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef UI_H
#define UI_H

#include <ncursesw/curses.h>
#include "browser.h"

/* Initialize CGA color pairs */
void ui_init_colors(void);

/* Safe addstr that truncates to screen width and doesn't crash at edges */
void ui_safe_addstr(WINDOW *w, int y, int x, const char *s, int attr);

/* Draw a string filling the full row width with attr, padding with spaces */
void ui_safe_addstr_full(WINDOW *w, int y, int x, const char *s, int attr, int width);

/* Main view drawing */
void ui_draw_tables_view(Browser *b, WINDOW *w, int height, int width);
void ui_draw_rows_view(Browser *b, WINDOW *w, int height, int width);
void ui_draw_fields_view(Browser *b, WINDOW *w, int height, int width);

/* Overlays */
void ui_draw_status_bar(Browser *b, WINDOW *w, int height, int width);
void ui_draw_help_overlay(Browser *b, WINDOW *w, int height, int width);
void ui_draw_sort_overlay(Browser *b, WINDOW *w, int height, int width);
void ui_draw_find_bar(Browser *b, WINDOW *w, int height, int width);
void ui_draw_command_bar(Browser *b, WINDOW *w, int height, int width);
void ui_draw_prompt_bar(Browser *b, WINDOW *w, int height, int width);
void ui_draw_find_dialog(Browser *b, WINDOW *w, int height, int width);

/* Partial redraws */
void ui_draw_single_table_row(Browser *b, WINDOW *w, int idx, int height, int width);
void ui_draw_single_row(Browser *b, WINDOW *w, int idx, int height, int width);

#endif
