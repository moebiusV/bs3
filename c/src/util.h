#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Die-on-OOM wrappers */
void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Dynamic string buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_append(Buf *b, const char *s, size_t n);
void buf_append_str(Buf *b, const char *s);
void buf_append_char(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Detach buffer contents (caller owns returned pointer) */
char *buf_detach(Buf *b);

/* Dynamic pointer array */
typedef struct {
    void **items;
    size_t len;
    size_t cap;
} Vec;

void vec_init(Vec *v);
void vec_free(Vec *v);
void vec_push(Vec *v, void *item);
void vec_clear(Vec *v);

/* String helpers */
/* Collapse runs of whitespace to single spaces, trim ends */
char *str_collapse_whitespace(const char *s);

#endif
