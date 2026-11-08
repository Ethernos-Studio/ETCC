#include "etcc/pp.h"

#include "etcc/platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct EtccPPCond {
    bool parent_active;
    bool active;
    bool any_taken;
    bool seen_else;
    EtccLoc loc;
} EtccPPCond;

#define ETCC_PP_MAX_DEPTH 64
#define ETCC_PP_MAX_COND 128

static bool pp_active(const EtccPP *pp) {
    if (pp->nconds == 0) return true;
    EtccPPCond *c = (EtccPPCond *)pp->conds;
    return c[pp->nconds - 1].active;
}

static bool pp_push_cond(EtccPP *pp, bool value, EtccLoc loc) {
    if (pp->nconds >= ETCC_PP_MAX_COND) {
        etcc_diag_error(pp->diag, loc, "conditional nesting is too deep");
        return false;
    }
    if (pp->nconds == pp->cap_conds) {
        size_t ncap = pp->cap_conds ? pp->cap_conds * 2 : 16;
        EtccPPCond *p = realloc(pp->conds, ncap * sizeof(*p));
        if (!p) return false;
        pp->conds = p;
        pp->cap_conds = ncap;
    }
    EtccPPCond *c = (EtccPPCond *)pp->conds;
    bool parent = pp_active(pp);
    c[pp->nconds++] = (EtccPPCond){
        .parent_active = parent,
        .active = parent && value,
        .any_taken = value,
        .seen_else = false,
        .loc = loc,
    };
    return true;
}

static void pp_pop_cond(EtccPP *pp) {
    if (pp->nconds) pp->nconds--;
}

static bool str_eq_n(const char *s, size_t n, const char *lit) {
    size_t m = strlen(lit);
    return n == m && memcmp(s, lit, m) == 0;
}

