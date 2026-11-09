#include "etcc/parser.h"

#include "etcc/lexer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    EtccCType *type;
} TypedefEntry;

typedef struct {
    EtccCTypeKind kind; /* CT_STRUCT or CT_UNION */
    const char *name;
    EtccCType *type;
    int depth;
} TagEntry;

typedef struct {
    const char *name;
    int64_t value;
    int depth;
} ParserEnumConst;

typedef struct {
    EtccCType *type;
    EtccStorageClass storage;
    bool saw_type;
    bool saw_storage;
    bool saw_const;
    EtccAstNode *enum_consts;
} DeclSpec;

typedef struct {
    char *name;
    EtccCType *type;
    EtccAstNode *params;
    bool is_function;
    EtccLoc loc;
} Declarator;

typedef struct EtccParser {
    EtccArena *arena;
    EtccDiagnostics *diag;
    EtccLexer lx;
    EtccToken cur;
    EtccToken peeked;
    bool has_peeked;
    ETCC_VEC(TypedefEntry) typedefs;
    ETCC_VEC(TagEntry) tags;
    ETCC_VEC(ParserEnumConst) enum_values;
    int tag_depth;
    bool fatal;
} EtccParser;

static EtccAstNode *parse_expression(EtccParser *p);
static EtccAstNode *parse_assignment(EtccParser *p);
static EtccAstNode *parse_initializer(EtccParser *p);
static EtccAstNode *parse_cast(EtccParser *p);
static EtccAstNode *parse_statement(EtccParser *p);
static EtccAstNode *parse_compound(EtccParser *p);
static EtccCType *parse_type_name(EtccParser *p);
static Declarator parse_declarator(EtccParser *p, EtccCType *base,
                                   bool abstract);
static bool parse_decl_specifiers(EtccParser *p, DeclSpec *ds);
static EtccCType *parse_struct_union_specifier(EtccParser *p, bool is_union);
static EtccCType *parse_enum_specifier(EtccParser *p, DeclSpec *ds);
static void append_chain(EtccAstNode **head, EtccAstNode **tail,
                         EtccAstNode *chain);

static void parser_error(EtccParser *p, EtccLoc loc, const char *fmt, ...) {
    if (p->fatal) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    etcc_diag_error(p->diag, loc, "%s", buf);
    if (p->diag->errors >= p->diag->max_errors) p->fatal = true;
}

static void parser_advance(EtccParser *p) {
    if (p->has_peeked) {
        p->cur = p->peeked;
        p->has_peeked = false;
    } else {
        p->cur = etcc_lexer_next(&p->lx);
    }
    while (p->cur.kind == TK_INVALID && p->cur.kind != TK_EOF &&
           !p->fatal) {
        p->cur = etcc_lexer_next(&p->lx);
    }
}

static EtccToken parser_peek(EtccParser *p) {
    if (!p->has_peeked) {
        p->peeked = etcc_lexer_next(&p->lx);
        while (p->peeked.kind == TK_INVALID && !p->fatal)
            p->peeked = etcc_lexer_next(&p->lx);
        p->has_peeked = true;
    }
    return p->peeked;
}

static bool check(EtccParser *p, EtccTokenKind kind) {
    return p->cur.kind == kind;
}

static bool accept(EtccParser *p, EtccTokenKind kind) {
    if (!check(p, kind)) return false;
    parser_advance(p);
    return true;
}

static bool expect(EtccParser *p, EtccTokenKind kind, const char *what) {
    if (check(p, kind)) {
        parser_advance(p);
        return true;
    }
    parser_error(p, p->cur.loc, "expected %s before '%s'", what,
                 etcc_token_kind_name(p->cur.kind));
    return false;
}

static char *ident_text(EtccParser *p, EtccToken t) {
    if (t.kind != TK_IDENT) return NULL;
    return etcc_arena_strndup(p->arena, t.v.ident.data, t.v.ident.len);
}

static const TypedefEntry *typedef_lookup(const EtccParser *p,
                                          const char *name, size_t len) {
    for (size_t i = p->typedefs.len; i > 0; i--) {
        const TypedefEntry *e = &p->typedefs.items[i - 1];
        size_t n = strlen(e->name);
        if (n == len && memcmp(e->name, name, n) == 0) return e;
    }
    return NULL;
}

static void typedef_add(EtccParser *p, const char *name, EtccCType *type) {
    if (typedef_lookup(p, name, strlen(name))) {
        parser_error(p, p->cur.loc, "redefinition of typedef '%s'", name);
        return;
    }
    TypedefEntry e = {etcc_arena_strdup(p->arena, name), type};
    if (!ETCC_VEC_PUSH(p->typedefs, e))
        parser_error(p, p->cur.loc, "out of memory");
}

static bool is_typedef_token(const EtccParser *p, EtccToken t) {
    return t.kind == TK_IDENT &&
           typedef_lookup(p, t.v.ident.data, t.v.ident.len) != NULL;
}

static TagEntry *tag_find_nearest(EtccParser *p, EtccCTypeKind kind,
                                  const char *name, size_t len) {
    for (size_t i = p->tags.len; i > 0; i--) {
        TagEntry *e = &p->tags.items[i - 1];
        if (e->kind != kind) continue;
        size_t n = strlen(e->name);
        if (n == len && memcmp(e->name, name, n) == 0) return e;
    }
    return NULL;
}

static TagEntry *tag_find_current(EtccParser *p, EtccCTypeKind kind,
                                  const char *name, size_t len) {
    for (size_t i = p->tags.len; i > 0; i--) {
        TagEntry *e = &p->tags.items[i - 1];
        if (e->depth != p->tag_depth) continue;
        if (e->kind != kind) continue;
        size_t n = strlen(e->name);
        if (n == len && memcmp(e->name, name, n) == 0) return e;
    }
    return NULL;
}

static TagEntry *tag_add(EtccParser *p, EtccCTypeKind kind, const char *name,
                         size_t len, EtccCType *type) {
    TagEntry e = {
        .kind = kind,
        .name = etcc_arena_strndup(p->arena, name, len),
        .type = type,
        .depth = p->tag_depth,
    };
    if (!ETCC_VEC_PUSH(p->tags, e)) {
        parser_error(p, p->cur.loc, "out of memory");
        return NULL;
    }
    return &p->tags.items[p->tags.len - 1];
}

static bool token_starts_type(const EtccParser *p, EtccToken t) {
    if (etcc_token_is_type_keyword(t.kind)) return true;
    return is_typedef_token(p, t);
}

static bool is_decl_start(const EtccParser *p) {
    if (etcc_token_is_type_keyword(p->cur.kind)) return true;
    if (etcc_token_is_storage_keyword(p->cur.kind)) return true;
    if (p->cur.kind == TK_KW_INLINE) return true;
    return is_typedef_token(p, p->cur);
}

static void skip_balanced(EtccParser *p, EtccTokenKind open,
                          EtccTokenKind close) {
    if (!check(p, open)) return;
    int depth = 0;
    while (!check(p, TK_EOF)) {
        if (check(p, open)) depth++;
        else if (check(p, close)) {
            depth--;
            if (depth == 0) {
                parser_advance(p);
                return;
            }
        }
        parser_advance(p);
    }
}

