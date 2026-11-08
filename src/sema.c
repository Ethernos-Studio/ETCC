#include "etcc/sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SYM_LOCAL_VAR,
    SYM_PARAM,
    SYM_GLOBAL_VAR,
    SYM_FUNCTION,
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
    EtccArena *arena;
    EtccDiagnostics *diag;
    IrModule *m;
    EtccScope *scope;
    EtccScope global_scope;

    IrFunction *fn;
    EtccCType *fn_ret;
    IrBlock *cur;
    int block_id;

    EtccLoop loops[64];
    int loop_depth;
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
    static EtccCType t = {CT_LLONG, 8, 8, true, false, NULL, 0, NULL, 0, false};
    return &t;
}

static EtccCType *ctype_uint(void) {
    static EtccCType t = {CT_INT, 4, 4, true, false, NULL, 0, NULL, 0, false};
    return &t;
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
        return ir_type_ptr(s->m, map_type(s, t->base, loc));
    case CT_ARRAY: {
        uint64_t count = t->array_len;
        if (count == 0) {
            sema_error(s, loc, "incomplete array type is not allowed here");
            count = 1;
        }
        return ir_type_array(s->m, map_type(s, t->base, loc), count);
    }
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

static IrValue *convert_value(EtccSema *s, IrValue *v, EtccCType *from,
                              EtccCType *to, EtccLoc loc, bool explicit_cast) {
    if (!v || !to) return v;
    if (!from) from = to;
    if (etcc_type_equal(from, to)) return v;

    if (to->kind == CT_FLOAT || to->kind == CT_DOUBLE ||
        from->kind == CT_FLOAT || from->kind == CT_DOUBLE) {
        sema_error(s, loc, "floating-point conversions are not supported yet");
        return v;
    }

    if (is_integer(to)) {
        if (is_integer(from)) {
            size_t fs = etcc_type_sizeof(from);
            size_t ts = etcc_type_sizeof(to);
            IrType *target = map_type(s, to, loc);
            if (to->kind == CT_BOOL) {
                IrValue *zero = zero_of_type(s, v->type);
                IrValue *cmp = ir_build_cmp(s->m, s->cur, IR_CMP_NE, v, zero);
                return ir_build_cast(s->m, s->cur, IR_CAST_TRUNC, cmp, target);
            }
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
        if (is_pointer(from)) return v;
        if (is_integer(from) && v->kind == IR_VALUE_CONST_INT && v->as.i64 == 0)
            return ir_const_null(s->m, map_type(s, to, loc));
        if (explicit_cast && is_integer(from))
            return ir_build_cast(s->m, s->cur, IR_CAST_INTTOPTR, v,
                                 map_type(s, to, loc));
        sema_error(s, loc, "cannot implicitly convert integer to pointer");
        return v;
    }

    if (is_void(to)) return v;
    return v;
}

static void load_lvalue(EtccSema *s, SemVal *v) {
    if (v->addr && v->type->kind != CT_ARRAY && v->type->kind != CT_FUNC)
        v->value = ir_build_load(s->m, s->cur, v->addr);
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
    case AST_MEMBER:
        sema_error(s, n->loc, "struct/union members are not supported yet");
        return false;
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

    IrValue **args = NULL;
    if (nargs) {
        args = etcc_arena_alloc(s->arena, nargs * sizeof(*args),
                                _Alignof(IrValue *));
    }

    size_t i = 0;
    for (EtccAstNode *a = n->u.call.args; a; a = a->next, i++) {
        SemVal av;
        if (!sema_expr(s, a, &av)) return false;
        if (!decay(s, &av, a->loc)) return false;
        if (i < ft->nparams) {
            EtccCType *pt = ft->params[i];
            av.value = convert_value(s, av.value, av.type, pt, a->loc, false);
            av.type = pt;
            args[i] = av.value;
        } else {
            if (av.type->kind == CT_FLOAT || av.type->kind == CT_DOUBLE) {
                sema_error(s, a->loc,
                           "floating-point varargs are not supported yet");
                return false;
            }
            EtccCType *promoted = integer_promote(s, av.type);
            av.value = convert_value(s, av.value, av.type, promoted, a->loc,
                                     false);
            av.type = promoted;
            args[i] = av.value;
        }
    }

    IrType *ret_ir = map_type(s, ft->base, n->loc);
    IrValue *result = ir_build_call(s->m, s->cur, name, args, nargs, ret_ir,
                                    ft->variadic);
    out->type = ft->base;
    out->value = result;
    out->addr = NULL;
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
    out->value = ir_build_load(s->m, s->cur, slot);
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
    if (op == TK_ASSIGN) {
        SemVal rhs;
        if (!sema_expr(s, n->u.assign.value, &rhs)) return false;
        if (!decay(s, &rhs, n->loc)) return false;
        rhs.value = convert_value(s, rhs.value, rhs.type, target.type, n->loc,
                                  false);
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
    EtccCType *ct = common_arithmetic(s, target.type, rhs.type);
    old.value = convert_value(s, old.value, old.type, ct, n->loc, false);
    rhs.value = convert_value(s, rhs.value, rhs.type, ct, n->loc, false);

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
        IrValue *old = ir_build_load(s->m, s->cur, target.addr);
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
    IrValue *old = ir_build_load(s->m, s->cur, target.addr);
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
    EtccCType *rt = NULL;
    if (tt && et && is_pointer(tt) && is_pointer(et))
        rt = tt;
    else if (tt && et && is_integer(tt) && is_integer(et))
        rt = common_arithmetic(s, tt, et);
    else if (tt && is_pointer(tt) && et && is_integer(et))
        rt = tt;
    else if (et && is_pointer(et) && tt && is_integer(tt))
        rt = et;
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
    out->value = ir_build_load(s->m, s->cur, slot);
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
        EtccCType *t;
        if (n->u.int_lit.is_unsigned) {
            if (v <= 0xFFFFFFFFull && !n->u.int_lit.is_unsigned)
                t = ctype_uint();
            else if (v <= 0xFFFFFFFFull)
                t = ctype_uint();
            else {
                static EtccCType ull = {CT_LLONG, 8, 8, true, false, NULL, 0,
                                        NULL, 0, false};
                t = &ull;
            }
        } else if (v <= 0x7FFFFFFFull) {
            t = etcc_type_builtin(CT_INT);
        } else if (v <= 0xFFFFFFFFull) {
            static EtccCType ul = {CT_LONG, 4, 4, true, false, NULL, 0, NULL,
                                   0, false};
            t = &ul;
        } else {
            t = etcc_type_builtin(CT_LLONG);
        }
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
        if (target->kind == CT_ARRAY || target->kind == CT_FUNC) {
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
        if (t->kind == CT_VOID || t->kind == CT_FUNC ||
            (t->kind == CT_ARRAY && t->array_len == 0)) {
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
    case AST_MEMBER:
        sema_error(s, n->loc, "struct/union members are not supported yet");
        return false;
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
        if (n->u.int_lit.is_unsigned) {
            static EtccCType u = {CT_INT, 4, 4, true, false, NULL, 0, NULL, 0, false};
            return &u;
        }
        return etcc_type_builtin(CT_INT);
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
        if (n->u.unary.op == TK_BANG) return etcc_type_builtin(CT_INT);
        if (n->u.unary.op == TK_PLUS_PLUS ||
            n->u.unary.op == TK_MINUS_MINUS)
            return infer_type(s, n->u.unary.operand);
        {
            EtccCType *t = infer_type(s, n->u.unary.operand);
            return t ? integer_promote(s, t) : NULL;
        }
    case AST_BINARY:
        if (n->u.binary.op == TK_EQ || n->u.binary.op == TK_NE ||
            n->u.binary.op == TK_LT || n->u.binary.op == TK_LE ||
            n->u.binary.op == TK_GT || n->u.binary.op == TK_GE ||
            n->u.binary.op == TK_AND_AND || n->u.binary.op == TK_OR_OR)
            return etcc_type_builtin(CT_INT);
        return infer_type(s, n->u.binary.lhs);
    case AST_ASSIGN:
        return infer_type(s, n->u.assign.target);
    case AST_COND:
        return infer_type(s, n->u.cond.then_expr);
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
    case AST_COMMA:
        return infer_type(s, n->u.comma.rhs);
    default:
        return NULL;
    }
}

static bool sema_decl_list(EtccSema *s, EtccAstNode *decls, bool global);

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

    EtccCType *t = n->u.decl.type;
    if (t->kind == CT_VOID || t->kind == CT_FUNC ||
        (t->kind == CT_ARRAY && t->array_len == 0)) {
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
        SemVal v;
        if (!sema_expr(s, n->u.decl.init, &v)) return false;
        if (!decay(s, &v, n->loc)) return false;
        v.value = convert_value(s, v.value, v.type, t, n->loc, false);
        ir_build_store(s->m, s->cur, slot, v.value);
    }
    return true;
}

static bool sema_decl_list(EtccSema *s, EtccAstNode *decls, bool global) {
    (void)global;
    for (EtccAstNode *n = decls; n && n->kind == AST_DECL; n = n->next) {
        if (!sema_local_decl(s, n)) return false;
    }
    return true;
}

static bool sema_for_stmt(EtccSema *s, EtccAstNode *n) {
    if (n->u.for_.init) {
        if (n->u.for_.init->kind == AST_DECL) {
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

    s->cur = bodyb;
    if (!sema_stmt(s, n->u.for_.body)) return false;
    if (block_open(s)) emit_br(s, stepb);

    s->cur = stepb;
    if (n->u.for_.step) {
        SemVal v;
        if (!sema_expr(s, n->u.for_.step, &v)) return false;
    }
    if (block_open(s)) emit_br(s, condb);
    s->loop_depth--;
    s->cur = end;
    return true;
}

static bool sema_stmt(EtccSema *s, EtccAstNode *n) {
    if (!n) return true;
    if (!block_open(s)) return true;

    switch (n->kind) {
    case AST_COMPOUND: {
        scope_push(s);
        for (EtccAstNode *st = n->u.compound.stmts; st; st = st->next) {
            if (!block_open(s)) break;
            if (!sema_stmt(s, st)) {
                scope_pop(s);
                return false;
            }
        }
        scope_pop(s);
        return true;
    }
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
        v.value = convert_value(s, v.value, v.type, s->fn_ret, n->loc, false);
        if (block_open(s)) ir_block_set_ret(s->m, s->cur, v.value);
        s->cur = NULL;
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
        s->cur = bodyb;
        if (!sema_stmt(s, n->u.while_.body)) return false;
        if (block_open(s)) emit_br(s, condb);
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

        s->cur = bodyb;
        if (!sema_stmt(s, n->u.do_.body)) return false;
        if (block_open(s)) emit_br(s, condb);
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
        if (s->loop_depth == 0) {
            sema_error(s, n->loc, "'break' outside of a loop");
            return false;
        }
        emit_br(s, s->loops[s->loop_depth - 1].break_block);
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

static bool const_eval(EtccSema *s, EtccAstNode *n, int64_t *out) {
    if (!n) return false;
    switch (n->kind) {
    case AST_INT_LIT:
        *out = (int64_t)n->u.int_lit.value;
        return true;
    case AST_CHAR_LIT:
        *out = n->u.char_lit.value;
        return true;
    case AST_UNARY: {
        int64_t v;
        if (!const_eval(s, n->u.unary.operand, &v)) return false;
        switch (n->u.unary.op) {
        case TK_PLUS: *out = v; return true;
        case TK_MINUS: *out = -v; return true;
        case TK_TILDE: *out = ~v; return true;
        case TK_BANG: *out = !v; return true;
        default: return false;
        }
    }
    case AST_BINARY: {
        int64_t a, b;
        if (!const_eval(s, n->u.binary.lhs, &a)) return false;
        if (!const_eval(s, n->u.binary.rhs, &b)) return false;
        switch (n->u.binary.op) {
        case TK_PLUS: *out = a + b; return true;
        case TK_MINUS: *out = a - b; return true;
        case TK_STAR: *out = a * b; return true;
        case TK_SLASH:
            if (!b) { sema_error(s, n->loc, "division by zero"); return false; }
            *out = a / b; return true;
        case TK_PERCENT:
            if (!b) { sema_error(s, n->loc, "division by zero"); return false; }
            *out = a % b; return true;
        case TK_SHL: *out = (b < 0 || b >= 64) ? 0 : (int64_t)((uint64_t)a << b); return true;
        case TK_SHR: *out = (b < 0 || b >= 64) ? 0 : (a >> b); return true;
        case TK_AMP: *out = a & b; return true;
        case TK_PIPE: *out = a | b; return true;
        case TK_CARET: *out = a ^ b; return true;
        case TK_LT: *out = a < b; return true;
        case TK_LE: *out = a <= b; return true;
        case TK_GT: *out = a > b; return true;
        case TK_GE: *out = a >= b; return true;
        case TK_EQ: *out = a == b; return true;
        case TK_NE: *out = a != b; return true;
        case TK_AND_AND: *out = (a && b); return true;
        case TK_OR_OR: *out = (a || b); return true;
        default: return false;
        }
    }
    case AST_COND: {
        int64_t c, a, b;
        if (!const_eval(s, n->u.cond.cond, &c)) return false;
        if (!const_eval(s, n->u.cond.then_expr, &a)) return false;
        if (!const_eval(s, n->u.cond.else_expr, &b)) return false;
        *out = c ? a : b;
        return true;
    }
    case AST_CAST: {
        int64_t v;
        if (!const_eval(s, n->u.cast.operand, &v)) return false;
        EtccCType *t = n->u.cast.target;
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
    IrFunction *f = ir_function_find(s->m, name);
    if (f) {
        if (f->nparams != ft->nparams || f->variadic != ft->variadic ||
            !ir_type_same(f->ret_type, map_type(s, ft->base, loc))) {
            sema_error(s, loc, "conflicting declaration of function '%s'", name);
            return f;
        }
        for (size_t i = 0; i < ft->nparams; i++) {
            IrType *want = map_type(s, ft->params[i], loc);
            if (!ir_type_same(f->param_types[i], want)) {
                sema_error(s, loc, "conflicting declaration of function '%s'",
                           name);
                return f;
            }
        }
        return f;
    }

    IrType *ret = map_type(s, ft->base, loc);
    IrType **pts = NULL;
    if (ft->nparams) {
        pts = etcc_arena_alloc(s->arena, ft->nparams * sizeof(*pts),
                               _Alignof(IrType *));
        for (size_t i = 0; i < ft->nparams; i++)
            pts[i] = map_type(s, ft->params[i], loc);
    }
    return ir_function_create(s->m, name, ret, pts, ft->nparams, ft->variadic);
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

static bool collect_global_var(EtccSema *s, EtccAstNode *n) {
    const char *name = n->u.decl.name;
    EtccCType *t = n->u.decl.type;
    EtccLoc loc = n->loc;
    if (!name) return false;

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
        g->is_external = false;

        if (n->u.decl.type->kind == CT_PTR) {
            if (n->u.decl.init->kind == AST_STRING_LIT) {
                IrGlobal *sg = ir_string_literal(s->m,
                                                 n->u.decl.init->u.string_lit.data,
                                                 n->u.decl.init->u.string_lit.len);
                g->has_ptr_init = true;
                g->ptr_init = sg;
                continue;
            }
            int64_t v;
            if (const_eval(s, n->u.decl.init, &v) && v == 0) {
                g->has_ptr_init = true;
                g->ptr_init = NULL;
                continue;
            }
            sema_error(s, n->loc,
                       "global pointer initializer must be a string literal or 0");
            continue;
        }
        if (n->u.decl.type->kind == CT_ARRAY) {
            sema_error(s, n->loc, "global array initializers are not supported yet");
            continue;
        }
        int64_t v;
        if (!const_eval(s, n->u.decl.init, &v)) {
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
        if (f->nparams != 0 && f->nparams != 2) {
            sema_error(s, n->loc,
                       "'main' must have no parameters or (int argc, char **argv)");
        } else if (f->nparams == 2) {
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
    scope_push(s);

    size_t expected = n->u.func.type->nparams;
    size_t actual = 0;
    for (EtccAstNode *p = n->u.func.params; p; p = p->next) actual++;
    if (actual != expected) {
        sema_error(s, n->loc, "parameter count mismatch in '%s'",
                   n->u.func.name);
    }

    size_t i = 0;
    for (EtccAstNode *p = n->u.func.params; p && i < f->nparams;
         p = p->next, i++) {
        if (!p->u.param.name) {
            sema_error(s, p->loc, "function definition parameter has no name");
            continue;
        }
        EtccCType *pt = p->u.param.type;
        IrType *pit = map_type(s, pt, p->loc);
        IrValue *slot = ir_build_alloca(s->m, f, pit);
        EtccSymbol *ps = symbol_add(s, s->scope, p->u.param.name, pt, SYM_PARAM,
                                    p->loc);
        ps->addr = slot;
        ir_build_store(s->m, s->cur, slot, f->params[i]);
    }

    if (n->u.func.body) sema_stmt(s, n->u.func.body);

    if (block_open(s)) {
        if (is_void(s->fn_ret)) {
            ir_block_set_ret(s->m, s->cur, NULL);
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
    s->cur = NULL;
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
