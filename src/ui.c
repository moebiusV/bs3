/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "ui.h"
#include "uniwidth.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <locale.h>

/* CGA palette RGB values (0-1000 scale for ncurses) */
#define CGA_BLACK_R   0, CGA_BLACK_G   0, CGA_BLACK_B   0
#define CGA_BLUE_R    0, CGA_BLUE_G    0, CGA_BLUE_B  667
#define CGA_GREEN_R   0, CGA_GREEN_G 667, CGA_GREEN_B   0
#define CGA_CYAN_R    0, CGA_CYAN_G  667, CGA_CYAN_B 667
#define CGA_RED_R   667, CGA_RED_G    0, CGA_RED_B     0
#define CGA_YELLOW_R 1000, CGA_YELLOW_G 1000, CGA_YELLOW_B 333
#define CGA_WHITE_R 1000, CGA_WHITE_G 1000, CGA_WHITE_B 1000

/* Color indices 16-22 for CGA palette */
enum {
    CI_BLACK = 16, CI_BLUE, CI_GREEN, CI_CYAN, CI_RED, CI_YELLOW, CI_WHITE
};

void ui_init_colors(void)
{
    start_color();

    if (can_change_color() && COLORS >= 23) {
        init_color(CI_BLACK,    0,    0,    0);
        init_color(CI_BLUE,     0,    0,  667);
        init_color(CI_GREEN,    0,  667,    0);
        init_color(CI_CYAN,     0,  667,  667);
        init_color(CI_RED,    667,    0,    0);
        init_color(CI_YELLOW, 1000, 1000, 333);
        init_color(CI_WHITE,  1000, 1000, 1000);

        init_pair(C_NORMAL,   CI_YELLOW, CI_BLUE);
        init_pair(C_SELECTED, CI_WHITE,  CI_GREEN);
        init_pair(C_BRIGHT,   CI_WHITE,  CI_BLUE);
        init_pair(C_TITLE,    CI_WHITE,  CI_CYAN);
        init_pair(C_STATUS,   CI_BLACK,  CI_CYAN);
        init_pair(C_ERROR,    CI_WHITE,  CI_RED);
        init_pair(C_HELP,     CI_BLACK,  CI_CYAN);
        init_pair(C_HELP_KEY, CI_WHITE,  CI_CYAN);
        init_pair(C_BORDER,   CI_CYAN,   CI_BLUE);
    } else {
        init_pair(C_NORMAL,   COLOR_YELLOW, COLOR_BLUE);
        init_pair(C_SELECTED, COLOR_WHITE,  COLOR_GREEN);
        init_pair(C_BRIGHT,   COLOR_WHITE,  COLOR_BLUE);
        init_pair(C_TITLE,    COLOR_WHITE,  COLOR_CYAN);
        init_pair(C_STATUS,   COLOR_BLACK,  COLOR_CYAN);
        init_pair(C_ERROR,    COLOR_WHITE,  COLOR_RED);
        init_pair(C_HELP,     COLOR_BLACK,  COLOR_CYAN);
        init_pair(C_HELP_KEY, COLOR_WHITE,  COLOR_CYAN);
        init_pair(C_BORDER,   COLOR_CYAN,   COLOR_BLUE);
    }
}

void ui_safe_addstr(WINDOW *w, int y, int x, const char *s, int attr)
{
    int maxy, maxx;
    getmaxyx(w, maxy, maxx);
    (void)maxy;
    if (y < 0 || x < 0 || x >= maxx) return;

    int avail = maxx - x;
    char *trunc = str_truncate_to_width(s, avail);
    wattron(w, attr);
    mvaddstr(y, x, trunc);
    wattroff(w, attr);
    free(trunc);
}

void ui_safe_addstr_full(WINDOW *w, int y, int x, const char *s, int attr, int width)
{
    int maxy, maxx;
    getmaxyx(w, maxy, maxx);
    (void)maxy;
    if (y < 0 || x < 0 || x >= maxx) return;

    int avail = maxx - x;
    if (width > avail) width = avail;

    char *trunc = str_truncate_to_width(s, width);
    wattron(w, attr);
    mvaddstr(y, x, trunc);
    /* Pad to full width using getyx.
     * If cursor wrapped past line y (string filled/overflowed the width),
     * the line is already full — skip padding to avoid corrupting line y+1. */
    int cy, cx;
    getyx(w, cy, cx);
    if (cy == y) {
        while (cx < x + width && cx < maxx) {
            addch(' ');
            cx++;
        }
    }
    wattroff(w, attr);
    free(trunc);
}