static void recover_statement(EtccParser *p) {
    while (!check(p, TK_EOF) && !check(p, TK_SEMICOLON) &&
           !check(p, TK_RBRACE))
        parser_advance(p);
    if (check(p, TK_SEMICOLON)) parser_advance(p);
}

static int64_t parser_const_ashr(int64_t v, int64_t count) {
    if (count < 0 || count >= 64) return v < 0 ? -1 : 0;
    if (v >= 0) return v >> count;
    return (int64_t)(~((~((uint64_t)v)) >> (unsigned)count));
}

static bool eval_const_int(EtccParser *p, EtccAstNode *n, int64_t *out) {
    if (!n) return false;
    switch (n->kind) {
    case AST_INT_LIT:
        *out = (int64_t)n->u.int_lit.value;
        return true;
    case AST_CHAR_LIT:
        *out = n->u.char_lit.value;
        return true;
    case AST_IDENT: {
        for (size_t i = p->enum_values.len; i > 0; i--) {
            const ParserEnumConst *e = &p->enum_values.items[i - 1];
            if (e->depth > p->tag_depth) continue;
            size_t elen = strlen(e->name);
            if (elen == n->u.ident.len &&
                memcmp(e->name, n->u.ident.name, elen) == 0) {
                *out = e->value;
                return true;
            }
        }
        return false;
    }
    case AST_UNARY: {
        int64_t v;
        if (!eval_const_int(p, n->u.unary.operand, &v)) return false;
        switch (n->u.unary.op) {
        case TK_PLUS: *out = v; return true;
        case TK_MINUS: *out = (int64_t)(0ull - (uint64_t)v); return true;
        case TK_TILDE: *out = ~v; return true;
        case TK_BANG: *out = !v; return true;
        default: return false;
        }
    }
    case AST_BINARY: {
        int64_t a, b;
        if (!eval_const_int(p, n->u.binary.lhs, &a)) return false;
        if (n->u.binary.op == TK_AND_AND) {
            if (!a) { *out = 0; return true; }
            if (!eval_const_int(p, n->u.binary.rhs, &b)) return false;
            *out = b != 0;
            return true;
        }
        if (n->u.binary.op == TK_OR_OR) {
            if (a) { *out = 1; return true; }
            if (!eval_const_int(p, n->u.binary.rhs, &b)) return false;
            *out = b != 0;
            return true;
        }
        if (!eval_const_int(p, n->u.binary.rhs, &b)) return false;
        switch (n->u.binary.op) {
        case TK_PLUS:
            *out = (int64_t)((uint64_t)a + (uint64_t)b);
            return true;
        case TK_MINUS:
            *out = (int64_t)((uint64_t)a - (uint64_t)b);
            return true;
        case TK_STAR:
            *out = (int64_t)((uint64_t)a * (uint64_t)b);
            return true;
        case TK_SLASH:
        case TK_PERCENT:
            if (b == 0) {
                parser_error(p, n->loc, "division by zero in constant expression");
                return false;
            }
            if (a == INT64_MIN && b == -1) {
                parser_error(p, n->loc,
                             "signed integer overflow in constant expression");
                return false;
            }
            *out = n->u.binary.op == TK_SLASH ? a / b : a % b;
            return true;
        case TK_SHL:
            *out = (b < 0 || b >= 64)
                       ? 0
                       : (int64_t)((uint64_t)a << (unsigned)b);
            return true;
        case TK_SHR:
            *out = parser_const_ashr(a, b);
            return true;
        case TK_AMP: *out = a & b; return true;
        case TK_PIPE: *out = a | b; return true;
        case TK_CARET: *out = a ^ b; return true;
        case TK_LT: *out = a < b; return true;
        case TK_LE: *out = a <= b; return true;
        case TK_GT: *out = a > b; return true;
        case TK_GE: *out = a >= b; return true;
        case TK_EQ: *out = a == b; return true;
        case TK_NE: *out = a != b; return true;
        default: return false;
        }
    }
    case AST_COND: {
        int64_t c;
        if (!eval_const_int(p, n->u.cond.cond, &c)) return false;
        return eval_const_int(p, c ? n->u.cond.then_expr : n->u.cond.else_expr,
                              out);
    }
    case AST_COMMA: {
        int64_t v;
        if (!eval_const_int(p, n->u.comma.rhs, &v)) return false;
        *out = v;
        return true;
    }
    case AST_SIZEOF:
        if (n->u.sizeof_.type) {
            *out = (int64_t)etcc_type_sizeof(n->u.sizeof_.type);
            return true;
        }
        return false;
    case AST_CAST: {
        int64_t v;
        if (!eval_const_int(p, n->u.cast.operand, &v)) return false;
        EtccCType *t = n->u.cast.target;
        if (t && t->kind == CT_BOOL) {
            *out = v != 0;
            return true;
        }
        if (t && etcc_type_is_integer(t) && t->size < 8) {
            uint64_t mask = (1ull << (t->size * 8)) - 1;
            uint64_t u = (uint64_t)v & mask;
            if (!t->is_unsigned && t->size && (u & (1ull << (t->size * 8 - 1))))
                u |= ~mask;
            *out = (int64_t)u;
        } else {
            *out = v;
        }
        return true;
    }
    default:
        return false;
    }
}

static bool parse_const_int(EtccParser *p, int64_t *out) {
    EtccAstNode *e = parse_assignment(p);
    if (!e) return false;
    if (!eval_const_int(p, e, out)) {
        parser_error(p, e->loc, "expected an integer constant expression");
        return false;
    }
    return true;
}

static bool field_name_exists(const EtccField *fields, const char *name,
                              size_t len) {
    for (const EtccField *f = fields; f; f = f->next) {
        size_t n = strlen(f->name);
        if (n == len && memcmp(f->name, name, n) == 0) return true;
    }
    return false;
}

static bool field_type_incomplete(const EtccCType *t) {
    if (!t) return true;
    if (t->kind == CT_VOID || t->kind == CT_FUNC) return true;
    if ((t->kind == CT_STRUCT || t->kind == CT_UNION) && !t->is_complete)
        return true;
    if (t->kind == CT_ARRAY)
        return t->array_len == 0 || field_type_incomplete(t->base);
    return false;
}

