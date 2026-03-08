/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "input.h"
#include "ui.h"
#include "uniwidth.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* --- Tables view --- */

void input_handle_tables(Browser *b, int key, int height)
{
    int count = b->ntables;
    int visible = height - 3;
    if (visible <= 0) return;

    switch (key) {
    case 'j': case KEY_DOWN:
        if (b->sel_table < count - 1) {
            b->sel_table++;
            if (b->sel_table >= b->table_scroll + visible)
                b->table_scroll = b->sel_table - visible + 1;
        }
        break;
    case 'k': case KEY_UP:
        if (b->sel_table > 0) {
            b->sel_table--;
            if (b->sel_table < b->table_scroll)
                b->table_scroll = b->sel_table;
        }
        break;
    case 'J': case KEY_NPAGE:
        b->sel_table += visible;
        if (b->sel_table >= count) b->sel_table = count - 1;
        if (b->sel_table < 0) b->sel_table = 0;
        b->table_scroll = b->sel_table - visible + 1;
        if (b->table_scroll < 0) b->table_scroll = 0;
        break;
    case 'K': case KEY_PPAGE:
        b->sel_table -= visible;
        if (b->sel_table < 0) b->sel_table = 0;
        b->table_scroll = b->sel_table;
        break;
    case '\n': case KEY_ENTER: case 'l': case KEY_RIGHT:
        if (count > 0) {
            browser_load_table(b, b->tables[b->sel_table].name);
            b->current_view = VIEW_ROWS;
            b->sel_row = 0;
            b->row_scroll = 0;
            b->row_horiz = 0;
        }
        break;
    case 9: /* Tab */
        if (b->sel_table < count - 1) {
            b->sel_table++;
            if (b->sel_table >= b->table_scroll + visible)
                b->table_scroll = b->sel_table - visible + 1;
        }
        break;
    case 'd': case KEY_DC:
        if (count > 0) {
            if (!b->safe_mode) {
                char *name = xstrdup(b->tables[b->sel_table].name);
                if (db_drop_table(b->db, name) == 0) {
                    Buf msg;
                    buf_init(&msg);
                    buf_printf(&msg, "Dropped table: %s", name);
                    browser_set_message(b, msg.data);
                    buf_free(&msg);
                    db_free_tables(b->tables, b->ntables);
                    b->tables = db_get_tables(b->db, &b->ntables);
                    if (b->sel_table >= b->ntables)
                        b->sel_table = b->ntables > 0 ? b->ntables - 1 : 0;
                } else {
                    char *m = db_readonly_message(b->db);
                    browser_set_message(b, m);
                    free(m);
                }
                free(name);
            } else {
                b->prompt_mode = 1;
                snprintf(b->prompt_text, sizeof(b->prompt_text),
                         " Drop table '%s'? (y/n) ", b->tables[b->sel_table].name);
                /* Store action for prompt handler */
                b->prompt_action = NULL; /* handled inline in prompt handler */
            }
        }
        break;
    case 'o':
        browser_open_sort(b);
        break;
    case '/':
        b->find_mode = 1;
        b->find_input[0] = '\0';
        b->find_input_len = 0;
        b->find_input_pos = 0;
        break;
    case ':':
        b->command_mode = 1;
        b->command_input[0] = '\0';
        b->command_len = 0;
        break;
    case KEY_F(1):
        b->help_mode = !b->help_mode;
        b->help_scroll = 0;
        break;
    case 'q': case 27: /* Esc */
        b->quit_flag = 1;
        break;
    }
}

/* --- Rows view --- */

