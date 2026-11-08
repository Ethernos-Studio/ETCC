#include "etcc/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EtccArenaBlock {
    EtccArenaBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[];
};

#define ETCC_ARENA_BLOCK_MIN 4096u
#define ETCC_ALIGN_UP(n, a) (((n) + ((a) - 1)) & ~((size_t)(a) - 1))

void etcc_arena_init(EtccArena *a) {
    a->head = NULL;
    a->total = 0;
}

static void *etcc_oom(void) {
    fputs("etcc: fatal: out of memory\n", stderr);
    exit(70);
    return NULL;
}

void *etcc_xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) etcc_oom();
    return p;
}

void *etcc_xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (!p) etcc_oom();
    return p;
}

char *etcc_xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = etcc_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

void *etcc_arena_alloc(EtccArena *a, size_t size, size_t align) {
    if (align == 0) align = 1;
    if (size == 0) size = 1;

    EtccArenaBlock *b = a->head;
    if (b) {
        size_t off = ETCC_ALIGN_UP(b->used, align);
        if (off <= b->cap && size <= b->cap - off) {
            b->used = off + size;
            return b->data + off;
        }
    }

    size_t cap = ETCC_ARENA_BLOCK_MIN;
    if (size + align > cap) cap = size + align;
    cap = ETCC_ALIGN_UP(cap, 16);

    b = malloc(sizeof(*b) + cap);
    if (!b) etcc_oom();
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total += cap;

    size_t off = ETCC_ALIGN_UP(b->used, align);
    b->used = off + size;
    return b->data + off;
}

char *etcc_arena_strndup(EtccArena *a, const char *s, size_t n) {
    char *p = etcc_arena_alloc(a, n + 1, 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *etcc_arena_strdup(EtccArena *a, const char *s) {
    return etcc_arena_strndup(a, s, strlen(s));
}

char *etcc_arena_vprintf(EtccArena *a, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return etcc_arena_strdup(a, "<format-error>");

    char *p = etcc_arena_alloc(a, (size_t)n + 1, 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *etcc_arena_printf(EtccArena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *p = etcc_arena_vprintf(a, fmt, ap);
    va_end(ap);
    return p;
}

void etcc_arena_reset(EtccArena *a) {
    EtccArenaBlock *b = a->head;
    while (b) {
        EtccArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
    a->total = 0;
}

void etcc_sb_init(EtccStrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

bool etcc_sb_reserve(EtccStrBuf *sb, size_t extra) {
    if (extra > SIZE_MAX - sb->len - 1) return false;
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return true;
    size_t cap = sb->cap ? sb->cap : 128;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *p = realloc(sb->data, cap);
    if (!p) return false;
    sb->data = p;
    sb->cap = cap;
    return true;
}

bool etcc_sb_append(EtccStrBuf *sb, const char *data, size_t len) {
    if (!etcc_sb_reserve(sb, len)) return false;
    if (len) memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

bool etcc_sb_append_cstr(EtccStrBuf *sb, const char *s) {
    return etcc_sb_append(sb, s, strlen(s));
}

bool etcc_sb_append_char(EtccStrBuf *sb, char c) {
    return etcc_sb_append(sb, &c, 1);
}

bool etcc_sb_printf(EtccStrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        return false;
    }
    if (!etcc_sb_reserve(sb, (size_t)n)) {
        va_end(ap);
        return false;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    sb->len += (size_t)n;
    va_end(ap);
    return true;
}

char *etcc_sb_detach(EtccStrBuf *sb) {
    if (!sb->data) {
        sb->data = etcc_xmalloc(1);
        sb->data[0] = '\0';
    }
    char *p = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return p;
}

void etcc_sb_free(EtccStrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

bool etcc_vec_grow(void **items, size_t *cap, size_t elem_size) {
    size_t ncap = *cap ? *cap * 2 : 8;
    if (ncap > SIZE_MAX / elem_size) return false;
    void *p = realloc(*items, ncap * elem_size);
    if (!p) return false;
    *items = p;
    *cap = ncap;
    return true;
}