static EtccCType *parse_struct_union_specifier(EtccParser *p, bool is_union) {
    EtccCTypeKind kind = is_union ? CT_UNION : CT_STRUCT;
    EtccLoc loc = p->cur.loc;
    parser_advance(p); /* struct / union */

    char *tag = NULL;
    size_t tag_len = 0;
    if (check(p, TK_IDENT)) {
        tag = ident_text(p, p->cur);
        tag_len = p->cur.v.ident.len;
        parser_advance(p);
    }

    if (check(p, TK_LBRACE)) {
        parser_advance(p);
        EtccCType *type = NULL;

        if (tag) {
            TagEntry *existing = tag_find_current(p, kind, tag, tag_len);
            if (existing) {
                if (existing->type->is_complete) {
                    parser_error(p, loc, "redefinition of %s '%s'",
                                 is_union ? "union" : "struct", tag);
                    type = etcc_type_struct(p->arena, kind, tag);
                } else {
                    type = existing->type;
                }
            } else {
                type = etcc_type_struct(p->arena, kind, tag);
                tag_add(p, kind, tag, tag_len, type);
            }
        } else {
            type = etcc_type_struct(p->arena, kind, NULL);
        }

        EtccField *head = NULL, *tail = NULL;
        size_t field_count = 0;

        while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
            DeclSpec fds;
            if (!parse_decl_specifiers(p, &fds)) {
                parser_error(p, p->cur.loc, "expected a struct/union field declaration");
                recover_statement(p);
                continue;
            }
            if (fds.storage != STORAGE_NONE) {
                parser_error(p, p->cur.loc,
                             "storage class is not allowed on a struct/union field");
            }
            if (check(p, TK_SEMICOLON)) {
                parser_error(p, p->cur.loc, "field declaration has no name");
                parser_advance(p);
                continue;
            }

            for (;;) {
                Declarator d = parse_declarator(p, fds.type, false);
                if (!d.name) {
                    parser_error(p, d.loc, "struct/union field has no name");
                } else if (field_type_incomplete(d.type)) {
                    parser_error(p, d.loc,
                                 "field '%s' has incomplete or invalid type",
                                 d.name);
                } else if (field_name_exists(head, d.name, strlen(d.name))) {
                    parser_error(p, d.loc, "duplicate field '%s'", d.name);
                } else {
                    EtccField *f = etcc_field_new(p->arena, d.name, d.type);
                    if (tail) tail->next = f;
                    else head = f;
                    tail = f;
                    field_count++;
                }

                if (check(p, TK_COLON)) {
                    parser_error(p, p->cur.loc,
                                 "bit-fields are not supported yet");
                    parser_advance(p);
                    parse_assignment(p);
                }
                if (check(p, TK_ASSIGN)) {
                    parser_error(p, p->cur.loc,
                                 "struct/union fields cannot have initializers");
                    parser_advance(p);
                    parse_initializer(p);
                }
                if (!accept(p, TK_COMMA)) break;
            }
            if (!expect(p, TK_SEMICOLON, "';'")) recover_statement(p);
        }
        expect(p, TK_RBRACE, "'}'");
        if (field_count == 0)
            parser_error(p, loc, "empty %s is not supported",
                         is_union ? "union" : "struct");
        if (!etcc_type_define(p->arena, type, head))
            parser_error(p, loc, "cannot lay out %s",
                         is_union ? "union" : "struct");
        return type;
    }

    if (!tag) {
        parser_error(p, loc, "expected a struct/union tag or a definition");
        return etcc_type_builtin(CT_INT);
    }

    TagEntry *e = tag_find_nearest(p, kind, tag, tag_len);
    if (e) return e->type;

    EtccCType *type = etcc_type_struct(p->arena, kind, tag);
    tag_add(p, kind, tag, tag_len, type);
    return type;
}

static void enum_const_append(DeclSpec *ds, EtccAstNode *node) {
    if (!ds->enum_consts) {
        ds->enum_consts = node;
        return;
    }
    EtccAstNode *tail = ds->enum_consts;
    while (tail->next) tail = tail->next;
    tail->next = node;
}

static EtccCType *parse_enum_specifier(EtccParser *p, DeclSpec *ds) {
    EtccLoc loc = p->cur.loc;
    parser_advance(p); /* enum */

    if (check(p, TK_IDENT)) parser_advance(p); /* tag; enum types are int */

    if (!check(p, TK_LBRACE)) {
        /* `enum Tag` is represented as int; the tag itself is not used for
         * layout or code generation in this milestone. */
        return etcc_type_builtin(CT_INT);
    }

    parser_advance(p);
    int64_t next_value = 0;
    size_t count = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        if (!check(p, TK_IDENT)) {
            parser_error(p, p->cur.loc, "expected an enumerator name");
            break;
        }
        EtccToken name_tok = p->cur;
        char *name = ident_text(p, name_tok);
        parser_advance(p);

        int64_t value = next_value;
        if (accept(p, TK_ASSIGN)) {
            if (!parse_const_int(p, &value)) {
                value = 0;
                recover_statement(p);
            }
        }

        EtccAstNode *node = etcc_ast_new(p->arena, AST_ENUM_CONST, name_tok.loc);
        node->u.enum_const.name = name;
        node->u.enum_const.len = name_tok.v.ident.len;
        node->u.enum_const.value = value;
        enum_const_append(ds, node);

        ParserEnumConst ec = {name, value, p->tag_depth};
        if (!ETCC_VEC_PUSH(p->enum_values, ec))
            parser_error(p, name_tok.loc, "out of memory");

        next_value = value + 1;
        count++;

        if (accept(p, TK_COMMA)) {
            if (check(p, TK_RBRACE)) break;
            continue;
        }
        break;
    }
    expect(p, TK_RBRACE, "'}'");
    if (count == 0) parser_error(p, loc, "empty enum is not supported");
    return etcc_type_builtin(CT_INT);
}

