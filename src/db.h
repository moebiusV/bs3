/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef DB_H
#define DB_H

#include <sqlite3.h>

/* Table metadata */
typedef struct {
    char *name;
    char **columns;
    int ncols;
    int is_view;
} TableInfo;

/* A single row of data */
typedef struct {
    long long rowid;
    char **values;   /* ncols strings; NULL for SQL NULL */
    int ncols;
} Row;

/* A set of rows */
typedef struct {
    Row *rows;
    int nrows;
    int cap;
} RowSet;

/* Database handle */
typedef struct {
    sqlite3 *db;
    char *path;
} Database;

/* Open/close */
Database *db_open(const char *path);
void db_close(Database *d);

/* Schema queries */
TableInfo *db_get_tables(Database *d, int *count);
void db_free_tables(TableInfo *tables, int count);

/* Primary keys for a table */
char **db_get_primary_keys(Database *d, const char *table, int *count);
void db_free_string_array(char **arr, int count);

/* Row data */
void db_load_rows(Database *d, const char *table, const char **columns, int ncols,
                  const char *where_clause, const char **where_params, int nparams,
                  const char *order_by, int is_view, RowSet *out);
void db_free_rowset(RowSet *rs);

/* Row count (with optional WHERE) */
int db_row_count(Database *d, const char *table,
                 const char *where_clause, const char **where_params, int nparams);

/* Mutations */
int db_delete_row(Database *d, const char *table, long long rowid);
int db_drop_table(Database *d, const char *table);
int db_update_row(Database *d, const char *table, const char **columns, int ncols,
                  const char **values, long long rowid);

/* Config persistence (_browse_config table) */
void db_ensure_config_table(Database *d);
char *db_load_config(Database *d, const char *key);   /* returns JSON string or NULL */
int db_save_config(Database *d, const char *key, const char *json_value);

/* Fetch one row using a custom SQL query with a single ? bind parameter.
   Column names come from the result set (sqlite3_column_name).
   Returns 1 on success, 0 on failure or no matching row.
   On success sets cols_out/ncols_out/vals_out; caller frees with db_free_string_array. */
int db_fetch_row_by_sql(Database *d, const char *sql, const char *bind_val,
                        char ***cols_out, int *ncols_out, char ***vals_out);

/* Check if database is read-only and build a message */
int db_is_readonly(Database *d);
char *db_readonly_message(Database *d);

#endif
