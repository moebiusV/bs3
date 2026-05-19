/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef BROWSER_H
#define BROWSER_H

#include <ncursesw/curses.h>
#include "db.h"

typedef enum { VIEW_TABLES, VIEW_ROWS, VIEW_FIELDS } View;

/* Color pair indices */
enum {
    C_NORMAL = 1,     /* yellow on blue */
    C_SELECTED,       /* white on green */
    C_BRIGHT,         /* white on blue */
    C_TITLE,          /* white on cyan */
    C_STATUS,         /* black on cyan */
    C_ERROR,          /* white on red */
    C_HELP,           /* black on cyan */
    C_HELP_KEY,       /* white on cyan */
    C_BORDER,         /* cyan on blue */
};

typedef struct Browser {
    /* Database */
    Database *db;

    /* Schema */
    TableInfo *tables;
    int ntables;

    /* Current view */
    View current_view;

    /* Table list state */
    int sel_table;
    int table_scroll;
    char **filtered_tables;  /* pointers into tables[] names */
    int *filtered_indices;   /* mapping back to tables[] */
    int nfiltered;

    /* Row browser state */
    char *current_table;
    int current_is_view;
    char **current_columns;
    int ncols;
    RowSet rowset;
    char **row_strings;      /* cached formatted row strings */
    int sel_row;
    int row_scroll;
    int row_horiz;
    int unfilt_row_count;

    /* Edit state */
    char **edit_values;
    char **edit_original;
    int sel_field;
    int field_scroll;

    /* Modal flags */
    int safe_mode;
    int command_mode;
    char command_input[256];
    int command_len;
    int prompt_mode;
    char prompt_text[256];
    void (*prompt_action)(struct Browser *);
    int help_mode;
    int help_scroll;

    /* Sort overlay */
    int sort_mode;
    char sort_context;       /* 't', 'c', 'r' */
    char **sort_items;
    int nsort_items;
    int sort_selected;
    int sort_grabbed;        /* -1 = not grabbed */
    int sort_scroll;
    /* Row sort state (for sort_context == 'r') */
    char **sort_active_cols;
    char **sort_directions;  /* "ASC" or "DESC" per active col */
    int nsort_active;

    /* Find/filter */
    int find_mode;
    char find_input[1024];
    int find_input_len;
    int find_input_pos;      /* cursor position */
    char find_filter[1024];
    char *find_where;
    char **find_params;
    int nfind_params;

    /* Find/filter history */
    char **find_history;     /* malloc'd strings, newest at [0] */
    int nfind_history;
    int find_history_pos;    /* -1 = editing fresh, >=0 = browsing index */

    /* Find dialog */
    int find_dialog;
    char **find_dialog_inputs;
    int *find_dialog_and_flags;
    int find_dialog_focus;
    int find_dialog_scroll;

    /* Feedback */
    char *message;
    int quit_flag;

    /* Clipboard */
    char *pending_paste;  /* held across safe-mode paste confirmation */
    int pending_cut;      /* 1 if safe-mode confirmation is for a cut (copy+NULL) */

    /* Drillthrough state (view row → source table full row) */
    int drillthrough_mode;
    char *drillthrough_table;
    char **drillthrough_cols;
    char **drillthrough_vals;
    int drillthrough_ncols;

    /* Config cache */
    char **table_display_order;
    int ntable_order;
} Browser;

/* Lifecycle */
void browser_init(Browser *b, Database *db);
void browser_destroy(Browser *b);

/* Load find history for a given view and context */
void browser_load_find_history(Browser *b, const char *view, const char *context);

/* Main loop */
void browser_run(Browser *b, WINDOW *stdscr);

/* Load table data into browser state */
void browser_load_table(Browser *b, const char *table_name);

/* Free current table data */
void browser_free_table_data(Browser *b);

/* Cache formatted row strings */
void browser_cache_row_strings(Browser *b);

/* Apply saved column order to current_columns and rebuild row data */
void browser_apply_column_order(Browser *b);

/* Populate edit_values/edit_original from current sel_row */
void browser_populate_edit(Browser *b);

/* Set a status message (takes ownership of msg if heap-allocated) */
void browser_set_message(Browser *b, const char *msg);

/* Dump the ncurses virtual screen (text + color-pair per row) to a timestamped
 * file in /tmp and set b->message to the path. */
void browser_dump_screen(Browser *b);

/* Sort overlay */
void browser_open_sort(Browser *b);
void browser_apply_sort(Browser *b);
void browser_reset_sort(Browser *b);
void browser_free_sort_state(Browser *b);

/* Find operations */
void browser_parse_find_pattern(Browser *b, const char *pattern);
void browser_enter_find_dialog(Browser *b);
void browser_apply_find_dialog(Browser *b);
void browser_free_find_dialog(Browser *b);

/* Config-driven table order */
void browser_load_config(Browser *b);

/* Drillthrough: open source table row from a view row */
bool browser_enter_drillthrough(Browser *b);
void browser_free_drillthrough(Browser *b);

#endif
