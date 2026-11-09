#include "etcc/sema.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SYM_LOCAL_VAR,
    SYM_PARAM,
    SYM_GLOBAL_VAR,
    SYM_FUNCTION,
    SYM_ENUM_CONST,
} SymbolKind;

typedef struct EtccSymbol {
    const char *name;
    EtccCType *type;
    SymbolKind kind;
    bool is_defined;
    bool is_static;
    EtccLoc loc;
    IrGlobal *global;
    IrFunction *func;
    IrValue *addr; /* global value or stack slot */
    int64_t enum_value;
    struct EtccSymbol *next;
} EtccSymbol;

typedef struct EtccScope {
    EtccSymbol *symbols;
    struct EtccScope *parent;
} EtccScope;

typedef struct {
    IrBlock *break_block;
    IrBlock *continue_block;
} EtccLoop;

typedef struct {
    const char *name;
    size_t len;
    IrBlock *block;
} EtccLabel;

typedef struct {
    EtccAstNode *node;
    int64_t value;
    IrBlock *block;
} EtccCase;

typedef struct {
    EtccAstNode *node;
    IrBlock *end;
    IrBlock *default_block;
    ETCC_VEC(EtccCase) cases;
} EtccSwitch;

typedef struct {
    EtccArena *arena;
    EtccDiagnostics *diag;
    IrModule *m;
    EtccScope *scope;
    EtccScope global_scope;

    IrFunction *fn;
    EtccCType *fn_ret;
    IrValue *sret; /* hidden return buffer for a large aggregate return */
    IrBlock *cur;
    int block_id;

    EtccLoop loops[64];
    int loop_depth;

    IrBlock *breaks[64];
    int break_depth;

    ETCC_VEC(EtccLabel) labels;

    EtccSwitch switches[32];
    int switch_depth;

    bool failed;
} EtccSema;

typedef struct {
    EtccCType *type;
    IrValue *value;
    IrValue *addr; /* non-NULL means lvalue */
    bool is_null_const;
} SemVal;

static bool sema_expr(EtccSema *s, EtccAstNode *n, SemVal *out);
static bool sema_lvalue(EtccSema *s, EtccAstNode *n, SemVal *out);
static bool sema_stmt(EtccSema *s, EtccAstNode *n);
static EtccCType *infer_type(EtccSema *s, EtccAstNode *n);
static IrType *map_type(EtccSema *s, EtccCType *t, EtccLoc loc);
static bool is_aggregate(const EtccCType *t);
static void sema_error(EtccSema *s, EtccLoc loc, const char *fmt, ...);