void input_handle_rows(Browser *b, int key, int height)
{
    int count = b->rowset.nrows;
    int visible = height - 3;
    if (visible <= 0) return;

    switch (key) {
    case 'j': case KEY_DOWN:
        if (b->sel_row < count - 1) {
            b->sel_row++;
            if (b->sel_row >= b->row_scroll + visible)
                b->row_scroll = b->sel_row - visible + 1;
        }
        break;
    case 'k': case KEY_UP:
        if (b->sel_row > 0) {
            b->sel_row--;
            if (b->sel_row < b->row_scroll)
                b->row_scroll = b->sel_row;
        }
        break;
    case 'J': case KEY_NPAGE:
        b->sel_row += visible;
        if (b->sel_row >= count) b->sel_row = count - 1;
        if (b->sel_row < 0) b->sel_row = 0;
        b->row_scroll = b->sel_row - visible + 1;
        if (b->row_scroll < 0) b->row_scroll = 0;
        break;
    case 'K': case KEY_PPAGE:
        b->sel_row -= visible;
        if (b->sel_row < 0) b->sel_row = 0;
        b->row_scroll = b->sel_row;
        break;
    case '\n': case KEY_ENTER: case 'l': case KEY_RIGHT:
        if (count > 0) {
            /* Enter edit view */
            b->current_view = VIEW_EDIT;
            b->sel_field = 0;
            b->field_scroll = 0;
            /* Copy current row values for editing */
            Row *r = &b->rowset.rows[b->sel_row];
            b->edit_values = xcalloc((size_t)b->ncols, sizeof(char *));
            b->edit_original = xcalloc((size_t)b->ncols, sizeof(char *));
            for (int i = 0; i < b->ncols; i++) {
                b->edit_values[i] = r->values[i] ? xstrdup(r->values[i]) : NULL;
                b->edit_original[i] = r->values[i] ? xstrdup(r->values[i]) : NULL;
            }
        }
        break;
    case KEY_BACKSPACE: case 127: case 'h': case KEY_LEFT:
        /* Go back to tables view */
        b->current_view = VIEW_TABLES;
        b->find_filter[0] = '\0';
        free(b->find_where);
        b->find_where = NULL;
        break;
    case 9: /* Tab */
        if (b->sel_row < count - 1) {
            b->sel_row++;
            if (b->sel_row >= b->row_scroll + visible)
                b->row_scroll = b->sel_row - visible + 1;
        }
        break;
    case 'd': case KEY_DC:
        if (count > 0) {
            if (!b->safe_mode) {
                long long rowid = b->rowset.rows[b->sel_row].rowid;
                if (db_delete_row(b->db, b->current_table, rowid) != 0) {
                    char *m = db_readonly_message(b->db);
                    browser_set_message(b, m);
                    free(m);
                } else {
                    browser_load_table(b, b->current_table);
                    if (b->sel_row >= b->rowset.nrows)
                        b->sel_row = b->rowset.nrows > 0 ? b->rowset.nrows - 1 : 0;
                }
            } else {
                b->prompt_mode = 1;
                snprintf(b->prompt_text, sizeof(b->prompt_text),
                         " Delete this row? (y/n) ");
                b->prompt_action = NULL;
            }
        }
        break;
    case KEY_SLEFT:
        if (b->row_horiz > 0)
            b->row_horiz -= 5;
        if (b->row_horiz < 0) b->row_horiz = 0;
        break;
    case KEY_SRIGHT:
        b->row_horiz += 5;
        break;
    case 'o':
        browser_open_sort(b);
        break;
    case 'f':
        browser_enter_find_dialog(b);
        break;
    case '/':
        b->find_mode = 1;
        b->find_input[0] = '\0';
        b->find_input_len = 0;
        b->find_input_pos = 0;
        break;
    case ':':
        b->command_mode = 1;
        b->command_input[0] = '\0';
        b->command_len = 0;
        break;
    case KEY_F(1):
        b->help_mode = !b->help_mode;
        b->help_scroll = 0;
        break;
    case 'q': case 27:
        b->quit_flag = 1;
        break;
    }
}

/* --- Edit view --- */