/* --- Title bar (row 0) --- */

static void draw_title_bar(Browser *b, WINDOW *w, int width)
{
    Buf title;
    buf_init(&title);
    if (b->current_view == VIEW_TABLES) {
        buf_printf(&title, " browse-sqlite3 -- %s ", b->db->path);
    } else if (b->current_view == VIEW_ROWS) {
        buf_printf(&title, " browse-sqlite3 -- %s -- %s (%d rows) ",
                   b->db->path, b->current_table, b->rowset.nrows);
    } else {
        buf_printf(&title, " browse-sqlite3 -- %s -- %s -- Edit ",
                   b->db->path, b->current_table);
    }
    ui_safe_addstr_full(w, 0, 0, title.data, COLOR_PAIR(C_TITLE) | A_BOLD, width);
    buf_free(&title);
}

/* --- Tables view --- */

void ui_draw_tables_view(Browser *b, WINDOW *w, int height, int width)
{
    draw_title_bar(b, w, width);

    /* Header line (row 1) */
    ui_safe_addstr_full(w, 1, 0, " Table Name             Columns",
                        COLOR_PAIR(C_BRIGHT) | A_BOLD, width);

    int visible = height - 3; /* title + header + status bar */
    if (visible <= 0) return;

    int count = b->ntables;

    for (int i = 0; i < visible; i++) {
        int idx = b->table_scroll + i;
        int row_y = 2 + i;
        if (idx >= count) {
            ui_safe_addstr_full(w, row_y, 0, "", COLOR_PAIR(C_NORMAL), width);
            continue;
        }

        TableInfo *t = &b->tables[idx];
        int is_selected = (idx == b->sel_table);
        int attr = COLOR_PAIR(is_selected ? C_SELECTED : C_NORMAL);

        /* Build line: marker + name + columns */
        Buf line;
        buf_init(&line);
        buf_printf(&line, " %s %-24s ", is_selected ? "\xe2\x96\xba" : " ", t->name);
        buf_append_char(&line, '(');
        for (int j = 0; j < t->ncols; j++) {
            if (j > 0) buf_append_str(&line, ", ");
            buf_append_str(&line, t->columns[j]);
        }
        buf_append_char(&line, ')');

        ui_safe_addstr_full(w, row_y, 0, line.data, attr, width);
        buf_free(&line);
    }

    ui_draw_status_bar(b, w, height, width);
}

/* --- Rows view --- */

void ui_draw_rows_view(Browser *b, WINDOW *w, int height, int width)
{
    draw_title_bar(b, w, width);

    /* Header line (row 1): column names */
    {
        Buf hdr;
        buf_init(&hdr);
        buf_append_str(&hdr, " ");
        for (int i = 0; i < b->ncols; i++) {
            if (i > 0) buf_append_str(&hdr, " | ");
            buf_append_str(&hdr, b->current_columns[i]);
        }

        /* Apply horizontal offset */
        const char *display = hdr.data;
        if (b->row_horiz > 0)
            display = str_skip_display_cols(hdr.data, b->row_horiz);

        ui_safe_addstr_full(w, 1, 0, display, COLOR_PAIR(C_BRIGHT) | A_BOLD, width);
        buf_free(&hdr);
    }

    int visible = height - 3;
    if (visible <= 0) return;

    for (int i = 0; i < visible; i++) {
        int idx = b->row_scroll + i;
        int row_y = 2 + i;
        if (idx >= b->rowset.nrows) {
            ui_safe_addstr_full(w, row_y, 0, "", COLOR_PAIR(C_NORMAL), width);
            continue;
        }

        int is_selected = (idx == b->sel_row);
        int attr = COLOR_PAIR(is_selected ? C_SELECTED : C_NORMAL);

        const char *display = " ";
        if (b->row_strings && b->row_strings[idx]) {
            display = b->row_strings[idx];
            if (b->row_horiz > 0)
                display = str_skip_display_cols(display, b->row_horiz);
        }

        ui_safe_addstr_full(w, row_y, 0, display, attr, width);
    }

    ui_draw_status_bar(b, w, height, width);
}

/* --- Edit view --- */