static bool abi_aggregate_by_register(const EtccCType *t) {
    if (!is_aggregate(t)) return false;
    size_t size = etcc_type_sizeof(t);
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static IrType *abi_aggregate_scalar(EtccSema *s, const EtccCType *t) {
    switch (etcc_type_sizeof(t)) {
    case 1: return ir_type_i8(s->m);
    case 2: return ir_type_i16(s->m);
    case 4: return ir_type_i32(s->m);
    default: return ir_type_i64(s->m);
    }
}

static bool abi_return_uses_sret(const EtccCType *t) {
    return is_aggregate(t) && !abi_aggregate_by_register(t);
}

static IrType *abi_param_type(EtccSema *s, EtccCType *t, EtccLoc loc) {
    if (!is_aggregate(t)) return map_type(s, t, loc);
    if (abi_aggregate_by_register(t)) return abi_aggregate_scalar(s, t);
    return ir_type_ptr(s->m, map_type(s, t, loc));
}

static IrType *abi_return_type(EtccSema *s, EtccCType *t, EtccLoc loc) {
    if (!is_aggregate(t)) return map_type(s, t, loc);
    if (abi_aggregate_by_register(t)) return abi_aggregate_scalar(s, t);
    return ir_type_void(s->m);
}

static IrType *aggregate_buffer_type(EtccSema *s, EtccCType *t,
                                     bool abi_aligned) {
    size_t align = t->align ? t->align : 1;
    if (abi_aligned && abi_aggregate_by_register(t) && t->size > align)
        align = t->size;
    return ir_type_struct(s->m, t->size, align);
}

static IrValue *aggregate_to_scalar(EtccSema *s, SemVal *v, EtccCType *t,
                                    EtccLoc loc) {
    if (!v->value) {
        sema_error(s, loc, "aggregate expression has no value");
        return ir_const_int(s->m, abi_aggregate_scalar(s, t), 0);
    }
    IrType *buf = aggregate_buffer_type(s, t, true);
    IrValue *tmp = ir_build_alloca(s->m, s->fn, buf);
    ir_build_store(s->m, s->cur, tmp, v->value);
    return ir_build_load(s->m, s->cur, tmp, abi_aggregate_scalar(s, t));
}

static IrValue *aggregate_to_pointer(EtccSema *s, SemVal *v, EtccCType *t,
                                     EtccLoc loc) {
    if (!v->value) {
        sema_error(s, loc, "aggregate expression has no value");
        return NULL;
    }
    IrType *buf = aggregate_buffer_type(s, t, false);
    IrValue *tmp = ir_build_alloca(s->m, s->fn, buf);
    ir_build_store(s->m, s->cur, tmp, v->value);
    return tmp;
}

static void aggregate_from_scalar(EtccSema *s, EtccCType *t, IrValue *scalar,
                                  IrValue *buffer, SemVal *out) {
    IrType *buf = aggregate_buffer_type(s, t, true);
    if (!buffer) buffer = ir_build_alloca(s->m, s->fn, buf);
    ir_build_store(s->m, s->cur, buffer, scalar);
    out->type = t;
    out->addr = buffer;
    out->value = ir_build_load(s->m, s->cur, buffer,
                               aggregate_buffer_type(s, t, true));
    out->is_null_const = false;
}
static bool type_is_complete_object(const EtccCType *t);
static bool const_eval(EtccSema *s, EtccAstNode *n, int64_t *out);

static void sema_error(EtccSema *s, EtccLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    etcc_diag_error(s->diag, loc, "%s", buf);
    s->failed = true;
}

static void sema_warning(EtccSema *s, EtccLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    etcc_diag_warning(s->diag, loc, "%s", buf);
}

static EtccCType *ctype_size_t(void) {
    static EtccCType t = {.kind = CT_LLONG,
                          .size = 8,
                          .align = 8,
                          .is_unsigned = true};
    return &t;
}

static EtccCType *ctype_uint(void) {
    static EtccCType t = {.kind = CT_INT,
                          .size = 4,
                          .align = 4,
                          .is_unsigned = true};
    return &t;
}

static EtccCType *ctype_ulong(void) {
    static EtccCType t = {.kind = CT_LONG,
                          .size = 4,
                          .align = 4,
                          .is_unsigned = true};
    return &t;
}

static EtccCType *ctype_ull(void) {
    static EtccCType t = {.kind = CT_LLONG,
                          .size = 8,
                          .align = 8,
                          .is_unsigned = true};
    return &t;
}

/* C99 6.4.4.1 integer-literal typing, shared by expression sema and
 * sizeof/conditional type inference. Windows LLP64: long is 32-bit. */
static EtccCType *int_literal_type(const EtccAstNode *n) {
    uint64_t v = n->u.int_lit.value;
    int lc = n->u.int_lit.long_count;
    bool u = n->u.int_lit.is_unsigned;

    if (lc >= 2) {
        if (u || v > 0x7FFFFFFFFFFFFFFFull) return ctype_ull();
        return etcc_type_builtin(CT_LLONG);
    }
    if (lc == 1) {
        if (u) return v <= 0xFFFFFFFFull ? ctype_ulong() : ctype_ull();
        if (v <= 0x7FFFFFFFull) return etcc_type_builtin(CT_LONG);
        return etcc_type_builtin(CT_LLONG);
    }
    if (u) return v <= 0xFFFFFFFFull ? ctype_uint() : ctype_ull();
    if (n->u.int_lit.is_decimal) {
        if (v <= 0x7FFFFFFFull) return etcc_type_builtin(CT_INT);
        if (v <= 0x7FFFFFFFFFFFFFFFull) return etcc_type_builtin(CT_LLONG);
        return ctype_ull();
    }
    /* Hexadecimal/octal/binary candidates: int, unsigned int, long,
     * unsigned long, long long, unsigned long long. */
    if (v <= 0x7FFFFFFFull) return etcc_type_builtin(CT_INT);
    if (v <= 0xFFFFFFFFull) return ctype_uint();
    if (v <= 0x7FFFFFFFFFFFFFFFull) return etcc_type_builtin(CT_LLONG);
    return ctype_ull();
}

static EtccCType *integer_promote(EtccSema *s, EtccCType *t) {
    if (!t) return etcc_type_builtin(CT_INT);
    switch (t->kind) {
    case CT_BOOL:
    case CT_CHAR:
    case CT_SHORT:
        return etcc_type_builtin(CT_INT);
    default:
        (void)s;
        return t;
    }
}

static EtccCType *common_arithmetic(EtccSema *s, EtccCType *a, EtccCType *b) {
    return etcc_type_common_arithmetic(integer_promote(s, a),
                                       integer_promote(s, b));
}

static EtccCType *ptr_to(EtccSema *s, EtccCType *base) {
    return etcc_type_ptr(s->arena, base);
}

static bool is_pointer(const EtccCType *t) {
    return t && t->kind == CT_PTR;
}

static bool is_integer(const EtccCType *t) {
    return etcc_type_is_integer(t);
}

static bool is_void(const EtccCType *t) {
    return t && t->kind == CT_VOID;
}

static bool is_aggregate(const EtccCType *t) {
    return etcc_type_is_aggregate(t);
}

static EtccCType *decay_type(EtccSema *s, EtccCType *t) {
    if (!t) return NULL;
    if (t->kind == CT_ARRAY) return ptr_to(s, t->base);
    if (t->kind == CT_FUNC) return ptr_to(s, t);
    return t;
}

static EtccCType *conditional_type(EtccSema *s, EtccCType *then_t,
                                   EtccCType *else_t) {
    then_t = decay_type(s, then_t);
    else_t = decay_type(s, else_t);
    if (!then_t || !else_t) return NULL;
    if (is_integer(then_t) && is_integer(else_t))
        return common_arithmetic(s, then_t, else_t);
    if (is_pointer(then_t) && is_pointer(else_t)) {
        if (etcc_type_equal(then_t, else_t)) return then_t;
        if (then_t->base && then_t->base->kind == CT_VOID) return then_t;
        if (else_t->base && else_t->base->kind == CT_VOID) return else_t;
        return NULL;
    }
    if (is_pointer(then_t) && is_integer(else_t)) return then_t;
    if (is_integer(then_t) && is_pointer(else_t)) return else_t;
    return NULL;
}

static EtccCType *binary_type(EtccSema *s, EtccTokenKind op, EtccCType *lhs,
                              EtccCType *rhs) {
    lhs = decay_type(s, lhs);
    rhs = decay_type(s, rhs);
    if (!lhs || !rhs) return NULL;

    if (op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE ||
        op == TK_GT || op == TK_GE || op == TK_AND_AND || op == TK_OR_OR) {
        bool lscalar = is_integer(lhs) || is_pointer(lhs);
        bool rscalar = is_integer(rhs) || is_pointer(rhs);
        return (lscalar && rscalar) ? etcc_type_builtin(CT_INT) : NULL;
    }

    if (op == TK_SHL || op == TK_SHR)
        return is_integer(lhs) ? integer_promote(s, lhs) : NULL;

    if (op == TK_PLUS &&
        ((is_pointer(lhs) && is_integer(rhs)) ||
         (is_integer(lhs) && is_pointer(rhs))))
        return is_pointer(lhs) ? lhs : rhs;

    if (op == TK_MINUS) {
        if (is_pointer(lhs) && is_integer(rhs)) return lhs;
        if (is_pointer(lhs) && is_pointer(rhs)) return etcc_type_builtin(CT_LLONG);
        if (is_pointer(lhs) || is_pointer(rhs)) return NULL;
    }

    if (is_pointer(lhs) || is_pointer(rhs)) return NULL;
    if (!is_integer(lhs) || !is_integer(rhs)) return NULL;
    return common_arithmetic(s, lhs, rhs);
}

static IrType *map_type_allow_incomplete(EtccSema *s, EtccCType *t,
                                         EtccLoc loc) {
    if (!t) return ir_type_i32(s->m);
    if ((t->kind == CT_STRUCT || t->kind == CT_UNION) && !t->is_complete)
        return ir_type_struct(s->m, 0, 1);
    return map_type(s, t, loc);
}

static IrType *map_type(EtccSema *s, EtccCType *t, EtccLoc loc) {
    if (!t) return ir_type_i32(s->m);
    switch (t->kind) {
    case CT_VOID: return ir_type_void(s->m);
    case CT_BOOL: return ir_type_i8(s->m);
    case CT_CHAR: return ir_type_i8(s->m);
    case CT_SHORT: return ir_type_i16(s->m);
    case CT_INT: return ir_type_i32(s->m);
    case CT_LONG: return ir_type_i32(s->m);
    case CT_LLONG: return ir_type_i64(s->m);
    case CT_FLOAT:
    case CT_DOUBLE:
        sema_error(s, loc, "floating-point types are not supported yet");
        return ir_type_i32(s->m);
    case CT_PTR:
        return ir_type_ptr(s->m, map_type_allow_incomplete(s, t->base, loc));
    case CT_ARRAY: {
        uint64_t count = t->array_len;
        if (count == 0) {
            sema_error(s, loc, "incomplete array type is not allowed here");
            count = 1;
        }
        return ir_type_array(s->m, map_type(s, t->base, loc), count);
    }
    case CT_STRUCT:
    case CT_UNION:
        if (!t->is_complete || t->size == 0) {
            sema_error(s, loc, "incomplete %s type is not allowed here",
                       t->kind == CT_UNION ? "union" : "struct");
            return ir_type_struct(s->m, 1, 1);
        }
        return ir_type_struct(s->m, t->size, t->align);
    case CT_FUNC:
        return ir_type_ptr(s->m, ir_type_void(s->m));
    default:
        return ir_type_i32(s->m);
    }
}

static EtccScope *scope_push(EtccSema *s) {
    EtccScope *sc = etcc_arena_alloc(s->arena, sizeof(*sc), _Alignof(EtccScope));
    memset(sc, 0, sizeof(*sc));
    sc->parent = s->scope;
    s->scope = sc;
    return sc;
}

static void scope_pop(EtccSema *s) {
    if (s->scope && s->scope->parent) s->scope = s->scope->parent;
}

static EtccSymbol *scope_find_local(EtccScope *sc, const char *name) {
    for (EtccSymbol *sym = sc->symbols; sym; sym = sym->next)
        if (strcmp(sym->name, name) == 0) return sym;
    return NULL;
}

static EtccSymbol *scope_lookup(EtccSema *s, const char *name) {
    for (EtccScope *sc = s->scope; sc; sc = sc->parent) {
        EtccSymbol *sym = scope_find_local(sc, name);
        if (sym) return sym;
    }
    return NULL;
}

static EtccSymbol *symbol_add(EtccSema *s, EtccScope *sc, const char *name,
                              EtccCType *type, SymbolKind kind, EtccLoc loc) {
    if (scope_find_local(sc, name)) {
        sema_error(s, loc, "redefinition of '%s'", name);
        return scope_find_local(sc, name);
    }
    EtccSymbol *sym = etcc_arena_alloc(s->arena, sizeof(*sym),
                                       _Alignof(EtccSymbol));
    memset(sym, 0, sizeof(*sym));
    sym->name = etcc_arena_strdup(s->arena, name);
    sym->type = type;
    sym->kind = kind;
    sym->loc = loc;
    sym->next = sc->symbols;
    sc->symbols = sym;
    return sym;
}

static IrBlock *new_block(EtccSema *s, const char *prefix) {
    char name[64];
    snprintf(name, sizeof(name), "%s.%d", prefix, s->block_id++);
    return ir_block_create(s->m, s->fn, name);
}

static bool block_open(EtccSema *s) {
    return s->cur && s->cur->term == IR_TERM_NONE;
}

static void emit_br(EtccSema *s, IrBlock *target) {
    if (!block_open(s)) return;
    ir_block_set_br(s->m, s->cur, target);
    s->cur = NULL;
}

static void collect_labels(EtccSema *s, EtccAstNode *n) {
    if (!n) return;
    switch (n->kind) {
    case AST_LABEL: {
        bool duplicate = false;
        for (size_t i = 0; i < s->labels.len; i++) {
            EtccLabel *l = &s->labels.items[i];
            if (l->len == n->u.label.len &&
                memcmp(l->name, n->u.label.name, l->len) == 0) {
                sema_error(s, n->loc, "duplicate label '%.*s'",
                           (int)n->u.label.len, n->u.label.name);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            EtccLabel label = {
                .name = n->u.label.name,
                .len = n->u.label.len,
                .block = new_block(s, "label"),
            };
            if (!ETCC_VEC_PUSH(s->labels, label))
                sema_error(s, n->loc, "out of memory");
        }
        collect_labels(s, n->u.label.body);
        return;
    }
    case AST_COMPOUND:
        for (EtccAstNode *st = n->u.compound.stmts; st; st = st->next)
            collect_labels(s, st);
        return;
    case AST_IF:
        collect_labels(s, n->u.if_.then_branch);
        collect_labels(s, n->u.if_.else_branch);
        return;
    case AST_WHILE:
        collect_labels(s, n->u.while_.body);
        return;
    case AST_DO:
        collect_labels(s, n->u.do_.body);
        return;
    case AST_FOR:
        collect_labels(s, n->u.for_.body);
        return;
    case AST_SWITCH:
        collect_labels(s, n->u.switch_.body);
        return;
    case AST_CASE:
        collect_labels(s, n->u.case_.body);
        return;
    case AST_DEFAULT:
        collect_labels(s, n->u.default_.body);
        return;
    default:
        return;
    }
}

static bool collect_switch_cases(EtccSema *s, EtccAstNode *n, EtccSwitch *sw) {
    if (!n) return true;
    switch (n->kind) {
    case AST_SWITCH:
        /* Case labels of a nested switch belong to that switch. */
        return true;
    case AST_CASE: {
        int64_t value = 0;
        if (!n->u.case_.value || !const_eval(s, n->u.case_.value, &value)) {
            sema_error(s, n->loc, "case label is not an integer constant");
            return false;
        }
        for (size_t i = 0; i < sw->cases.len; i++) {
            if (sw->cases.items[i].value == value) {
                sema_error(s, n->loc, "duplicate case value %lld",
                           (long long)value);
                return false;
            }
        }
        EtccCase c = {
            .node = n,
            .value = value,
            .block = new_block(s, "switch.case"),
        };
        if (!ETCC_VEC_PUSH(sw->cases, c)) {
            sema_error(s, n->loc, "out of memory");
            return false;
        }
        return collect_switch_cases(s, n->u.case_.body, sw);
    }
    case AST_DEFAULT:
        if (sw->default_block) {
            sema_error(s, n->loc, "duplicate default label");
            return false;
        }
        sw->default_block = new_block(s, "switch.default");
        return collect_switch_cases(s, n->u.default_.body, sw);
    case AST_COMPOUND:
        for (EtccAstNode *st = n->u.compound.stmts; st; st = st->next) {
            if (!collect_switch_cases(s, st, sw)) return false;
        }
        return true;
    case AST_IF:
        return collect_switch_cases(s, n->u.if_.then_branch, sw) &&
               collect_switch_cases(s, n->u.if_.else_branch, sw);
    case AST_WHILE:
        return collect_switch_cases(s, n->u.while_.body, sw);
    case AST_DO:
        return collect_switch_cases(s, n->u.do_.body, sw);
    case AST_FOR:
        return collect_switch_cases(s, n->u.for_.body, sw);
    case AST_LABEL:
        return collect_switch_cases(s, n->u.label.body, sw);
    default:
        return true;
    }
}

static EtccCase *switch_find_case(EtccSwitch *sw, EtccAstNode *node) {
    for (size_t i = 0; i < sw->cases.len; i++) {
        if (sw->cases.items[i].node == node) return &sw->cases.items[i];
    }
    return NULL;
}

static EtccLabel *label_find(EtccSema *s, const char *name, size_t len) {
    for (size_t i = 0; i < s->labels.len; i++) {
        EtccLabel *l = &s->labels.items[i];
        if (l->len == len && memcmp(l->name, name, len) == 0) return l;
    }
    return NULL;
}

static IrValue *zero_of_type(EtccSema *s, IrType *t) {
    if (t->kind == IR_TYPE_PTR) return ir_const_null(s->m, t);
    return ir_const_int(s->m, t, 0);
}

static IrValue *const_int_typed(EtccSema *s, EtccCType *ct, uint64_t v) {
    return ir_const_int(s->m, map_type(s, ct, ETCC_LOC_NONE), v);
}

static IrValue *truthy(EtccSema *s, SemVal *v, EtccLoc loc) {
    if (!v->value) {
        sema_error(s, loc, "expression has no value");
        return ir_const_int(s->m, ir_type_i32(s->m), 0);
    }
    IrType *t = v->value->type;
    IrValue *zero = zero_of_type(s, t);
    return ir_build_cmp(s->m, s->cur, IR_CMP_NE, v->value, zero);
}

static bool decay(EtccSema *s, SemVal *v, EtccLoc loc) {
    if (!v->type) return false;
    if (v->type->kind == CT_ARRAY) {
        if (!v->addr) {
            sema_error(s, loc, "array expression has no address");
            return false;
        }
        if (v->type->array_len == 0)
            sema_error(s, loc, "cannot use an incomplete array value");
        EtccCType *elem = v->type->base;
        IrType *elem_ir = map_type(s, elem, loc);
        IrValue *idx = ir_const_int(s->m, ir_type_i64(s->m), 0);
        v->value = ir_build_gep(s->m, s->cur, v->addr, elem_ir, idx);
        v->type = ptr_to(s, elem);
        v->addr = NULL;
        return true;
    }
    if (v->type->kind == CT_FUNC) {
        if (!v->value) {
            IrFunction *f = NULL;
            (void)f;
            v->value = ir_const_null(s->m, ir_type_ptr(s->m, ir_type_void(s->m)));
        }
        return true;
    }
    return true;
}

static bool pointer_types_compatible(const EtccCType *from,
                                     const EtccCType *to) {
    if (!is_pointer(from) || !is_pointer(to)) return false;
    if (from->base && from->base->kind == CT_FUNC)
        return to->base && etcc_type_equal(from->base, to->base);
    if (to->base && to->base->kind == CT_FUNC)
        return from->base && etcc_type_equal(from->base, to->base);
    if ((from->base && from->base->kind == CT_VOID) ||
        (to->base && to->base->kind == CT_VOID))
        return true;
    return etcc_type_equal(from->base, to->base);
}

static IrValue *convert_value(EtccSema *s, IrValue *v, EtccCType *from,
                              EtccCType *to, EtccLoc loc, bool explicit_cast) {
    if (!to) return v;
    if (!v) {
        if (!is_void(to)) {
            sema_error(s, loc, "cannot convert a void expression to a value");
        }
        return v;
    }
    if (!from) from = to;
    if (etcc_type_equal(from, to)) return v;

    if (is_aggregate(to) || is_aggregate(from)) {
        sema_error(s, loc, "incompatible aggregate types in conversion");
        return v;
    }

    if (to->kind == CT_FLOAT || to->kind == CT_DOUBLE ||
        from->kind == CT_FLOAT || from->kind == CT_DOUBLE) {
        sema_error(s, loc, "floating-point conversions are not supported yet");
        return v;
    }

    if (is_void(from) && !is_void(to)) {
        sema_error(s, loc, "cannot convert a void expression to a value");
        return v;
    }

    if (to->kind == CT_BOOL) {
        IrValue *zero;
        if (is_integer(from)) {
            zero = zero_of_type(s, v->type);
        } else if (is_pointer(from)) {
            zero = ir_const_null(s->m, map_type(s, from, loc));
        } else {
            sema_error(s, loc, "cannot convert this expression to _Bool");
            return v;
        }
        IrValue *cmp = ir_build_cmp(s->m, s->cur, IR_CMP_NE, v, zero);
        return ir_build_cast(s->m, s->cur, IR_CAST_TRUNC, cmp,
                             ir_type_i8(s->m));
    }

    if (is_integer(to)) {
        if (is_integer(from)) {
            size_t fs = etcc_type_sizeof(from);
            size_t ts = etcc_type_sizeof(to);
            IrType *target = map_type(s, to, loc);
            if (ts < fs)
                return ir_build_cast(s->m, s->cur, IR_CAST_TRUNC, v, target);
            if (ts > fs)
                return ir_build_cast(s->m, s->cur,
                                     from->is_unsigned ? IR_CAST_ZEXT : IR_CAST_SEXT,
                                     v, target);
            return v;
        }
        if (is_pointer(from)) {
            if (!explicit_cast) {
                sema_error(s, loc, "cannot implicitly convert pointer to integer");
                return v;
            }
            IrValue *wide = ir_build_cast(s->m, s->cur, IR_CAST_PTRTOINT, v,
                                          ir_type_i64(s->m));
            if (etcc_type_sizeof(to) < 8)
                return ir_build_cast(s->m, s->cur, IR_CAST_TRUNC, wide,
                                     map_type(s, to, loc));
            return wide;
        }
    }

    if (is_pointer(to)) {
        IrType *target = map_type(s, to, loc);
        if (is_pointer(from)) {
            if (!explicit_cast && !pointer_types_compatible(from, to)) {
                sema_error(s, loc, "incompatible pointer types in conversion");
                return v;
            }
            return ir_build_cast(s->m, s->cur, IR_CAST_BITCAST, v, target);
        }
        if (is_integer(from) && v->kind == IR_VALUE_CONST_INT && v->as.i64 == 0)
            return ir_const_null(s->m, target);
        if (explicit_cast && is_integer(from))
            return ir_build_cast(s->m, s->cur, IR_CAST_INTTOPTR, v, target);
        sema_error(s, loc, "cannot implicitly convert integer to pointer");
        return v;
    }

    if (is_void(to)) return v;

    sema_error(s, loc, "incompatible types in conversion");
    return v;
}

static void load_lvalue(EtccSema *s, SemVal *v) {
    if (v->addr && v->type->kind != CT_ARRAY && v->type->kind != CT_FUNC)
        v->value = ir_build_load(s->m, s->cur, v->addr,
                                 map_type(s, v->type, ETCC_LOC_NONE));
}

static IrValue *byte_offset_ptr(EtccSema *s, IrValue *base, size_t offset) {
    if (!base) return NULL;
    if (offset == 0) return base;
    IrValue *idx = ir_const_int(s->m, ir_type_i64(s->m), (uint64_t)offset);
    return ir_build_gep(s->m, s->cur, base, ir_type_i8(s->m), idx);
}

static bool sema_lvalue(EtccSema *s, EtccAstNode *n, SemVal *out) {
    memset(out, 0, sizeof(*out));
    if (!n) return false;

    switch (n->kind) {
    case AST_IDENT: {
        char *name = etcc_arena_strndup(s->arena, n->u.ident.name,
                                        n->u.ident.len);
        EtccSymbol *sym = scope_lookup(s, name);
        if (!sym) {
            sema_error(s, n->loc, "use of undeclared identifier '%s'", name);
            return false;
        }
        if (sym->kind == SYM_FUNCTION) {
            sema_error(s, n->loc, "function '%s' is not an lvalue", name);
            return false;
        }
        if (sym->kind == SYM_ENUM_CONST) {
            sema_error(s, n->loc, "enumerator '%s' is not an lvalue", name);
            return false;
        }
        out->type = sym->type;
        out->addr = sym->addr;
        out->value = sym->addr;
        return true;
    }
    case AST_UNARY:
        if (n->u.unary.op == TK_STAR) {
            SemVal p;
            if (!sema_expr(s, n->u.unary.operand, &p)) return false;
            if (!decay(s, &p, n->loc)) return false;
            if (!is_pointer(p.type)) {
                sema_error(s, n->loc, "cannot dereference a non-pointer value");
                return false;
            }
            out->type = p.type->base;
            out->addr = p.value;
            out->value = p.value;
            return true;
        }
        break;
    case AST_INDEX: {
        SemVal base;
        if (!sema_expr(s, n->u.index.base, &base)) return false;
        if (!decay(s, &base, n->loc)) return false;
        if (!is_pointer(base.type)) {
            sema_error(s, n->loc, "subscripted value is not a pointer");
            return false;
        }
        SemVal idx;
        if (!sema_expr(s, n->u.index.index, &idx)) return false;
        if (!is_integer(idx.type)) {
            sema_error(s, n->loc, "array subscript is not an integer");
            return false;
        }
        idx.value = convert_value(s, idx.value, idx.type, ctype_size_t(), n->loc,
                                  true);
        idx.type = ctype_size_t();
        IrType *elem_ir = map_type(s, base.type->base, n->loc);
        out->type = base.type->base;
        out->addr = ir_build_gep(s->m, s->cur, base.value, elem_ir, idx.value);
        out->value = out->addr;
        return true;
    }
    case AST_MEMBER: {
        SemVal base;
        EtccCType *st = NULL;
        IrValue *base_addr = NULL;
        if (n->u.member.arrow) {
            if (!sema_expr(s, n->u.member.base, &base)) return false;
            if (!decay(s, &base, n->loc)) return false;
            if (!is_pointer(base.type)) {
                sema_error(s, n->loc, "'->' requires a pointer to struct/union");
                return false;
            }
            st = base.type->base;
            base_addr = base.value;
        } else {
            SemVal bv;
            if (!sema_expr(s, n->u.member.base, &bv)) return false;
            if (!decay(s, &bv, n->loc)) return false;
            if (!is_aggregate(bv.type)) {
                sema_error(s, n->loc,
                           "member access requires a struct or union");
                return false;
            }
            if (!bv.addr) {
                sema_error(s, n->loc,
                           "member base is not an addressable struct/union");
                return false;
            }
            st = bv.type;
            base_addr = bv.addr;
        }
        if (!is_aggregate(st)) {
            sema_error(s, n->loc, "member access requires a struct or union");
            return false;
        }
        if (!st->is_complete) {
            sema_error(s, n->loc, "member access on an incomplete type");
            return false;
        }
        EtccField *f = etcc_type_find_field(st, n->u.member.field,
                                            n->u.member.field_len);
        if (!f) {
            sema_error(s, n->loc, "no member named '%.*s'",
                       (int)n->u.member.field_len, n->u.member.field);
            return false;
        }
        out->type = f->type;
        out->addr = byte_offset_ptr(s, base_addr, f->offset);
        out->value = out->addr;
        return true;
    }
    default:
        sema_error(s, n->loc, "expression is not assignable");
        return false;
    }
    return false;
}

static bool sema_condition(EtccSema *s, EtccAstNode *n, IrValue **out) {
    SemVal v;
    if (!sema_expr(s, n, &v)) return false;
    if (!decay(s, &v, n->loc)) return false;
    if (!is_integer(v.type) && !is_pointer(v.type)) {
        sema_error(s, n->loc, "condition must have scalar type");
        return false;
    }
    *out = truthy(s, &v, n->loc);
    return true;
}

static bool sema_call(EtccSema *s, EtccAstNode *n, SemVal *out) {
    EtccAstNode *callee = n->u.call.callee;
    if (!callee || callee->kind != AST_IDENT) {
        sema_error(s, n->loc, "only direct function calls are supported yet");
        return false;
    }
    char *name = etcc_arena_strndup(s->arena, callee->u.ident.name,
                                    callee->u.ident.len);
    EtccSymbol *sym = scope_lookup(s, name);
    if (!sym) {
        sema_error(s, n->loc,
                   "implicit declaration of function '%s' is not allowed", name);
        return false;
    }
    if (sym->kind != SYM_FUNCTION) {
        sema_error(s, n->loc, "'%s' is not a function", name);
        return false;
    }
    EtccCType *ft = sym->type;
    if (!ft || ft->kind != CT_FUNC) {
        sema_error(s, n->loc, "'%s' is not a function", name);
        return false;
    }
    size_t nargs = 0;
    for (EtccAstNode *a = n->u.call.args; a; a = a->next) nargs++;
    if ((!ft->variadic && nargs != ft->nparams) ||
        (ft->variadic && nargs < ft->nparams)) {
        sema_error(s, n->loc, "function '%s' expects %zu argument%s, got %zu",
                   name, ft->nparams, ft->nparams == 1 ? "" : "s", nargs);
        return false;
    }

    IrFunction *fn = sym->func;
    if (!fn) {
        sema_error(s, n->loc, "function '%s' has no IR declaration", name);
        return false;
    }

    bool sret = abi_return_uses_sret(ft->base);
    size_t abi_nargs = nargs + (sret ? 1 : 0);
    IrValue **args = NULL;
    if (abi_nargs) {
        args = etcc_arena_alloc(s->arena, abi_nargs * sizeof(*args),
                                _Alignof(IrValue *));
    }

    size_t argi = 0;
    IrValue *retbuf = NULL;
    if (sret) {
        retbuf = ir_build_alloca(s->m, s->fn,
                                 aggregate_buffer_type(s, ft->base, false));
        if (!retbuf) {
            sema_error(s, n->loc, "cannot allocate aggregate return buffer");
            return false;
        }
        args[argi++] = retbuf;
    }

    size_t i = 0;
    for (EtccAstNode *a = n->u.call.args; a; a = a->next, i++) {
        SemVal av;
        if (!sema_expr(s, a, &av)) return false;
        if (!decay(s, &av, a->loc)) return false;
        if (i < ft->nparams) {
            EtccCType *pt = ft->params[i];
            if (is_aggregate(pt)) {
                if (!is_aggregate(av.type) ||
                    !etcc_type_equal(av.type, pt)) {
                    sema_error(s, a->loc,
                               "incompatible struct/union argument type");
                    return false;
                }
                if (abi_aggregate_by_register(pt)) {
                    args[argi++] = aggregate_to_scalar(s, &av, pt, a->loc);
                } else {
                    IrValue *copy = aggregate_to_pointer(s, &av, pt, a->loc);
                    if (!copy) return false;
                    args[argi++] = copy;
                }
            } else {
                if (is_aggregate(av.type)) {
                    sema_error(s, a->loc,
                               "cannot pass struct/union to a scalar parameter");
                    return false;
                }
                av.value = convert_value(s, av.value, av.type, pt, a->loc,
                                         false);
                if (!av.value) return false;
                args[argi++] = av.value;
            }
        } else {
            if (is_aggregate(av.type)) {
                sema_error(s, a->loc,
                           "passing struct/union through varargs is not supported");
                return false;
            }
            if (av.type->kind == CT_FLOAT || av.type->kind == CT_DOUBLE) {
                sema_error(s, a->loc,
                           "floating-point varargs are not supported yet");
                return false;
            }
            EtccCType *promoted = integer_promote(s, av.type);
            av.value = convert_value(s, av.value, av.type, promoted, a->loc,
                                     false);
            if (!av.value) return false;
            args[argi++] = av.value;
        }
    }

    if (argi != abi_nargs) {
        sema_error(s, n->loc, "internal argument lowering mismatch");
        return false;
    }

    IrValue *result = ir_build_call(s->m, s->cur, name, args, abi_nargs,
                                    fn->ret_type, ft->variadic);

    if (is_aggregate(ft->base)) {
        if (sret) {
            out->type = ft->base;
            out->addr = retbuf;
            out->value = ir_build_load(s->m, s->cur, retbuf,
                                       aggregate_buffer_type(s, ft->base,
                                                             false));
        } else {
            aggregate_from_scalar(s, ft->base, result, NULL, out);
        }
        out->is_null_const = false;
        return true;
    }

    out->type = ft->base;
    out->value = result;
    out->addr = NULL;
    out->is_null_const = false;
    return true;
}

static bool sema_short_circuit(EtccSema *s, EtccAstNode *n, SemVal *out,
                               bool is_and) {
    IrValue *slot = ir_build_alloca(s->m, s->fn, ir_type_i32(s->m));
    if (!slot) {
        sema_error(s, n->loc, "cannot allocate temporary");
        return false;
    }

    IrBlock *rhs = new_block(s, is_and ? "and.rhs" : "or.rhs");
    IrBlock *shortb = new_block(s, is_and ? "and.false" : "or.true");
    IrBlock *end = new_block(s, is_and ? "and.end" : "or.end");

    IrValue *lhs_cond;
    if (!sema_condition(s, n->u.binary.lhs, &lhs_cond)) return false;
    if (is_and) {
        ir_block_set_cond_br(s->m, s->cur, lhs_cond, rhs, shortb);
    } else {
        ir_block_set_cond_br(s->m, s->cur, lhs_cond, shortb, rhs);
    }
    s->cur = NULL;

    s->cur = shortb;
    IrValue *short_val = ir_const_int(s->m, ir_type_i32(s->m), is_and ? 0 : 1);
    ir_build_store(s->m, s->cur, slot, short_val);
    ir_block_set_br(s->m, s->cur, end);

    s->cur = rhs;
    IrValue *rhs_cond;
    if (!sema_condition(s, n->u.binary.rhs, &rhs_cond)) return false;
    ir_build_store(s->m, s->cur, slot, rhs_cond);
    ir_block_set_br(s->m, s->cur, end);

    s->cur = end;
    out->type = etcc_type_builtin(CT_INT);
    out->value = ir_build_load(s->m, s->cur, slot, ir_type_i32(s->m));
    out->addr = NULL;
    return true;
}

static bool sema_binary(EtccSema *s, EtccAstNode *n, SemVal *out) {
    EtccTokenKind op = n->u.binary.op;
    if (op == TK_AND_AND) return sema_short_circuit(s, n, out, true);
    if (op == TK_OR_OR) return sema_short_circuit(s, n, out, false);

    SemVal a, b;
    if (!sema_expr(s, n->u.binary.lhs, &a)) return false;
    if (!sema_expr(s, n->u.binary.rhs, &b)) return false;
    if (!decay(s, &a, n->loc)) return false;
    if (!decay(s, &b, n->loc)) return false;

    bool aptr = is_pointer(a.type);
    bool bptr = is_pointer(b.type);

    if ((op == TK_PLUS || op == TK_MINUS) && (aptr || bptr)) {
        if (op == TK_PLUS && aptr && !bptr) {
            if (!is_integer(b.type)) {
                sema_error(s, n->loc, "pointer addition requires an integer");
                return false;
            }
            IrValue *idx = convert_value(s, b.value, b.type, ctype_size_t(),
                                         n->loc, true);
            out->type = a.type;
            out->value = ir_build_gep(s->m, s->cur, a.value,
                                      map_type(s, a.type->base, n->loc), idx);
            out->addr = NULL;
            return true;
        }
        if (op == TK_PLUS && bptr && !aptr) {
            if (!is_integer(a.type)) {
                sema_error(s, n->loc, "pointer addition requires an integer");
                return false;
            }
            IrValue *idx = convert_value(s, a.value, a.type, ctype_size_t(),
                                         n->loc, true);
            out->type = b.type;
            out->value = ir_build_gep(s->m, s->cur, b.value,
                                      map_type(s, b.type->base, n->loc), idx);
            out->addr = NULL;
            return true;
        }
        if (op == TK_MINUS && aptr && bptr) {
            if (!etcc_type_equal(a.type->base, b.type->base))
                sema_warning(s, n->loc,
                             "pointer subtraction between different types");
            IrType *size_t_ir = ir_type_i64(s->m);
            IrValue *ai = ir_build_cast(s->m, s->cur, IR_CAST_PTRTOINT, a.value,
                                        size_t_ir);
            IrValue *bi = ir_build_cast(s->m, s->cur, IR_CAST_PTRTOINT, b.value,
                                        size_t_ir);
            IrValue *diff = ir_build_bin(s->m, s->cur, IR_BIN_SUB, ai, bi);
            size_t elem_size = etcc_type_sizeof(a.type->base);
            if (elem_size > 1) {
                IrValue *sz = ir_const_int(s->m, size_t_ir,
                                           (uint64_t)elem_size);
                diff = ir_build_bin(s->m, s->cur, IR_BIN_SDIV, diff, sz);
            }
            out->type = etcc_type_builtin(CT_LLONG);
            out->value = diff;
            out->addr = NULL;
            return true;
        }
        if (op == TK_MINUS && aptr && is_integer(b.type)) {
            IrValue *idx = convert_value(s, b.value, b.type, ctype_size_t(),
                                         n->loc, true);
            IrValue *zero = ir_const_int(s->m, ir_type_i64(s->m), 0);
            IrValue *neg = ir_build_bin(s->m, s->cur, IR_BIN_SUB, zero, idx);
            out->type = a.type;
            out->value = ir_build_gep(s->m, s->cur, a.value,
                                      map_type(s, a.type->base, n->loc), neg);
            out->addr = NULL;
            return true;
        }
        sema_error(s, n->loc, "invalid pointer arithmetic");
        return false;
    }

    if (op == TK_EQ || op == TK_NE || op == TK_LT || op == TK_LE ||
        op == TK_GT || op == TK_GE) {
        EtccCType *ct = NULL;
        bool unsigned_cmp;
        if (aptr || bptr) {
            if (aptr && bptr) {
                ct = a.type;
            } else if (aptr && is_integer(b.type) && b.value->kind ==
                                                    IR_VALUE_CONST_INT &&
                       b.value->as.i64 == 0) {
                b.value = ir_const_null(s->m, map_type(s, a.type, n->loc));
                b.type = a.type;
                ct = a.type;
            } else if (bptr && is_integer(a.type) && a.value->kind ==
                                                    IR_VALUE_CONST_INT &&
                       a.value->as.i64 == 0) {
                a.value = ir_const_null(s->m, map_type(s, b.type, n->loc));
                a.type = b.type;
                ct = b.type;
            } else {
                sema_error(s, n->loc,
                           "comparison between pointer and non-null integer");
                return false;
            }
            unsigned_cmp = true;
        } else {
            if (!is_integer(a.type) || !is_integer(b.type)) {
                sema_error(s, n->loc, "comparison requires scalar operands");
                return false;
            }
            ct = common_arithmetic(s, a.type, b.type);
            unsigned_cmp = ct->is_unsigned;
        }

        a.value = convert_value(s, a.value, a.type, ct, n->loc, false);
        b.value = convert_value(s, b.value, b.type, ct, n->loc, false);
        IrCmpOp cop = IR_CMP_EQ;
        switch (op) {
        case TK_EQ: cop = IR_CMP_EQ; break;
        case TK_NE: cop = IR_CMP_NE; break;
        case TK_LT: cop = unsigned_cmp ? IR_CMP_ULT : IR_CMP_SLT; break;
        case TK_LE: cop = unsigned_cmp ? IR_CMP_ULE : IR_CMP_SLE; break;
        case TK_GT: cop = unsigned_cmp ? IR_CMP_UGT : IR_CMP_SGT; break;
        case TK_GE: cop = unsigned_cmp ? IR_CMP_UGE : IR_CMP_SGE; break;
        default: break;
        }
        out->type = etcc_type_builtin(CT_INT);
        out->value = ir_build_cmp(s->m, s->cur, cop, a.value, b.value);
        out->addr = NULL;
        return true;
    }

    if (!is_integer(a.type) || !is_integer(b.type)) {
        sema_error(s, n->loc, "operator requires integer operands");
        return false;
    }

    if (op == TK_SHL || op == TK_SHR) {
        EtccCType *ct = integer_promote(s, a.type);
        a.value = convert_value(s, a.value, a.type, ct, n->loc, false);
        b.value = convert_value(s, b.value, b.type, ct, n->loc, false);
        IrBinOp bop = op == TK_SHL ? IR_BIN_SHL
                                   : (ct->is_unsigned ? IR_BIN_LSHR : IR_BIN_ASHR);
        out->type = ct;
        out->value = ir_build_bin(s->m, s->cur, bop, a.value, b.value);
        out->addr = NULL;
        return true;
    }

    EtccCType *ct = common_arithmetic(s, a.type, b.type);
    a.value = convert_value(s, a.value, a.type, ct, n->loc, false);
    b.value = convert_value(s, b.value, b.type, ct, n->loc, false);

    IrBinOp bop = IR_BIN_ADD;
    switch (op) {
    case TK_PLUS: bop = IR_BIN_ADD; break;
    case TK_MINUS: bop = IR_BIN_SUB; break;
    case TK_STAR: bop = IR_BIN_MUL; break;
    case TK_SLASH: bop = ct->is_unsigned ? IR_BIN_UDIV : IR_BIN_SDIV; break;
    case TK_PERCENT: bop = ct->is_unsigned ? IR_BIN_UREM : IR_BIN_SREM; break;
    case TK_AMP: bop = IR_BIN_AND; break;
    case TK_PIPE: bop = IR_BIN_OR; break;
    case TK_CARET: bop = IR_BIN_XOR; break;
    default:
        sema_error(s, n->loc, "unsupported binary operator");
        return false;
    }
    out->type = ct;
    out->value = ir_build_bin(s->m, s->cur, bop, a.value, b.value);
    out->addr = NULL;
    return true;
}

static bool sema_assign(EtccSema *s, EtccAstNode *n, SemVal *out) {
    SemVal target;
    if (!sema_lvalue(s, n->u.assign.target, &target)) return false;
    if (target.type->kind == CT_ARRAY || target.type->kind == CT_FUNC) {
        sema_error(s, n->loc, "cannot assign to this expression");
        return false;
    }

    EtccTokenKind op = n->u.assign.op;
    if (is_aggregate(target.type)) {
        if (op != TK_ASSIGN) {
            sema_error(s, n->loc,
                       "compound assignment is not valid for struct/union");
            return false;
        }
        SemVal rhs;
        if (!sema_expr(s, n->u.assign.value, &rhs)) return false;
        if (!decay(s, &rhs, n->loc)) return false;
        if (!etcc_type_equal(rhs.type, target.type)) {
            sema_error(s, n->loc, "incompatible struct/union assignment");
            return false;
        }
        ir_build_store(s->m, s->cur, target.addr, rhs.value);
        out->type = target.type;
        out->value = rhs.value;
        out->addr = NULL;
        return true;
    }
    if (op == TK_ASSIGN) {
        SemVal rhs;
        if (!sema_expr(s, n->u.assign.value, &rhs)) return false;
        if (!decay(s, &rhs, n->loc)) return false;
        rhs.value = convert_value(s, rhs.value, rhs.type, target.type, n->loc,
                                  false);
        if (!rhs.value) return false;
        ir_build_store(s->m, s->cur, target.addr, rhs.value);
        out->type = target.type;
        out->value = rhs.value;
        out->addr = NULL;
        return true;
    }

    SemVal old;
    old.type = target.type;
    old.addr = target.addr;
    old.value = target.addr;
    old.is_null_const = false;
    load_lvalue(s, &old);

    SemVal rhs;
    if (!sema_expr(s, n->u.assign.value, &rhs)) return false;
    if (!decay(s, &rhs, n->loc)) return false;

    if (is_pointer(target.type)) {
        if (op != TK_PLUS_ASSIGN && op != TK_MINUS_ASSIGN) {
            sema_error(s, n->loc, "invalid compound assignment to pointer");
            return false;
        }
        if (!is_integer(rhs.type)) {
            sema_error(s, n->loc, "pointer arithmetic requires an integer");
            return false;
        }
        IrValue *idx = convert_value(s, rhs.value, rhs.type, ctype_size_t(),
                                     n->loc, true);
        if (op == TK_MINUS_ASSIGN) {
            IrValue *zero = ir_const_int(s->m, ir_type_i64(s->m), 0);
            idx = ir_build_bin(s->m, s->cur, IR_BIN_SUB, zero, idx);
        }
        IrValue *newv = ir_build_gep(s->m, s->cur, old.value,
                                     map_type(s, target.type->base, n->loc), idx);
        ir_build_store(s->m, s->cur, target.addr, newv);
        out->type = target.type;
        out->value = newv;
        out->addr = NULL;
        return true;
    }

    if (!is_integer(target.type) || !is_integer(rhs.type)) {
        sema_error(s, n->loc, "compound assignment requires integer operands");
        return false;
    }
    EtccCType *ct;
    if (op == TK_SHL_ASSIGN || op == TK_SHR_ASSIGN) {
        /* Shift compound assignments use the promoted left operand type;
         * the right operand only supplies the shift count. */
        ct = integer_promote(s, target.type);
        old.value = convert_value(s, old.value, old.type, ct, n->loc, false);
        rhs.value = convert_value(s, rhs.value, rhs.type, ct, n->loc, false);
    } else {
        ct = common_arithmetic(s, target.type, rhs.type);
        old.value = convert_value(s, old.value, old.type, ct, n->loc, false);
        rhs.value = convert_value(s, rhs.value, rhs.type, ct, n->loc, false);
    }

    IrBinOp bop = IR_BIN_ADD;
    switch (op) {
    case TK_PLUS_ASSIGN: bop = IR_BIN_ADD; break;
    case TK_MINUS_ASSIGN: bop = IR_BIN_SUB; break;
    case TK_STAR_ASSIGN: bop = IR_BIN_MUL; break;
    case TK_SLASH_ASSIGN: bop = ct->is_unsigned ? IR_BIN_UDIV : IR_BIN_SDIV; break;
    case TK_PERCENT_ASSIGN: bop = ct->is_unsigned ? IR_BIN_UREM : IR_BIN_SREM; break;
    case TK_AMP_ASSIGN: bop = IR_BIN_AND; break;
    case TK_PIPE_ASSIGN: bop = IR_BIN_OR; break;
    case TK_CARET_ASSIGN: bop = IR_BIN_XOR; break;
    case TK_SHL_ASSIGN: bop = IR_BIN_SHL; break;
    case TK_SHR_ASSIGN: bop = ct->is_unsigned ? IR_BIN_LSHR : IR_BIN_ASHR; break;
    default:
        sema_error(s, n->loc, "unsupported compound assignment");
        return false;
    }
    IrValue *newv = ir_build_bin(s->m, s->cur, bop, old.value, rhs.value);
    newv = convert_value(s, newv, ct, target.type, n->loc, false);
    ir_build_store(s->m, s->cur, target.addr, newv);
    out->type = target.type;
    out->value = newv;
    out->addr = NULL;
    return true;
}

static bool sema_incdec(EtccSema *s, EtccAstNode *n, SemVal *out) {
    SemVal target;
    if (!sema_lvalue(s, n->u.unary.operand, &target)) return false;
    bool inc = n->u.unary.op == TK_PLUS_PLUS;
    if (is_pointer(target.type)) {
        IrValue *old = ir_build_load(s->m, s->cur, target.addr,
                                     map_type(s, target.type, n->loc));
        IrValue *one = ir_const_int(s->m, ir_type_i64(s->m), 1);
        if (!inc) {
            IrValue *zero = ir_const_int(s->m, ir_type_i64(s->m), 0);
            one = ir_build_bin(s->m, s->cur, IR_BIN_SUB, zero, one);
        }
        IrValue *nv = ir_build_gep(s->m, s->cur, old,
                                   map_type(s, target.type->base, n->loc), one);
        ir_build_store(s->m, s->cur, target.addr, nv);
        out->type = target.type;
        out->value = n->u.unary.postfix ? old : nv;
        out->addr = NULL;
        return true;
    }
    if (!is_integer(target.type)) {
        sema_error(s, n->loc, "increment/decrement requires a scalar operand");
        return false;
    }
    EtccCType *ct = integer_promote(s, target.type);
    IrValue *old = ir_build_load(s->m, s->cur, target.addr,
                                 map_type(s, target.type, n->loc));
    old = convert_value(s, old, target.type, ct, n->loc, false);
    IrValue *one = ir_const_int(s->m, map_type(s, ct, n->loc), 1);
    IrValue *nv = ir_build_bin(s->m, s->cur, inc ? IR_BIN_ADD : IR_BIN_SUB,
                               old, one);
    nv = convert_value(s, nv, ct, target.type, n->loc, false);
    ir_build_store(s->m, s->cur, target.addr, nv);
    out->type = target.type;
    out->value = nv;
    if (n->u.unary.postfix) {
        /* The old value was already converted to `ct`; convert it back. */
        out->value = convert_value(s, old, ct, target.type, n->loc, false);
    }
    out->addr = NULL;
    return true;
}

static bool sema_cond_expr(EtccSema *s, EtccAstNode *n, SemVal *out) {
    IrValue *cond;
    if (!sema_condition(s, n->u.cond.cond, &cond)) return false;

    EtccCType *tt = infer_type(s, n->u.cond.then_expr);
    EtccCType *et = infer_type(s, n->u.cond.else_expr);
    EtccCType *rt = conditional_type(s, tt, et);
    if (!rt) {
        sema_error(s, n->loc, "incompatible types in conditional expression");
        return false;
    }

    IrType *rt_ir = map_type(s, rt, n->loc);
    IrValue *slot = ir_build_alloca(s->m, s->fn, rt_ir);
    if (!slot) return false;

    IrBlock *thenb = new_block(s, "cond.then");
    IrBlock *elseb = new_block(s, "cond.else");
    IrBlock *end = new_block(s, "cond.end");
    ir_block_set_cond_br(s->m, s->cur, cond, thenb, elseb);
    s->cur = NULL;

    s->cur = thenb;
    SemVal tv;
    if (!sema_expr(s, n->u.cond.then_expr, &tv)) return false;
    if (!decay(s, &tv, n->loc)) return false;
    tv.value = convert_value(s, tv.value, tv.type, rt, n->loc, false);
    ir_build_store(s->m, s->cur, slot, tv.value);
    ir_block_set_br(s->m, s->cur, end);

    s->cur = elseb;
    SemVal ev;
    if (!sema_expr(s, n->u.cond.else_expr, &ev)) return false;
    if (!decay(s, &ev, n->loc)) return false;
    ev.value = convert_value(s, ev.value, ev.type, rt, n->loc, false);
    ir_build_store(s->m, s->cur, slot, ev.value);
    ir_block_set_br(s->m, s->cur, end);

    s->cur = end;
    out->type = rt;
    out->value = ir_build_load(s->m, s->cur, slot, rt_ir);
    out->addr = NULL;
    return true;
}

static bool sema_expr(EtccSema *s, EtccAstNode *n, SemVal *out) {
    memset(out, 0, sizeof(*out));
    if (!n) return false;
    if (!s->cur && n->kind != AST_INT_LIT && n->kind != AST_CHAR_LIT &&
        n->kind != AST_STRING_LIT) {
        sema_error(s, n->loc, "unreachable expression");
        return false;
    }

    switch (n->kind) {
    case AST_INT_LIT: {
        uint64_t v = n->u.int_lit.value;
        EtccCType *t = int_literal_type(n);
        out->type = t;
        out->value = const_int_typed(s, t, v);
        out->is_null_const = (v == 0);
        return true;
    }
    case AST_CHAR_LIT:
        out->type = etcc_type_builtin(CT_INT);
        out->value = const_int_typed(s, out->type,
                                     (uint64_t)n->u.char_lit.value);
        return true;
    case AST_STRING_LIT: {
        IrGlobal *g = ir_string_literal(s->m, n->u.string_lit.data,
                                        n->u.string_lit.len);
        EtccCType *arr = etcc_type_array(s->arena,
                                         etcc_type_builtin(CT_CHAR),
                                         (uint64_t)n->u.string_lit.len + 1);
        out->type = arr;
        out->value = ir_global_value(s->m, g);
        out->addr = out->value;
        return true;
    }
    case AST_IDENT: {
        char *name = etcc_arena_strndup(s->arena, n->u.ident.name,
                                        n->u.ident.len);
        EtccSymbol *sym = scope_lookup(s, name);
        if (!sym) {
            sema_error(s, n->loc, "use of undeclared identifier '%s'", name);
            return false;
        }
        if (sym->kind == SYM_FUNCTION) {
            out->type = sym->type;
            out->value = ir_function_value(s->m, sym->func);
            out->addr = NULL;
            return true;
        }
        if (sym->kind == SYM_ENUM_CONST) {
            out->type = etcc_type_builtin(CT_INT);
            out->value = const_int_typed(s, out->type,
                                         (uint64_t)sym->enum_value);
            out->is_null_const = (sym->enum_value == 0);
            out->addr = NULL;
            return true;
        }
        out->type = sym->type;
        out->addr = sym->addr;
        out->value = sym->addr;
        load_lvalue(s, out);
        if (!decay(s, out, n->loc)) return false;
        return true;
    }
    case AST_UNARY:
        if (n->u.unary.op == TK_AMP) {
            SemVal t;
            if (!sema_lvalue(s, n->u.unary.operand, &t)) return false;
            out->type = ptr_to(s, t.type);
            out->value = t.addr;
            return true;
        }
        if (n->u.unary.op == TK_STAR) {
            SemVal t;
            if (!sema_lvalue(s, n, &t)) return false;
            load_lvalue(s, &t);
            if (!decay(s, &t, n->loc)) return false;
            *out = t;
            return true;
        }
        if (n->u.unary.op == TK_PLUS_PLUS ||
            n->u.unary.op == TK_MINUS_MINUS)
            return sema_incdec(s, n, out);
        if (n->u.unary.op == TK_PLUS || n->u.unary.op == TK_MINUS ||
            n->u.unary.op == TK_TILDE || n->u.unary.op == TK_BANG) {
            SemVal v;
            if (!sema_expr(s, n->u.unary.operand, &v)) return false;
            if (!decay(s, &v, n->loc)) return false;
            if (n->u.unary.op == TK_BANG) {
                IrValue *zero = zero_of_type(s, v.value->type);
                out->type = etcc_type_builtin(CT_INT);
                out->value = ir_build_cmp(s->m, s->cur, IR_CMP_EQ, v.value,
                                           zero);
                return true;
            }
            if (!is_integer(v.type)) {
                sema_error(s, n->loc, "unary operator requires an integer");
                return false;
            }
            EtccCType *ct = integer_promote(s, v.type);
            v.value = convert_value(s, v.value, v.type, ct, n->loc, false);
            out->type = ct;
            if (n->u.unary.op == TK_PLUS) {
                out->value = v.value;
            } else if (n->u.unary.op == TK_MINUS) {
                IrValue *zero = ir_const_int(s->m, map_type(s, ct, n->loc), 0);
                out->value = ir_build_bin(s->m, s->cur, IR_BIN_SUB, zero,
                                           v.value);
            } else {
                uint64_t all = etcc_type_sizeof(ct) >= 8
                                   ? UINT64_MAX
                                   : ((1ull << (etcc_type_sizeof(ct) * 8)) - 1);
                IrValue *mask = ir_const_int(s->m, map_type(s, ct, n->loc), all);
                out->value = ir_build_bin(s->m, s->cur, IR_BIN_XOR, v.value,
                                           mask);
            }
            return true;
        }
        sema_error(s, n->loc, "unsupported unary operator");
        return false;
    case AST_BINARY:
        return sema_binary(s, n, out);
    case AST_ASSIGN:
        return sema_assign(s, n, out);
    case AST_COND:
        return sema_cond_expr(s, n, out);
    case AST_CALL:
        return sema_call(s, n, out);
    case AST_INDEX: {
        SemVal t;
        if (!sema_lvalue(s, n, &t)) return false;
        load_lvalue(s, &t);
        if (!decay(s, &t, n->loc)) return false;
        *out = t;
        return true;
    }
    case AST_CAST: {
        SemVal v;
        if (!sema_expr(s, n->u.cast.operand, &v)) return false;
        if (!decay(s, &v, n->loc)) return false;
        EtccCType *target = n->u.cast.target;
        if (target->kind == CT_ARRAY || target->kind == CT_FUNC ||
            is_aggregate(target)) {
            sema_error(s, n->loc, "cast target must be a scalar type");
            return false;
        }
        v.value = convert_value(s, v.value, v.type, target, n->loc, true);
        v.type = target;
        v.addr = NULL;
        *out = v;
        return true;
    }
    case AST_SIZEOF: {
        EtccCType *t = n->u.sizeof_.type;
        if (!t) t = infer_type(s, n->u.sizeof_.operand);
        if (!t) {
            sema_error(s, n->loc, "cannot determine the size of this expression");
            return false;
        }
        if (!type_is_complete_object(t)) {
            sema_error(s, n->loc, "sizeof applied to an incomplete type");
            return false;
        }
        out->type = ctype_size_t();
        out->value = const_int_typed(s, out->type, etcc_type_sizeof(t));
        return true;
    }
    case AST_COMMA: {
        SemVal a;
        if (!sema_expr(s, n->u.comma.lhs, &a)) return false;
        return sema_expr(s, n->u.comma.rhs, out);
    }
    case AST_MEMBER: {
        SemVal t;
        if (!sema_lvalue(s, n, &t)) return false;
        load_lvalue(s, &t);
        if (!decay(s, &t, n->loc)) return false;
        *out = t;
        return true;
    }
    default:
        sema_error(s, n->loc, "unsupported expression (%s)",
                   etcc_ast_kind_name(n->kind));
        return false;
    }
}

static EtccCType *infer_type(EtccSema *s, EtccAstNode *n) {
    if (!n) return NULL;
    switch (n->kind) {
    case AST_INT_LIT:
        return int_literal_type(n);
    case AST_CHAR_LIT: return etcc_type_builtin(CT_INT);
    case AST_STRING_LIT:
        return etcc_type_array(s->arena, etcc_type_builtin(CT_CHAR),
                               (uint64_t)n->u.string_lit.len + 1);
    case AST_IDENT: {
        char *name = etcc_arena_strndup(s->arena, n->u.ident.name,
                                        n->u.ident.len);
        EtccSymbol *sym = scope_lookup(s, name);
        return sym ? sym->type : NULL;
    }
    case AST_UNARY:
        if (n->u.unary.op == TK_AMP) {
            EtccCType *t = infer_type(s, n->u.unary.operand);
            return t ? ptr_to(s, t) : NULL;
        }
        if (n->u.unary.op == TK_STAR) {
            EtccCType *t = infer_type(s, n->u.unary.operand);
            return t && t->kind == CT_PTR ? t->base : NULL;
        }
        if (n->u.unary.op == TK_BANG) {
            EtccCType *t = infer_type(s, n->u.unary.operand);
            return (is_integer(t) || is_pointer(t))
                       ? etcc_type_builtin(CT_INT)
                       : NULL;
        }
        if (n->u.unary.op == TK_PLUS_PLUS ||
            n->u.unary.op == TK_MINUS_MINUS)
            return infer_type(s, n->u.unary.operand);
        {
            EtccCType *t = infer_type(s, n->u.unary.operand);
            return is_integer(t) ? integer_promote(s, t) : NULL;
        }
    case AST_BINARY:
        return binary_type(s, n->u.binary.op,
                           infer_type(s, n->u.binary.lhs),
                           infer_type(s, n->u.binary.rhs));
    case AST_ASSIGN:
        return infer_type(s, n->u.assign.target);
    case AST_COND:
        return conditional_type(s, infer_type(s, n->u.cond.then_expr),
                                infer_type(s, n->u.cond.else_expr));
    case AST_CAST:
        return n->u.cast.target;
    case AST_INDEX: {
        EtccCType *t = infer_type(s, n->u.index.base);
        if (t && t->kind == CT_PTR) return t->base;
        if (t && t->kind == CT_ARRAY) return t->base;
        return NULL;
    }
    case AST_CALL: {
        if (n->u.call.callee && n->u.call.callee->kind == AST_IDENT) {
            char *name = etcc_arena_strndup(s->arena,
                                            n->u.call.callee->u.ident.name,
                                            n->u.call.callee->u.ident.len);
            EtccSymbol *sym = scope_lookup(s, name);
            if (sym && sym->type && sym->type->kind == CT_FUNC)
                return sym->type->base;
        }
        return NULL;
    }
    case AST_SIZEOF:
        return ctype_size_t();
    case AST_MEMBER: {
        EtccCType *bt = infer_type(s, n->u.member.base);
        if (n->u.member.arrow) {
            if (!bt || bt->kind != CT_PTR) return NULL;
            bt = bt->base;
        }
        if (!is_aggregate(bt) || !bt->is_complete) return NULL;
        EtccField *f = etcc_type_find_field(bt, n->u.member.field,
                                            n->u.member.field_len);
        return f ? f->type : NULL;
    }
    case AST_COMMA:
        return infer_type(s, n->u.comma.rhs);
    default:
        return NULL;
    }
}

static bool sema_decl_list(EtccSema *s, EtccAstNode *decls, bool global);

static bool is_char_type(const EtccCType *t) {
    return t && t->kind == CT_CHAR;
}

static size_t count_init_items(const EtccAstNode *init) {
    size_t n = 0;
    for (const EtccAstNode *it = init ? init->u.init_list.items : NULL; it;
         it = it->next)
        n++;
    return n;
}

static bool type_is_complete_object(const EtccCType *t) {
    if (!t) return false;
    switch (t->kind) {
    case CT_VOID:
    case CT_FUNC:
        return false;
    case CT_ARRAY:
        return t->array_len > 0 && type_is_complete_object(t->base);
    case CT_STRUCT:
    case CT_UNION:
        return t->is_complete;
    default:
        return true;
    }
}

static EtccCType *complete_array_type(EtccSema *s, EtccCType *t,
                                      EtccAstNode *init, EtccLoc loc) {
    if (!t || t->kind != CT_ARRAY || t->array_len != 0) return t;

    uint64_t len = 0;
    if (init && init->kind == AST_STRING_LIT && is_char_type(t->base)) {
        len = (uint64_t)init->u.string_lit.len + 1;
    } else if (init && init->kind == AST_INIT_LIST) {
        len = (uint64_t)count_init_items(init);
    } else {
        sema_error(s, loc,
                   "array with unspecified size requires a brace or string initializer");
        len = 1;
    }
    if (len == 0) {
        sema_error(s, loc, "array initializer must not be empty");
        len = 1;
    }
    if (len > SIZE_MAX / (etcc_type_sizeof(t->base) ? etcc_type_sizeof(t->base) : 1)) {
        sema_error(s, loc, "array is too large");
        len = 1;
    }
    return etcc_type_array(s->arena, t->base, len);
}

static IrValue *zero_for_ctype(EtccSema *s, EtccCType *t, EtccLoc loc) {
    IrType *it = map_type(s, t, loc);
    if (is_pointer(t)) return ir_const_null(s->m, it);
    return ir_const_int(s->m, it, 0);
}

static bool zero_object_at(EtccSema *s, EtccCType *t, IrValue *base,
                           size_t offset, EtccLoc loc) {
    if (!t) return false;
    switch (t->kind) {
    case CT_ARRAY:
        for (uint64_t i = 0; i < t->array_len; i++) {
            if (!zero_object_at(s, t->base, base,
                                offset + (size_t)i * etcc_type_sizeof(t->base),
                                loc))
                return false;
        }
        return true;
    case CT_STRUCT:
        for (EtccField *f = t->fields; f; f = f->next) {
            if (!zero_object_at(s, f->type, base, offset + f->offset, loc))
                return false;
        }
        return true;
    case CT_UNION: {
        EtccField *f = t->fields;
        if (!f) return false;
        return zero_object_at(s, f->type, base, offset, loc);
    }
    default: {
        IrValue *p = byte_offset_ptr(s, base, offset);
        if (!p) return false;
        ir_build_store(s->m, s->cur, p, zero_for_ctype(s, t, loc));
        return true;
    }
    }
}

static bool initialize_local_object(EtccSema *s, EtccCType *t, IrValue *base,
                                    EtccAstNode *init, size_t offset,
                                    EtccLoc loc) {
    if (!init) return true;

    if (t->kind == CT_ARRAY) {
        if (init->kind == AST_STRING_LIT) {
            if (!is_char_type(t->base)) {
                sema_error(s, loc, "string initializer requires a char array");
                return false;
            }
            if ((uint64_t)init->u.string_lit.len > t->array_len) {
                sema_error(s, loc, "string literal is too long for the array");
                return false;
            }
            if (!zero_object_at(s, t, base, offset, loc)) return false;
            for (size_t i = 0; i < init->u.string_lit.len; i++) {
                IrValue *p = byte_offset_ptr(s, base, offset + i);
                if (!p) return false;
                IrValue *ch = ir_const_int(s->m, ir_type_i8(s->m),
                                           (unsigned char)init->u.string_lit.data[i]);
                ir_build_store(s->m, s->cur, p, ch);
            }
            return true;
        }
        if (init->kind != AST_INIT_LIST) {
            sema_error(s, loc, "array initializer must be a brace list or string");
            return false;
        }
        if (!zero_object_at(s, t, base, offset, loc)) return false;
        size_t i = 0;
        EtccAstNode *it = init->u.init_list.items;
        for (; it && i < t->array_len; it = it->next, i++) {
            if (!initialize_local_object(s, t->base, base, it,
                                         offset + i * etcc_type_sizeof(t->base),
                                         loc))
                return false;
        }
        if (it)
            sema_error(s, loc, "too many elements in array initializer");
        return true;
    }

    if (is_aggregate(t)) {
        if (!t->is_complete) {
            sema_error(s, loc, "initializer for an incomplete type");
            return false;
        }
        if (init->kind == AST_INIT_LIST) {
            if (!zero_object_at(s, t, base, offset, loc)) return false;
            EtccField *f = t->fields;
            EtccAstNode *it = init->u.init_list.items;
            for (; f && it; f = f->next, it = it->next) {
                if (!initialize_local_object(s, f->type, base, it,
                                             offset + f->offset, loc))
                    return false;
                if (t->kind == CT_UNION) {
                    it = it->next;
                    break;
                }
            }
            if (it)
                sema_error(s, loc, "too many elements in aggregate initializer");
            return true;
        }
        SemVal v;
        if (!sema_expr(s, init, &v)) return false;
        if (!decay(s, &v, loc)) return false;
        if (!etcc_type_equal(v.type, t)) {
            sema_error(s, loc, "incompatible initializer type");
            return false;
        }
        IrValue *p = byte_offset_ptr(s, base, offset);
        if (!p) return false;
        ir_build_store(s->m, s->cur, p, v.value);
        return true;
    }

    if (init->kind == AST_INIT_LIST) {
        EtccAstNode *only = init->u.init_list.items;
        if (!only || only->next) {
            sema_error(s, loc, "scalar initializer must contain exactly one value");
            return false;
        }
        return initialize_local_object(s, t, base, only, offset, loc);
    }

    SemVal v;
    if (!sema_expr(s, init, &v)) return false;
    if (!decay(s, &v, loc)) return false;
    v.value = convert_value(s, v.value, v.type, t, loc, false);
    if (!v.value) return false;
    IrValue *p = byte_offset_ptr(s, base, offset);
    if (!p) return false;
    ir_build_store(s->m, s->cur, p, v.value);
    return true;
}

static bool sema_enum_const(EtccSema *s, EtccAstNode *n) {
    if (!n->u.enum_const.name) return false;
    if (scope_find_local(s->scope, n->u.enum_const.name)) {
        sema_error(s, n->loc, "redefinition of '%s'", n->u.enum_const.name);
        return false;
    }
    EtccSymbol *sym = symbol_add(s, s->scope, n->u.enum_const.name,
                                 etcc_type_builtin(CT_INT), SYM_ENUM_CONST,
                                 n->loc);
    sym->enum_value = n->u.enum_const.value;
    return true;
}

static bool sema_switch_stmt(EtccSema *s, EtccAstNode *n) {
    SemVal v;
    if (!sema_expr(s, n->u.switch_.cond, &v)) return false;
    if (!decay(s, &v, n->loc)) return false;
    if (!is_integer(v.type)) {
        sema_error(s, n->loc, "switch expression must have integer type");
        return false;
    }
    EtccCType *ct = integer_promote(s, v.type);
    v.value = convert_value(s, v.value, v.type, ct, n->loc, false);
    v.type = ct;

    if (s->switch_depth >= (int)(sizeof(s->switches) / sizeof(s->switches[0]))) {
        sema_error(s, n->loc, "switch nesting is too deep");
        return false;
    }

    EtccSwitch *sw = &s->switches[s->switch_depth];
    memset(sw, 0, sizeof(*sw));
    sw->node = n;
    sw->end = new_block(s, "switch.end");

    if (!collect_switch_cases(s, n->u.switch_.body, sw)) {
        free(sw->cases.items);
        sw->cases.items = NULL;
        return false;
    }

    for (size_t i = 0; i < sw->cases.len; i++) {
        EtccCase *c = &sw->cases.items[i];
        IrValue *case_value = ir_const_int(s->m, v.value->type,
                                           (uint64_t)c->value);
        IrValue *match = ir_build_cmp(s->m, s->cur, IR_CMP_EQ, v.value,
                                      case_value);
        IrBlock *next = new_block(s, "switch.next");
        ir_block_set_cond_br(s->m, s->cur, match, c->block, next);
        s->cur = next;
    }
    IrBlock *fallback = sw->default_block ? sw->default_block : sw->end;
    ir_block_set_br(s->m, s->cur, fallback);
    s->cur = NULL;

    IrBlock *body_start = new_block(s, "switch.body");
    s->cur = body_start;
    s->switch_depth++;

    if (s->break_depth >= (int)(sizeof(s->breaks) / sizeof(s->breaks[0]))) {
        sema_error(s, n->loc, "break nesting is too deep");
        s->switch_depth--;
        free(sw->cases.items);
        return false;
    }
    s->breaks[s->break_depth++] = sw->end;

    bool ok = sema_stmt(s, n->u.switch_.body);
    s->break_depth--;
    s->switch_depth--;

    if (block_open(s)) emit_br(s, sw->end);
    s->cur = sw->end;
    free(sw->cases.items);
    sw->cases.items = NULL;
    return ok;
}

static bool sema_local_decl(EtccSema *s, EtccAstNode *n) {
    if (n->u.decl.storage == STORAGE_STATIC) {
        sema_error(s, n->loc,
                   "static local variables are not supported yet");
    }
    if (n->u.decl.storage == STORAGE_EXTERN) {
        EtccSymbol *g = scope_find_local(&s->global_scope, n->u.decl.name);
        if (!g) {
            sema_error(s, n->loc, "extern declaration of '%s' has no definition",
                       n->u.decl.name);
            return false;
        }
        EtccSymbol *sym = symbol_add(s, s->scope, n->u.decl.name, g->type,
                                     SYM_GLOBAL_VAR, n->loc);
        sym->global = g->global;
        sym->addr = g->addr;
        return true;
    }

    EtccCType *t = complete_array_type(s, n->u.decl.type, n->u.decl.init,
                                       n->loc);
    n->u.decl.type = t;
    if (!type_is_complete_object(t) || t->kind == CT_FUNC) {
        sema_error(s, n->loc, "variable '%s' has an incomplete type",
                   n->u.decl.name);
        return false;
    }
    IrType *it = map_type(s, t, n->loc);
    IrValue *slot = ir_build_alloca(s->m, s->fn, it);
    if (!slot) {
        sema_error(s, n->loc, "cannot allocate local variable");
        return false;
    }
    EtccSymbol *sym = symbol_add(s, s->scope, n->u.decl.name, t, SYM_LOCAL_VAR,
                                 n->loc);
    sym->addr = slot;

    if (n->u.decl.init) {
        if (!initialize_local_object(s, t, slot, n->u.decl.init, 0, n->loc))
            return false;
    }
    return true;
}

static bool sema_decl_list(EtccSema *s, EtccAstNode *decls, bool global) {
    (void)global;
    for (EtccAstNode *n = decls;
         n && (n->kind == AST_DECL || n->kind == AST_ENUM_CONST);
         n = n->next) {
        if (n->kind == AST_ENUM_CONST) {
            if (!sema_enum_const(s, n)) return false;
        } else if (!sema_local_decl(s, n)) {
            return false;
        }
    }
    return true;
}

static bool sema_for_stmt(EtccSema *s, EtccAstNode *n) {
    if (n->u.for_.init) {
        if (n->u.for_.init->kind == AST_DECL ||
            n->u.for_.init->kind == AST_ENUM_CONST) {
            if (!sema_decl_list(s, n->u.for_.init, false)) return false;
        } else {
            SemVal v;
            if (!sema_expr(s, n->u.for_.init->u.expr_stmt.expr, &v))
                return false;
        }
    }
    IrBlock *condb = new_block(s, "for.cond");
    IrBlock *bodyb = new_block(s, "for.body");
    IrBlock *stepb = new_block(s, "for.step");
    IrBlock *end = new_block(s, "for.end");
    emit_br(s, condb);

    s->cur = condb;
    IrValue *cond;
    if (n->u.for_.cond) {
        if (!sema_condition(s, n->u.for_.cond, &cond)) return false;
    } else {
        cond = ir_const_int(s->m, ir_type_i32(s->m), 1);
    }
    ir_block_set_cond_br(s->m, s->cur, cond, bodyb, end);
    s->cur = NULL;

    if (s->loop_depth >= (int)(sizeof(s->loops) / sizeof(s->loops[0]))) {
        sema_error(s, n->loc, "loop nesting is too deep");
        return false;
    }
    s->loops[s->loop_depth++] = (EtccLoop){end, stepb};
    if (s->break_depth >= (int)(sizeof(s->breaks) / sizeof(s->breaks[0]))) {
        sema_error(s, n->loc, "break nesting is too deep");
        return false;
    }
    s->breaks[s->break_depth++] = end;

    s->cur = bodyb;
    if (!sema_stmt(s, n->u.for_.body)) return false;
    if (block_open(s)) emit_br(s, stepb);

    s->cur = stepb;
    if (n->u.for_.step) {
        SemVal v;
        if (!sema_expr(s, n->u.for_.step, &v)) return false;
    }
    if (block_open(s)) emit_br(s, condb);
    s->break_depth--;
    s->loop_depth--;
    s->cur = end;
    return true;
}

static bool sema_stmt(EtccSema *s, EtccAstNode *n) {
    if (!n) return true;

    if (n->kind == AST_LABEL) {
        EtccLabel *label = label_find(s, n->u.label.name, n->u.label.len);
        if (!label) {
            sema_error(s, n->loc, "unknown label");
            return false;
        }
        if (block_open(s)) emit_br(s, label->block);
        s->cur = label->block;
        return sema_stmt(s, n->u.label.body);
    }

    if (n->kind == AST_CASE) {
        if (s->switch_depth == 0) {
            sema_error(s, n->loc, "'case' outside of a switch");
            return false;
        }
        EtccSwitch *sw = &s->switches[s->switch_depth - 1];
        EtccCase *c = switch_find_case(sw, n);
        if (!c) {
            sema_error(s, n->loc, "'case' label was not collected");
            return false;
        }
        if (block_open(s)) emit_br(s, c->block);
        s->cur = c->block;
        return sema_stmt(s, n->u.case_.body);
    }

    if (n->kind == AST_DEFAULT) {
        if (s->switch_depth == 0) {
            sema_error(s, n->loc, "'default' outside of a switch");
            return false;
        }
        EtccSwitch *sw = &s->switches[s->switch_depth - 1];
        if (!sw->default_block) {
            sema_error(s, n->loc, "'default' label was not collected");
            return false;
        }
        if (block_open(s)) emit_br(s, sw->default_block);
        s->cur = sw->default_block;
        return sema_stmt(s, n->u.default_.body);
    }

    if (!block_open(s)) {
        /* Keep lowering syntactically unreachable code so that labels and
         * case labels inside it still get valid blocks (goto may target them).
         * The block has no predecessor and is therefore dead. */
        s->cur = new_block(s, "dead");
    }

    switch (n->kind) {
    case AST_COMPOUND: {
        scope_push(s);
        for (EtccAstNode *st = n->u.compound.stmts; st; st = st->next) {
            if (!sema_stmt(s, st)) {
                scope_pop(s);
                return false;
            }
        }
        scope_pop(s);
        return true;
    }
    case AST_ENUM_CONST:
        return sema_enum_const(s, n);
    case AST_DECL:
        return sema_local_decl(s, n);
    case AST_EXPR_STMT:
        if (!n->u.expr_stmt.expr) return true;
        {
            SemVal v;
            return sema_expr(s, n->u.expr_stmt.expr, &v);
        }
    case AST_EMPTY:
        return true;
    case AST_RETURN: {
        SemVal v;
        bool has = n->u.ret.expr != NULL;
        if (is_void(s->fn_ret)) {
            if (has) {
                sema_error(s, n->loc, "void function cannot return a value");
                if (!sema_expr(s, n->u.ret.expr, &v)) return false;
            }
            if (block_open(s)) ir_block_set_ret(s->m, s->cur, NULL);
            s->cur = NULL;
            return true;
        }
        if (!has) {
            sema_error(s, n->loc, "non-void function must return a value");
            return false;
        }
        if (!sema_expr(s, n->u.ret.expr, &v)) return false;
        if (!decay(s, &v, n->loc)) return false;
        if (is_aggregate(s->fn_ret)) {
            if (!is_aggregate(v.type) || !etcc_type_equal(v.type, s->fn_ret)) {
                sema_error(s, n->loc,
                           "returning an incompatible struct/union type");
                return false;
            }
            if (s->sret) {
                if (block_open(s)) {
                    ir_build_store(s->m, s->cur, s->sret, v.value);
                    ir_block_set_ret(s->m, s->cur, NULL);
                }
            } else {
                IrValue *scalar = aggregate_to_scalar(s, &v, s->fn_ret, n->loc);
                if (block_open(s)) ir_block_set_ret(s->m, s->cur, scalar);
            }
            s->cur = NULL;
            return true;
        }
        if (is_aggregate(v.type)) {
            sema_error(s, n->loc,
                       "cannot return struct/union from a scalar function");
            return false;
        }
        v.value = convert_value(s, v.value, v.type, s->fn_ret, n->loc, false);
        if (!v.value) return false;
        if (block_open(s)) ir_block_set_ret(s->m, s->cur, v.value);
        s->cur = NULL;
        return true;
    }
    case AST_SWITCH:
        return sema_switch_stmt(s, n);
    case AST_GOTO: {
        EtccLabel *label = label_find(s, n->u.goto_.name, n->u.goto_.len);
        if (!label) {
            sema_error(s, n->loc, "goto to undefined label '%.*s'",
                       (int)n->u.goto_.len, n->u.goto_.name);
            return false;
        }
        emit_br(s, label->block);
        return true;
    }
    case AST_IF: {
        IrValue *cond;
        if (!sema_condition(s, n->u.if_.cond, &cond)) return false;
        IrBlock *thenb = new_block(s, "if.then");
        IrBlock *elseb = new_block(s, "if.else");
        IrBlock *end = new_block(s, "if.end");
        ir_block_set_cond_br(s->m, s->cur, cond, thenb, elseb);
        s->cur = NULL;

        s->cur = thenb;
        if (!sema_stmt(s, n->u.if_.then_branch)) return false;
        if (block_open(s)) emit_br(s, end);

        s->cur = elseb;
        if (n->u.if_.else_branch) {
            if (!sema_stmt(s, n->u.if_.else_branch)) return false;
        }
        if (block_open(s)) emit_br(s, end);

        s->cur = end;
        return true;
    }
    case AST_WHILE: {
        IrBlock *condb = new_block(s, "while.cond");
        IrBlock *bodyb = new_block(s, "while.body");
        IrBlock *end = new_block(s, "while.end");
        emit_br(s, condb);

        s->cur = condb;
        IrValue *cond;
        if (!sema_condition(s, n->u.while_.cond, &cond)) return false;
        ir_block_set_cond_br(s->m, s->cur, cond, bodyb, end);
        s->cur = NULL;

        if (s->loop_depth >= (int)(sizeof(s->loops) / sizeof(s->loops[0]))) {
            sema_error(s, n->loc, "loop nesting is too deep");
            return false;
        }
        s->loops[s->loop_depth++] = (EtccLoop){end, condb};
        if (s->break_depth >= (int)(sizeof(s->breaks) / sizeof(s->breaks[0]))) {
            sema_error(s, n->loc, "break nesting is too deep");
            return false;
        }
        s->breaks[s->break_depth++] = end;
        s->cur = bodyb;
        if (!sema_stmt(s, n->u.while_.body)) return false;
        if (block_open(s)) emit_br(s, condb);
        s->break_depth--;
        s->loop_depth--;
        s->cur = end;
        return true;
    }
    case AST_DO: {
        IrBlock *bodyb = new_block(s, "do.body");
        IrBlock *condb = new_block(s, "do.cond");
        IrBlock *end = new_block(s, "do.end");
        emit_br(s, bodyb);

        if (s->loop_depth >= (int)(sizeof(s->loops) / sizeof(s->loops[0]))) {
            sema_error(s, n->loc, "loop nesting is too deep");
            return false;
        }
        s->loops[s->loop_depth++] = (EtccLoop){end, condb};
        if (s->break_depth >= (int)(sizeof(s->breaks) / sizeof(s->breaks[0]))) {
            sema_error(s, n->loc, "break nesting is too deep");
            return false;
        }
        s->breaks[s->break_depth++] = end;

        s->cur = bodyb;
        if (!sema_stmt(s, n->u.do_.body)) return false;
        if (block_open(s)) emit_br(s, condb);
        s->break_depth--;
        s->loop_depth--;

        s->cur = condb;
        IrValue *cond;
        if (!sema_condition(s, n->u.do_.cond, &cond)) return false;
        ir_block_set_cond_br(s->m, s->cur, cond, bodyb, end);
        s->cur = end;
        return true;
    }
    case AST_FOR: {
        scope_push(s);
        bool ok = sema_for_stmt(s, n);
        scope_pop(s);
        return ok;
    }
    case AST_BREAK:
        if (s->break_depth == 0) {
            sema_error(s, n->loc, "'break' outside of a loop or switch");
            return false;
        }
        emit_br(s, s->breaks[s->break_depth - 1]);
        return true;
    case AST_CONTINUE:
        if (s->loop_depth == 0) {
            sema_error(s, n->loc, "'continue' outside of a loop");
            return false;
        }
        emit_br(s, s->loops[s->loop_depth - 1].continue_block);
        return true;
    default:
        sema_error(s, n->loc, "unsupported statement (%s)",
                   etcc_ast_kind_name(n->kind));
        return false;
    }
}

static int64_t const_ashr(int64_t v, int64_t count) {
    if (count < 0 || count >= 64) return v < 0 ? -1 : 0;
    if (v >= 0) return v >> count;
    return (int64_t)(~((~((uint64_t)v)) >> (unsigned)count));
}

static bool const_eval(EtccSema *s, EtccAstNode *n, int64_t *out) {
    if (!n) return false;
    switch (n->kind) {
    case AST_INT_LIT:
        *out = (int64_t)n->u.int_lit.value;
        return true;
    case AST_CHAR_LIT:
        *out = n->u.char_lit.value;
        return true;
    case AST_IDENT: {
        char *name = etcc_arena_strndup(s->arena, n->u.ident.name,
                                        n->u.ident.len);
        EtccSymbol *sym = scope_lookup(s, name);
        if (sym && sym->kind == SYM_ENUM_CONST) {
            *out = sym->enum_value;
            return true;
        }
        return false;
    }
    case AST_UNARY: {
        int64_t v;
        if (!const_eval(s, n->u.unary.operand, &v)) return false;
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
        if (!const_eval(s, n->u.binary.lhs, &a)) return false;
        if (n->u.binary.op == TK_AND_AND) {
            if (!a) { *out = 0; return true; }
            if (!const_eval(s, n->u.binary.rhs, &b)) return false;
            *out = b != 0;
            return true;
        }
        if (n->u.binary.op == TK_OR_OR) {
            if (a) { *out = 1; return true; }
            if (!const_eval(s, n->u.binary.rhs, &b)) return false;
            *out = b != 0;
            return true;
        }
        if (!const_eval(s, n->u.binary.rhs, &b)) return false;
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
            if (!b) {
                sema_error(s, n->loc, "division by zero");
                return false;
            }
            if (a == INT64_MIN && b == -1) {
                sema_error(s, n->loc,
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
            *out = const_ashr(a, b);
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
        if (!const_eval(s, n->u.cond.cond, &c)) return false;
        return const_eval(s, c ? n->u.cond.then_expr : n->u.cond.else_expr,
                          out);
    }
    case AST_CAST: {
        int64_t v;
        if (!const_eval(s, n->u.cast.operand, &v)) return false;
        EtccCType *t = n->u.cast.target;
        if (t && t->kind == CT_BOOL) {
            *out = v != 0;
            return true;
        }
        if (t && is_integer(t) && t->size < 8) {
            uint64_t mask = (1ull << (t->size * 8)) - 1;
            uint64_t u = (uint64_t)v & mask;
            if (!t->is_unsigned && (u & (1ull << (t->size * 8 - 1)))) u |= ~mask;
            *out = (int64_t)u;
        } else {
            *out = v;
        }
        return true;
    }
    case AST_SIZEOF:
        if (n->u.sizeof_.type) {
            *out = (int64_t)etcc_type_sizeof(n->u.sizeof_.type);
            return true;
        }
        return false;
    case AST_COMMA:
        return const_eval(s, n->u.comma.rhs, out);
    default:
        return false;
    }
}

static bool ir_type_same(const IrType *a, const IrType *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    if (a->kind == IR_TYPE_PTR)
        return ir_type_same(a->elem, b->elem);
    if (a->kind == IR_TYPE_ARRAY)
        return a->count == b->count && ir_type_same(a->elem, b->elem);
    return a->size == b->size && a->is_signed == b->is_signed;
}

static IrFunction *ensure_function(EtccSema *s, const char *name,
                                   EtccCType *ft, EtccLoc loc) {
    bool sret = abi_return_uses_sret(ft->base);
    size_t base = sret ? 1 : 0;
    size_t abi_nparams = ft->nparams + base;
    IrType *ret = abi_return_type(s, ft->base, loc);

    IrFunction *f = ir_function_find(s->m, name);
    if (f) {
        if (f->nparams != abi_nparams || f->variadic != ft->variadic ||
            f->has_sret != sret || !ir_type_same(f->ret_type, ret)) {
            sema_error(s, loc, "conflicting declaration of function '%s'", name);
            return f;
        }
        if (sret) {
            IrType *want = ir_type_ptr(s->m, map_type(s, ft->base, loc));
            if (!ir_type_same(f->param_types[0], want)) {
                sema_error(s, loc, "conflicting declaration of function '%s'",
                           name);
                return f;
            }
        }
        for (size_t i = 0; i < ft->nparams; i++) {
            IrType *want = abi_param_type(s, ft->params[i], loc);
            if (!ir_type_same(f->param_types[base + i], want)) {
                sema_error(s, loc, "conflicting declaration of function '%s'",
                           name);
                return f;
            }
        }
        return f;
    }

    IrType **pts = NULL;
    if (abi_nparams) {
        pts = etcc_arena_alloc(s->arena, abi_nparams * sizeof(*pts),
                               _Alignof(IrType *));
        if (sret) pts[0] = ir_type_ptr(s->m, map_type(s, ft->base, loc));
        for (size_t i = 0; i < ft->nparams; i++)
            pts[base + i] = abi_param_type(s, ft->params[i], loc);
    }
    f = ir_function_create(s->m, name, ret, pts, abi_nparams, ft->variadic);
    f->has_sret = sret;
    return f;
}

static bool collect_function(EtccSema *s, const char *name, EtccCType *ft,
                             EtccStorageClass storage, bool definition,
                             EtccLoc loc) {
    if (!name) return false;

    EtccSymbol *sym = scope_find_local(&s->global_scope, name);
    if (sym && sym->kind != SYM_FUNCTION) {
        sema_error(s, loc, "'%s' redeclared as a function", name);
        return false;
    }
    if (!sym) {
        sym = symbol_add(s, &s->global_scope, name, ft, SYM_FUNCTION, loc);
    } else {
        if (!etcc_type_equal(sym->type, ft))
            sema_error(s, loc, "conflicting declaration of function '%s'", name);
    }

    IrFunction *f = ensure_function(s, name, ft, loc);
    sym->func = f;
    sym->type = ft;
    if (definition) {
        if (sym->is_defined) {
            sema_error(s, loc, "redefinition of function '%s'", name);
            return false;
        }
        sym->is_defined = true;
        if (storage == STORAGE_STATIC) {
            sym->is_static = true;
            f->is_internal = true;
        }
    }
    return true;
}

static void write_uint_le(unsigned char *buf, size_t offset, uint64_t value,
                          size_t size) {
    for (size_t i = 0; i < size; i++)
        buf[offset + i] = (unsigned char)((value >> (i * 8)) & 0xFF);
}

static bool fill_global_object(EtccSema *s, EtccCType *t, EtccAstNode *init,
                               unsigned char *buf, size_t offset,
                               EtccLoc loc) {
    if (!init) return true;

    if (t->kind == CT_ARRAY) {
        if (init->kind == AST_STRING_LIT) {
            if (!is_char_type(t->base)) {
                sema_error(s, loc, "string initializer requires a char array");
                return false;
            }
            if ((uint64_t)init->u.string_lit.len > t->array_len) {
                sema_error(s, loc, "string literal is too long for the array");
                return false;
            }
            for (size_t i = 0; i < init->u.string_lit.len; i++)
                buf[offset + i] = (unsigned char)init->u.string_lit.data[i];
            return true;
        }
        if (init->kind != AST_INIT_LIST) {
            sema_error(s, loc, "array initializer must be a brace list or string");
            return false;
        }
        size_t i = 0;
        EtccAstNode *it = init->u.init_list.items;
        for (; it && i < t->array_len; it = it->next, i++) {
            if (!fill_global_object(s, t->base, it, buf,
                                    offset + i * etcc_type_sizeof(t->base), loc))
                return false;
        }
        if (it) sema_error(s, loc, "too many elements in array initializer");
        return true;
    }

    if (is_aggregate(t)) {
        if (!t->is_complete) {
            sema_error(s, loc, "initializer for an incomplete type");
            return false;
        }
        if (init->kind != AST_INIT_LIST) {
            sema_error(s, loc,
                       "global aggregate initializer must be a brace list");
            return false;
        }
        EtccField *f = t->fields;
        EtccAstNode *it = init->u.init_list.items;
        for (; f && it; f = f->next, it = it->next) {
            if (!fill_global_object(s, f->type, it, buf, offset + f->offset, loc))
                return false;
            if (t->kind == CT_UNION) {
                it = it->next;
                break;
            }
        }
        if (it) sema_error(s, loc, "too many elements in aggregate initializer");
        return true;
    }

    if (init->kind == AST_INIT_LIST) {
        EtccAstNode *only = init->u.init_list.items;
        if (!only || only->next) {
            sema_error(s, loc, "scalar initializer must contain exactly one value");
            return false;
        }
        return fill_global_object(s, t, only, buf, offset, loc);
    }

    if (is_pointer(t)) {
        if (init->kind == AST_STRING_LIT) {
            sema_error(s, loc,
                       "pointer relocations in aggregate globals are not supported yet");
            return false;
        }
        int64_t v;
        if (!const_eval(s, init, &v)) {
            sema_error(s, loc, "global initializer is not a constant expression");
            return false;
        }
        if (v != 0) {
            sema_error(s, loc,
                       "non-null pointer relocations are not supported in aggregate globals yet");
            return false;
        }
        write_uint_le(buf, offset, 0, 8);
        return true;
    }

    if (!is_integer(t)) {
        sema_error(s, loc, "unsupported global initializer type");
        return false;
    }
    int64_t v;
    if (!const_eval(s, init, &v)) {
        sema_error(s, loc, "global initializer is not a constant expression");
        return false;
    }
    write_uint_le(buf, offset, (uint64_t)v, etcc_type_sizeof(t));
    return true;
}

static bool collect_global_var(EtccSema *s, EtccAstNode *n) {
    const char *name = n->u.decl.name;
    EtccCType *t = complete_array_type(s, n->u.decl.type, n->u.decl.init,
                                       n->loc);
    n->u.decl.type = t;
    EtccLoc loc = n->loc;
    if (!name) return false;

    if (!type_is_complete_object(t)) {
        sema_error(s, loc, "variable '%s' has an incomplete type", name);
        return false;
    }

    EtccSymbol *sym = scope_find_local(&s->global_scope, name);
    if (sym && sym->kind == SYM_FUNCTION) {
        sema_error(s, loc, "'%s' redeclared as a variable", name);
        return false;
    }
    if (sym) {
        if (!etcc_type_equal(sym->type, t))
            sema_error(s, loc, "conflicting declaration of '%s'", name);
        if (n->u.decl.is_definition && sym->is_defined)
            sema_error(s, loc, "redefinition of '%s'", name);
    } else {
        sym = symbol_add(s, &s->global_scope, name, t, SYM_GLOBAL_VAR, loc);
        IrType *gt = map_type(s, t, loc);
        IrGlobal *g = ir_global_create(s->m, name, gt, false,
                                       n->u.decl.storage == STORAGE_STATIC);
        sym->global = g;
        sym->addr = ir_global_value(s->m, g);
    }

    if (n->u.decl.is_definition) {
        sym->is_defined = true;
        sym->global->is_external = false;
    } else if (!sym->is_defined) {
        sym->global->is_external = true;
    }
    if (n->u.decl.storage == STORAGE_STATIC) sym->global->is_internal = true;
    return true;
}

static bool collect_globals(EtccSema *s, EtccAstNode *tu) {
    for (EtccAstNode *n = tu->u.tu.decls; n; n = n->next) {
        if (n->kind == AST_FUNC_DEF) {
            if (!collect_function(s, n->u.func.name, n->u.func.type,
                                  n->u.func.storage, true, n->loc))
                return false;
        } else if (n->kind == AST_ENUM_CONST) {
            if (!sema_enum_const(s, n)) return false;
        } else if (n->kind == AST_DECL) {
            if (n->u.decl.type && n->u.decl.type->kind == CT_FUNC) {
                if (!collect_function(s, n->u.decl.name, n->u.decl.type,
                                      n->u.decl.storage, false, n->loc))
                    return false;
            } else {
                if (!collect_global_var(s, n)) return false;
            }
        }
    }
    return true;
}

static bool global_initializers(EtccSema *s, EtccAstNode *tu) {
    for (EtccAstNode *n = tu->u.tu.decls; n; n = n->next) {
        if (n->kind != AST_DECL || !n->u.decl.init) continue;
        if (n->u.decl.type->kind == CT_FUNC) continue;
        EtccSymbol *sym = scope_find_local(&s->global_scope, n->u.decl.name);
        if (!sym || sym->kind != SYM_GLOBAL_VAR) continue;
        IrGlobal *g = sym->global;
        EtccCType *t = sym->type;
        g->is_external = false;

        if (t->kind == CT_PTR) {
            EtccAstNode *init = n->u.decl.init;
            if (init->kind == AST_INIT_LIST) {
                EtccAstNode *only = init->u.init_list.items;
                if (only && !only->next) init = only;
            }
            if (init->kind == AST_STRING_LIT) {
                IrGlobal *sg = ir_string_literal(s->m,
                                                 init->u.string_lit.data,
                                                 init->u.string_lit.len);
                g->has_ptr_init = true;
                g->ptr_init = sg;
                continue;
            }
            int64_t v;
            if (const_eval(s, init, &v) && v == 0) {
                g->has_ptr_init = true;
                g->ptr_init = NULL;
                continue;
            }
            sema_error(s, n->loc,
                       "global pointer initializer must be a string literal or 0");
            continue;
        }

        if (t->kind == CT_ARRAY || is_aggregate(t)) {
            if (t->size == 0) {
                sema_error(s, n->loc, "global initializer has incomplete type");
                continue;
            }
            unsigned char *buf =
                etcc_arena_alloc(s->arena, t->size, 1);
            memset(buf, 0, t->size);
            if (!fill_global_object(s, t, n->u.decl.init, buf, 0, n->loc))
                continue;
            g->has_bytes_init = true;
            g->init_data = (const char *)buf;
            g->init_len = t->size;
            continue;
        }

        EtccAstNode *init = n->u.decl.init;
        if (init->kind == AST_INIT_LIST) {
            EtccAstNode *only = init->u.init_list.items;
            if (!only || only->next) {
                sema_error(s, n->loc,
                           "scalar initializer must contain exactly one value");
                continue;
            }
            init = only;
        }
        int64_t v;
        if (!const_eval(s, init, &v)) {
            sema_error(s, n->loc,
                       "initializer for '%s' is not a constant expression",
                       n->u.decl.name);
            continue;
        }
        g->has_int_init = true;
        g->init_int = (uint64_t)v;
    }
    return true;
}

static bool analyze_function(EtccSema *s, EtccAstNode *n) {
    EtccSymbol *sym = scope_find_local(&s->global_scope, n->u.func.name);
    if (!sym || !sym->func) return false;
    IrFunction *f = sym->func;
    if (f->is_definition) {
        sema_error(s, n->loc, "redefinition of function '%s'", n->u.func.name);
        return false;
    }

    s->fn = f;
    s->fn_ret = n->u.func.type->base;
    s->block_id = 0;
    s->loop_depth = 0;
    s->break_depth = 0;
    s->switch_depth = 0;
    s->labels.len = 0;
    s->sret = NULL;
    s->cur = NULL;

    if (f->variadic) {
        sema_error(s, n->loc,
                   "variadic function definitions are not supported yet");
        return false;
    }

    if (strcmp(n->u.func.name, "main") == 0) {
        if (!etcc_type_equal(s->fn_ret, etcc_type_builtin(CT_INT))) {
            sema_error(s, n->loc, "'main' must return 'int'");
        }
        if (n->u.func.type->nparams != 0 && n->u.func.type->nparams != 2) {
            sema_error(s, n->loc,
                       "'main' must have no parameters or (int argc, char **argv)");
        } else if (n->u.func.type->nparams == 2) {
            if (!etcc_type_equal(n->u.func.type->params[0],
                                 etcc_type_builtin(CT_INT)) ||
                n->u.func.type->params[1]->kind != CT_PTR ||
                n->u.func.type->params[1]->base->kind != CT_PTR) {
                sema_error(s, n->loc,
                           "'main' parameters must be (int, char **)");
            }
        }
        if (n->u.func.storage == STORAGE_STATIC)
            sema_error(s, n->loc, "'main' cannot be static");
    }

    ir_function_begin(s->m, f);
    s->cur = f->entry;
    s->sret = f->has_sret ? f->params[0] : NULL;
    collect_labels(s, n->u.func.body);
    scope_push(s);

    size_t expected = n->u.func.type->nparams;
    size_t actual = 0;
    for (EtccAstNode *p = n->u.func.params; p; p = p->next) actual++;
    if (actual != expected) {
        sema_error(s, n->loc, "parameter count mismatch in '%s'",
                   n->u.func.name);
    }

    size_t i = 0;
    size_t abi_base = f->has_sret ? 1 : 0;
    for (EtccAstNode *p = n->u.func.params; p && i < n->u.func.type->nparams;
         p = p->next, i++) {
        if (!p->u.param.name) {
            sema_error(s, p->loc, "function definition parameter has no name");
            continue;
        }
        EtccCType *pt = p->u.param.type;
        IrType *pit = f->param_types[abi_base + i];
        IrValue *pv = f->params[abi_base + i];
        EtccSymbol *ps = symbol_add(s, s->scope, p->u.param.name, pt, SYM_PARAM,
                                    p->loc);
        if (is_aggregate(pt) && !abi_aggregate_by_register(pt)) {
            /* The caller passes a pointer to a copy; use it directly. */
            ps->addr = pv;
        } else {
            IrValue *slot = ir_build_alloca(s->m, f, pit);
            ps->addr = slot;
            ir_build_store(s->m, s->cur, slot, pv);
        }
    }

    if (n->u.func.body) {
        if (!sema_stmt(s, n->u.func.body)) {
            scope_pop(s);
            s->fn = NULL;
            s->fn_ret = NULL;
            s->sret = NULL;
            s->cur = NULL;
            free(s->labels.items);
            s->labels.items = NULL;
            s->labels.len = 0;
            s->labels.cap = 0;
            return false;
        }
    }

    if (block_open(s)) {
        if (is_void(s->fn_ret)) {
            ir_block_set_ret(s->m, s->cur, NULL);
        } else if (is_aggregate(s->fn_ret)) {
            sema_warning(s, n->loc,
                         "control reaches end of non-void function '%s'",
                         n->u.func.name);
            if (f->has_sret) {
                ir_block_set_ret(s->m, s->cur, NULL);
            } else {
                IrValue *z = ir_const_int(s->m, f->ret_type, 0);
                ir_block_set_ret(s->m, s->cur, z);
            }
        } else if (strcmp(n->u.func.name, "main") == 0) {
            IrValue *z = ir_const_int(s->m, ir_type_i32(s->m), 0);
            z = convert_value(s, z, etcc_type_builtin(CT_INT), s->fn_ret,
                              n->loc, false);
            ir_block_set_ret(s->m, s->cur, z);
        } else {
            sema_warning(s, n->loc,
                         "control reaches end of non-void function '%s'",
                         n->u.func.name);
            IrValue *z = ir_const_int(s->m, map_type(s, s->fn_ret, n->loc), 0);
            ir_block_set_ret(s->m, s->cur, z);
        }
        s->cur = NULL;
    }

    scope_pop(s);
    s->fn = NULL;
    s->fn_ret = NULL;
    s->sret = NULL;
    s->cur = NULL;
    free(s->labels.items);
    s->labels.items = NULL;
    s->labels.len = 0;
    s->labels.cap = 0;
    return true;
}

bool etcc_sema_run(EtccAstNode *tu, EtccArena *arena, EtccDiagnostics *diag,
                   IrModule *module) {
    EtccSema s;
    memset(&s, 0, sizeof(s));
    s.arena = arena;
    s.diag = diag;
    s.m = module;
    s.scope = &s.global_scope;

    if (!collect_globals(&s, tu)) return false;
    if (!global_initializers(&s, tu)) return false;

    for (EtccAstNode *n = tu->u.tu.decls; n; n = n->next) {
        if (n->kind == AST_FUNC_DEF) {
            if (!analyze_function(&s, n)) return false;
        }
    }

    if (!ir_validate(module, diag)) return false;
    return diag->errors == 0;
}
