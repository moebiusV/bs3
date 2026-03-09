/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "browser.h"
#include "ui.h"
#include "input.h"
#include "uniwidth.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

void browser_init(Browser *b, Database *db)
{
    memset(b, 0, sizeof(*b));
    b->db = db;
    b->current_view = VIEW_TABLES;
    b->safe_mode = 1;
    b->sort_grabbed = -1;
    b->tables = db_get_tables(db, &b->ntables);
    db_ensure_config_table(db);
    browser_load_config(b);
}

void browser_destroy(Browser *b)
{
    browser_free_table_data(b);
    browser_free_sort_state(b);
    browser_free_find_dialog(b);
    db_free_tables(b->tables, b->ntables);
    free(b->message);
    free(b->find_where);
    if (b->find_params) {
        for (int i = 0; i < b->nfind_params; i++)
            free(b->find_params[i]);
        free(b->find_params);
    }
    if (b->table_display_order) {
        for (int i = 0; i < b->ntable_order; i++)
            free(b->table_display_order[i]);
        free(b->table_display_order);
    }
}

void browser_free_table_data(Browser *b)
{
    if (b->row_strings) {
        for (int i = 0; i < b->rowset.nrows; i++)
            free(b->row_strings[i]);
        free(b->row_strings);
        b->row_strings = NULL;
    }
    db_free_rowset(&b->rowset);
    if (b->current_columns) {
        for (int i = 0; i < b->ncols; i++)
            free(b->current_columns[i]);
        free(b->current_columns);
        b->current_columns = NULL;
    }
    free(b->current_table);
    b->current_table = NULL;
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
    b->ncols = 0;
}

void browser_load_table(Browser *b, const char *table_name)
{
    /* Copy table_name early — caller may pass b->current_table which
     * gets freed below (use-after-free if we defer the strdup). */
    char *name_copy = xstrdup(table_name);

    /* Free previous data but save ncols for edit cleanup */
    int old_ncols = b->ncols;
    if (b->edit_values) {
        for (int i = 0; i < old_ncols; i++) free(b->edit_values[i]);
        free(b->edit_values);
        b->edit_values = NULL;
    }
    if (b->edit_original) {
        for (int i = 0; i < old_ncols; i++) free(b->edit_original[i]);
        free(b->edit_original);
        b->edit_original = NULL;
    }
    if (b->row_strings) {
        for (int i = 0; i < b->rowset.nrows; i++)
            free(b->row_strings[i]);
        free(b->row_strings);
        b->row_strings = NULL;
    }
    db_free_rowset(&b->rowset);
    if (b->current_columns) {
        for (int i = 0; i < b->ncols; i++)
            free(b->current_columns[i]);
        free(b->current_columns);
    }
    free(b->current_table);

    b->current_table = name_copy;
    /* From here on, use b->current_table — table_name may be dangling */

    /* Find table info to get columns */
    for (int i = 0; i < b->ntables; i++) {
        if (strcmp(b->tables[i].name, b->current_table) == 0) {
            b->ncols = b->tables[i].ncols;
            b->current_columns = xcalloc((size_t)b->ncols, sizeof(char *));
            for (int j = 0; j < b->ncols; j++)
                b->current_columns[j] = xstrdup(b->tables[i].columns[j]);
            break;
        }
    }

    /* Build WHERE clause from find_filter if set */
    free(b->find_where);
    b->find_where = NULL;
    if (b->find_params) {
        for (int i = 0; i < b->nfind_params; i++)
            free(b->find_params[i]);
        free(b->find_params);
        b->find_params = NULL;
    }
    b->nfind_params = 0;

    if (b->find_filter[0] && b->current_view == VIEW_ROWS)
        browser_parse_find_pattern(b, b->find_filter);

    /* Get unfiltered count */
    b->unfilt_row_count = db_row_count(b->db, b->current_table, NULL, NULL, 0);

    /* Build ORDER BY from saved sort config */
    char *order_by = NULL;
    {
        Buf key;
        buf_init(&key);
        buf_printf(&key, "row_sort:%s", b->current_table);
        char *json = db_load_config(b->db, key.data);
        buf_free(&key);
        if (json && json[0] == '[') {
            /* Simple JSON array parser: [["col","ASC"],["col","DESC"]] */
            Buf ob;
            buf_init(&ob);
            const char *p = json + 1; /* skip '[' */
            int first = 1;
            while (*p) {
                if (*p == '[') {
                    p++;
                    /* Parse col name */
                    if (*p == '"') p++;
                    const char *cs = p;
                    while (*p && *p != '"') p++;
                    char *col = xstrndup(cs, (size_t)(p - cs));
                    if (*p == '"') p++;
                    if (*p == ',') p++;
                    /* Parse direction */
                    if (*p == '"') p++;
                    const char *ds = p;
                    while (*p && *p != '"') p++;
                    char *dir = xstrndup(ds, (size_t)(p - ds));
                    if (*p == '"') p++;
                    if (*p == ']') p++;
                    if (*p == ',') p++;
                    /* Verify col exists */
                    int found = 0;
                    for (int i = 0; i < b->ncols; i++) {
                        if (strcmp(b->current_columns[i], col) == 0) { found = 1; break; }
                    }
                    if (found) {
                        if (!first) buf_append_str(&ob, ", ");
                        buf_printf(&ob, "\"%s\" %s", col,
                                   (strcmp(dir, "DESC") == 0) ? "DESC" : "ASC");
                        first = 0;
                    }
                    free(col);
                    free(dir);
                } else {
                    p++;
                }
            }
            if (ob.len > 0) order_by = buf_detach(&ob);
            else buf_free(&ob);
        }
        free(json);
    }

    /* Load rows */
    db_load_rows(b->db, b->current_table,
                 (const char **)b->current_columns, b->ncols,
                 b->find_where,
                 (const char **)b->find_params, b->nfind_params,
                 order_by,
                 &b->rowset);
    free(order_by);

    browser_cache_row_strings(b);

    /* Apply saved column display order if any */
    browser_apply_column_order(b);
}