static bool ci_path_equal(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static bool pp_was_once(const EtccPP *pp, const char *canonical) {
    for (size_t i = 0; i < pp->once_files.len; i++) {
        if (ci_path_equal(pp->once_files.items[i], canonical)) return true;
    }
    return false;
}

static void pp_mark_once(EtccPP *pp, const char *canonical) {
    if (pp_was_once(pp, canonical)) return;
    char *copy = etcc_xstrdup(canonical);
    if (!ETCC_VEC_PUSH(pp->once_files, copy)) {
        free(copy);
        etcc_diag_error_global(pp->diag, "out of memory");
    }
}

const EtccMacro *etcc_macro_find(const EtccMacroTable *t, const char *name,
                                 size_t len) {
    for (size_t i = 0; i < t->len; i++) {
        size_t n = strlen(t->items[i].name);
        if (n == len && memcmp(t->items[i].name, name, n) == 0)
            return &t->items[i];
    }
    return NULL;
}

bool etcc_macro_define(EtccArena *a, EtccMacroTable *t, const char *name,
                       size_t name_len, const char *body, size_t body_len,
                       bool function_like) {
    const EtccMacro *existing = etcc_macro_find(t, name, name_len);
    if (existing) {
        EtccMacro *m = (EtccMacro *)existing;
        m->body = etcc_arena_strndup(a, body, body_len);
        m->body_len = body_len;
        m->function_like = function_like;
        return true;
    }
    if (t->len == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 16;
        EtccMacro *p = realloc(t->items, ncap * sizeof(*p));
        if (!p) return false;
        t->items = p;
        t->cap = ncap;
    }
    t->items[t->len++] = (EtccMacro){
        .name = etcc_arena_strndup(a, name, name_len),
        .body = etcc_arena_strndup(a, body, body_len),
        .body_len = body_len,
        .function_like = function_like,
    };
    return true;
}

bool etcc_macro_undef(EtccMacroTable *t, const char *name, size_t len) {
    for (size_t i = 0; i < t->len; i++) {
        size_t n = strlen(t->items[i].name);
        if (n == len && memcmp(t->items[i].name, name, n) == 0) {
            t->items[i] = t->items[t->len - 1];
            t->len--;
            return true;
        }
    }
    return true;
}

void etcc_pp_init(EtccPP *pp, EtccArena *arena, EtccDiagnostics *diag) {
    memset(pp, 0, sizeof(*pp));
    pp->arena = arena;
    pp->diag = diag;
    etcc_source_map_init(&pp->map, NULL);
    etcc_sb_init(&pp->out);
}

void etcc_pp_add_include_dir(EtccPP *pp, const char *dir) {
    char *copy = etcc_xstrdup(dir);
    if (!ETCC_VEC_PUSH(pp->include_dirs, copy)) {
        free(copy);
        etcc_diag_error_global(pp->diag, "out of memory");
    }
}

static EtccSourceFile *pp_register_file(EtccPP *pp, const char *display_path,
                                        const char *canonical, char *data,
                                        size_t len) {
    EtccSourceFile *f = etcc_xmalloc(sizeof(*f));
    f->path = etcc_arena_strdup(pp->arena, display_path);
    f->canonical = etcc_arena_strdup(pp->arena, canonical);
    f->data = data;
    f->len = len;

    if (pp->nfiles == pp->cap_files) {
        size_t ncap = pp->cap_files ? pp->cap_files * 2 : 8;
        EtccSourceFile **p = realloc(pp->files, ncap * sizeof(*p));
        if (!p) {
            etcc_diag_error_global(pp->diag, "out of memory");
            return NULL;
        }
        pp->files = p;
        pp->cap_files = ncap;
    }
    pp->files[pp->nfiles++] = f;
    if (pp->map.fallback == NULL) pp->map.fallback = f;
    return f;
}

static EtccSourceFile *pp_load_file(EtccPP *pp, const char *path,
                                    EtccLoc include_loc) {
    char *canonical = etcc_path_canonical(pp->arena, path);
    if (pp_was_once(pp, canonical)) return NULL;

    size_t len = 0;
    const char *err = NULL;
    char *data = etcc_read_file(path, &len, &err);
    if (!data) {
        etcc_diag_error(pp->diag, include_loc, "cannot read '%s': %s", path,
                        err ? err : "unknown error");
        return NULL;
    }
    return pp_register_file(pp, path, canonical, data, len);
}

static char *pp_resolve_include(EtccPP *pp, const EtccSourceFile *current,
                                const char *name, bool quoted, EtccLoc loc) {
    if (quoted && current && strcmp(current->canonical, "<buffer>") != 0) {
        char *dir = etcc_path_dirname(pp->arena, current->path);
        char *candidate = etcc_path_join(pp->arena, dir, name);
        if (etcc_file_exists(candidate)) return candidate;
    }
    for (size_t i = 0; i < pp->include_dirs.len; i++) {
        char *candidate =
            etcc_path_join(pp->arena, pp->include_dirs.items[i], name);
        if (etcc_file_exists(candidate)) return candidate;
    }
    if (quoted) {
        if (etcc_file_exists(name)) return etcc_arena_strdup(pp->arena, name);
    }
    etcc_diag_error(pp->diag, loc, "include file not found: '%s'", name);
    return NULL;
}

/* ------------------------- #if expression evaluator ---------------------- */

typedef struct {
    EtccPP *pp;
    const char *s;
    size_t len;
    size_t pos;
    EtccLoc loc;
    int depth;
    bool failed;
} PPExpr;

static void pe_skip(PPExpr *e) {
    for (;;) {
        while (e->pos < e->len &&
               (e->s[e->pos] == ' ' || e->s[e->pos] == '\t' ||
                e->s[e->pos] == '\r' || e->s[e->pos] == '\n'))
            e->pos++;
        if (e->pos + 1 < e->len && e->s[e->pos] == '/' &&
            e->s[e->pos + 1] == '/') {
            e->pos = e->len;
            return;
        }
        if (e->pos + 1 < e->len && e->s[e->pos] == '/' &&
            e->s[e->pos + 1] == '*') {
            e->pos += 2;
            while (e->pos + 1 < e->len &&
                   !(e->s[e->pos] == '*' && e->s[e->pos + 1] == '/'))
                e->pos++;
            if (e->pos + 1 < e->len) e->pos += 2;
            continue;
        }
        return;
    }
}

static bool pe_match(PPExpr *e, const char *op) {
    pe_skip(e);
    size_t n = strlen(op);
    if (e->pos + n <= e->len && memcmp(e->s + e->pos, op, n) == 0) {
        e->pos += n;
        return true;
    }
    return false;
}

static int64_t pe_cond(PPExpr *e);
static int64_t pe_lor(PPExpr *e);

static int64_t pe_primary(PPExpr *e) {
    pe_skip(e);
    if (e->pos >= e->len) {
        etcc_diag_error(e->pp->diag, e->loc,
                        "expected expression in preprocessor condition");
        e->failed = true;
        return 0;
    }

    if (e->s[e->pos] == '(') {
        e->pos++;
        int64_t v = pe_cond(e);
        pe_skip(e);
        if (e->pos < e->len && e->s[e->pos] == ')') e->pos++;
        else {
            etcc_diag_error(e->pp->diag, e->loc, "missing ')' in #if");
            e->failed = true;
        }
        return v;
    }

    if (e->s[e->pos] == '\'') {
        e->pos++;
        int64_t v = 0;
        if (e->pos < e->len && e->s[e->pos] == '\\') {
            e->pos++;
            if (e->pos < e->len) {
                char c = e->s[e->pos++];
                switch (c) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0; break;
                case '\\': v = '\\'; break;
                case '\'': v = '\''; break;
                default: v = (unsigned char)c; break;
                }
            }
        } else if (e->pos < e->len) {
            v = (unsigned char)e->s[e->pos++];
        }
        if (e->pos < e->len && e->s[e->pos] == '\'') e->pos++;
        else {
            etcc_diag_error(e->pp->diag, e->loc, "unterminated character literal");
            e->failed = true;
        }
        return v;
    }

    if (isdigit((unsigned char)e->s[e->pos])) {
        char *end = NULL;
        char *tmp = etcc_arena_strndup(e->pp->arena, e->s + e->pos,
                                       e->len - e->pos);
        unsigned long long v = strtoull(tmp, &end, 0);
        size_t used = (size_t)(end - tmp);
        if (used == 0) {
            etcc_diag_error(e->pp->diag, e->loc, "invalid integer in #if");
            e->failed = true;
            return 0;
        }
        e->pos += used;
        /* Skip integer suffixes. */
        while (e->pos < e->len &&
               strchr("uUlL", e->s[e->pos]) != NULL)
            e->pos++;
        return (int64_t)v;
    }

    if (isalpha((unsigned char)e->s[e->pos]) || e->s[e->pos] == '_') {
        size_t start = e->pos;
        while (e->pos < e->len &&
               (isalnum((unsigned char)e->s[e->pos]) || e->s[e->pos] == '_'))
            e->pos++;
        size_t n = e->pos - start;
        const char *name = e->s + start;

        if (str_eq_n(name, n, "defined")) {
            pe_skip(e);
            bool paren = false;
            if (e->pos < e->len && e->s[e->pos] == '(') {
                paren = true;
                e->pos++;
                pe_skip(e);
            }
            size_t ns = e->pos;
            while (e->pos < e->len &&
                   (isalnum((unsigned char)e->s[e->pos]) ||
                    e->s[e->pos] == '_'))
                e->pos++;
            size_t nn = e->pos - ns;
            bool found = nn && etcc_macro_find(&e->pp->macros,
                                               e->s + ns, nn) != NULL;
            if (paren) {
                pe_skip(e);
                if (e->pos < e->len && e->s[e->pos] == ')') e->pos++;
                else {
                    etcc_diag_error(e->pp->diag, e->loc,
                                    "missing ')' after defined(");
                    e->failed = true;
                }
            }
            return found ? 1 : 0;
        }

        const EtccMacro *m = etcc_macro_find(&e->pp->macros, name, n);
        if (!m || m->function_like) return 0;
        if (e->depth > 32) {
            etcc_diag_error(e->pp->diag, e->loc,
                            "macro expansion is too deep in #if");
            e->failed = true;
            return 0;
        }
        PPExpr sub = {
            .pp = e->pp,
            .s = m->body,
            .len = m->body_len,
            .pos = 0,
            .loc = e->loc,
            .depth = e->depth + 1,
        };
        int64_t v = m->body_len ? pe_cond(&sub) : 0;
        if (sub.failed) e->failed = true;
        return v;
    }

    etcc_diag_error(e->pp->diag, e->loc,
                    "unexpected character '%c' in #if", e->s[e->pos]);
    e->failed = true;
    e->pos++;
    return 0;
}

