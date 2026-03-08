#include "db.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>

Database *db_open(const char *path)
{
    Database *d = xcalloc(1, sizeof(*d));
    d->path = xstrdup(path);
    int rc = sqlite3_open(path, &d->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(d->db));
        sqlite3_close(d->db);
        free(d->path);
        free(d);
        return NULL;
    }
    return d;
}

void db_close(Database *d)
{
    if (!d) return;
    sqlite3_close(d->db);
    free(d->path);
    free(d);
}

TableInfo *db_get_tables(Database *d, int *count)
{
    *count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db,
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' "
        "AND name != '_browse_config' ORDER BY name",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    Vec names;
    vec_init(&names);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        vec_push(&names, xstrdup(name));
    }
    sqlite3_finalize(stmt);

    int n = (int)names.len;
    TableInfo *tables = xcalloc((size_t)n, sizeof(TableInfo));
    for (int i = 0; i < n; i++) {
        tables[i].name = names.items[i];
        /* Get columns via PRAGMA */
        Buf sql;
        buf_init(&sql);
        buf_printf(&sql, "PRAGMA table_info(\"%s\")", tables[i].name);
        rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
        buf_free(&sql);
        if (rc != SQLITE_OK) continue;

        Vec cols;
        vec_init(&cols);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(stmt, 1);
            vec_push(&cols, xstrdup(col));
        }
        sqlite3_finalize(stmt);

        tables[i].ncols = (int)cols.len;
        tables[i].columns = xcalloc(cols.len, sizeof(char *));
        for (size_t j = 0; j < cols.len; j++)
            tables[i].columns[j] = cols.items[j];
        vec_free(&cols);
    }
    vec_free(&names);

    *count = n;
    return tables;
}

void db_free_tables(TableInfo *tables, int count)
{
    if (!tables) return;
    for (int i = 0; i < count; i++) {
        free(tables[i].name);
        for (int j = 0; j < tables[i].ncols; j++)
            free(tables[i].columns[j]);
        free(tables[i].columns);
    }
    free(tables);
}

char **db_get_primary_keys(Database *d, const char *table, int *count)
{
    *count = 0;
    Buf sql;
    buf_init(&sql);
    buf_printf(&sql, "PRAGMA table_info(\"%s\")", table);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) return NULL;

    Vec pks;
    vec_init(&pks);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int pk = sqlite3_column_int(stmt, 5);
        if (pk > 0) {
            const char *col = (const char *)sqlite3_column_text(stmt, 1);
            vec_push(&pks, xstrdup(col));
        }
    }
    sqlite3_finalize(stmt);

    *count = (int)pks.len;
    char **result = xcalloc(pks.len ? pks.len : 1, sizeof(char *));
    for (size_t i = 0; i < pks.len; i++)
        result[i] = pks.items[i];
    vec_free(&pks);
    return result;
}

void db_free_string_array(char **arr, int count)
{
    if (!arr) return;
    for (int i = 0; i < count; i++)
        free(arr[i]);
    free(arr);
}

void db_load_rows(Database *d, const char *table, const char **columns, int ncols,
                  const char *where_clause, const char **where_params, int nparams,
                  const char *order_by, RowSet *out)
{
    out->rows = NULL;
    out->nrows = 0;
    out->cap = 0;

    Buf sql;
    buf_init(&sql);
    buf_append_str(&sql, "SELECT rowid");
    for (int i = 0; i < ncols; i++)
        buf_printf(&sql, ", \"%s\"", columns[i]);
    buf_printf(&sql, " FROM \"%s\"", table);
    if (where_clause && where_clause[0])
        buf_printf(&sql, " WHERE %s", where_clause);
    if (order_by && order_by[0])
        buf_printf(&sql, " ORDER BY %s", order_by);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) return;

    for (int i = 0; i < nparams; i++)
        sqlite3_bind_text(stmt, i + 1, where_params[i], -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (out->nrows >= out->cap) {
            out->cap = out->cap ? out->cap * 2 : 256;
            out->rows = xrealloc(out->rows, (size_t)out->cap * sizeof(Row));
        }
        Row *r = &out->rows[out->nrows];
        r->rowid = sqlite3_column_int64(stmt, 0);
        r->ncols = ncols;
        r->values = xcalloc((size_t)ncols, sizeof(char *));
        for (int i = 0; i < ncols; i++) {
            if (sqlite3_column_type(stmt, i + 1) == SQLITE_NULL)
                r->values[i] = NULL;
            else {
                const char *val = (const char *)sqlite3_column_text(stmt, i + 1);
                r->values[i] = xstrdup(val ? val : "");
            }
        }
        out->nrows++;
    }
    sqlite3_finalize(stmt);
}