void browser_cache_row_strings(Browser *b)
{
    if (b->row_strings) {
        for (int i = 0; i < b->rowset.nrows; i++)
            free(b->row_strings[i]);
        free(b->row_strings);
    }
    b->row_strings = xcalloc((size_t)b->rowset.nrows, sizeof(char *));

    for (int i = 0; i < b->rowset.nrows; i++) {
        Row *r = &b->rowset.rows[i];
        Buf line;
        buf_init(&line);
        buf_append_str(&line, " ");
        for (int j = 0; j < r->ncols; j++) {
            if (j > 0) buf_append_str(&line, " | ");
            const char *val = r->values[j];
            if (val == NULL) {
                buf_append_str(&line, "NULL");
            } else {
                /* Collapse whitespace */
                char *clean = str_collapse_whitespace(val);
                buf_append_str(&line, clean);
                free(clean);
            }
        }
        b->row_strings[i] = buf_detach(&line);
    }
}

void browser_apply_column_order(Browser *b)
{
    if (!b->current_table || b->ncols <= 0) return;

    /* Load saved column order */
    Buf key;
    buf_init(&key);
    buf_printf(&key, "column_order:%s", b->current_table);
    char *json = db_load_config(b->db, key.data);
    buf_free(&key);
    if (!json || json[0] != '[') {
        free(json);
        return;
    }

    /* Parse ["col1","col2",...] */
    Vec names;
    vec_init(&names);
    const char *p = json + 1;
    while (*p) {
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            vec_push(&names, xstrndup(start, (size_t)(p - start)));
            if (*p == '"') p++;
        } else {
            p++;
        }
    }
    free(json);

    /* Build permutation: perm[new_idx] = old_idx */
    int *perm = xcalloc((size_t)b->ncols, sizeof(int));
    int valid = 1;
    int used_count = 0;
    for (int i = 0; i < b->ncols; i++) perm[i] = -1;

    for (int i = 0; i < (int)names.len && i < b->ncols; i++) {
        int found = -1;
        for (int j = 0; j < b->ncols; j++) {
            if (strcmp(b->current_columns[j], names.items[i]) == 0) {
                int dup = 0;
                for (int k = 0; k < i; k++)
                    if (perm[k] == j) { dup = 1; break; }
                if (!dup) { found = j; break; }
            }
        }
        if (found >= 0) {
            perm[i] = found;
            used_count++;
        } else {
            valid = 0;
            break;
        }
    }

    /* Must have exactly all columns */
    if (!valid || used_count != b->ncols || (int)names.len != b->ncols) {
        free(perm);
        for (size_t i = 0; i < names.len; i++) free(names.items[i]);
        vec_free(&names);
        return;
    }
    for (size_t i = 0; i < names.len; i++) free(names.items[i]);
    vec_free(&names);

    /* Apply permutation to current_columns */
    char **new_cols = xcalloc((size_t)b->ncols, sizeof(char *));
    for (int i = 0; i < b->ncols; i++)
        new_cols[i] = b->current_columns[perm[i]];
    memcpy(b->current_columns, new_cols, (size_t)b->ncols * sizeof(char *));
    free(new_cols);

    /* Apply permutation to each row's values */
    for (int r = 0; r < b->rowset.nrows; r++) {
        Row *row = &b->rowset.rows[r];
        char **new_vals = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++)
            new_vals[i] = row->values[perm[i]];
        memcpy(row->values, new_vals, (size_t)b->ncols * sizeof(char *));
        free(new_vals);
    }

    free(perm);

    /* Rebuild row_strings with new column order */
    browser_cache_row_strings(b);
}