static int64_t pe_unary(PPExpr *e) {
    if (pe_match(e, "!")) return !pe_unary(e);
    if (pe_match(e, "~")) return ~pe_unary(e);
    if (pe_match(e, "-")) return -pe_unary(e);
    if (pe_match(e, "+")) return +pe_unary(e);
    return pe_primary(e);
}

static int64_t pe_mul(PPExpr *e) {
    int64_t v = pe_unary(e);
    for (;;) {
        if (pe_match(e, "*")) v *= pe_unary(e);
        else if (pe_match(e, "/")) {
            int64_t r = pe_unary(e);
            if (r == 0) {
                etcc_diag_error(e->pp->diag, e->loc,
                                "division by zero in #if");
                e->failed = true;
            } else v /= r;
        } else if (pe_match(e, "%")) {
            int64_t r = pe_unary(e);
            if (r == 0) {
                etcc_diag_error(e->pp->diag, e->loc,
                                "division by zero in #if");
                e->failed = true;
            } else v %= r;
        } else break;
    }
    return v;
}

static int64_t pe_add(PPExpr *e) {
    int64_t v = pe_mul(e);
    for (;;) {
        if (pe_match(e, "+")) v += pe_mul(e);
        else if (pe_match(e, "-")) v -= pe_mul(e);
        else break;
    }
    return v;
}