static bool parse_decl_specifiers(EtccParser *p, DeclSpec *ds) {
    memset(ds, 0, sizeof(*ds));
    bool saw_signed = false, saw_unsigned = false;
    bool saw_char = false, saw_short = false, saw_int = false;
    bool saw_void = false, saw_bool = false;
    bool saw_float = false, saw_double = false;
    int long_count = 0;
    EtccCType *explicit_type = NULL;
    bool any = false;

    for (;;) {
        EtccTokenKind k = p->cur.kind;
        if (etcc_token_is_storage_keyword(k)) {
            if (ds->saw_storage)
                parser_error(p, p->cur.loc, "multiple storage classes");
            ds->saw_storage = true;
            switch (k) {
            case TK_KW_STATIC: ds->storage = STORAGE_STATIC; break;
            case TK_KW_EXTERN: ds->storage = STORAGE_EXTERN; break;
            case TK_KW_TYPEDEF: ds->storage = STORAGE_TYPEDEF; break;
            case TK_KW_AUTO: ds->storage = STORAGE_AUTO; break;
            case TK_KW_REGISTER: ds->storage = STORAGE_REGISTER; break;
            default: break;
            }
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_INLINE) {
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_CONST) {
            ds->saw_const = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_VOLATILE || k == TK_KW_RESTRICT) {
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_SIGNED) {
            saw_signed = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_UNSIGNED) {
            saw_unsigned = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_CHAR) {
            saw_char = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_SHORT) {
            saw_short = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_INT) {
            saw_int = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_LONG) {
            long_count++;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_VOID) {
            saw_void = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_BOOL) {
            saw_bool = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_FLOAT) {
            saw_float = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_DOUBLE) {
            saw_double = true;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        if (k == TK_KW_STRUCT || k == TK_KW_UNION) {
            if (explicit_type || saw_signed || saw_unsigned || saw_char ||
                saw_short || saw_int || long_count || saw_void || saw_bool ||
                saw_float || saw_double) {
                parser_error(p, p->cur.loc,
                             "cannot combine a struct/union with other type specifiers");
            }
            explicit_type = parse_struct_union_specifier(p, k == TK_KW_UNION);
            ds->saw_type = true;
            any = true;
            continue;
        }
        if (k == TK_KW_ENUM) {
            if (explicit_type || saw_signed || saw_unsigned || saw_char ||
                saw_short || saw_int || long_count || saw_void ||
                saw_bool || saw_float || saw_double) {
                parser_error(p, p->cur.loc,
                             "cannot combine an enum with other type specifiers");
            }
            explicit_type = parse_enum_specifier(p, ds);
            ds->saw_type = true;
            any = true;
            continue;
        }
        if (is_typedef_token(p, p->cur)) {
            if (explicit_type || saw_signed || saw_unsigned || saw_char ||
                saw_short || saw_int || long_count || saw_void || saw_bool ||
                saw_float || saw_double) {
                parser_error(p, p->cur.loc,
                             "cannot combine a typedef name with other type specifiers");
            }
            explicit_type = typedef_lookup(p, p->cur.v.ident.data,
                                           p->cur.v.ident.len)->type;
            ds->saw_type = true;
            any = true;
            parser_advance(p);
            continue;
        }
        break;
    }

    if (!any) return false;

    EtccCType *type = explicit_type;
    if (!type) {
        if (saw_signed && saw_unsigned)
            parser_error(p, p->cur.loc,
                         "both 'signed' and 'unsigned' in declaration");
        if (saw_void) {
            if (saw_char || saw_short || saw_int || long_count || saw_float ||
                saw_double || saw_bool)
                parser_error(p, p->cur.loc, "invalid combination with 'void'");
            type = etcc_type_builtin(CT_VOID);
        } else if (saw_float) {
            if (saw_double || saw_char || saw_short || saw_int || long_count)
                parser_error(p, p->cur.loc, "invalid floating-point type");
            type = etcc_type_builtin(CT_FLOAT);
        } else if (saw_double) {
            if (long_count)
                parser_error(p, p->cur.loc, "'long double' is not supported");
            type = etcc_type_builtin(CT_DOUBLE);
        } else if (saw_char) {
            type = etcc_type_builtin(CT_CHAR);
        } else if (saw_short) {
            type = etcc_type_builtin(CT_SHORT);
        } else if (saw_bool) {
            type = etcc_type_builtin(CT_BOOL);
        } else if (long_count >= 2) {
            type = etcc_type_builtin(CT_LLONG);
        } else if (long_count == 1) {
            type = etcc_type_builtin(CT_LONG);
        } else {
            type = etcc_type_builtin(CT_INT);
        }

        if ((saw_signed || saw_unsigned) && type &&
            (type->kind == CT_CHAR || type->kind == CT_SHORT ||
             type->kind == CT_INT || type->kind == CT_LONG ||
             type->kind == CT_LLONG)) {
            if (type->is_unsigned != saw_unsigned) {
                type = etcc_type_copy(p->arena, type);
                type->is_unsigned = saw_unsigned;
            }
        } else if (saw_signed || saw_unsigned) {
            if (saw_float || saw_double || saw_void)
                parser_error(p, p->cur.loc,
                             "signed/unsigned cannot be applied to this type");
        }
    }

    if (ds->saw_const && type) {
        type = etcc_type_copy(p->arena, type);
        type->is_const = true;
    }
    ds->type = type;
    return true;
}

static bool parse_parameter_list(EtccParser *p, EtccAstNode **params_out,
                                 EtccCType ***types_out, size_t *nparams_out,
                                 bool *variadic_out) {
    *params_out = NULL;
    *types_out = NULL;
    *nparams_out = 0;
    *variadic_out = false;

    if (!expect(p, TK_LPAREN, "'('")) return false;

    if (check(p, TK_RPAREN)) {
        parser_advance(p);
        return true;
    }
    if (check(p, TK_KW_VOID) && parser_peek(p).kind == TK_RPAREN) {
        parser_advance(p);
        parser_advance(p);
        return true;
    }

    ETCC_VEC(EtccCType *) types = {0};
    EtccAstNode *head = NULL, *tail = NULL;

    for (;;) {
        if (check(p, TK_ELLIPSIS)) {
            parser_advance(p);
            *variadic_out = true;
            break;
        }

        DeclSpec ds;
        if (!parse_decl_specifiers(p, &ds)) {
            parser_error(p, p->cur.loc, "expected parameter declaration");
            break;
        }
        Declarator d = parse_declarator(p, ds.type, true);
        EtccCType *pt = d.type;
        if (pt && pt->kind == CT_ARRAY) {
            pt = etcc_type_ptr(p->arena, pt->base);
        } else if (pt && pt->kind == CT_FUNC) {
            pt = etcc_type_ptr(p->arena, pt);
        }
        if (pt && pt->kind == CT_VOID) {
            parser_error(p, d.loc, "parameter has incomplete type 'void'");
        }

        EtccAstNode *param = etcc_ast_new(p->arena, AST_PARAM, d.loc);
        param->u.param.name = d.name;
        param->u.param.type = pt;
        if (tail) tail->next = param;
        else head = param;
        tail = param;

        if (!ETCC_VEC_PUSH(types, pt)) {
            parser_error(p, d.loc, "out of memory");
            break;
        }

        if (accept(p, TK_COMMA)) {
            if (check(p, TK_RPAREN)) {
                parser_error(p, p->cur.loc, "trailing comma in parameter list");
                break;
            }
            continue;
        }
        break;
    }

    expect(p, TK_RPAREN, "')'");

    EtccCType **arr = NULL;
    if (types.len) {
        arr = etcc_arena_alloc(p->arena, types.len * sizeof(*arr),
                               _Alignof(EtccCType *));
        memcpy(arr, types.items, types.len * sizeof(*arr));
    }
    free(types.items);
    *params_out = head;
    *types_out = arr;
    *nparams_out = types.len;
    return true;
}

static Declarator parse_declarator(EtccParser *p, EtccCType *base,
                                   bool abstract) {
    Declarator d;
    memset(&d, 0, sizeof(d));
    d.type = base;
    d.loc = p->cur.loc;

    while (check(p, TK_STAR)) {
        parser_advance(p);
        bool is_const = false;
        while (check(p, TK_KW_CONST) || check(p, TK_KW_VOLATILE) ||
               check(p, TK_KW_RESTRICT)) {
            if (check(p, TK_KW_CONST)) is_const = true;
            parser_advance(p);
        }
        EtccCType *ptr = etcc_type_ptr(p->arena, d.type);
        if (is_const) {
            ptr = etcc_type_copy(p->arena, ptr);
            ptr->is_const = true;
        }
        d.type = ptr;
    }

    if (check(p, TK_LPAREN)) {
        parser_error(p, p->cur.loc,
                     "parenthesized declarators (function pointers) are not supported yet");
        skip_balanced(p, TK_LPAREN, TK_RPAREN);
    } else if (check(p, TK_IDENT)) {
        d.name = ident_text(p, p->cur);
        d.loc = p->cur.loc;
        parser_advance(p);
    } else if (!abstract) {
        parser_error(p, p->cur.loc, "expected a declarator name");
    }

    /* Collect array dimensions; apply them right-to-left. */
    ETCC_VEC(uint64_t) dims = {0};
    while (check(p, TK_LBRACKET)) {
        parser_advance(p);
        int64_t len = 0;
        if (check(p, TK_RBRACKET)) {
            len = 0;
        } else {
            if (!parse_const_int(p, &len)) {
                len = 0;
                recover_statement(p);
            }
            if (len < 0) {
                parser_error(p, p->cur.loc, "array size must not be negative");
                len = 0;
            }
        }
        if (!expect(p, TK_RBRACKET, "']'")) break;
        uint64_t u = (uint64_t)len;
        if (!ETCC_VEC_PUSH(dims, u)) {
            parser_error(p, d.loc, "out of memory");
            break;
        }
    }

    if (check(p, TK_LPAREN)) {
        EtccAstNode *params = NULL;
        EtccCType **ptypes = NULL;
        size_t nparams = 0;
        bool variadic = false;
        parse_parameter_list(p, &params, &ptypes, &nparams, &variadic);
        EtccCType *ft = etcc_type_func(p->arena, d.type, ptypes, nparams,
                                       variadic);
        d.type = ft;
        d.params = params;
        d.is_function = true;
        if (dims.len) {
            parser_error(p, d.loc,
                         "array of functions is not a valid type");
        }
    } else {
        for (size_t i = dims.len; i > 0; i--)
            d.type = etcc_type_array(p->arena, d.type, dims.items[i - 1]);
    }
    free(dims.items);
    return d;
}

static EtccCType *parse_type_name(EtccParser *p) {
    DeclSpec ds;
    if (!parse_decl_specifiers(p, &ds)) {
        parser_error(p, p->cur.loc, "expected a type name");
        return etcc_type_builtin(CT_INT);
    }
    Declarator d = parse_declarator(p, ds.type, true);
    return d.type;
}

static EtccAstNode *make_binary(EtccParser *p, EtccTokenKind op,
                                EtccAstNode *lhs, EtccAstNode *rhs,
                                EtccLoc loc) {
    if (!lhs || !rhs) return NULL;
    EtccAstNode *n = etcc_ast_new(p->arena, AST_BINARY, loc);
    n->u.binary.op = op;
    n->u.binary.lhs = lhs;
    n->u.binary.rhs = rhs;
    return n;
}

static EtccAstNode *parse_primary(EtccParser *p) {
    EtccToken t = p->cur;

    if (t.kind == TK_INT_LIT) {
        EtccAstNode *n = etcc_ast_new(p->arena, AST_INT_LIT, t.loc);
        n->u.int_lit.value = t.v.int_lit.value;
        n->u.int_lit.is_unsigned = t.v.int_lit.is_unsigned;
        n->u.int_lit.long_count = t.v.int_lit.long_count;
        n->u.int_lit.is_decimal = t.v.int_lit.is_decimal;
        parser_advance(p);
        return n;
    }
    if (t.kind == TK_CHAR_LIT) {
        EtccAstNode *n = etcc_ast_new(p->arena, AST_CHAR_LIT, t.loc);
        n->u.char_lit.value = t.v.char_lit.value;
        parser_advance(p);
        return n;
    }
    if (t.kind == TK_STRING_LIT) {
        EtccStrBuf sb;
        etcc_sb_init(&sb);
        EtccLoc loc = t.loc;
        while (check(p, TK_STRING_LIT)) {
            etcc_sb_append(&sb, p->cur.v.string_lit.data,
                           p->cur.v.string_lit.len);
            parser_advance(p);
        }
        char *data = etcc_arena_alloc(p->arena, sb.len + 1, 1);
        if (sb.len) memcpy(data, sb.data, sb.len);
        data[sb.len] = '\0';
        size_t len = sb.len;
        etcc_sb_free(&sb);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_STRING_LIT, loc);
        n->u.string_lit.data = data;
        n->u.string_lit.len = len;
        return n;
    }
    if (t.kind == TK_IDENT) {
        EtccAstNode *n = etcc_ast_new(p->arena, AST_IDENT, t.loc);
        n->u.ident.name = t.v.ident.data;
        n->u.ident.len = t.v.ident.len;
        parser_advance(p);
        return n;
    }
    if (t.kind == TK_LPAREN) {
        parser_advance(p);
        EtccAstNode *e = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        return e;
    }

    parser_error(p, t.loc, "expected an expression, found '%s'",
                 etcc_token_kind_name(t.kind));
    if (t.kind != TK_EOF) parser_advance(p);
    return NULL;
}