void ui_draw_fields_view(Browser *b, WINDOW *w, int height, int width)
{
    draw_title_bar(b, w, width);

    /* Header: row N of M */
    {
        Buf hdr;
        buf_init(&hdr);
        buf_printf(&hdr, " Row %d of %d", b->sel_row + 1, b->rowset.nrows);
        ui_safe_addstr_full(w, 1, 0, hdr.data, COLOR_PAIR(C_BRIGHT) | A_BOLD, width);
        buf_free(&hdr);
    }

    int visible = height - 3;
    if (visible <= 0) return;

    /* Find max column name width for alignment */
    int max_name_w = 0;
    for (int i = 0; i < b->ncols; i++) {
        int nw = str_display_width(b->current_columns[i]);
        if (nw > max_name_w) max_name_w = nw;
    }
    int label_w = max_name_w + 4; /* padding + ": " */
    if (label_w > width / 3) label_w = width / 3;

    for (int i = 0; i < visible; i++) {
        int idx = b->field_scroll + i;
        int row_y = 2 + i;
        if (idx >= b->ncols) {
            ui_safe_addstr_full(w, row_y, 0, "", COLOR_PAIR(C_NORMAL), width);
            continue;
        }

        int is_selected = (idx == b->sel_field);
        int attr = COLOR_PAIR(is_selected ? C_SELECTED : C_NORMAL);

        /* Check for modifications */
        int modified = 0;
        if (b->edit_values && b->edit_original) {
            const char *cur = b->edit_values[idx];
            const char *orig = b->edit_original[idx];
            if (cur == NULL && orig != NULL) modified = 1;
            else if (cur != NULL && orig == NULL) modified = 1;
            else if (cur && orig && strcmp(cur, orig) != 0) modified = 1;
        }

        Buf line;
        buf_init(&line);
        buf_printf(&line, " %s%-*s: ", modified ? "*" : " ",
                   max_name_w, b->current_columns[idx]);

        const char *val = b->edit_values ? b->edit_values[idx] : NULL;
        if (val == NULL)
            buf_append_str(&line, "NULL");
        else
            buf_append_str(&line, val);

        ui_safe_addstr_full(w, row_y, 0, line.data, attr, width);
        buf_free(&line);
    }

    ui_draw_status_bar(b, w, height, width);
}

/* --- Status bar --- */

void ui_draw_status_bar(Browser *b, WINDOW *w, int height, int width)
{
    int y = height - 1;

    /* If there's a message, show it on the error color briefly */
    if (b->message) {
        Buf msg;
        buf_init(&msg);
        buf_printf(&msg, " %s ", b->message);
        ui_safe_addstr_full(w, y, 0, msg.data, COLOR_PAIR(C_ERROR) | A_BOLD, width);
        buf_free(&msg);
        return;
    }

    const char *mode_str = b->safe_mode ? "SAFE" : "UNSAFE";
    Buf bar;
    buf_init(&bar);

    if (b->current_view == VIEW_TABLES) {
        buf_printf(&bar, " %s | hjkl:Nav | JK/PgDn | Tab:> | l:Open | d:Drop | o:Order | /:Search | q:Quit | F1:Help ",
                   mode_str);
    } else if (b->current_view == VIEW_ROWS) {
        buf_printf(&bar, " %s | hjkl:Nav | JK/PgDn | Tab:> | l:Edit | d:Del | h:Back | o:Sort | /:Search | f:Find | q:Quit | F1:Help ",
                   mode_str);
    } else {
        buf_printf(&bar, " %s | hjkl:Nav | JK/PgDn | Tab:> | e:Edit | E:Ext | s:Save | h:Back | o:Order | /:Search | q:Quit | F1:Help ",
                   mode_str);
    }

    ui_safe_addstr_full(w, y, 0, bar.data, COLOR_PAIR(C_STATUS), width);
    buf_free(&bar);
}

/* --- Overlays (stubs for Phase 1, filled in later) --- */

/* Box drawing chars (UTF-8) */
#define BOX_TL "\xe2\x94\x8c"
#define BOX_TR "\xe2\x94\x90"
#define BOX_BL "\xe2\x94\x94"
#define BOX_BR "\xe2\x94\x98"
#define BOX_HZ "\xe2\x94\x80"
#define BOX_VT "\xe2\x94\x82"