void browser_populate_edit(Browser *b)
{
    /* Free old edit state */
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
    if (b->sel_row < 0 || b->sel_row >= b->rowset.nrows || b->ncols <= 0)
        return;
    Row *r = &b->rowset.rows[b->sel_row];
    b->edit_values = xcalloc((size_t)b->ncols, sizeof(char *));
    b->edit_original = xcalloc((size_t)b->ncols, sizeof(char *));
    for (int i = 0; i < b->ncols; i++) {
        b->edit_values[i] = r->values[i] ? xstrdup(r->values[i]) : NULL;
        b->edit_original[i] = r->values[i] ? xstrdup(r->values[i]) : NULL;
    }
}

void browser_set_message(Browser *b, const char *msg)
{
    free(b->message);
    b->message = msg ? xstrdup(msg) : NULL;
}

/* --- Main loop --- */

void browser_run(Browser *b, WINDOW *stdscr)
{
    if (has_colors())
        ui_init_colors();
    bkgd(COLOR_PAIR(C_NORMAL));
    curs_set(0);
    erase();

    const double FRAME_SEC = 1.0 / 30.0;
    int prev_view = -1;
    int prev_sel_table = -1;
    int prev_table_scroll = -1;
    int prev_sel_row = -1;
    int prev_row_scroll = -1;
    int prev_row_horiz = -1;
    int full_redraw = 1;

    while (!b->quit_flag) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        int height, width;
        getmaxyx(stdscr, height, width);

        /* --- Partial vs full redraw --- */
        int did_partial = 0;
        if (!full_redraw
            && (int)b->current_view == prev_view
            && !b->find_mode && !b->command_mode
            && !b->prompt_mode && !b->help_mode
            && !b->sort_mode && !b->find_dialog) {

            if (b->current_view == VIEW_ROWS
                && b->row_scroll == prev_row_scroll
                && b->row_horiz == prev_row_horiz
                && b->sel_row != prev_sel_row) {
                ui_draw_single_row(b, stdscr, prev_sel_row, height, width);
                ui_draw_single_row(b, stdscr, b->sel_row, height, width);
                ui_draw_status_bar(b, stdscr, height, width);
                did_partial = 1;
            } else if (b->current_view == VIEW_TABLES
                       && b->table_scroll == prev_table_scroll
                       && b->sel_table != prev_sel_table) {
                ui_draw_single_table_row(b, stdscr, prev_sel_table, height, width);
                ui_draw_single_table_row(b, stdscr, b->sel_table, height, width);
                ui_draw_status_bar(b, stdscr, height, width);
                did_partial = 1;
            }
        }

        if (!did_partial) {
            erase();

            if (b->current_view == VIEW_TABLES)
                ui_draw_tables_view(b, stdscr, height, width);
            else if (b->current_view == VIEW_ROWS)
                ui_draw_rows_view(b, stdscr, height, width);
            else
                ui_draw_fields_view(b, stdscr, height, width);

            if (b->find_mode)
                ui_draw_find_bar(b, stdscr, height, width);
            else if (b->command_mode)
                ui_draw_command_bar(b, stdscr, height, width);
            else if (b->prompt_mode)
                ui_draw_prompt_bar(b, stdscr, height, width);
            else if (b->help_mode)
                ui_draw_help_overlay(b, stdscr, height, width);
            else if (b->sort_mode)
                ui_draw_sort_overlay(b, stdscr, height, width);
            else if (b->find_dialog)
                ui_draw_find_dialog(b, stdscr, height, width);
            else
                curs_set(0);
        }

        if (!did_partial)
            touchwin(stdscr);
        refresh();

        /* Save state for partial redraw detection */
        prev_view = (int)b->current_view;
        prev_sel_table = b->sel_table;
        prev_table_scroll = b->table_scroll;
        prev_sel_row = b->sel_row;
        prev_row_scroll = b->row_scroll;
        prev_row_horiz = b->row_horiz;
        full_redraw = 0;

        /* --- Input with frame pacing --- */
        int key = getch();
        if (key == ERR) continue;
        if (key == 3) { b->quit_flag = 1; continue; } /* ^C */

        /* Drain buffered navigation keys (30fps batching) */
        if (key == KEY_UP || key == KEY_DOWN || key == 'j' || key == 'k' ||
            key == KEY_NPAGE || key == KEY_PPAGE || key == 'J' || key == 'K') {
            nodelay(stdscr, TRUE);
            int net = 0;
            int is_page = (key == KEY_NPAGE || key == KEY_PPAGE || key == 'J' || key == 'K');
            if (key == KEY_DOWN || key == 'j' || key == KEY_NPAGE || key == 'J')
                net = 1;
            else
                net = -1;

            for (;;) {
                int nk = getch();
                if (nk == ERR) break;
                if (nk == KEY_DOWN || nk == 'j' || nk == KEY_NPAGE || nk == 'J')
                    net++;
                else if (nk == KEY_UP || nk == 'k' || nk == KEY_PPAGE || nk == 'K')
                    net--;
                else {
                    ungetch(nk);
                    break;
                }
            }
            nodelay(stdscr, FALSE);

            /* Apply net displacement */
            if (net > 0)
                key = is_page ? 'J' : 'j';
            else if (net < 0)
                key = is_page ? 'K' : 'k';
            else
                continue; /* net zero, skip */
        }

        /* Clear message after display */
        free(b->message);
        b->message = NULL;

        /* Dispatch to modal handlers first */
        if (b->help_mode) {
            input_handle_help(b, key, height);
            full_redraw = 1;
            continue;
        }
        if (b->find_dialog) {
            input_handle_find_dialog(b, key);
            full_redraw = 1;
            continue;
        }
        if (b->sort_mode) {
            input_handle_sort(b, key, height);
            full_redraw = 1;
            continue;
        }
        if (b->prompt_mode) {
            input_handle_prompt(b, key);
            full_redraw = 1;
            continue;
        }
        if (b->command_mode) {
            input_handle_command(b, key);
            full_redraw = 1;
            continue;
        }
        if (b->find_mode) {
            input_handle_find(b, key);
            full_redraw = 1;
            continue;
        }

        /* Dispatch to view handler */
        int old_view = (int)b->current_view;
        if (b->current_view == VIEW_TABLES)
            input_handle_tables(b, key, height);
        else if (b->current_view == VIEW_ROWS)
            input_handle_rows(b, key, height);
        else
            input_handle_fields(b, stdscr, key, height, width);

        if ((int)b->current_view != old_view)
            full_redraw = 1;

        /* Frame pacing */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - frame_start.tv_sec)
                       + (now.tv_nsec - frame_start.tv_nsec) / 1e9;
        if (elapsed < FRAME_SEC) {
            int delay_ms = (int)((FRAME_SEC - elapsed) * 1000);
            if (delay_ms > 0) {
                timeout(delay_ms);
                int peek = getch();
                timeout(-1);
                if (peek != ERR) ungetch(peek);
            }
        }
    }
}

