#include "etcc/lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct EtccMacroFrame {
    const char *data;
    size_t len;
    size_t pos;
    EtccLoc loc;
    const char *name;
    EtccMacroFrame *prev;
};

static const char *L_DATA(const EtccLexer *lx) {
    return lx->frames ? lx->frames->data : lx->data;
}

static size_t *L_POS(EtccLexer *lx) {
    return lx->frames ? &lx->frames->pos : &lx->pos;
}

static size_t L_LEN(const EtccLexer *lx) {
    return lx->frames ? lx->frames->len : lx->len;
}

static EtccLoc lex_loc(const EtccLexer *lx) {
    if (lx->frames) return lx->frames->loc;
    if (lx->map) return etcc_source_map_lookup(lx->map, lx->pos);
    return (EtccLoc){NULL, 1, (uint32_t)(lx->pos + 1)};
}

static EtccToken make_token(EtccTokenKind kind, EtccLoc loc) {
    EtccToken t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.loc = loc;
    return t;
}

static void lex_error(EtccLexer *lx, EtccLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    etcc_diag_error(lx->diag, loc, "%s", buf);
}

static bool macro_active(const EtccLexer *lx, const char *name) {
    for (size_t i = 0; i < lx->nactive; i++) {
        if (lx->active[i] == name) return true;
    }
    return false;
}

static void push_macro_frame(EtccLexer *lx, const EtccMacro *m, EtccLoc loc) {
    EtccMacroFrame *f = etcc_xmalloc(sizeof(*f));
    f->data = m->body;
    f->len = m->body_len;
    f->pos = 0;
    f->loc = loc;
    f->name = m->name;
    f->prev = lx->frames;
    lx->frames = f;
    if (lx->nactive < sizeof(lx->active) / sizeof(lx->active[0]))
        lx->active[lx->nactive++] = m->name;
}

static void pop_macro_frame(EtccLexer *lx) {
    EtccMacroFrame *f = lx->frames;
    if (!f) return;
    lx->frames = f->prev;
    if (lx->nactive > 0 && lx->active[lx->nactive - 1] == f->name)
        lx->nactive--;
    free(f);
}