void db_free_rowset(RowSet *rs)
{
    if (!rs) return;
    for (int i = 0; i < rs->nrows; i++) {
        for (int j = 0; j < rs->rows[i].ncols; j++)
            free(rs->rows[i].values[j]);
        free(rs->rows[i].values);
    }
    free(rs->rows);
    rs->rows = NULL;
    rs->nrows = 0;
    rs->cap = 0;
}

int db_row_count(Database *d, const char *table,
                 const char *where_clause, const char **where_params, int nparams)
{
    Buf sql;
    buf_init(&sql);
    buf_printf(&sql, "SELECT count(*) FROM \"%s\"", table);
    if (where_clause && where_clause[0])
        buf_printf(&sql, " WHERE %s", where_clause);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) return 0;

    for (int i = 0; i < nparams; i++)
        sqlite3_bind_text(stmt, i + 1, where_params[i], -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int db_delete_row(Database *d, const char *table, long long rowid)
{
    Buf sql;
    buf_init(&sql);
    buf_printf(&sql, "DELETE FROM \"%s\" WHERE rowid = ?", table);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, rowid);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_drop_table(Database *d, const char *table)
{
    Buf sql;
    buf_init(&sql);
    buf_printf(&sql, "DROP TABLE \"%s\"", table);
    char *errmsg = NULL;
    int rc = sqlite3_exec(d->db, sql.data, NULL, NULL, &errmsg);
    buf_free(&sql);
    if (errmsg) sqlite3_free(errmsg);
    return (rc == SQLITE_OK) ? 0 : -1;
}

int db_update_row(Database *d, const char *table, const char **columns, int ncols,
                  const char **values, long long rowid)
{
    Buf sql;
    buf_init(&sql);
    buf_printf(&sql, "UPDATE \"%s\" SET ", table);
    for (int i = 0; i < ncols; i++) {
        if (i > 0) buf_append_str(&sql, ", ");
        buf_printf(&sql, "\"%s\" = ?", columns[i]);
    }
    buf_append_str(&sql, " WHERE rowid = ?");

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db, sql.data, -1, &stmt, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) return -1;

    for (int i = 0; i < ncols; i++) {
        if (values[i] == NULL)
            sqlite3_bind_null(stmt, i + 1);
        else
            sqlite3_bind_text(stmt, i + 1, values[i], -1, SQLITE_STATIC);
    }
    sqlite3_bind_int64(stmt, ncols + 1, rowid);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* --- Config persistence --- */

void db_ensure_config_table(Database *d)
{
    sqlite3_exec(d->db,
        "CREATE TABLE IF NOT EXISTS _browse_config "
        "(key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        NULL, NULL, NULL);
}

char *db_load_config(Database *d, const char *key)
{
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db,
        "SELECT value FROM _browse_config WHERE key = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = xstrdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

int db_save_config(Database *d, const char *key, const char *json_value)
{
    db_ensure_config_table(d);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(d->db,
        "INSERT OR REPLACE INTO _browse_config (key, value) VALUES (?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, json_value, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* --- Read-only detection --- */

int db_is_readonly(Database *d)
{
    return sqlite3_db_readonly(d->db, "main");
}

char *db_readonly_message(Database *d)
{
    struct stat st;
    if (stat(d->path, &st) == 0) {
        /* Build permission string like -r--r--r-- */
        char mode[11];
        mode[0] = S_ISDIR(st.st_mode) ? 'd' : '-';
        mode[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
        mode[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
        mode[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
        mode[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
        mode[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
        mode[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
        mode[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
        mode[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
        mode[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
        mode[10] = '\0';

        const char *owner = "?";
        const char *group = "?";
        struct passwd *pw = getpwuid(st.st_uid);
        if (pw) owner = pw->pw_name;
        struct group *gr = getgrgid(st.st_gid);
        if (gr) group = gr->gr_name;

        Buf b;
        buf_init(&b);
        buf_printf(&b, "Database is read-only (%s %s:%s)", mode, owner, group);
        return buf_detach(&b);
    }
    return xstrdup("Database is read-only");
}