/* --- Sort overlay --- */

void browser_free_sort_state(Browser *b)
{
    if (b->sort_items) {
        for (int i = 0; i < b->nsort_items; i++)
            free(b->sort_items[i]);
        free(b->sort_items);
        b->sort_items = NULL;
    }
    b->nsort_items = 0;
    if (b->sort_active_cols) {
        for (int i = 0; i < b->nsort_active; i++) {
            free(b->sort_active_cols[i]);
            free(b->sort_directions[i]);
        }
        free(b->sort_active_cols);
        free(b->sort_directions);
        b->sort_active_cols = NULL;
        b->sort_directions = NULL;
    }
    b->nsort_active = 0;
}

void browser_open_sort(Browser *b)
{
    browser_free_sort_state(b);

    if (b->current_view == VIEW_TABLES) {
        b->sort_context = 't';
        b->nsort_items = b->ntables;
        b->sort_items = xcalloc((size_t)b->ntables, sizeof(char *));
        for (int i = 0; i < b->ntables; i++)
            b->sort_items[i] = xstrdup(b->tables[i].name);
    } else if (b->current_view == VIEW_ROWS) {
        b->sort_context = 'r';
        b->nsort_items = b->ncols;
        b->sort_items = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++)
            b->sort_items[i] = xstrdup(b->current_columns[i]);
        /* Load existing sort config */
        Buf key;
        buf_init(&key);
        buf_printf(&key, "row_sort:%s", b->current_table);
        char *json = db_load_config(b->db, key.data);
        buf_free(&key);
        if (json && json[0] == '[') {
            /* Parse [[col,dir],...] */
            Vec cols, dirs;
            vec_init(&cols);
            vec_init(&dirs);
            const char *p = json + 1;
            while (*p) {
                if (*p == '[') {
                    p++;
                    if (*p == '"') p++;
                    const char *cs = p;
                    while (*p && *p != '"') p++;
                    char *col = xstrndup(cs, (size_t)(p - cs));
                    if (*p == '"') p++;
                    if (*p == ',') p++;
                    if (*p == '"') p++;
                    const char *ds = p;
                    while (*p && *p != '"') p++;
                    char *dir = xstrndup(ds, (size_t)(p - ds));
                    if (*p == '"') p++;
                    if (*p == ']') p++;
                    if (*p == ',') p++;
                    /* Only add if column exists */
                    int found = 0;
                    for (int i = 0; i < b->ncols; i++)
                        if (strcmp(b->current_columns[i], col) == 0) { found = 1; break; }
                    if (found) {
                        vec_push(&cols, col);
                        vec_push(&dirs, dir);
                    } else {
                        free(col);
                        free(dir);
                    }
                } else {
                    p++;
                }
            }
            b->nsort_active = (int)cols.len;
            b->sort_active_cols = xcalloc(cols.len ? cols.len : 1, sizeof(char *));
            b->sort_directions = xcalloc(cols.len ? cols.len : 1, sizeof(char *));
            for (size_t i = 0; i < cols.len; i++) {
                b->sort_active_cols[i] = cols.items[i];
                b->sort_directions[i] = dirs.items[i];
            }
            vec_free(&cols);
            vec_free(&dirs);
        }
        free(json);
    } else { /* VIEW_FIELDS */
        b->sort_context = 'c';
        b->nsort_items = b->ncols;
        b->sort_items = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++)
            b->sort_items[i] = xstrdup(b->current_columns[i]);
    }

    b->sort_selected = 0;
    b->sort_grabbed = -1;
    b->sort_scroll = 0;
    b->sort_mode = 1;
}