static bool is_ident_start(int c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_cont(int c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns decoded byte value, or -1 for an invalid/unsupported escape. */
static int decode_escape(EtccLexer *lx, EtccLoc loc) {
    const char *data = L_DATA(lx);
    size_t *pos = L_POS(lx);
    size_t len = L_LEN(lx);

    if (*pos >= len) {
        lex_error(lx, loc, "unterminated escape sequence");
        return -1;
    }
    int c = (unsigned char)data[*pos];
    (*pos)++;
    switch (c) {
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case 'x': {
        int v = 0;
        int digits = 0;
        while (*pos < len) {
            int h = hex_value((unsigned char)data[*pos]);
            if (h < 0) break;
            v = (v << 4) | h;
            (*pos)++;
            digits++;
        }
        if (digits == 0) {
            lex_error(lx, loc, "\\x escape requires at least one hex digit");
            return -1;
        }
        return v & 0xFF;
    }
    default:
        if (c >= '0' && c <= '7') {
            int v = c - '0';
            int digits = 1;
            while (digits < 3 && *pos < len) {
                int d = (unsigned char)data[*pos];
                if (d < '0' || d > '7') break;
                v = (v << 3) | (d - '0');
                (*pos)++;
                digits++;
            }
            return v & 0xFF;
        }
        if (c == 'u' || c == 'U') {
            lex_error(lx, loc, "universal character names are not supported");
            return -1;
        }
        return c;
    }
}

static EtccToken lex_string(EtccLexer *lx, EtccLoc loc) {
    const char *data = L_DATA(lx);
    size_t *pos = L_POS(lx);
    size_t len = L_LEN(lx);

    (*pos)++; /* opening quote */
    size_t max = len - *pos + 1;
    unsigned char *buf = etcc_arena_alloc(lx->arena, max, 1);
    size_t out = 0;
    bool ok = true;

    while (*pos < len) {
        char c = data[*pos];
        if (c == '"') {
            (*pos)++;
            EtccToken t = make_token(TK_STRING_LIT, loc);
            t.v.string_lit.data = (const char *)buf;
            t.v.string_lit.len = out;
            return t;
        }
        if (c == '\n' || c == '\r') {
            lex_error(lx, loc, "unterminated string literal");
            ok = false;
            break;
        }
        if (c == '\\') {
            (*pos)++;
            int v = decode_escape(lx, loc);
            if (v < 0) {
                ok = false;
                break;
            }
            buf[out++] = (unsigned char)v;
        } else {
            buf[out++] = (unsigned char)c;
            (*pos)++;
        }
    }
    if (ok) lex_error(lx, loc, "unterminated string literal");
    EtccToken t = make_token(TK_INVALID, loc);
    t.v.string_lit.data = (const char *)buf;
    t.v.string_lit.len = out;
    return t;
}

static EtccToken lex_char(EtccLexer *lx, EtccLoc loc) {
    const char *data = L_DATA(lx);
    size_t *pos = L_POS(lx);
    size_t len = L_LEN(lx);

    (*pos)++; /* opening quote */
    if (*pos >= len) {
        lex_error(lx, loc, "unterminated character literal");
        return make_token(TK_INVALID, loc);
    }

    int value = 0;
    if (data[*pos] == '\\') {
        (*pos)++;
        value = decode_escape(lx, loc);
        if (value < 0) return make_token(TK_INVALID, loc);
    } else if (data[*pos] == '\'') {
        lex_error(lx, loc, "empty character literal");
        (*pos)++;
        return make_token(TK_INVALID, loc);
    } else if (data[*pos] == '\n' || data[*pos] == '\r') {
        lex_error(lx, loc, "unterminated character literal");
        return make_token(TK_INVALID, loc);
    } else {
        value = (unsigned char)data[*pos];
        (*pos)++;
    }

    if (*pos >= len || data[*pos] != '\'') {
        lex_error(lx, loc, "character literal must contain exactly one character");
        while (*pos < len && data[*pos] != '\'' && data[*pos] != '\n')
            (*pos)++;
        if (*pos < len && data[*pos] == '\'') (*pos)++;
        return make_token(TK_INVALID, loc);
    }
    (*pos)++;

    EtccToken t = make_token(TK_CHAR_LIT, loc);
    t.v.char_lit.value = value;
    return t;
}

static EtccToken lex_number(EtccLexer *lx, EtccLoc loc) {
    const char *data = L_DATA(lx);
    size_t *pos = L_POS(lx);
    size_t len = L_LEN(lx);

    int base = 10;
    bool any_digit = false;

    if (data[*pos] == '0') {
        if (*pos + 1 < len && (data[*pos + 1] == 'x' || data[*pos + 1] == 'X')) {
            base = 16;
            *pos += 2;
        } else if (*pos + 1 < len && data[*pos + 1] == 'b') {
            base = 2;
            *pos += 2;
        } else {
            base = 8;
        }
    }

    uint64_t value = 0;
    bool overflow = false;
    while (*pos < len) {
        int d;
        if (base == 16) d = hex_value((unsigned char)data[*pos]);
        else if (data[*pos] >= '0' && data[*pos] <= '9')
            d = data[*pos] - '0';
        else
            d = -1;
        if (d < 0 || d >= base) break;
        any_digit = true;
        if (value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
            overflow = true;
        else
            value = value * (uint64_t)base + (uint64_t)d;
        (*pos)++;
    }

    if (!any_digit) {
        lex_error(lx, loc, "integer literal has no digits");
        return make_token(TK_INVALID, loc);
    }
    if (overflow) {
        lex_error(lx, loc, "integer literal is too large");
        return make_token(TK_INVALID, loc);
    }

    /* Floating literals are recognized only to give a useful diagnostic. */
    if (*pos < len &&
        (data[*pos] == '.' || data[*pos] == 'e' || data[*pos] == 'E' ||
         data[*pos] == 'p' || data[*pos] == 'P')) {
        lex_error(lx, loc, "floating-point literals are not supported yet");
        while (*pos < len &&
               (isalnum((unsigned char)data[*pos]) || data[*pos] == '.' ||
                data[*pos] == '+' || data[*pos] == '-'))
            (*pos)++;
        return make_token(TK_INVALID, loc);
    }

    bool is_unsigned = false;
    int long_count = 0;
    while (*pos < len) {
        char c = data[*pos];
        if (c == 'u' || c == 'U') {
            if (is_unsigned) break;
            is_unsigned = true;
            (*pos)++;
        } else if (c == 'l' || c == 'L') {
            if (long_count >= 2) break;
            if (*pos + 1 < len &&
                (data[*pos + 1] == 'l' || data[*pos + 1] == 'L')) {
                long_count = 2;
                *pos += 2;
            } else {
                long_count = 1;
                (*pos)++;
            }
        } else {
            break;
        }
    }

    EtccToken t = make_token(TK_INT_LIT, loc);
    t.v.int_lit.value = value;
    t.v.int_lit.is_unsigned = is_unsigned;
    t.v.int_lit.is_long = long_count > 0;
    return t;
}

static EtccToken lex_ident(EtccLexer *lx, EtccLoc loc) {
    const char *data = L_DATA(lx);
    size_t *pos = L_POS(lx);
    size_t len = L_LEN(lx);

    size_t start = *pos;
    while (*pos < len && is_ident_cont((unsigned char)data[*pos])) (*pos)++;
    size_t n = *pos - start;
    const char *s = data + start;

    EtccTokenKind kind = etcc_keyword_kind(s, n);
    if (kind != TK_IDENT) {
        return make_token(kind, loc);
    }

    if (lx->macros) {
        const EtccMacro *m = etcc_macro_find(lx->macros, s, n);
        if (m) {
            if (m->function_like) {
                lex_error(lx, loc,
                          "function-like macro '%.*s' is not supported yet",
                          (int)n, s);
                return make_token(TK_INVALID, loc);
            }
            if (macro_active(lx, m->name)) {
                EtccToken t = make_token(TK_IDENT, loc);
                t.v.ident.data = s;
                t.v.ident.len = n;
                return t;
            }
            if (m->body_len == 0) {
                return etcc_lexer_next(lx);
            }
            push_macro_frame(lx, m, loc);
            return etcc_lexer_next(lx);
        }
    }

    EtccToken t = make_token(TK_IDENT, loc);
    t.v.ident.data = s;
    t.v.ident.len = n;
    return t;
}

static EtccToken simple(EtccLexer *lx, EtccTokenKind kind, EtccLoc loc) {
    (*L_POS(lx))++;
    return make_token(kind, loc);
}

EtccToken etcc_lexer_next(EtccLexer *lx) {
    for (;;) {
        if (!lx->frames && lx->pos >= lx->len) {
            return make_token(TK_EOF, lex_loc(lx));
        }
        if (lx->frames && lx->frames->pos >= lx->frames->len) {
            pop_macro_frame(lx);
            continue;
        }

        const char *data = L_DATA(lx);
        size_t *posp = L_POS(lx);
        size_t len = L_LEN(lx);
        EtccLoc loc = lex_loc(lx);

        char c = data[*posp];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v') {
            (*posp)++;
            continue;
        }

        if (c == '/' && *posp + 1 < len && data[*posp + 1] == '/') {
            *posp += 2;
            while (*posp < len && data[*posp] != '\n') (*posp)++;
            continue;
        }
        if (c == '/' && *posp + 1 < len && data[*posp + 1] == '*') {
            EtccLoc start = loc;
            *posp += 2;
            while (*posp + 1 < len &&
                   !(data[*posp] == '*' && data[*posp + 1] == '/'))
                (*posp)++;
            if (*posp + 1 >= len) {
                lex_error(lx, start, "unterminated block comment");
                *posp = len;
                return make_token(TK_EOF, start);
            }
            *posp += 2;
            continue;
        }

        if (is_ident_start((unsigned char)c)) return lex_ident(lx, loc);
        if (isdigit((unsigned char)c)) return lex_number(lx, loc);

        if (c == '"') return lex_string(lx, loc);
        if (c == '\'') return lex_char(lx, loc);

        switch (c) {
        case '(': return simple(lx, TK_LPAREN, loc);
        case ')': return simple(lx, TK_RPAREN, loc);
        case '{': return simple(lx, TK_LBRACE, loc);
        case '}': return simple(lx, TK_RBRACE, loc);
        case '[': return simple(lx, TK_LBRACKET, loc);
        case ']': return simple(lx, TK_RBRACKET, loc);
        case ';': return simple(lx, TK_SEMICOLON, loc);
        case ',': return simple(lx, TK_COMMA, loc);
        case '?': return simple(lx, TK_QUESTION, loc);
        case ':': return simple(lx, TK_COLON, loc);
        case '~': return simple(lx, TK_TILDE, loc);
        case '+':
            if (*posp + 1 < len && data[*posp + 1] == '+')
                return (*posp += 2, make_token(TK_PLUS_PLUS, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_PLUS_ASSIGN, loc));
            return simple(lx, TK_PLUS, loc);
        case '-':
            if (*posp + 1 < len && data[*posp + 1] == '-')
                return (*posp += 2, make_token(TK_MINUS_MINUS, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_MINUS_ASSIGN, loc));
            if (*posp + 1 < len && data[*posp + 1] == '>')
                return (*posp += 2, make_token(TK_ARROW, loc));
            return simple(lx, TK_MINUS, loc);
        case '*':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_STAR_ASSIGN, loc));
            return simple(lx, TK_STAR, loc);
        case '/':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_SLASH_ASSIGN, loc));
            return simple(lx, TK_SLASH, loc);
        case '%':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_PERCENT_ASSIGN, loc));
            return simple(lx, TK_PERCENT, loc);
        case '&':
            if (*posp + 1 < len && data[*posp + 1] == '&')
                return (*posp += 2, make_token(TK_AND_AND, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_AMP_ASSIGN, loc));
            return simple(lx, TK_AMP, loc);
        case '|':
            if (*posp + 1 < len && data[*posp + 1] == '|')
                return (*posp += 2, make_token(TK_OR_OR, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_PIPE_ASSIGN, loc));
            return simple(lx, TK_PIPE, loc);
        case '^':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_CARET_ASSIGN, loc));
            return simple(lx, TK_CARET, loc);
        case '!':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_NE, loc));
            return simple(lx, TK_BANG, loc);
        case '=':
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_EQ, loc));
            return simple(lx, TK_ASSIGN, loc);
        case '<':
            if (*posp + 2 < len && data[*posp + 1] == '<' &&
                data[*posp + 2] == '=')
                return (*posp += 3, make_token(TK_SHL_ASSIGN, loc));
            if (*posp + 1 < len && data[*posp + 1] == '<')
                return (*posp += 2, make_token(TK_SHL, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_LE, loc));
            return simple(lx, TK_LT, loc);
        case '>':
            if (*posp + 2 < len && data[*posp + 1] == '>' &&
                data[*posp + 2] == '=')
                return (*posp += 3, make_token(TK_SHR_ASSIGN, loc));
            if (*posp + 1 < len && data[*posp + 1] == '>')
                return (*posp += 2, make_token(TK_SHR, loc));
            if (*posp + 1 < len && data[*posp + 1] == '=')
                return (*posp += 2, make_token(TK_GE, loc));
            return simple(lx, TK_GT, loc);
        case '.':
            if (*posp + 2 < len && data[*posp + 1] == '.' &&
                data[*posp + 2] == '.')
                return (*posp += 3, make_token(TK_ELLIPSIS, loc));
            return simple(lx, TK_DOT, loc);
        default:
            break;
        }

        lex_error(lx, loc, "unexpected character '%c' (0x%02X)", c,
                  (unsigned char)c);
        (*posp)++;
        return make_token(TK_INVALID, loc);
    }
}

void etcc_lexer_init(EtccLexer *lx, EtccArena *arena, EtccDiagnostics *diag,
                     const char *data, size_t len, const EtccSourceMap *map,
                     const EtccMacroTable *macros) {
    memset(lx, 0, sizeof(*lx));
    lx->arena = arena;
    lx->diag = diag;
    lx->data = data;
    lx->len = len;
    lx->map = map;
    lx->macros = macros;
}