/* Helper: draw a centered overlay box border with title */
static void draw_box_top(WINDOW *w, int y, int x, int box_w, const char *title, int attr)
{
    Buf line;
    buf_init(&line);
    int title_len = (int)strlen(title);
    int pad_total = box_w - 2 - title_len;
    if (pad_total < 0) pad_total = 0;
    int pad_left = pad_total / 2;
    int pad_right = pad_total - pad_left;
    buf_append_str(&line, BOX_TL);
    for (int i = 0; i < pad_left; i++) buf_append_str(&line, BOX_HZ);
    buf_append_str(&line, title);
    for (int i = 0; i < pad_right; i++) buf_append_str(&line, BOX_HZ);
    buf_append_str(&line, BOX_TR);
    ui_safe_addstr(w, y, x, line.data, attr);
    buf_free(&line);
}

static void draw_box_bottom(WINDOW *w, int y, int x, int box_w, const char *hint, int attr)
{
    Buf line;
    buf_init(&line);
    int hint_len = (int)strlen(hint);
    int pad_total = box_w - 2 - hint_len;
    if (pad_total < 0) pad_total = 0;
    int pad_left = pad_total / 2;
    int pad_right = pad_total - pad_left;
    buf_append_str(&line, BOX_BL);
    for (int i = 0; i < pad_left; i++) buf_append_str(&line, BOX_HZ);
    buf_append_str(&line, hint);
    for (int i = 0; i < pad_right; i++) buf_append_str(&line, BOX_HZ);
    buf_append_str(&line, BOX_BR);
    ui_safe_addstr(w, y, x, line.data, attr);
    buf_free(&line);
}

static void draw_box_row(WINDOW *w, int y, int x, int box_w, const char *content, int inner_w __attribute__((unused)), int attr, int border_attr)
{
    wattron(w, border_attr);
    mvaddstr(y, x, BOX_VT);
    wattroff(w, border_attr);
    /* " content " padded to inner_w */
    Buf padded;
    buf_init(&padded);
    buf_append_char(&padded, ' ');
    buf_append_str(&padded, content);
    buf_append_char(&padded, ' ');
    ui_safe_addstr_full(w, y, x + 1, padded.data, attr, box_w - 2);
    buf_free(&padded);
    wattron(w, border_attr);
    mvaddstr(y, x + box_w - 1, BOX_VT);
    wattroff(w, border_attr);
}

/* Help line data */
typedef struct { const char *key; const char *desc; } HelpLine;