void input_handle_edit(Browser *b, WINDOW *w, int key, int height, int width)
{
    int visible = height - 3;
    if (visible <= 0) return;
    (void)w;
    (void)width;

    switch (key) {
    case 'j': case KEY_DOWN:
        if (b->sel_field < b->ncols - 1) {
            b->sel_field++;
            if (b->sel_field >= b->field_scroll + visible)
                b->field_scroll = b->sel_field - visible + 1;
        }
        break;
    case 'k': case KEY_UP:
        if (b->sel_field > 0) {
            b->sel_field--;
            if (b->sel_field < b->field_scroll)
                b->field_scroll = b->sel_field;
        }
        break;
    case 'J': case KEY_NPAGE:
        b->sel_field += visible;
        if (b->sel_field >= b->ncols) b->sel_field = b->ncols - 1;
        b->field_scroll = b->sel_field - visible + 1;
        if (b->field_scroll < 0) b->field_scroll = 0;
        break;
    case 'K': case KEY_PPAGE:
        b->sel_field -= visible;
        if (b->sel_field < 0) b->sel_field = 0;
        b->field_scroll = b->sel_field;
        break;
    case 9: /* Tab */
        if (b->sel_field < b->ncols - 1) {
            b->sel_field++;
            if (b->sel_field >= b->field_scroll + visible)
                b->field_scroll = b->sel_field - visible + 1;
        }
        break;
    case 's': {
        /* Save changes */
        Row *r = &b->rowset.rows[b->sel_row];
        int rc = db_update_row(b->db, b->current_table,
                               (const char **)b->current_columns, b->ncols,
                               (const char **)b->edit_values, r->rowid);
        if (rc == 0) {
            browser_set_message(b, "Changes saved.");
            browser_load_table(b, b->current_table);
            /* Re-enter edit for same row */
            if (b->sel_row < b->rowset.nrows) {
                Row *nr = &b->rowset.rows[b->sel_row];
                for (int i = 0; i < b->ncols; i++) {
                    free(b->edit_values[i]);
                    free(b->edit_original[i]);
                    b->edit_values[i] = nr->values[i] ? xstrdup(nr->values[i]) : NULL;
                    b->edit_original[i] = nr->values[i] ? xstrdup(nr->values[i]) : NULL;
                }
            }
        } else {
            char *m = db_readonly_message(b->db);
            if (!b->message)
                browser_set_message(b, m);
            free(m);
        }
        break;
    }
    case KEY_BACKSPACE: case 127: case 'h': case KEY_LEFT:
        /* Go back to rows view */
        b->current_view = VIEW_ROWS;
        /* Free edit state */
        if (b->edit_values) {
            for (int i = 0; i < b->ncols; i++) free(b->edit_values[i]);
            free(b->edit_values);
            b->edit_values = NULL;
        }
        if (b->edit_original) {
            for (int i = 0; i < b->ncols; i++) free(b->edit_original[i]);
            free(b->edit_original);
            b->edit_original = NULL;
        }
        break;
    case 'E': {
        /* External editor */
        const char *val = b->edit_values[b->sel_field];
        char *newval = input_external_editor(val);
        if (newval) {
            free(b->edit_values[b->sel_field]);
            b->edit_values[b->sel_field] = newval;
        }
        break;
    }
    case 'v': case '\n': case KEY_ENTER:
        /* View in pager */
        if (b->edit_values[b->sel_field])
            input_view_in_pager(b->edit_values[b->sel_field]);
        break;
    case 'd': case KEY_DC:
        /* Set to NULL */
        if (!b->safe_mode) {
            free(b->edit_values[b->sel_field]);
            b->edit_values[b->sel_field] = NULL;
        } else {
            b->prompt_mode = 1;
            snprintf(b->prompt_text, sizeof(b->prompt_text),
                     " Set field to NULL? (y/n) ");
            b->prompt_action = NULL;
        }
        break;
    case 'o':
        browser_open_sort(b);
        break;
    case ':':
        b->command_mode = 1;
        b->command_input[0] = '\0';
        b->command_len = 0;
        break;
    case KEY_F(1):
        b->help_mode = !b->help_mode;
        b->help_scroll = 0;
        break;
    case 'q': case 27:
        b->quit_flag = 1;
        break;
    }
}

/* --- Command mode --- */

void input_handle_command(Browser *b, int key)
{
    switch (key) {
    case '\n': case KEY_ENTER: {
        /* Execute command */
        if (strcmp(b->command_input, "unsafe") == 0) {
            b->safe_mode = 0;
            browser_set_message(b, "Safety mode OFF - destructive actions enabled");
        } else if (strcmp(b->command_input, "safe") == 0) {
            b->safe_mode = 1;
            browser_set_message(b, "Safety mode ON - destructive actions require confirmation");
        } else {
            Buf msg;
            buf_init(&msg);
            buf_printf(&msg, "Unknown command: %s", b->command_input);
            browser_set_message(b, msg.data);
            buf_free(&msg);
        }
        b->command_mode = 0;
        break;
    }
    case 27: /* Esc */
    case 7:  /* ^G */
        b->command_mode = 0;
        break;
    case KEY_BACKSPACE: case 127:
        if (b->command_len > 0)
            b->command_input[--b->command_len] = '\0';
        else
            b->command_mode = 0;
        break;
    default:
        if (key >= 32 && key <= 126 && b->command_len < (int)sizeof(b->command_input) - 1) {
            b->command_input[b->command_len++] = (char)key;
            b->command_input[b->command_len] = '\0';
        }
        break;
    }
}