static int64_t pe_shift(PPExpr *e) {
    int64_t v = pe_add(e);
    for (;;) {
        if (pe_match(e, "<<")) {
            int64_t r = pe_add(e);
            v = (r < 0 || r >= 64) ? 0 : (int64_t)((uint64_t)v << r);
        } else if (pe_match(e, ">>")) {
            int64_t r = pe_add(e);
            v = (r < 0 || r >= 64) ? 0 : (v >> r);
        } else break;
    }
    return v;
}

static int64_t pe_rel(PPExpr *e) {
    int64_t v = pe_shift(e);
    for (;;) {
        if (pe_match(e, "<=")) v = v <= pe_shift(e);
        else if (pe_match(e, ">=")) v = v >= pe_shift(e);
        else if (pe_match(e, "<")) v = v < pe_shift(e);
        else if (pe_match(e, ">")) v = v > pe_shift(e);
        else break;
    }
    return v;
}

static int64_t pe_eq(PPExpr *e) {
    int64_t v = pe_rel(e);
    for (;;) {
        if (pe_match(e, "==")) v = v == pe_rel(e);
        else if (pe_match(e, "!=")) v = v != pe_rel(e);
        else break;
    }
    return v;
}

static int64_t pe_bitand(PPExpr *e) {
    int64_t v = pe_eq(e);
    for (;;) {
        pe_skip(e);
        if (e->pos + 1 < e->len && e->s[e->pos] == '&' &&
            e->s[e->pos + 1] == '&')
            break;
        if (e->pos < e->len && e->s[e->pos] == '&') {
            e->pos++;
            v &= pe_eq(e);
        } else {
            break;
        }
    }
    return v;
}

static int64_t pe_bitxor(PPExpr *e) {
    int64_t v = pe_bitand(e);
    while (pe_match(e, "^")) v ^= pe_bitand(e);
    return v;
}

static int64_t pe_bitor(PPExpr *e) {
    int64_t v = pe_bitxor(e);
    for (;;) {
        pe_skip(e);
        if (e->pos + 1 < e->len && e->s[e->pos] == '|' &&
            e->s[e->pos + 1] == '|')
            break;
        if (e->pos < e->len && e->s[e->pos] == '|') {
            e->pos++;
            v |= pe_bitxor(e);
        } else {
            break;
        }
    }
    return v;
}

static int64_t pe_land(PPExpr *e) {
    int64_t v = pe_bitor(e);
    while (pe_match(e, "&&")) {
        int64_t r = pe_bitor(e);
        v = (v != 0 && r != 0);
    }
    return v;
}

static int64_t pe_lor(PPExpr *e) {
    int64_t v = pe_land(e);
    while (pe_match(e, "||")) {
        int64_t r = pe_land(e);
        v = (v != 0 || r != 0);
    }
    return v;
}

static int64_t pe_cond(PPExpr *e) {
    int64_t c = pe_lor(e);
    if (pe_match(e, "?")) {
        int64_t a = pe_cond(e);
        if (!pe_match(e, ":")) {
            etcc_diag_error(e->pp->diag, e->loc, "missing ':' in #if");
            e->failed = true;
        }
        int64_t b = pe_cond(e);
        return c ? a : b;
    }
    return c;
}