static EtccAstNode *parse_postfix(EtccParser *p) {
    EtccAstNode *e = parse_primary(p);
    if (!e) return NULL;

    for (;;) {
        if (check(p, TK_LPAREN)) {
            EtccLoc loc = p->cur.loc;
            parser_advance(p);
            EtccAstNode *head = NULL, *tail = NULL;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    EtccAstNode *arg = parse_assignment(p);
                    if (!arg) break;
                    if (tail) tail->next = arg;
                    else head = arg;
                    tail = arg;
                    if (accept(p, TK_COMMA)) continue;
                    break;
                }
            }
            expect(p, TK_RPAREN, "')'");
            EtccAstNode *call = etcc_ast_new(p->arena, AST_CALL, loc);
            call->u.call.callee = e;
            call->u.call.args = head;
            e = call;
            continue;
        }
        if (check(p, TK_LBRACKET)) {
            EtccLoc loc = p->cur.loc;
            parser_advance(p);
            EtccAstNode *idx = parse_expression(p);
            expect(p, TK_RBRACKET, "']'");
            EtccAstNode *n = etcc_ast_new(p->arena, AST_INDEX, loc);
            n->u.index.base = e;
            n->u.index.index = idx;
            e = n;
            continue;
        }
        if (check(p, TK_DOT) || check(p, TK_ARROW)) {
            bool arrow = check(p, TK_ARROW);
            EtccLoc loc = p->cur.loc;
            parser_advance(p);
            if (!check(p, TK_IDENT)) {
                parser_error(p, p->cur.loc, "expected a member name");
                return e;
            }
            EtccAstNode *n = etcc_ast_new(p->arena, AST_MEMBER, loc);
            n->u.member.base = e;
            n->u.member.field = p->cur.v.ident.data;
            n->u.member.field_len = p->cur.v.ident.len;
            n->u.member.arrow = arrow;
            parser_advance(p);
            e = n;
            continue;
        }
        if (check(p, TK_PLUS_PLUS) || check(p, TK_MINUS_MINUS)) {
            EtccToken op = p->cur;
            parser_advance(p);
            EtccAstNode *n = etcc_ast_new(p->arena, AST_UNARY, op.loc);
            n->u.unary.op = op.kind;
            n->u.unary.operand = e;
            n->u.unary.postfix = true;
            e = n;
            continue;
        }
        break;
    }
    return e;
}