/* --- Prompt mode --- */

void input_handle_prompt(Browser *b, int key)
{
    switch (key) {
    case 'y': case '\n': case KEY_ENTER:
        if (b->prompt_action)
            b->prompt_action(b);
        b->prompt_mode = 0;
        b->prompt_action = NULL;
        break;
    case 'n': case 7: case 27: /* n, ^G, Esc */
        b->prompt_mode = 0;
        b->prompt_action = NULL;
        browser_set_message(b, "Cancelled.");
        break;
    case 3: /* ^C */
        b->quit_flag = 1;
        break;
    }
}

/* --- Find mode --- */

void input_handle_find(Browser *b, int key)
{
    switch (key) {
    case '\n': case KEY_ENTER: {
        /* Apply filter */
        /* Trim and check for empty/"." to clear */
        const char *text = b->find_input;
        while (*text == ' ') text++;
        if (text[0] == '\0' || (text[0] == '.' && text[1] == '\0')) {
            b->find_filter[0] = '\0';
            free(b->find_where);
            b->find_where = NULL;
            if (b->find_params) {
                for (int i = 0; i < b->nfind_params; i++) free(b->find_params[i]);
                free(b->find_params);
                b->find_params = NULL;
            }
            b->nfind_params = 0;
        } else {
            strncpy(b->find_filter, b->find_input, sizeof(b->find_filter) - 1);
            b->find_filter[sizeof(b->find_filter) - 1] = '\0';
        }
        b->find_mode = 0;
        /* Reload table data with filter if in rows view */
        if (b->current_view == VIEW_ROWS && b->current_table)
            browser_load_table(b, b->current_table);
        b->sel_row = 0;
        b->row_scroll = 0;
        break;
    }
    case 27: /* Esc */
    case 7:  /* ^G */
        b->find_mode = 0;
        break;
    case KEY_BACKSPACE: case 127:
        if (b->find_input_len > 0)
            b->find_input[--b->find_input_len] = '\0';
        break;
    default:
        if (key >= 32 && key <= 126 && b->find_input_len < (int)sizeof(b->find_input) - 1) {
            b->find_input[b->find_input_len++] = (char)key;
            b->find_input[b->find_input_len] = '\0';
        }
        break;
    }
}

/* --- Help mode --- */

void input_handle_help(Browser *b, int key, int height)
{
    (void)height;
    switch (key) {
    case 'j': case KEY_DOWN:
        b->help_scroll++;
        break;
    case 'k': case KEY_UP:
        if (b->help_scroll > 0) b->help_scroll--;
        break;
    case KEY_F(1): case 'q': case 27:
        b->help_mode = 0;
        break;
    }
}

/* --- Sort mode --- */

