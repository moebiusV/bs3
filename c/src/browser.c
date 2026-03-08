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
}

void browser_destroy(Browser *b)
{
    browser_free_table_data(b);
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
    b->ncols = 0;
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
}

void browser_load_table(Browser *b, const char *table_name)
{
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

    b->current_table = xstrdup(table_name);

    /* Find table info to get columns */
    for (int i = 0; i < b->ntables; i++) {
        if (strcmp(b->tables[i].name, table_name) == 0) {
            b->ncols = b->tables[i].ncols;
            b->current_columns = xcalloc((size_t)b->ncols, sizeof(char *));
            for (int j = 0; j < b->ncols; j++)
                b->current_columns[j] = xstrdup(b->tables[i].columns[j]);
            break;
        }
    }

    /* Build WHERE clause from find_filter if set */
    /* TODO: structured find parsing (Phase 4) */
    /* For now, free-text search across all columns */
    free(b->find_where);
    b->find_where = NULL;
    if (b->find_params) {
        for (int i = 0; i < b->nfind_params; i++)
            free(b->find_params[i]);
        free(b->find_params);
        b->find_params = NULL;
    }
    b->nfind_params = 0;

    if (b->find_filter[0] && b->current_view == VIEW_ROWS) {
        /* Build OR clause across all columns */
        Buf where;
        buf_init(&where);
        b->find_params = xcalloc((size_t)b->ncols, sizeof(char *));
        for (int i = 0; i < b->ncols; i++) {
            if (i > 0) buf_append_str(&where, " OR ");
            buf_printf(&where, "\"%s\" LIKE ?", b->current_columns[i]);
            Buf param;
            buf_init(&param);
            buf_printf(&param, "%%%s%%", b->find_filter);
            b->find_params[i] = buf_detach(&param);
        }
        b->nfind_params = b->ncols;
        b->find_where = buf_detach(&where);
    }

    /* Get unfiltered count */
    b->unfilt_row_count = db_row_count(b->db, table_name, NULL, NULL, 0);

    /* Load rows */
    db_load_rows(b->db, table_name,
                 (const char **)b->current_columns, b->ncols,
                 b->find_where,
                 (const char **)b->find_params, b->nfind_params,
                 NULL, /* TODO: order_by from sort config */
                 &b->rowset);

    browser_cache_row_strings(b);
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
                ui_draw_edit_view(b, stdscr, height, width);

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
            input_handle_edit(b, stdscr, key, height, width);

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