void ui_draw_help_overlay(Browser *b, WINDOW *w, int height, int width)
{
    /* Build help lines for current view */
    HelpLine lines[60];
    int nlines = 0;

    #define HL(k, d) do { lines[nlines].key = (k); lines[nlines].desc = (d); nlines++; } while(0)

    if (b->current_view == VIEW_TABLES) {
        HL("", "-- Table List --");
        HL("k / Up", "Move selection up");
        HL("j / Down", "Move selection down");
        HL("K / PgUp", "Page up");
        HL("J / PgDn", "Page down");
        HL("Tab", "Next table");
        HL("l / Right / Enter", "Open selected table");
        HL("d / Del", "Drop selected table");
        HL("", "  SAFE: prompts | UNSAFE: immediate");
        HL("o", "Set table display order");
        HL("/", "Search/filter tables");
    } else if (b->current_view == VIEW_ROWS) {
        HL("", "-- Row Browser --");
        HL("k / Up", "Move selection up");
        HL("j / Down", "Move selection down");
        HL("K / PgUp", "Page up");
        HL("J / PgDn", "Page down");
        HL("Tab", "Next row");
        HL("l / Right / Enter", "Edit selected row");
        HL("h / Left", "Go back to table list");
        HL("d / Del", "Delete selected row");
        HL("", "  SAFE: prompts | UNSAFE: immediate");
        HL("Shift+Left", "Scroll display left");
        HL("Shift+Right", "Scroll display right");
        HL("o", "Sort rows by column(s)");
        HL("/", "Search rows (any field)");
        HL("f", "Find by column (dialog)");
    } else {
        HL("", "-- Field Editor --");
        HL("k / Up", "Move to previous field");
        HL("j / Down", "Move to next field");
        HL("K / PgUp", "Page up through fields");
        HL("J / PgDn", "Page down through fields");
        HL("Tab", "Next field");
        HL("Enter / v", "View field in pager ($PAGER)");
        HL("E", "Edit in external editor ($EDITOR)");
        HL("d / Del", "Set field to NULL");
        HL("", "  SAFE: prompts | UNSAFE: immediate");
        HL("s", "Save all changes to database");
        HL("o", "Set column display order");
    }

    HL("", "");
    HL("", "-- General (all screens) --");
    HL("q / Esc", "Quit program without saving");
    HL("^C", "Quit program without saving");
    HL("F1", "Toggle this help screen");
    HL(":", "Enter command mode");
    HL("", "");
    HL("", "-- Safety Mode --");
    HL("", b->safe_mode ? "  Current mode: SAFE" : "  Current mode: UNSAFE");
    HL("", "  :safe    - Enable safe mode (default)");
    HL("", "  :unsafe  - Disable safe mode");

    #undef HL

    int box_w = 62;
    if (box_w > width - 4) box_w = width - 4;
    if (box_w < 20) box_w = 20;
    int box_h = nlines + 4;
    if (box_h > height - 2) box_h = height - 2;
    int start_y = (height - box_h) / 2;
    int start_x = (width - box_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;
    int inner_w = box_w - 4;
    int content_h = box_h - 4;

    int border_attr = COLOR_PAIR(C_TITLE) | A_BOLD;
    int help_attr = COLOR_PAIR(C_HELP);
    int key_attr = COLOR_PAIR(C_HELP_KEY) | A_BOLD;

    /* Title */
    const char *view_name = b->current_view == VIEW_TABLES ? "Table List"
                          : b->current_view == VIEW_ROWS ? "Row Browser"
                          : "Field Editor";
    Buf title;
    buf_init(&title);
    buf_printf(&title, " Help: %s ", view_name);
    draw_box_top(w, start_y, start_x, box_w, title.data, border_attr);
    buf_free(&title);

    /* Blank line */
    Buf blank;
    buf_init(&blank);
    for (int i = 0; i < inner_w; i++) buf_append_char(&blank, ' ');
    draw_box_row(w, start_y + 1, start_x, box_w, blank.data, inner_w, help_attr, border_attr);

    /* Content */
    int key_col_w = 20;
    for (int i = 0; i < content_h; i++) {
        int cy = start_y + 2 + i;
        int idx = b->help_scroll + i;
        if (idx < nlines) {
            const char *k = lines[idx].key;
            const char *d = lines[idx].desc;
            if (!k[0]) {
                /* Section header or description-only */
                Buf row;
                buf_init(&row);
                char *trunc = str_truncate_to_width(d, inner_w);
                buf_append_str(&row, trunc);
                int dw = str_display_width(trunc);
                for (int p = dw; p < inner_w; p++) buf_append_char(&row, ' ');
                free(trunc);
                draw_box_row(w, cy, start_x, box_w, row.data, inner_w, help_attr, border_attr);
                buf_free(&row);
            } else {
                /* Key + description */
                Buf row;
                buf_init(&row);
                char *ktrunc = str_truncate_to_width(k, key_col_w);
                int kw = str_display_width(ktrunc);
                buf_append_str(&row, ktrunc);
                for (int p = kw; p < key_col_w; p++) buf_append_char(&row, ' ');
                free(ktrunc);
                int desc_w = inner_w - key_col_w;
                char *dtrunc = str_truncate_to_width(d, desc_w);
                int ddw = str_display_width(dtrunc);
                buf_append_str(&row, dtrunc);
                for (int p = ddw; p < desc_w; p++) buf_append_char(&row, ' ');
                free(dtrunc);
                draw_box_row(w, cy, start_x, box_w, row.data, inner_w, help_attr, border_attr);
                buf_free(&row);

                /* Overwrite key portion with key_attr */
                Buf kpad;
                buf_init(&kpad);
                char *kt2 = str_truncate_to_width(k, key_col_w);
                int kw2 = str_display_width(kt2);
                buf_append_str(&kpad, kt2);
                for (int p = kw2; p < key_col_w; p++) buf_append_char(&kpad, ' ');
                free(kt2);
                ui_safe_addstr(w, cy, start_x + 2, kpad.data, key_attr);
                buf_free(&kpad);
            }
        } else {
            draw_box_row(w, cy, start_x, box_w, blank.data, inner_w, help_attr, border_attr);
        }
    }

    /* Blank line before bottom */
    draw_box_row(w, start_y + box_h - 2, start_x, box_w, blank.data, inner_w, help_attr, border_attr);

    /* Bottom border with hint */
    int can_scroll = (nlines > content_h);
    Buf hint;
    buf_init(&hint);
    if (can_scroll) {
        int pos = b->help_scroll + 1;
        int total = nlines - content_h + 1;
        if (total < 1) total = 1;
        buf_printf(&hint, " j/k:Scroll  Other:Close (%d/%d) ", pos, total);
    } else {
        buf_append_str(&hint, " Any key to close ");
    }
    draw_box_bottom(w, start_y + box_h - 1, start_x, box_w, hint.data, border_attr);
    buf_free(&hint);
    buf_free(&blank);

    curs_set(0);
}

void ui_draw_sort_overlay(Browser *b, WINDOW *w, int height, int width)
{
    int n = b->nsort_items;

    int box_w = 52;
    if (box_w > width - 4) box_w = width - 4;
    if (box_w < 20) box_w = 20;
    int box_h = n + 4;
    if (box_h > height - 2) box_h = height - 2;
    int start_y = (height - box_h) / 2;
    int start_x = (width - box_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;
    int inner_w = box_w - 4;
    int content_h = box_h - 4;

    int border_attr = COLOR_PAIR(C_TITLE) | A_BOLD;
    int help_attr = COLOR_PAIR(C_HELP);
    int sel_attr = COLOR_PAIR(C_SELECTED);

    /* Title */
    const char *title = b->sort_context == 't' ? " Table Display Order "
                      : b->sort_context == 'c' ? " Column Display Order "
                      : " Sort by Column(s) ";
    draw_box_top(w, start_y, start_x, box_w, title, border_attr);

    /* Blank line */
    Buf blank;
    buf_init(&blank);
    for (int i = 0; i < inner_w; i++) buf_append_char(&blank, ' ');
    draw_box_row(w, start_y + 1, start_x, box_w, blank.data, inner_w, help_attr, border_attr);

    /* Adjust scroll */
    if (b->sort_selected < b->sort_scroll)
        b->sort_scroll = b->sort_selected;
    else if (b->sort_selected >= b->sort_scroll + content_h)
        b->sort_scroll = b->sort_selected - content_h + 1;

    /* Content */
    for (int i = 0; i < content_h; i++) {
        int cy = start_y + 2 + i;
        int idx = b->sort_scroll + i;
        if (idx < n) {
            const char *item = b->sort_items[idx];
            Buf label;
            buf_init(&label);

            if (b->sort_context == 'r') {
                /* Check if active sort column */
                int priority = 0;
                const char *dir = "ASC";
                for (int s = 0; s < b->nsort_active; s++) {
                    if (strcmp(b->sort_active_cols[s], item) == 0) {
                        priority = s + 1;
                        dir = b->sort_directions[s];
                        break;
                    }
                }
                if (priority > 0)
                    buf_printf(&label, "%d. %s [%s]", priority, item, dir);
                else
                    buf_printf(&label, "   %s", item);
            } else {
                int grabbed = (idx == b->sort_grabbed);
                buf_printf(&label, "%s%s", grabbed ? "\xe2\x89\xa1 " : "  ", item);
            }

            /* Pad to inner_w */
            char *trunc = str_truncate_to_width(label.data, inner_w);
            int tw = str_display_width(trunc);
            Buf padded;
            buf_init(&padded);
            buf_append_str(&padded, trunc);
            for (int p = tw; p < inner_w; p++) buf_append_char(&padded, ' ');
            free(trunc);

            int attr = (idx == b->sort_selected) ? sel_attr : help_attr;
            draw_box_row(w, cy, start_x, box_w, padded.data, inner_w, attr, border_attr);
            buf_free(&padded);
            buf_free(&label);
        } else {
            draw_box_row(w, cy, start_x, box_w, blank.data, inner_w, help_attr, border_attr);
        }
    }

    /* Blank line before bottom */
    draw_box_row(w, start_y + box_h - 2, start_x, box_w, blank.data, inner_w, help_attr, border_attr);

    /* Bottom hint */
    const char *hint = b->sort_context == 'r'
        ? " Spc:Toggle a/d:Dir j/k:Move r:Reset Esc:OK "
        : " Spc:Grab/Drop j/k:Move r:Reset Esc:OK ";
    draw_box_bottom(w, start_y + box_h - 1, start_x, box_w, hint, border_attr);
    buf_free(&blank);
    curs_set(0);
}

void ui_draw_find_bar(Browser *b, WINDOW *w, int height, int width)
{
    /* Column hint on line above (rows view only) */
    if (b->current_view == VIEW_ROWS && b->current_columns && b->ncols > 0) {
        Buf hint;
        buf_init(&hint);
        buf_append_str(&hint, " Columns: ");
        for (int i = 0; i < b->ncols; i++) {
            if (i > 0) buf_append_str(&hint, ", ");
            buf_append_str(&hint, b->current_columns[i]);
        }
        buf_append_str(&hint, "  (col=val col~val)");
        ui_safe_addstr_full(w, height - 2, 0, hint.data, COLOR_PAIR(C_STATUS), width);
        buf_free(&hint);
    }

    int y = height - 1;
    Buf bar;
    buf_init(&bar);
    buf_printf(&bar, " /%s", b->find_input);
    ui_safe_addstr_full(w, y, 0, bar.data, COLOR_PAIR(C_STATUS), width);
    buf_free(&bar);

    /* Position cursor */
    int cx = 2 + str_display_width(b->find_input);
    if (cx >= width) cx = width - 1;
    curs_set(1);
    move(y, cx);
}

void ui_draw_command_bar(Browser *b, WINDOW *w, int height, int width)
{
    int y = height - 1;
    Buf bar;
    buf_init(&bar);
    buf_printf(&bar, ":%s", b->command_input);
    ui_safe_addstr_full(w, y, 0, bar.data, COLOR_PAIR(C_STATUS), width);
    buf_free(&bar);

    int cx = 1 + str_display_width(b->command_input);
    if (cx >= width) cx = width - 1;
    curs_set(1);
    move(y, cx);
}

void ui_draw_prompt_bar(Browser *b, WINDOW *w, int height, int width)
{
    int y = height - 1;
    ui_safe_addstr_full(w, y, 0, b->prompt_text, COLOR_PAIR(C_ERROR) | A_BOLD, width);
    curs_set(0);
}

void ui_draw_find_dialog(Browser *b, WINDOW *w, int height, int width)
{
    int n = b->ncols;
    if (n <= 0 || !b->find_dialog_inputs) return;

    int box_w = 60;
    if (box_w > width - 4) box_w = width - 4;
    if (box_w < 30) box_w = 30;
    int box_h = n + 4;
    if (box_h > height - 2) box_h = height - 2;
    int start_y = (height - box_h) / 2;
    int start_x = (width - box_w) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;
    int inner_w = box_w - 4;
    int content_h = box_h - 4;

    int border_attr = COLOR_PAIR(C_TITLE) | A_BOLD;
    int help_attr = COLOR_PAIR(C_HELP);
    int sel_attr = COLOR_PAIR(C_SELECTED);

    draw_box_top(w, start_y, start_x, box_w, " Find by Column ", border_attr);

    Buf blank;
    buf_init(&blank);
    for (int i = 0; i < inner_w; i++) buf_append_char(&blank, ' ');
    draw_box_row(w, start_y + 1, start_x, box_w, blank.data, inner_w, help_attr, border_attr);

    /* Adjust scroll */
    if (b->find_dialog_focus < b->find_dialog_scroll)
        b->find_dialog_scroll = b->find_dialog_focus;
    else if (b->find_dialog_focus >= b->find_dialog_scroll + content_h)
        b->find_dialog_scroll = b->find_dialog_focus - content_h + 1;

    /* Label width */
    int max_label = 0;
    for (int i = 0; i < n; i++) {
        int cw = str_display_width(b->current_columns[i]);
        if (cw > max_label) max_label = cw;
    }
    int label_w = max_label + 2;
    if (label_w > inner_w / 2) label_w = inner_w / 2;
    int indicator_w = 4; /* "OR  " or "AND " */
    int input_w = inner_w - label_w - indicator_w;
    if (input_w < 5) input_w = 5;

    int cursor_y = -1, cursor_x = -1;

    for (int i = 0; i < content_h; i++) {
        int cy = start_y + 2 + i;
        int idx = b->find_dialog_scroll + i;
        if (idx < n) {
            int is_focus = (idx == b->find_dialog_focus);
            int is_and = b->find_dialog_and_flags[idx];

            /* Draw border */
            wattron(w, border_attr);
            mvaddstr(cy, start_x, BOX_VT);
            wattroff(w, border_attr);

            /* Label */
            Buf lbl;
            buf_init(&lbl);
            char *ltrunc = str_truncate_to_width(b->current_columns[idx], label_w - 2);
            buf_append_str(&lbl, ltrunc);
            buf_append_str(&lbl, ": ");
            int lw = str_display_width(lbl.data);
            for (int p = lw; p < label_w; p++) buf_append_char(&lbl, ' ');
            free(ltrunc);

            Buf prefix;
            buf_init(&prefix);
            buf_append_char(&prefix, ' ');
            buf_append_str(&prefix, lbl.data);
            ui_safe_addstr(w, cy, start_x + 1, prefix.data, help_attr);
            buf_free(&prefix);

            /* Indicator */
            int ind_x = start_x + 2 + label_w;
            const char *indicator = is_and ? "AND " : "OR  ";
            int ind_attr = is_and ? (int)((unsigned)help_attr | A_BOLD) : help_attr;
            if (is_focus && is_and) ind_attr = (int)((unsigned)sel_attr | A_BOLD);
            ui_safe_addstr(w, cy, ind_x, indicator, ind_attr);

            /* Input field */
            int input_x = ind_x + indicator_w;
            const char *val = b->find_dialog_inputs[idx] ? b->find_dialog_inputs[idx] : "";
            char *vtrunc = str_truncate_to_width(val, input_w);
            int vw = str_display_width(vtrunc);
            Buf vpad;
            buf_init(&vpad);
            buf_append_str(&vpad, vtrunc);
            for (int p = vw; p < input_w; p++) buf_append_char(&vpad, ' ');
            free(vtrunc);
            ui_safe_addstr(w, cy, input_x, vpad.data, is_focus ? sel_attr : help_attr);
            buf_free(&vpad);

            if (is_focus) {
                cursor_y = cy;
                cursor_x = input_x + vw;
                if (cursor_x >= start_x + box_w - 1)
                    cursor_x = start_x + box_w - 2;
            }

            /* Right border */
            wattron(w, border_attr);
            mvaddstr(cy, start_x + box_w - 1, BOX_VT);
            wattroff(w, border_attr);

            buf_free(&lbl);
        } else {
            draw_box_row(w, cy, start_x, box_w, blank.data, inner_w, help_attr, border_attr);
        }
    }

    draw_box_row(w, start_y + box_h - 2, start_x, box_w, blank.data, inner_w, help_attr, border_attr);
    draw_box_bottom(w, start_y + box_h - 1, start_x, box_w,
                    " Tab:Next ^A:AND/OR ^R:Clear Enter:Apply Esc:Cancel ", border_attr);
    buf_free(&blank);

    if (cursor_y >= 0 && cursor_x >= 0) {
        curs_set(1);
        move(cursor_y, cursor_x);
    }
}

/* --- Partial redraws --- */

void ui_draw_single_table_row(Browser *b, WINDOW *w, int idx, int height, int width)
{
    int visible = height - 3;
    int screen_row = idx - b->table_scroll;
    if (screen_row < 0 || screen_row >= visible) return;
    int row_y = 2 + screen_row;

    if (idx >= b->ntables) {
        ui_safe_addstr_full(w, row_y, 0, "", COLOR_PAIR(C_NORMAL), width);
        return;
    }

    TableInfo *t = &b->tables[idx];
    int is_selected = (idx == b->sel_table);
    int attr = COLOR_PAIR(is_selected ? C_SELECTED : C_NORMAL);

    Buf line;
    buf_init(&line);
    buf_printf(&line, " %s %-24s ", is_selected ? "\xe2\x96\xba" : " ", t->name);
    buf_append_char(&line, '(');
    for (int j = 0; j < t->ncols; j++) {
        if (j > 0) buf_append_str(&line, ", ");
        buf_append_str(&line, t->columns[j]);
    }
    buf_append_char(&line, ')');

    ui_safe_addstr_full(w, row_y, 0, line.data, attr, width);
    buf_free(&line);
}

void ui_draw_single_row(Browser *b, WINDOW *w, int idx, int height, int width)
{
    int visible = height - 3;
    int screen_row = idx - b->row_scroll;
    if (screen_row < 0 || screen_row >= visible) return;
    int row_y = 2 + screen_row;

    if (idx >= b->rowset.nrows) {
        ui_safe_addstr_full(w, row_y, 0, "", COLOR_PAIR(C_NORMAL), width);
        return;
    }

    int is_selected = (idx == b->sel_row);
    int attr = COLOR_PAIR(is_selected ? C_SELECTED : C_NORMAL);

    const char *display = " ";
    if (b->row_strings && b->row_strings[idx]) {
        display = b->row_strings[idx];
        if (b->row_horiz > 0)
            display = str_skip_display_cols(display, b->row_horiz);
    }

    ui_safe_addstr_full(w, row_y, 0, display, attr, width);
}
