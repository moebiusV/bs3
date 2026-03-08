/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ncursesw/curses.h>
#include "browser.h"
#include "db.h"
#include "util.h"

static void usage(void)
{
    fprintf(stderr, "Usage: bs3 <database.db> [table]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
        usage();

    const char *db_path = argv[1];
    const char *initial_table = argc == 3 ? argv[2] : NULL;

    /* Required for wide-character ncurses support */
    setlocale(LC_ALL, "");

    Database *db = db_open(db_path);
    if (!db) return 1;

    Browser b;
    browser_init(&b, db);

    /* Jump to specific table if requested */
    if (initial_table) {
        int found = 0;
        for (int i = 0; i < b.ntables; i++) {
            if (strcasecmp(b.tables[i].name, initial_table) == 0) {
                b.sel_table = i;
                browser_load_table(&b, b.tables[i].name);
                b.current_view = VIEW_ROWS;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Table not found: %s\n", initial_table);
            browser_destroy(&b);
            db_close(db);
            return 1;
        }
    }

    /* Initialize curses */
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    browser_run(&b, stdscr);

    endwin();
    browser_destroy(&b);
    db_close(db);
    return 0;
}
