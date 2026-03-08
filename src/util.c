/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* --- Die-on-OOM wrappers --- */

void *xmalloc(size_t size)
{
    void *p = malloc(size);
    if (!p && size) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p = calloc(count, size);
    if (!p && count && size) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (!p && size) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

char *xstrdup(const char *s)
{
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    if (!s) return NULL;
    char *p = strndup(s, n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

/* --- Dynamic string buffer --- */

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void buf_clear(Buf *b)
{
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_grow(Buf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 64;
    while (newcap < b->len + need + 1)
        newcap *= 2;
    b->data = xrealloc(b->data, newcap);
    b->cap = newcap;
}

void buf_append(Buf *b, const char *s, size_t n)
{
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(Buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_append_char(Buf *b, char c)
{
    buf_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *buf_detach(Buf *b)
{
    char *p = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

/* --- Dynamic pointer array --- */

void vec_init(Vec *v)
{
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

void vec_free(Vec *v)
{
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
}

void vec_push(Vec *v, void *item)
{
    if (v->len >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = xrealloc(v->items, v->cap * sizeof(void *));
    }
    v->items[v->len++] = item;
}

void vec_clear(Vec *v)
{
    v->len = 0;
}

/* --- String helpers --- */

char *str_collapse_whitespace(const char *s)
{
    if (!s) return xstrdup("");
    size_t len = strlen(s);
    char *out = xmalloc(len + 1);
    size_t j = 0;
    int in_space = 1; /* skip leading whitespace */
    for (size_t i = 0; i < len; i++) {
        if (isspace((unsigned char)s[i])) {
            if (!in_space) {
                out[j++] = ' ';
                in_space = 1;
            }
        } else {
            out[j++] = s[i];
            in_space = 0;
        }
    }
    /* trim trailing space */
    if (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
    return out;
}
