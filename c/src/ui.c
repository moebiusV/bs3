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
    /* Pad to full width using getyx */
    int cy, cx;
    getyx(w, cy, cx);
    (void)cy;
    while (cx < x + width && cx < maxx) {
        addch(' ');
        cx++;
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

void ui_draw_edit_view(Browser *b, WINDOW *w, int height, int width)
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

void ui_draw_help_overlay(Browser *b, WINDOW *w, int height, int width)
{
    (void)b; (void)w; (void)height; (void)width;
    /* TODO: Phase 4 */
}

void ui_draw_sort_overlay(Browser *b, WINDOW *w, int height, int width)
{
    (void)b; (void)w; (void)height; (void)width;
    /* TODO: Phase 4 */
}

void ui_draw_find_bar(Browser *b, WINDOW *w, int height, int width)
{
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
    (void)b; (void)w; (void)height; (void)width;
    /* TODO: Phase 4 */
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