static EtccAstNode *parse_unary(EtccParser *p) {
    EtccTokenKind k = p->cur.kind;
    if (k == TK_PLUS_PLUS || k == TK_MINUS_MINUS || k == TK_AMP ||
        k == TK_STAR || k == TK_PLUS || k == TK_MINUS || k == TK_TILDE ||
        k == TK_BANG) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *operand = parse_cast(p);
        if (!operand) return NULL;
        EtccAstNode *n = etcc_ast_new(p->arena, AST_UNARY, op.loc);
        n->u.unary.op = op.kind;
        n->u.unary.operand = operand;
        n->u.unary.postfix = false;
        return n;
    }
    if (k == TK_KW_SIZEOF) {
        EtccLoc loc = p->cur.loc;
        parser_advance(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_SIZEOF, loc);
        if (check(p, TK_LPAREN) && token_starts_type(p, parser_peek(p))) {
            parser_advance(p);
            n->u.sizeof_.type = parse_type_name(p);
            expect(p, TK_RPAREN, "')'");
        } else {
            n->u.sizeof_.operand = parse_unary(p);
            if (!n->u.sizeof_.operand) return NULL;
        }
        return n;
    }
    return parse_postfix(p);
}

static EtccAstNode *parse_cast(EtccParser *p) {
    if (check(p, TK_LPAREN) && token_starts_type(p, parser_peek(p))) {
        EtccLoc loc = p->cur.loc;
        parser_advance(p);
        EtccCType *type = parse_type_name(p);
        expect(p, TK_RPAREN, "')'");
        EtccAstNode *operand = parse_cast(p);
        if (!operand) return NULL;
        EtccAstNode *n = etcc_ast_new(p->arena, AST_CAST, loc);
        n->u.cast.target = type;
        n->u.cast.operand = operand;
        return n;
    }
    return parse_unary(p);
}