void browser_apply_sort(Browser *b)
{
    if (b->sort_context == 't') {
        /* Save table order as JSON array */
        Buf json;
        buf_init(&json);
        buf_append_char(&json, '[');
        for (int i = 0; i < b->nsort_items; i++) {
            if (i > 0) buf_append_char(&json, ',');
            buf_printf(&json, "\"%s\"", b->sort_items[i]);
        }
        buf_append_char(&json, ']');
        db_save_config(b->db, "table_order", json.data);
        buf_free(&json);
        /* Update table_display_order */
        if (b->table_display_order) {
            for (int i = 0; i < b->ntable_order; i++)
                free(b->table_display_order[i]);
            free(b->table_display_order);
        }
        b->ntable_order = b->nsort_items;
        b->table_display_order = xcalloc((size_t)b->nsort_items, sizeof(char *));
        for (int i = 0; i < b->nsort_items; i++)
            b->table_display_order[i] = xstrdup(b->sort_items[i]);
        /* Reload tables and reorder according to display_order */
        db_free_tables(b->tables, b->ntables);
        b->tables = db_get_tables(b->db, &b->ntables);
        if (b->table_display_order && b->ntable_order > 0) {
            TableInfo *reordered = xcalloc((size_t)b->ntables, sizeof(TableInfo));
            int *used = xcalloc((size_t)b->ntables, sizeof(int));
            int pos = 0;
            /* Place tables in saved order first */
            for (int i = 0; i < b->ntable_order; i++) {
                for (int j = 0; j < b->ntables; j++) {
                    if (!used[j] && strcmp(b->tables[j].name, b->table_display_order[i]) == 0) {
                        reordered[pos++] = b->tables[j];
                        used[j] = 1;
                        break;
                    }
                }
            }
            /* Append any tables not in the saved order */
            for (int j = 0; j < b->ntables; j++) {
                if (!used[j])
                    reordered[pos++] = b->tables[j];
            }
            /* Shallow-copy back (TableInfo structs own their strings) */
            memcpy(b->tables, reordered, (size_t)b->ntables * sizeof(TableInfo));
            free(reordered);
            free(used);
        }
    } else if (b->sort_context == 'c') {
        /* Save column order */
        Buf key, json;
        buf_init(&key);
        buf_printf(&key, "column_order:%s", b->current_table);
        buf_init(&json);
        buf_append_char(&json, '[');
        for (int i = 0; i < b->nsort_items; i++) {
            if (i > 0) buf_append_char(&json, ',');
            buf_printf(&json, "\"%s\"", b->sort_items[i]);
        }
        buf_append_char(&json, ']');
        db_save_config(b->db, key.data, json.data);
        buf_free(&key);
        buf_free(&json);
        /* Apply new column order without full reload (preserves edit state and find filter) */
        browser_apply_column_order(b);
        if (b->current_view == VIEW_FIELDS)
            browser_populate_edit(b);
    } else if (b->sort_context == 'r') {
        /* Save row sort config */
        Buf key, json;
        buf_init(&key);
        buf_printf(&key, "row_sort:%s", b->current_table);
        buf_init(&json);
        buf_append_char(&json, '[');
        for (int i = 0; i < b->nsort_active; i++) {
            if (i > 0) buf_append_char(&json, ',');
            buf_printf(&json, "[\"%s\",\"%s\"]",
                       b->sort_active_cols[i], b->sort_directions[i]);
        }
        buf_append_char(&json, ']');
        db_save_config(b->db, key.data, json.data);
        buf_free(&key);
        buf_free(&json);
        browser_load_table(b, b->current_table);
        b->sel_row = 0;
        b->row_scroll = 0;
    }
}