static bool pp_eval_if(EtccPP *pp, const char *text, size_t len, EtccLoc loc) {
    PPExpr e = {.pp = pp, .s = text, .len = len, .pos = 0, .loc = loc};
    int64_t v = pe_cond(&e);
    pe_skip(&e);
    if (e.pos < e.len && !e.failed) {
        etcc_diag_error(pp->diag, loc, "extra tokens at end of #if expression");
        e.failed = true;
    }
    return !e.failed && v != 0;
}

/* --------------------------- directive handling -------------------------- */

static const char *skip_ws(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static const char *skip_ident(const char *s, const char *end) {
    while (s < end &&
           (isalnum((unsigned char)*s) || *s == '_'))
        s++;
    return s;
}

static EtccLoc file_loc(const EtccSourceFile *f, uint32_t line) {
    return (EtccLoc){f, line, 1};
}

static bool pp_process_file(EtccPP *pp, EtccSourceFile *file);

static bool pp_handle_include(EtccPP *pp, EtccSourceFile *file,
                              const char *rest, const char *end,
                              EtccLoc loc) {
    rest = skip_ws(rest, end);
    if (rest >= end) {
        etcc_diag_error(pp->diag, loc, "expected file name after #include");
        return false;
    }
    char close;
    bool quoted;
    if (*rest == '"') {
        close = '"';
        quoted = true;
        rest++;
    } else if (*rest == '<') {
        close = '>';
        quoted = false;
        rest++;
    } else {
        etcc_diag_error(pp->diag, loc,
                        "expected \"file\" or <file> after #include");
        return false;
    }
    const char *p = rest;
    while (p < end && *p != close) p++;
    if (p >= end) {
        etcc_diag_error(pp->diag, loc, "unterminated #include file name");
        return false;
    }
    size_t n = (size_t)(p - rest);
    if (n == 0) {
        etcc_diag_error(pp->diag, loc, "empty #include file name");
        return false;
    }
    char *name = etcc_arena_strndup(pp->arena, rest, n);
    char *resolved = pp_resolve_include(pp, file, name, quoted, loc);
    if (!resolved) return false;

    char *canonical = etcc_path_canonical(pp->arena, resolved);
    if (pp_was_once(pp, canonical)) return true;

    if (pp->depth >= ETCC_PP_MAX_DEPTH) {
        etcc_diag_error(pp->diag, loc, "#include nested too deeply");
        return false;
    }

    EtccSourceFile *included = pp_load_file(pp, resolved, loc);
    if (!included) return false;

    pp->depth++;
    bool ok = pp_process_file(pp, included);
    pp->depth--;
    return ok;
}

static bool handle_directive(EtccPP *pp, EtccSourceFile *file,
                             const char *hash, const char *end,
                             uint32_t line) {
    const char *p = skip_ws(hash + 1, end);
    const char *name_start = p;
    while (p < end && isalpha((unsigned char)*p)) p++;
    size_t name_len = (size_t)(p - name_start);
    const char *rest = p;
    EtccLoc loc = file_loc(file, line);

    if (name_len == 0) {
        etcc_diag_error(pp->diag, loc, "empty preprocessor directive");
        return false;
    }
    const char *name = name_start;

    if (str_eq_n(name, name_len, "include")) {
        if (!pp_active(pp)) return true;
        return pp_handle_include(pp, file, rest, end, loc);
    }

    if (str_eq_n(name, name_len, "define")) {
        if (!pp_active(pp)) return true;
        const char *s = skip_ws(rest, end);
        const char *ident = s;
        s = skip_ident(s, end);
        if (s == ident) {
            etcc_diag_error(pp->diag, loc, "expected macro name after #define");
            return false;
        }
        size_t ident_len = (size_t)(s - ident);
        bool function_like = (s < end && *s == '(');
        const char *body = function_like ? s : skip_ws(s, end);
        while (end > body && (end[-1] == ' ' || end[-1] == '\t' ||
                              end[-1] == '\r'))
            end--;
        if (!etcc_macro_define(pp->arena, &pp->macros, ident, ident_len, body,
                               (size_t)(end - body), function_like)) {
            etcc_diag_error_global(pp->diag, "out of memory");
            return false;
        }
        return true;
    }

    if (str_eq_n(name, name_len, "undef")) {
        if (!pp_active(pp)) return true;
        const char *s = skip_ws(rest, end);
        const char *ident = s;
        s = skip_ident(s, end);
        if (s == ident) {
            etcc_diag_error(pp->diag, loc, "expected macro name after #undef");
            return false;
        }
        etcc_macro_undef(&pp->macros, ident, (size_t)(s - ident));
        return true;
    }

    if (str_eq_n(name, name_len, "ifdef") ||
        str_eq_n(name, name_len, "ifndef")) {
        bool negate = str_eq_n(name, name_len, "ifndef");
        bool value = false;
        if (pp_active(pp)) {
            const char *s = skip_ws(rest, end);
            const char *ident = s;
            s = skip_ident(s, end);
            if (s == ident) {
                etcc_diag_error(pp->diag, loc, "expected macro name after #%s",
                                negate ? "ifndef" : "ifdef");
            } else {
                value = etcc_macro_find(&pp->macros, ident,
                                        (size_t)(s - ident)) != NULL;
                if (negate) value = !value;
            }
        }
        return pp_push_cond(pp, value, loc);
    }

    if (str_eq_n(name, name_len, "if")) {
        bool value = false;
        if (pp_active(pp)) value = pp_eval_if(pp, rest, (size_t)(end - rest), loc);
        return pp_push_cond(pp, value, loc);
    }

    if (str_eq_n(name, name_len, "elif")) {
        if (pp->nconds == 0) {
            etcc_diag_error(pp->diag, loc, "#elif without matching #if");
            return false;
        }
        EtccPPCond *c = (EtccPPCond *)pp->conds;
        EtccPPCond *top = &c[pp->nconds - 1];
        if (top->seen_else) {
            etcc_diag_error(pp->diag, loc, "#elif after #else");
            return false;
        }
        bool value = false;
        if (top->parent_active && !top->any_taken)
            value = pp_eval_if(pp, rest, (size_t)(end - rest), loc);
        top->active = top->parent_active && !top->any_taken && value;
        top->any_taken = top->any_taken || value;
        return true;
    }

    if (str_eq_n(name, name_len, "else")) {
        if (pp->nconds == 0) {
            etcc_diag_error(pp->diag, loc, "#else without matching #if");
            return false;
        }
        EtccPPCond *c = (EtccPPCond *)pp->conds;
        EtccPPCond *top = &c[pp->nconds - 1];
        if (top->seen_else) {
            etcc_diag_error(pp->diag, loc, "duplicate #else");
            return false;
        }
        top->seen_else = true;
        top->active = top->parent_active && !top->any_taken;
        top->any_taken = true;
        return true;
    }

    if (str_eq_n(name, name_len, "endif")) {
        if (pp->nconds == 0) {
            etcc_diag_error(pp->diag, loc, "#endif without matching #if");
            return false;
        }
        pp_pop_cond(pp);
        return true;
    }

    if (str_eq_n(name, name_len, "pragma")) {
        if (!pp_active(pp)) return true;
        const char *s = skip_ws(rest, end);
        const char *word = s;
        s = skip_ident(s, end);
        if (str_eq_n(word, (size_t)(s - word), "once")) {
            pp_mark_once(pp, file->canonical);
        }
        return true;
    }

    if (str_eq_n(name, name_len, "error")) {
        if (!pp_active(pp)) return true;
        const char *s = skip_ws(rest, end);
        size_t n = (size_t)(end - s);
        etcc_diag_error(pp->diag, loc, "#error %.*s", (int)n, s);
        return false;
    }

    if (str_eq_n(name, name_len, "warning")) {
        if (!pp_active(pp)) return true;
        const char *s = skip_ws(rest, end);
        size_t n = (size_t)(end - s);
        etcc_diag_warning(pp->diag, loc, "#warning %.*s", (int)n, s);
        return true;
    }

    if (str_eq_n(name, name_len, "line")) {
        return true;
    }

    if (pp_active(pp)) {
        etcc_diag_error(pp->diag, loc, "unknown preprocessor directive '#%.*s'",
                        (int)name_len, name);
        return false;
    }
    return true;
}

/* Update block-comment state for a physical line, respecting strings. */
static void scan_comment_state(const char *s, const char *end, bool *in_block) {
    bool in_string = false;
    bool in_char = false;
    while (s < end) {
        if (*in_block) {
            if (s + 1 < end && s[0] == '*' && s[1] == '/') {
                *in_block = false;
                s += 2;
            } else {
                s++;
            }
            continue;
        }
        if (in_string) {
            if (*s == '\\' && s + 1 < end) {
                s += 2;
            } else if (*s == '"') {
                in_string = false;
                s++;
            } else {
                s++;
            }
            continue;
        }
        if (in_char) {
            if (*s == '\\' && s + 1 < end) {
                s += 2;
            } else if (*s == '\'') {
                in_char = false;
                s++;
            } else {
                s++;
            }
            continue;
        }
        if (s + 1 < end && s[0] == '/' && s[1] == '/') {
            return;
        }
        if (s + 1 < end && s[0] == '/' && s[1] == '*') {
            *in_block = true;
            s += 2;
            continue;
        }
        if (*s == '"') {
            in_string = true;
            s++;
            continue;
        }
        if (*s == '\'') {
            in_char = true;
            s++;
            continue;
        }
        s++;
    }
}

static bool pp_process_file(EtccPP *pp, EtccSourceFile *file) {
    size_t start_cond = pp->nconds;
    bool in_block = false;
    const char *data = file->data;
    size_t len = file->len;
    size_t pos = 0;
    uint32_t line = 1;

    while (pos <= len) {
        size_t line_start = pos;
        size_t line_end = pos;
        while (line_end < len && data[line_end] != '\n') line_end++;
        size_t content_end = line_end;
        if (content_end > line_start && data[content_end - 1] == '\r')
            content_end--;

        const char *first = skip_ws(data + line_start, data + content_end);
        bool is_directive =
            !in_block && first < data + content_end && *first == '#';

        if (is_directive) {
            if (!handle_directive(pp, file, first, data + content_end, line))
                pp->failed = true;
            scan_comment_state(data + line_start, data + content_end, &in_block);
        } else if (pp_active(pp)) {
            if (!etcc_source_map_add(&pp->map, pp->out.len, file, line)) {
                etcc_diag_error_global(pp->diag, "out of memory");
                return false;
            }
            if (!etcc_sb_append(&pp->out, data + line_start,
                                content_end - line_start) ||
                !etcc_sb_append_char(&pp->out, '\n')) {
                etcc_diag_error_global(pp->diag, "out of memory");
                return false;
            }
            scan_comment_state(data + line_start, data + content_end, &in_block);
        } else {
            scan_comment_state(data + line_start, data + content_end, &in_block);
        }

        if (line_end >= len) break;
        pos = line_end + 1;
        line++;
    }

    if (pp->nconds != start_cond) {
        EtccPPCond *c = (EtccPPCond *)pp->conds;
        EtccLoc loc = c ? c[pp->nconds - 1].loc : file_loc(file, line);
        etcc_diag_error(pp->diag, loc, "unterminated conditional directive");
        pp->nconds = start_cond;
        pp->failed = true;
    }
    return !pp->failed;
}

bool etcc_pp_run_file(EtccPP *pp, const char *path) {
    char *canonical = etcc_path_canonical(pp->arena, path);
    if (pp_was_once(pp, canonical)) return true;
    EtccSourceFile *f = pp_load_file(pp, path, ETCC_LOC_NONE);
    if (!f) return false;
    return pp_process_file(pp, f);
}

bool etcc_pp_run_buffer(EtccPP *pp, const char *name, const char *data,
                        size_t len) {
    char *copy = etcc_xmalloc(len + 1);
    memcpy(copy, data, len);
    copy[len] = '\0';
    EtccSourceFile *f = pp_register_file(pp, name, "<buffer>", copy, len);
    if (!f) return false;
    return pp_process_file(pp, f);
}

const char *etcc_pp_text(const EtccPP *pp, size_t *len) {
    if (len) *len = pp->out.len;
    return pp->out.data ? pp->out.data : "";
}

void etcc_pp_destroy(EtccPP *pp) {
    for (size_t i = 0; i < pp->nfiles; i++) {
        free(pp->files[i]->data);
        free(pp->files[i]);
    }
    free(pp->files);
    for (size_t i = 0; i < pp->include_dirs.len; i++)
        free(pp->include_dirs.items[i]);
    free(pp->include_dirs.items);
    for (size_t i = 0; i < pp->once_files.len; i++)
        free(pp->once_files.items[i]);
    free(pp->once_files.items);
    free(pp->conds);
    free(pp->map.segs);
    free(pp->macros.items);
    etcc_sb_free(&pp->out);
    memset(pp, 0, sizeof(*pp));
}
