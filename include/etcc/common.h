/*
 * Ethernos Tiny C Compiler (ETCC)
 * common.h - arena allocation, string builder and small utilities.
 */
#ifndef ETCC_COMMON_H
#define ETCC_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct EtccArenaBlock EtccArenaBlock;

typedef struct EtccArena {
    EtccArenaBlock *head;
    size_t total;
} EtccArena;

void etcc_arena_init(EtccArena *a);
void *etcc_arena_alloc(EtccArena *a, size_t size, size_t align);
char *etcc_arena_strndup(EtccArena *a, const char *s, size_t n);
char *etcc_arena_strdup(EtccArena *a, const char *s);
char *etcc_arena_vprintf(EtccArena *a, const char *fmt, va_list ap);
char *etcc_arena_printf(EtccArena *a, const char *fmt, ...);
void etcc_arena_reset(EtccArena *a);

typedef struct EtccStrBuf {
    char *data;
    size_t len;
    size_t cap;
} EtccStrBuf;

void etcc_sb_init(EtccStrBuf *sb);
bool etcc_sb_reserve(EtccStrBuf *sb, size_t extra);
bool etcc_sb_append(EtccStrBuf *sb, const char *data, size_t len);
bool etcc_sb_append_cstr(EtccStrBuf *sb, const char *s);
bool etcc_sb_append_char(EtccStrBuf *sb, char c);
bool etcc_sb_printf(EtccStrBuf *sb, const char *fmt, ...);
char *etcc_sb_detach(EtccStrBuf *sb);
void etcc_sb_free(EtccStrBuf *sb);

/* Grow a vector's backing store. Returns false on OOM. */
bool etcc_vec_grow(void **items, size_t *cap, size_t elem_size);

#define ETCC_VEC(T) struct { T *items; size_t len; size_t cap; }

/*
 * Push into an ETCC_VEC. `v` must be an lvalue and `value` must be
 * assignable to its element type. Evaluates to bool.
 */
#define ETCC_VEC_PUSH(v, value)                                              \
    (((v).len == (v).cap                                                   \
          ? etcc_vec_grow((void **)&(v).items, &(v).cap, sizeof(*(v).items)) \
          : true)                                                          \
         ? ((v).items[(v).len++] = (value), true)                           \
         : false)

void *etcc_xmalloc(size_t size);
void *etcc_xrealloc(void *ptr, size_t size);
char *etcc_xstrdup(const char *s);

#endif /* ETCC_COMMON_H */