void browser_reset_sort(Browser *b)
{
    if (b->sort_context == 't') {
        db_save_config(b->db, "table_order", "null");
        if (b->table_display_order) {
            for (int i = 0; i < b->ntable_order; i++)
                free(b->table_display_order[i]);
            free(b->table_display_order);
            b->table_display_order = NULL;
            b->ntable_order = 0;
        }
        db_free_tables(b->tables, b->ntables);
        b->tables = db_get_tables(b->db, &b->ntables);
        /* Refresh sort items */
        for (int i = 0; i < b->nsort_items; i++) free(b->sort_items[i]);
        free(b->sort_items);
        b->nsort_items = b->ntables;
        b->sort_items = xcalloc((size_t)b->ntables, sizeof(char *));
        for (int i = 0; i < b->ntables; i++)
            b->sort_items[i] = xstrdup(b->tables[i].name);
    } else if (b->sort_context == 'c') {
        Buf key;
        buf_init(&key);
        buf_printf(&key, "column_order:%s", b->current_table);
        db_save_config(b->db, key.data, "null");
        buf_free(&key);
        browser_load_table(b, b->current_table);
        if (b->current_view == VIEW_FIELDS)
            browser_populate_edit(b);
        for (int i = 0; i < b->nsort_items; i++) free(b->sort_items[i]);
        free(b->sort_items);
        b->nsort_items = b->ncols;
        b->sort_items = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++)
            b->sort_items[i] = xstrdup(b->current_columns[i]);
    } else if (b->sort_context == 'r') {
        Buf key;
        buf_init(&key);
        buf_printf(&key, "row_sort:%s", b->current_table);
        db_save_config(b->db, key.data, "null");
        buf_free(&key);
        /* Clear active sort */
        for (int i = 0; i < b->nsort_active; i++) {
            free(b->sort_active_cols[i]);
            free(b->sort_directions[i]);
        }
        free(b->sort_active_cols);
        free(b->sort_directions);
        b->sort_active_cols = NULL;
        b->sort_directions = NULL;
        b->nsort_active = 0;
        browser_load_table(b, b->current_table);
        for (int i = 0; i < b->nsort_items; i++) free(b->sort_items[i]);
        free(b->sort_items);
        b->nsort_items = b->ncols;
        b->sort_items = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++)
            b->sort_items[i] = xstrdup(b->current_columns[i]);
        b->sel_row = 0;
        b->row_scroll = 0;
    }
    b->sort_selected = 0;
    b->sort_scroll = 0;
    b->sort_grabbed = -1;
    browser_set_message(b, "Reset to default order.");
}

/* --- Find pattern parsing --- */