static EtccAstNode *parse_mul(EtccParser *p) {
    EtccAstNode *e = parse_cast(p);
    while (e && (check(p, TK_STAR) || check(p, TK_SLASH) ||
                 check(p, TK_PERCENT))) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_cast(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_add(EtccParser *p) {
    EtccAstNode *e = parse_mul(p);
    while (e && (check(p, TK_PLUS) || check(p, TK_MINUS))) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_mul(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_shift(EtccParser *p) {
    EtccAstNode *e = parse_add(p);
    while (e && (check(p, TK_SHL) || check(p, TK_SHR))) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_add(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_relational(EtccParser *p) {
    EtccAstNode *e = parse_shift(p);
    while (e && (check(p, TK_LT) || check(p, TK_LE) || check(p, TK_GT) ||
                 check(p, TK_GE))) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_shift(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_equality(EtccParser *p) {
    EtccAstNode *e = parse_relational(p);
    while (e && (check(p, TK_EQ) || check(p, TK_NE))) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_relational(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_bitand(EtccParser *p) {
    EtccAstNode *e = parse_equality(p);
    while (e && check(p, TK_AMP)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_equality(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_bitxor(EtccParser *p) {
    EtccAstNode *e = parse_bitand(p);
    while (e && check(p, TK_CARET)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_bitand(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_bitor(EtccParser *p) {
    EtccAstNode *e = parse_bitxor(p);
    while (e && check(p, TK_PIPE)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_bitxor(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_logical_and(EtccParser *p) {
    EtccAstNode *e = parse_bitor(p);
    while (e && check(p, TK_AND_AND)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_bitor(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_logical_or(EtccParser *p) {
    EtccAstNode *e = parse_logical_and(p);
    while (e && check(p, TK_OR_OR)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_logical_and(p);
        e = make_binary(p, op.kind, e, r, op.loc);
    }
    return e;
}

static EtccAstNode *parse_conditional(EtccParser *p) {
    EtccAstNode *cond = parse_logical_or(p);
    if (!cond) return NULL;
    if (check(p, TK_QUESTION)) {
        EtccLoc loc = p->cur.loc;
        parser_advance(p);
        EtccAstNode *then_expr = parse_expression(p);
        expect(p, TK_COLON, "':'");
        EtccAstNode *else_expr = parse_conditional(p);
        if (!then_expr || !else_expr) return cond;
        EtccAstNode *n = etcc_ast_new(p->arena, AST_COND, loc);
        n->u.cond.cond = cond;
        n->u.cond.then_expr = then_expr;
        n->u.cond.else_expr = else_expr;
        return n;
    }
    return cond;
}

static bool is_assignment_op(EtccTokenKind k) {
    switch (k) {
    case TK_ASSIGN:
    case TK_PLUS_ASSIGN:
    case TK_MINUS_ASSIGN:
    case TK_STAR_ASSIGN:
    case TK_SLASH_ASSIGN:
    case TK_PERCENT_ASSIGN:
    case TK_AMP_ASSIGN:
    case TK_PIPE_ASSIGN:
    case TK_CARET_ASSIGN:
    case TK_SHL_ASSIGN:
    case TK_SHR_ASSIGN:
        return true;
    default:
        return false;
    }
}

static EtccAstNode *parse_assignment(EtccParser *p) {
    EtccAstNode *lhs = parse_conditional(p);
    if (!lhs) return NULL;
    if (is_assignment_op(p->cur.kind)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *rhs = parse_assignment(p);
        if (!rhs) return lhs;
        EtccAstNode *n = etcc_ast_new(p->arena, AST_ASSIGN, op.loc);
        n->u.assign.op = op.kind;
        n->u.assign.target = lhs;
        n->u.assign.value = rhs;
        return n;
    }
    return lhs;
}

static EtccAstNode *parse_initializer(EtccParser *p) {
    if (!check(p, TK_LBRACE)) return parse_assignment(p);

    EtccLoc loc = p->cur.loc;
    parser_advance(p);
    EtccAstNode *n = etcc_ast_new(p->arena, AST_INIT_LIST, loc);
    EtccAstNode *head = NULL, *tail = NULL;

    if (!check(p, TK_RBRACE)) {
        for (;;) {
            if (check(p, TK_DOT)) {
                parser_error(p, p->cur.loc,
                             "designated initializers are not supported yet");
                parser_advance(p);
                if (check(p, TK_IDENT)) parser_advance(p);
                if (accept(p, TK_ASSIGN)) {
                }
            } else if (check(p, TK_LBRACKET)) {
                parser_error(p, p->cur.loc,
                             "array designators are not supported yet");
                skip_balanced(p, TK_LBRACKET, TK_RBRACKET);
                if (accept(p, TK_ASSIGN)) {
                }
            }

            EtccAstNode *item = parse_initializer(p);
            if (item) {
                if (tail) tail->next = item;
                else head = item;
                tail = item;
            }
            if (!accept(p, TK_COMMA)) break;
            if (check(p, TK_RBRACE)) break;
        }
    }
    expect(p, TK_RBRACE, "'}'");
    n->u.init_list.items = head;
    return n;
}

static EtccAstNode *parse_expression(EtccParser *p) {
    EtccAstNode *e = parse_assignment(p);
    while (e && check(p, TK_COMMA)) {
        EtccToken op = p->cur;
        parser_advance(p);
        EtccAstNode *r = parse_assignment(p);
        if (!r) break;
        EtccAstNode *n = etcc_ast_new(p->arena, AST_COMMA, op.loc);
        n->u.comma.lhs = e;
        n->u.comma.rhs = r;
        e = n;
    }
    return e;
}

static EtccAstNode *parse_statement(EtccParser *p) {
    EtccToken t = p->cur;

    if (check(p, TK_LBRACE)) return parse_compound(p);
    if (check(p, TK_SEMICOLON)) {
        EtccAstNode *n = etcc_ast_new(p->arena, AST_EMPTY, t.loc);
        parser_advance(p);
        return n;
    }
    if (check(p, TK_KW_RETURN)) {
        parser_advance(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_RETURN, t.loc);
        if (!check(p, TK_SEMICOLON)) n->u.ret.expr = parse_expression(p);
        expect(p, TK_SEMICOLON, "';'");
        return n;
    }
    if (check(p, TK_KW_IF)) {
        parser_advance(p);
        expect(p, TK_LPAREN, "'('");
        EtccAstNode *cond = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        EtccAstNode *then_branch = parse_statement(p);
        EtccAstNode *else_branch = NULL;
        if (accept(p, TK_KW_ELSE)) else_branch = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_IF, t.loc);
        n->u.if_.cond = cond;
        n->u.if_.then_branch = then_branch;
        n->u.if_.else_branch = else_branch;
        return n;
    }
    if (check(p, TK_KW_WHILE)) {
        parser_advance(p);
        expect(p, TK_LPAREN, "'('");
        EtccAstNode *cond = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_WHILE, t.loc);
        n->u.while_.cond = cond;
        n->u.while_.body = body;
        return n;
    }
    if (check(p, TK_KW_DO)) {
        parser_advance(p);
        EtccAstNode *body = parse_statement(p);
        expect(p, TK_KW_WHILE, "'while'");
        expect(p, TK_LPAREN, "'('");
        EtccAstNode *cond = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        expect(p, TK_SEMICOLON, "';'");
        EtccAstNode *n = etcc_ast_new(p->arena, AST_DO, t.loc);
        n->u.do_.body = body;
        n->u.do_.cond = cond;
        return n;
    }
    if (check(p, TK_KW_FOR)) {
        parser_advance(p);
        expect(p, TK_LPAREN, "'('");
        EtccAstNode *init = NULL;
        if (!check(p, TK_SEMICOLON)) {
            if (is_decl_start(p)) {
                DeclSpec ds;
                parse_decl_specifiers(p, &ds);
                EtccAstNode *head = NULL, *tail = NULL;
                append_chain(&head, &tail, ds.enum_consts);
                for (;;) {
                    Declarator d = parse_declarator(p, ds.type, false);
                    if (d.name) {
                        EtccAstNode *dn =
                            etcc_ast_new(p->arena, AST_DECL, d.loc);
                        dn->u.decl.name = d.name;
                        dn->u.decl.type = d.type;
                        dn->u.decl.storage = ds.storage;
                        dn->u.decl.is_definition = true;
                        if (check(p, TK_ASSIGN)) {
                            parser_advance(p);
                            dn->u.decl.init = parse_initializer(p);
                        }
                        if (tail) tail->next = dn;
                        else head = dn;
                        tail = dn;
                    }
                    if (!accept(p, TK_COMMA)) break;
                }
                init = head;
            } else {
                EtccAstNode *e = parse_expression(p);
                if (e) {
                    init = etcc_ast_new(p->arena, AST_EXPR_STMT, e->loc);
                    init->u.expr_stmt.expr = e;
                }
            }
        }
        expect(p, TK_SEMICOLON, "';'");
        EtccAstNode *cond = NULL;
        if (!check(p, TK_SEMICOLON)) cond = parse_expression(p);
        expect(p, TK_SEMICOLON, "';'");
        EtccAstNode *step = NULL;
        if (!check(p, TK_RPAREN)) step = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_FOR, t.loc);
        n->u.for_.init = init;
        n->u.for_.cond = cond;
        n->u.for_.step = step;
        n->u.for_.body = body;
        return n;
    }
    if (check(p, TK_KW_SWITCH)) {
        parser_advance(p);
        expect(p, TK_LPAREN, "'('");
        EtccAstNode *cond = parse_expression(p);
        expect(p, TK_RPAREN, "')'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_SWITCH, t.loc);
        n->u.switch_.cond = cond;
        n->u.switch_.body = body;
        return n;
    }
    if (check(p, TK_KW_CASE)) {
        parser_advance(p);
        EtccAstNode *value = parse_assignment(p);
        expect(p, TK_COLON, "':'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_CASE, t.loc);
        n->u.case_.value = value;
        n->u.case_.body = body;
        return n;
    }
    if (check(p, TK_KW_DEFAULT)) {
        parser_advance(p);
        expect(p, TK_COLON, "':'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_DEFAULT, t.loc);
        n->u.default_.body = body;
        return n;
    }
    if (check(p, TK_KW_GOTO)) {
        parser_advance(p);
        if (!check(p, TK_IDENT)) {
            parser_error(p, p->cur.loc, "expected a label name after 'goto'");
            recover_statement(p);
            return NULL;
        }
        EtccAstNode *n = etcc_ast_new(p->arena, AST_GOTO, t.loc);
        n->u.goto_.name = p->cur.v.ident.data;
        n->u.goto_.len = p->cur.v.ident.len;
        parser_advance(p);
        expect(p, TK_SEMICOLON, "';'");
        return n;
    }
    if (check(p, TK_KW_BREAK)) {
        parser_advance(p);
        expect(p, TK_SEMICOLON, "';'");
        return etcc_ast_new(p->arena, AST_BREAK, t.loc);
    }
    if (check(p, TK_KW_CONTINUE)) {
        parser_advance(p);
        expect(p, TK_SEMICOLON, "';'");
        return etcc_ast_new(p->arena, AST_CONTINUE, t.loc);
    }
    if (check(p, TK_IDENT) && parser_peek(p).kind == TK_COLON) {
        EtccToken name = p->cur;
        parser_advance(p);
        expect(p, TK_COLON, "':'");
        EtccAstNode *body = parse_statement(p);
        EtccAstNode *n = etcc_ast_new(p->arena, AST_LABEL, name.loc);
        n->u.label.name = name.v.ident.data;
        n->u.label.len = name.v.ident.len;
        n->u.label.body = body;
        return n;
    }

    EtccAstNode *e = parse_expression(p);
    if (!e) {
        recover_statement(p);
        return NULL;
    }
    expect(p, TK_SEMICOLON, "';'");
    EtccAstNode *n = etcc_ast_new(p->arena, AST_EXPR_STMT, e->loc);
    n->u.expr_stmt.expr = e;
    return n;
}

static void append_node(EtccAstNode **head, EtccAstNode **tail,
                        EtccAstNode *n) {
    if (!n) return;
    if (*tail) (*tail)->next = n;
    else *head = n;
    *tail = n;
}

static void append_chain(EtccAstNode **head, EtccAstNode **tail,
                         EtccAstNode *chain) {
    while (chain) {
        EtccAstNode *next = chain->next;
        chain->next = NULL;
        append_node(head, tail, chain);
        chain = next;
    }
}

static EtccAstNode *parse_compound(EtccParser *p) {
    EtccLoc loc = p->cur.loc;
    if (!expect(p, TK_LBRACE, "'{'")) return NULL;
    size_t typedef_mark = p->typedefs.len;
    size_t tag_mark = p->tags.len;
    int old_tag_depth = p->tag_depth;
    p->tag_depth++;
    EtccAstNode *head = NULL, *tail = NULL;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF) && !p->fatal) {
        if (is_decl_start(p)) {
            DeclSpec ds;
            if (!parse_decl_specifiers(p, &ds)) {
                parser_advance(p);
                continue;
            }
            if (check(p, TK_SEMICOLON)) {
                /* A tag/enum declaration such as `struct S;` or
                 * `enum E { A, B };` has no declarator. */
                append_chain(&head, &tail, ds.enum_consts);
                parser_advance(p);
                continue;
            }
            append_chain(&head, &tail, ds.enum_consts);
            bool ok = true;
            for (;;) {
                Declarator d = parse_declarator(p, ds.type, false);
                if (ds.storage == STORAGE_TYPEDEF) {
                    if (d.name) typedef_add(p, d.name, d.type);
                    if (check(p, TK_ASSIGN)) {
                        parser_error(p, p->cur.loc,
                                     "typedef declaration cannot have an initializer");
                        parser_advance(p);
                        parse_assignment(p);
                    }
                } else if (d.name) {
                    EtccAstNode *dn = etcc_ast_new(p->arena, AST_DECL, d.loc);
                    dn->u.decl.name = d.name;
                    dn->u.decl.type = d.type;
                    dn->u.decl.storage = ds.storage;
                    dn->u.decl.is_definition = true;
                    if (check(p, TK_ASSIGN)) {
                        parser_advance(p);
                        dn->u.decl.init = parse_initializer(p);
                    }
                    append_node(&head, &tail, dn);
                } else {
                    ok = false;
                }
                if (!accept(p, TK_COMMA)) break;
            }
            if (!expect(p, TK_SEMICOLON, "';'")) {
                recover_statement(p);
            }
            (void)ok;
            continue;
        }

        EtccAstNode *stmt = parse_statement(p);
        append_node(&head, &tail, stmt);
        if (!stmt && !check(p, TK_RBRACE) && !check(p, TK_EOF))
            recover_statement(p);
    }

    expect(p, TK_RBRACE, "'}'");
    p->typedefs.len = typedef_mark;
    p->tags.len = tag_mark;
    p->tag_depth = old_tag_depth;

    EtccAstNode *n = etcc_ast_new(p->arena, AST_COMPOUND, loc);
    n->u.compound.stmts = head;
    return n;
}

static EtccAstNode *parse_global_declaration(EtccParser *p, DeclSpec *ds,
                                             Declarator first) {
    EtccAstNode *head = NULL, *tail = NULL;
    Declarator d = first;

    for (;;) {
        if (ds->storage == STORAGE_TYPEDEF) {
            if (d.name) typedef_add(p, d.name, d.type);
            if (check(p, TK_ASSIGN)) {
                parser_error(p, p->cur.loc,
                             "typedef declaration cannot have an initializer");
                parser_advance(p);
                parse_assignment(p);
            }
        } else if (d.name) {
            EtccAstNode *n = etcc_ast_new(p->arena, AST_DECL, d.loc);
            n->u.decl.name = d.name;
            n->u.decl.type = d.type;
            n->u.decl.storage = ds->storage;
            n->u.decl.is_definition =
                (d.type && d.type->kind != CT_FUNC) &&
                ds->storage != STORAGE_EXTERN;
            if (check(p, TK_ASSIGN)) {
                parser_advance(p);
                n->u.decl.init = parse_initializer(p);
            }
            append_node(&head, &tail, n);
        } else {
            parser_error(p, d.loc, "declaration has no name");
        }

        if (!accept(p, TK_COMMA)) break;
        d = parse_declarator(p, ds->type, false);
    }
    expect(p, TK_SEMICOLON, "';'");
    return head;
}

EtccAstNode *etcc_parse_translation_unit(EtccArena *arena,
                                         EtccDiagnostics *diag,
                                         const char *text, size_t len,
                                         const EtccSourceMap *map,
                                         const EtccMacroTable *macros) {
    EtccParser p;
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.diag = diag;
    etcc_lexer_init(&p.lx, arena, diag, text, len, map, macros);
    parser_advance(&p);

    EtccAstNode *tu = etcc_ast_new(arena, AST_TRANSLATION_UNIT, p.cur.loc);
    EtccAstNode *head = NULL, *tail = NULL;

    while (!check(&p, TK_EOF) && !p.fatal) {
        if (check(&p, TK_SEMICOLON)) {
            parser_advance(&p);
            continue;
        }
        if (!is_decl_start(&p)) {
            parser_error(&p, p.cur.loc, "expected a declaration");
            parser_advance(&p);
            recover_statement(&p);
            continue;
        }

        DeclSpec ds;
        if (!parse_decl_specifiers(&p, &ds)) {
            parser_error(&p, p.cur.loc, "expected declaration specifiers");
            parser_advance(&p);
            recover_statement(&p);
            continue;
        }
        append_chain(&head, &tail, ds.enum_consts);
        if (check(&p, TK_SEMICOLON)) {
            parser_advance(&p);
            continue;
        }

        Declarator d = parse_declarator(&p, ds.type, false);

        if (d.type && d.type->kind == CT_FUNC && check(&p, TK_LBRACE) &&
            ds.storage != STORAGE_TYPEDEF) {
            if (!d.name) {
                parser_error(&p, d.loc, "function definition has no name");
                skip_balanced(&p, TK_LBRACE, TK_RBRACE);
                continue;
            }
            EtccAstNode *body = parse_compound(&p);
            EtccAstNode *fn = etcc_ast_new(arena, AST_FUNC_DEF, d.loc);
            fn->u.func.name = d.name;
            fn->u.func.type = d.type;
            fn->u.func.params = d.params;
            fn->u.func.body = body;
            fn->u.func.storage = ds.storage;
            append_node(&head, &tail, fn);
            continue;
        }

        EtccAstNode *decls = parse_global_declaration(&p, &ds, d);
        EtccAstNode *n = decls;
        while (n) {
            EtccAstNode *next = n->next;
            n->next = NULL;
            append_node(&head, &tail, n);
            n = next;
        }
        if (p.diag->errors && !check(&p, TK_EOF) && !is_decl_start(&p))
            recover_statement(&p);
    }

    tu->u.tu.decls = head;
    free(p.typedefs.items);
    free(p.tags.items);
    free(p.enum_values.items);
    return tu;
}
