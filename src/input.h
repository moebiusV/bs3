/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef INPUT_H
#define INPUT_H

#include <ncursesw/curses.h>
#include "browser.h"

/* Per-view input handlers */
void input_handle_tables(Browser *b, int key, int height);
void input_handle_rows(Browser *b, int key, int height);
void input_handle_edit(Browser *b, WINDOW *w, int key, int height, int width);

/* Modal input handlers */
void input_handle_command(Browser *b, int key);
void input_handle_prompt(Browser *b, int key);
void input_handle_find(Browser *b, int key);
void input_handle_help(Browser *b, int key, int height);
void input_handle_sort(Browser *b, int key, int height);
void input_handle_find_dialog(Browser *b, int key);

/* Readline-style inline field editor.
 * Returns new value (caller owns) or NULL if cancelled. */
char *input_edit_field(WINDOW *w, int y, int x, int max_width,
                       const char *initial, int attr);

/* Launch external editor on a value, return edited string or NULL */
char *input_external_editor(const char *value);

/* Launch pager on a value */
void input_view_in_pager(const char *value);

#endif