void browser_parse_find_pattern(Browser *b, const char *pattern)
{
    /* Free previous */
    free(b->find_where);
    b->find_where = NULL;
    if (b->find_params) {
        for (int i = 0; i < b->nfind_params; i++)
            free(b->find_params[i]);
        free(b->find_params);
        b->find_params = NULL;
    }
    b->nfind_params = 0;

    if (!pattern || !pattern[0] || !b->current_columns || b->ncols == 0) return;

    Buf where;
    buf_init(&where);
    Vec params;
    vec_init(&params);

    /* Tokenize by spaces */
    char *copy = xstrdup(pattern);
    char *saveptr;
    char *token = strtok_r(copy, " ", &saveptr);
    int first_clause = 1;

    while (token) {
        char *tilde = strchr(token, '~');
        char *eq = strchr(token, '=');

        if (tilde) {
            *tilde = '\0';
            const char *col = token;
            const char *val = tilde + 1;
            /* Check if column exists */
            int found = 0;
            for (int i = 0; i < b->ncols; i++)
                if (strcmp(b->current_columns[i], col) == 0) { found = 1; break; }
            if (found) {
                if (!first_clause) buf_append_str(&where, " AND ");
                buf_printf(&where, "\"%s\" LIKE ?", col);
                Buf p; buf_init(&p);
                buf_printf(&p, "%%%s%%", val);
                vec_push(&params, buf_detach(&p));
                first_clause = 0;
            } else {
                /* Treat as plain text */
                *tilde = '~';
                goto plain_text;
            }
        } else if (eq) {
            *eq = '\0';
            const char *col = token;
            const char *val = eq + 1;
            int found = 0;
            for (int i = 0; i < b->ncols; i++)
                if (strcmp(b->current_columns[i], col) == 0) { found = 1; break; }
            if (found) {
                if (!first_clause) buf_append_str(&where, " AND ");
                buf_printf(&where, "\"%s\" = ?", col);
                vec_push(&params, xstrdup(val));
                first_clause = 0;
            } else {
                *eq = '=';
                goto plain_text;
            }
        } else {
        plain_text:
            if (!first_clause) buf_append_str(&where, " AND ");
            buf_append_char(&where, '(');
            for (int i = 0; i < b->ncols; i++) {
                if (i > 0) buf_append_str(&where, " OR ");
                buf_printf(&where, "\"%s\" LIKE ?", b->current_columns[i]);
                Buf p; buf_init(&p);
                buf_printf(&p, "%%%s%%", token);
                vec_push(&params, buf_detach(&p));
            }
            buf_append_char(&where, ')');
            first_clause = 0;
        }

        token = strtok_r(NULL, " ", &saveptr);
    }

    free(copy);

    if (where.len > 0) {
        b->find_where = buf_detach(&where);
        b->nfind_params = (int)params.len;
        b->find_params = xcalloc(params.len ? params.len : 1, sizeof(char *));
        for (size_t i = 0; i < params.len; i++)
            b->find_params[i] = params.items[i];
        vec_free(&params);
    } else {
        buf_free(&where);
        vec_free(&params);
    }
}

/* --- Find dialog --- */

void browser_enter_find_dialog(Browser *b)
{
    if (b->current_view != VIEW_ROWS || !b->current_columns || b->ncols <= 0) return;
    browser_free_find_dialog(b);
    b->find_dialog = 1;
    b->find_dialog_focus = 0;
    b->find_dialog_scroll = 0;
    b->find_dialog_inputs = xcalloc((size_t)b->ncols, sizeof(char *));
    b->find_dialog_and_flags = xcalloc((size_t)b->ncols, sizeof(int));
    for (int i = 0; i < b->ncols; i++)
        b->find_dialog_inputs[i] = xstrdup("");
}

