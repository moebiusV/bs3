#ifndef BROWSER_H
#define BROWSER_H

#include <ncursesw/curses.h>
#include "db.h"

typedef enum { VIEW_TABLES, VIEW_ROWS, VIEW_EDIT } View;

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

    /* Find/filter */
    int find_mode;
    char find_input[1024];
    int find_input_len;
    int find_input_pos;      /* cursor position */
    char find_filter[1024];
    char *find_where;
    char **find_params;
    int nfind_params;

    /* Find dialog */
    int find_dialog;
    char **find_dialog_inputs;
    int *find_dialog_and_flags;
    int find_dialog_focus;
    int find_dialog_scroll;

    /* Feedback */
    char *message;
    int quit_flag;

    /* Config cache */
    char **table_display_order;
    int ntable_order;
} Browser;

/* Lifecycle */
void browser_init(Browser *b, Database *db);
void browser_destroy(Browser *b);

/* Main loop */
void browser_run(Browser *b, WINDOW *stdscr);

/* Load table data into browser state */
void browser_load_table(Browser *b, const char *table_name);

/* Free current table data */
void browser_free_table_data(Browser *b);

/* Cache formatted row strings */
void browser_cache_row_strings(Browser *b);

/* Set a status message (takes ownership of msg if heap-allocated) */
void browser_set_message(Browser *b, const char *msg);

#endif