void input_handle_sort(Browser *b, int key, int height)
{
    (void)height;
    int n = b->nsort_items;
    if (n == 0) {
        if (key == 27 || key == '\n' || key == KEY_ENTER)
            b->sort_mode = 0;
        return;
    }

    if (key == 'q') { b->quit_flag = 1; b->sort_mode = 0; return; }
    if (key == KEY_F(1)) { b->help_mode = 1; b->help_scroll = 0; return; }
    if (key == 3) { b->quit_flag = 1; b->sort_mode = 0; return; }
    if (key == 27) { b->sort_mode = 0; return; }

    if (key == KEY_DOWN || key == 'j') {
        if (b->sort_grabbed >= 0) {
            if (b->sort_context == 'r') {
                /* Move active sort col priority down */
                const char *item = b->sort_items[b->sort_selected];
                int ai = -1;
                for (int i = 0; i < b->nsort_active; i++)
                    if (strcmp(b->sort_active_cols[i], item) == 0) { ai = i; break; }
                if (ai >= 0 && ai < b->nsort_active - 1) {
                    char *tmp;
                    tmp = b->sort_active_cols[ai];
                    b->sort_active_cols[ai] = b->sort_active_cols[ai + 1];
                    b->sort_active_cols[ai + 1] = tmp;
                    tmp = b->sort_directions[ai];
                    b->sort_directions[ai] = b->sort_directions[ai + 1];
                    b->sort_directions[ai + 1] = tmp;
                    browser_apply_sort(b);
                }
            } else {
                int gi = b->sort_grabbed;
                if (gi < n - 1) {
                    char *tmp = b->sort_items[gi];
                    b->sort_items[gi] = b->sort_items[gi + 1];
                    b->sort_items[gi + 1] = tmp;
                    b->sort_grabbed = gi + 1;
                    b->sort_selected = gi + 1;
                    browser_apply_sort(b);
                }
            }
        } else {
            if (b->sort_selected < n - 1)
                b->sort_selected++;
        }
    } else if (key == KEY_UP || key == 'k') {
        if (b->sort_grabbed >= 0) {
            if (b->sort_context == 'r') {
                const char *item = b->sort_items[b->sort_selected];
                int ai = -1;
                for (int i = 0; i < b->nsort_active; i++)
                    if (strcmp(b->sort_active_cols[i], item) == 0) { ai = i; break; }
                if (ai > 0) {
                    char *tmp;
                    tmp = b->sort_active_cols[ai];
                    b->sort_active_cols[ai] = b->sort_active_cols[ai - 1];
                    b->sort_active_cols[ai - 1] = tmp;
                    tmp = b->sort_directions[ai];
                    b->sort_directions[ai] = b->sort_directions[ai - 1];
                    b->sort_directions[ai - 1] = tmp;
                    browser_apply_sort(b);
                }
            } else {
                int gi = b->sort_grabbed;
                if (gi > 0) {
                    char *tmp = b->sort_items[gi];
                    b->sort_items[gi] = b->sort_items[gi - 1];
                    b->sort_items[gi - 1] = tmp;
                    b->sort_grabbed = gi - 1;
                    b->sort_selected = gi - 1;
                    browser_apply_sort(b);
                }
            }
        } else {
            if (b->sort_selected > 0)
                b->sort_selected--;
        }
    } else if (key == ' ' || key == '\n' || key == KEY_ENTER) {
        if (b->sort_context == 'r') {
            /* Toggle sort field */
            const char *item = b->sort_items[b->sort_selected];
            int ai = -1;
            for (int i = 0; i < b->nsort_active; i++)
                if (strcmp(b->sort_active_cols[i], item) == 0) { ai = i; break; }
            if (ai >= 0) {
                /* Remove */
                free(b->sort_active_cols[ai]);
                free(b->sort_directions[ai]);
                for (int i = ai; i < b->nsort_active - 1; i++) {
                    b->sort_active_cols[i] = b->sort_active_cols[i + 1];
                    b->sort_directions[i] = b->sort_directions[i + 1];
                }
                b->nsort_active--;
            } else {
                /* Add */
                b->sort_active_cols = xrealloc(b->sort_active_cols,
                    (size_t)(b->nsort_active + 1) * sizeof(char *));
                b->sort_directions = xrealloc(b->sort_directions,
                    (size_t)(b->nsort_active + 1) * sizeof(char *));
                b->sort_active_cols[b->nsort_active] = xstrdup(item);
                b->sort_directions[b->nsort_active] = xstrdup("ASC");
                b->nsort_active++;
            }
        } else {
            /* Grab/drop */
            if (b->sort_grabbed >= 0)
                b->sort_grabbed = -1;
            else
                b->sort_grabbed = b->sort_selected;
        }
        browser_apply_sort(b);
    } else if (key == 'a' && b->sort_context == 'r') {
        const char *item = b->sort_items[b->sort_selected];
        for (int i = 0; i < b->nsort_active; i++) {
            if (strcmp(b->sort_active_cols[i], item) == 0) {
                free(b->sort_directions[i]);
                b->sort_directions[i] = xstrdup("ASC");
                browser_apply_sort(b);
                break;
            }
        }
    } else if (key == 'd' && b->sort_context == 'r') {
        const char *item = b->sort_items[b->sort_selected];
        for (int i = 0; i < b->nsort_active; i++) {
            if (strcmp(b->sort_active_cols[i], item) == 0) {
                free(b->sort_directions[i]);
                b->sort_directions[i] = xstrdup("DESC");
                browser_apply_sort(b);
                break;
            }
        }
    } else if (key == 'r') {
        browser_reset_sort(b);
    }

    /* Keep scroll in bounds */
    if (b->sort_selected < b->sort_scroll)
        b->sort_scroll = b->sort_selected;
    else if (b->sort_selected >= b->sort_scroll + 20)
        b->sort_scroll = b->sort_selected - 19;
}