void browser_apply_find_dialog(Browser *b)
{
    Buf where;
    buf_init(&where);
    Vec and_params, or_params, or_clauses, and_clauses;
    vec_init(&and_params);
    vec_init(&or_params);
    vec_init(&or_clauses);
    vec_init(&and_clauses);
    Buf filter;
    buf_init(&filter);

    for (int i = 0; i < b->ncols; i++) {
        const char *val = b->find_dialog_inputs[i];
        if (!val || !val[0]) continue;

        Buf clause;
        buf_init(&clause);
        buf_printf(&clause, "\"%s\" LIKE ?", b->current_columns[i]);
        Buf param;
        buf_init(&param);
        buf_printf(&param, "%%%s%%", val);

        if (b->find_dialog_and_flags[i]) {
            vec_push(&and_clauses, buf_detach(&clause));
            vec_push(&and_params, buf_detach(&param));
            if (filter.len > 0) buf_append_char(&filter, ' ');
            buf_printf(&filter, "+%s~%s", b->current_columns[i], val);
        } else {
            vec_push(&or_clauses, buf_detach(&clause));
            vec_push(&or_params, buf_detach(&param));
            if (filter.len > 0) buf_append_char(&filter, ' ');
            buf_printf(&filter, "%s~%s", b->current_columns[i], val);
        }
    }

    /* Build combined WHERE */
    int first = 1;
    for (size_t i = 0; i < and_clauses.len; i++) {
        if (!first) buf_append_str(&where, " AND ");
        buf_append_str(&where, and_clauses.items[i]);
        first = 0;
    }
    if (or_clauses.len > 0) {
        if (!first) buf_append_str(&where, " AND ");
        buf_append_char(&where, '(');
        for (size_t i = 0; i < or_clauses.len; i++) {
            if (i > 0) buf_append_str(&where, " OR ");
            buf_append_str(&where, or_clauses.items[i]);
        }
        buf_append_char(&where, ')');
    }

    /* Apply */
    free(b->find_where);
    if (b->find_params) {
        for (int i = 0; i < b->nfind_params; i++) free(b->find_params[i]);
        free(b->find_params);
    }

    if (where.len > 0) {
        b->find_where = buf_detach(&where);
        int total = (int)(and_params.len + or_params.len);
        b->find_params = xcalloc((size_t)total, sizeof(char *));
        b->nfind_params = total;
        int idx = 0;
        for (size_t i = 0; i < and_params.len; i++)
            b->find_params[idx++] = and_params.items[i];
        for (size_t i = 0; i < or_params.len; i++)
            b->find_params[idx++] = or_params.items[i];
        strncpy(b->find_filter, filter.data ? filter.data : "", sizeof(b->find_filter) - 1);
    } else {
        b->find_where = NULL;
        b->find_params = NULL;
        b->nfind_params = 0;
        b->find_filter[0] = '\0';
        buf_free(&where);
    }
    buf_free(&filter);

    /* Free clause strings (params ownership transferred) */
    for (size_t i = 0; i < and_clauses.len; i++) free(and_clauses.items[i]);
    for (size_t i = 0; i < or_clauses.len; i++) free(or_clauses.items[i]);
    vec_free(&and_clauses);
    vec_free(&or_clauses);
    vec_free(&and_params);
    vec_free(&or_params);

    browser_load_table(b, b->current_table);
    b->sel_row = 0;
    b->row_scroll = 0;
}

void browser_free_find_dialog(Browser *b)
{
    if (b->find_dialog_inputs) {
        for (int i = 0; i < b->ncols; i++)
            free(b->find_dialog_inputs[i]);
        free(b->find_dialog_inputs);
        b->find_dialog_inputs = NULL;
    }
    free(b->find_dialog_and_flags);
    b->find_dialog_and_flags = NULL;
    b->find_dialog = 0;
}

/* --- Config loading --- */

void browser_load_config(Browser *b)
{
    /* Load table display order */
    char *json = db_load_config(b->db, "table_order");
    if (json && json[0] == '[' && json[1] != ']') {
        Vec names;
        vec_init(&names);
        const char *p = json + 1;
        while (*p) {
            if (*p == '"') {
                p++;
                const char *start = p;
                while (*p && *p != '"') p++;
                vec_push(&names, xstrndup(start, (size_t)(p - start)));
                if (*p == '"') p++;
            } else {
                p++;
            }
        }
        b->ntable_order = (int)names.len;
        b->table_display_order = xcalloc(names.len, sizeof(char *));
        for (size_t i = 0; i < names.len; i++)
            b->table_display_order[i] = names.items[i];
        vec_free(&names);

        /* Reorder tables array to match saved order */
        if (b->ntables > 0) {
            TableInfo *reordered = xcalloc((size_t)b->ntables, sizeof(TableInfo));
            int *used = xcalloc((size_t)b->ntables, sizeof(int));
            int pos = 0;
            for (int i = 0; i < b->ntable_order; i++) {
                for (int j = 0; j < b->ntables; j++) {
                    if (!used[j] && strcmp(b->tables[j].name, b->table_display_order[i]) == 0) {
                        reordered[pos++] = b->tables[j];
                        used[j] = 1;
                        break;
                    }
                }
            }
            for (int j = 0; j < b->ntables; j++) {
                if (!used[j])
                    reordered[pos++] = b->tables[j];
            }
            memcpy(b->tables, reordered, (size_t)b->ntables * sizeof(TableInfo));
            free(reordered);
            free(used);
        }
    }
    free(json);
}