/* --- Find dialog --- */

void input_handle_find_dialog(Browser *b, int key)
{
    if (key == 27) { /* Esc - cancel */
        browser_free_find_dialog(b);
        return;
    }
    if (key == '\n' || key == KEY_ENTER) {
        browser_apply_find_dialog(b);
        browser_free_find_dialog(b);
        return;
    }
    if (key == 3) { /* ^C */
        b->quit_flag = 1;
        return;
    }
    if (key == 9) { /* Tab - next field */
        b->find_dialog_focus = (b->find_dialog_focus + 1) % b->ncols;
    } else if (key == KEY_BTAB) { /* Shift+Tab */
        b->find_dialog_focus = (b->find_dialog_focus - 1 + b->ncols) % b->ncols;
    } else if (key == KEY_DOWN) {
        if (b->find_dialog_focus < b->ncols - 1)
            b->find_dialog_focus++;
    } else if (key == KEY_UP) {
        if (b->find_dialog_focus > 0)
            b->find_dialog_focus--;
    } else if (key == KEY_BACKSPACE || key == 127) {
        int idx = b->find_dialog_focus;
        char *val = b->find_dialog_inputs[idx];
        if (val && val[0]) {
            size_t len = strlen(val);
            val[len - 1] = '\0';
        }
    } else if (key >= 32 && key <= 126) {
        int idx = b->find_dialog_focus;
        char *old = b->find_dialog_inputs[idx];
        size_t len = old ? strlen(old) : 0;
        char *newval = xmalloc(len + 2);
        if (old) memcpy(newval, old, len);
        newval[len] = (char)key;
        newval[len + 1] = '\0';
        free(old);
        b->find_dialog_inputs[idx] = newval;
    } else if (key == 1) { /* ^A - toggle AND/OR */
        int idx = b->find_dialog_focus;
        b->find_dialog_and_flags[idx] = !b->find_dialog_and_flags[idx];
    } else if (key == 18) { /* ^R - clear all */
        for (int i = 0; i < b->ncols; i++) {
            free(b->find_dialog_inputs[i]);
            b->find_dialog_inputs[i] = xstrdup("");
            b->find_dialog_and_flags[i] = 0;
        }
    }
}

/* --- External editor --- */

char *input_external_editor(const char *value)
{
    const char *editor = getenv("EDITOR");
    if (!editor) editor = "vi";

    char tmppath[] = "/tmp/bs3-XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return NULL;

    if (value) {
        size_t len = strlen(value);
        if (write(fd, value, len) < 0) { /* ignore */ }
    }
    close(fd);

    /* Leave curses mode */
    def_prog_mode();
    endwin();

    /* Build command */
    Buf cmd;
    buf_init(&cmd);
    /* If editor contains "vi", open at end in insert mode */
    if (strstr(editor, "vi"))
        buf_printf(&cmd, "%s '+normal Go' '%s'", editor, tmppath);
    else
        buf_printf(&cmd, "%s '%s'", editor, tmppath);

    int status = system(cmd.data);
    buf_free(&cmd);

    /* Re-enter curses mode */
    reset_prog_mode();
    refresh();

    if (status != 0) {
        unlink(tmppath);
        return NULL;
    }

    /* Read back */
    FILE *f = fopen(tmppath, "r");
    if (!f) { unlink(tmppath); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *result = xmalloc((size_t)fsize + 1);
    size_t nread = fread(result, 1, (size_t)fsize, f);
    result[nread] = '\0';
    fclose(f);
    unlink(tmppath);

    /* Strip trailing newline */
    while (nread > 0 && result[nread - 1] == '\n')
        result[--nread] = '\0';

    return result;
}

/* --- Pager --- */

void input_view_in_pager(const char *value)
{
    if (!value) return;
    const char *pager = getenv("PAGER");
    if (!pager) pager = "less";

    char tmppath[] = "/tmp/bs3-XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return;
    size_t len = strlen(value);
    if (write(fd, value, len) < 0) { /* ignore */ }
    close(fd);

    def_prog_mode();
    endwin();

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s '%s'", pager, tmppath);
    int rc __attribute__((unused)) = system(cmd.data);
    buf_free(&cmd);

    reset_prog_mode();
    refresh();
    unlink(tmppath);
}
