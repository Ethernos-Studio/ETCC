#include "etcc/ir.h"

#include <stdio.h>
#include <string.h>

IrModule *ir_module_create(EtccArena *arena) {
    IrModule *m = etcc_arena_alloc(arena, sizeof(*m), _Alignof(IrModule));
    memset(m, 0, sizeof(*m));
    m->arena = arena;
    m->void_type = etcc_arena_alloc(arena, sizeof(IrType), _Alignof(IrType));
    *m->void_type = (IrType){IR_TYPE_VOID, 0, 1, false, NULL, 0};
    m->i8 = etcc_arena_alloc(arena, sizeof(IrType), _Alignof(IrType));
    *m->i8 = (IrType){IR_TYPE_I8, 1, 1, false, NULL, 0};
    m->i16 = etcc_arena_alloc(arena, sizeof(IrType), _Alignof(IrType));
    *m->i16 = (IrType){IR_TYPE_I16, 2, 2, false, NULL, 0};
    m->i32 = etcc_arena_alloc(arena, sizeof(IrType), _Alignof(IrType));
    *m->i32 = (IrType){IR_TYPE_I32, 4, 4, true, NULL, 0};
    m->i64 = etcc_arena_alloc(arena, sizeof(IrType), _Alignof(IrType));
    *m->i64 = (IrType){IR_TYPE_I64, 8, 8, true, NULL, 0};
    m->next_value_id = 1;
    return m;
}

IrType *ir_type_void(IrModule *m) { return m->void_type; }
IrType *ir_type_i8(IrModule *m) { return m->i8; }
IrType *ir_type_i16(IrModule *m) { return m->i16; }
IrType *ir_type_i32(IrModule *m) { return m->i32; }
IrType *ir_type_i64(IrModule *m) { return m->i64; }

IrType *ir_type_ptr(IrModule *m, IrType *elem) {
    IrType *t = etcc_arena_alloc(m->arena, sizeof(*t), _Alignof(IrType));
    *t = (IrType){IR_TYPE_PTR, 8, 8, true, elem, 0};
    return t;
}

IrType *ir_type_array(IrModule *m, IrType *elem, uint64_t count) {
    IrType *t = etcc_arena_alloc(m->arena, sizeof(*t), _Alignof(IrType));
    *t = (IrType){
        IR_TYPE_ARRAY,
        elem ? elem->size * (size_t)count : 0,
        elem ? elem->align : 1,
        false,
        elem,
        count,
    };
    return t;
}

IrType *ir_type_struct(IrModule *m, size_t size, size_t align) {
    IrType *t = etcc_arena_alloc(m->arena, sizeof(*t), _Alignof(IrType));
    *t = (IrType){
        IR_TYPE_STRUCT,
        size,
        align ? align : 1,
        false,
        NULL,
        0,
    };
    return t;
}

static IrValue *new_value(IrModule *m, IrType *type, IrValueKind kind) {
    IrValue *v = etcc_arena_alloc(m->arena, sizeof(*v), _Alignof(IrValue));
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->kind = kind;
    v->id = m->next_value_id++;
    return v;
}

IrValue *ir_const_int(IrModule *m, IrType *type, uint64_t value) {
    IrValue *v = new_value(m, type, IR_VALUE_CONST_INT);
    v->as.i64 = value;
    return v;
}

IrValue *ir_const_null(IrModule *m, IrType *pointer_type) {
    IrValue *v = new_value(m, pointer_type, IR_VALUE_CONST_NULL);
    return v;
}

IrValue *ir_global_value(IrModule *m, IrGlobal *g) {
    IrValue *v = new_value(m, ir_type_ptr(m, g->type), IR_VALUE_GLOBAL);
    v->as.global = g;
    return v;
}

IrValue *ir_function_value(IrModule *m, IrFunction *f) {
    IrValue *v = new_value(m, ir_type_ptr(m, m->void_type), IR_VALUE_FUNCTION);
    v->as.function = f;
    return v;
}

IrGlobal *ir_global_find(IrModule *m, const char *name) {
    for (IrGlobal *g = m->globals; g; g = g->next)
        if (strcmp(g->name, name) == 0) return g;
    return NULL;
}

IrGlobal *ir_global_create(IrModule *m, const char *name, IrType *type,
                           bool is_const, bool is_internal) {
    IrGlobal *g = etcc_arena_alloc(m->arena, sizeof(*g), _Alignof(IrGlobal));
    memset(g, 0, sizeof(*g));
    g->name = etcc_arena_strdup(m->arena, name);
    g->type = type;
    g->is_const = is_const;
    g->is_internal = is_internal;
    if (m->globals) {
        IrGlobal *tail = m->globals;
        while (tail->next) tail = tail->next;
        tail->next = g;
    } else {
        m->globals = g;
    }
    return g;
}

IrGlobal *ir_string_literal(IrModule *m, const char *data, size_t len) {
    char name[64];
    snprintf(name, sizeof(name), ".str.%d", m->next_string_id++);
    IrType *arr = ir_type_array(m, m->i8, (uint64_t)len + 1);
    IrGlobal *g = ir_global_create(m, name, arr, true, true);
    char *bytes = etcc_arena_alloc(m->arena, len + 1, 1);
    if (len) memcpy(bytes, data, len);
    bytes[len] = '\0';
    g->is_string = true;
    g->init_data = bytes;
    g->init_len = len + 1;
    return g;
}

IrFunction *ir_function_find(IrModule *m, const char *name) {
    for (IrFunction *f = m->functions; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

IrFunction *ir_function_create(IrModule *m, const char *name, IrType *ret,
                               IrType **param_types, size_t nparams,
                               bool variadic) {
    IrFunction *f = etcc_arena_alloc(m->arena, sizeof(*f), _Alignof(IrFunction));
    memset(f, 0, sizeof(*f));
    f->name = etcc_arena_strdup(m->arena, name);
    f->ret_type = ret;
    f->nparams = nparams;
    f->variadic = variadic;
    if (nparams) {
        f->param_types =
            etcc_arena_alloc(m->arena, nparams * sizeof(*f->param_types),
                             _Alignof(IrType *));
        memcpy(f->param_types, param_types, nparams * sizeof(*param_types));
    }
    if (m->functions) {
        IrFunction *tail = m->functions;
        while (tail->next) tail = tail->next;
        tail->next = f;
    } else {
        m->functions = f;
    }
    return f;
}

void ir_function_begin(IrModule *m, IrFunction *fn) {
    if (fn->entry) return;
    fn->is_definition = true;
    fn->entry = ir_block_create(m, fn, "entry");
    if (fn->nparams) {
        fn->params = etcc_arena_alloc(m->arena,
                                      fn->nparams * sizeof(*fn->params),
                                      _Alignof(IrValue *));
        for (size_t i = 0; i < fn->nparams; i++) {
            IrValue *v = new_value(m, fn->param_types[i], IR_VALUE_PARAM);
            fn->params[i] = v;
        }
    }
}

IrBlock *ir_block_create(IrModule *m, IrFunction *fn, const char *name) {
    IrBlock *b = etcc_arena_alloc(m->arena, sizeof(*b), _Alignof(IrBlock));
    memset(b, 0, sizeof(*b));
    b->name = etcc_arena_strdup(m->arena, name);
    if (fn->blocks) {
        IrBlock *tail = fn->blocks;
        while (tail->next) tail = tail->next;
        tail->next = b;
    } else {
        fn->blocks = b;
    }
    return b;
}

void ir_block_set_ret(IrModule *m, IrBlock *b, IrValue *value) {
    (void)m;
    b->term = IR_TERM_RET;
    b->ret_value = value;
}

void ir_block_set_br(IrModule *m, IrBlock *b, IrBlock *target) {
    (void)m;
    b->term = IR_TERM_BR;
    b->target = target;
}

void ir_block_set_cond_br(IrModule *m, IrBlock *b, IrValue *cond,
                          IrBlock *then_block, IrBlock *else_block) {
    (void)m;
    b->term = IR_TERM_COND_BR;
    b->cond = cond;
    b->then_block = then_block;
    b->else_block = else_block;
}

static IrInstr *instr_new(IrModule *m, IrBlock *b, IrOp op, IrType *type,
                          bool has_result) {
    IrInstr *in = etcc_arena_alloc(m->arena, sizeof(*in), _Alignof(IrInstr));
    memset(in, 0, sizeof(*in));
    in->op = op;
    in->type = type;
    if (has_result) in->result = new_value(m, type, IR_VALUE_INSTRUCTION);
    if (b->last) b->last->next = in;
    else b->first = in;
    b->last = in;
    return in;
}

IrValue *ir_build_alloca(IrModule *m, IrFunction *fn, IrType *type) {
    IrBlock *b = fn->entry;
    if (!b) return NULL;
    IrInstr *in = instr_new(m, b, IR_OP_ALLOCA, ir_type_ptr(m, type), true);
    in->u.alloca.alloc_type = type;
    return in->result;
}

IrValue *ir_build_load(IrModule *m, IrBlock *b, IrValue *ptr,
                       IrType *type) {
    if (!ptr || !ptr->type || ptr->type->kind != IR_TYPE_PTR) return NULL;
    if (!type) type = ptr->type->elem;
    IrInstr *in = instr_new(m, b, IR_OP_LOAD, type, true);
    in->u.load.ptr = ptr;
    return in->result;
}

void ir_build_store(IrModule *m, IrBlock *b, IrValue *ptr, IrValue *value) {
    if (!ptr || !value) return;
    IrInstr *in = instr_new(m, b, IR_OP_STORE, NULL, false);
    in->u.store.ptr = ptr;
    in->u.store.value = value;
}

IrValue *ir_build_gep(IrModule *m, IrBlock *b, IrValue *ptr, IrType *elem,
                      IrValue *index) {
    IrInstr *in = instr_new(m, b, IR_OP_GEP, ir_type_ptr(m, elem), true);
    in->u.gep.ptr = ptr;
    in->u.gep.elem = elem;
    in->u.gep.index = index;
    return in->result;
}

IrValue *ir_build_bin(IrModule *m, IrBlock *b, IrBinOp op, IrValue *lhs,
                      IrValue *rhs) {
    if (!lhs || !rhs) return NULL;
    IrInstr *in = instr_new(m, b, IR_OP_BIN, lhs->type, true);
    in->u.bin.op = op;
    in->u.bin.lhs = lhs;
    in->u.bin.rhs = rhs;
    return in->result;
}

IrValue *ir_build_cmp(IrModule *m, IrBlock *b, IrCmpOp op, IrValue *lhs,
                      IrValue *rhs) {
    IrInstr *in = instr_new(m, b, IR_OP_CMP, m->i32, true);
    in->u.cmp.op = op;
    in->u.cmp.lhs = lhs;
    in->u.cmp.rhs = rhs;
    return in->result;
}

IrValue *ir_build_cast(IrModule *m, IrBlock *b, IrCastOp op, IrValue *value,
                       IrType *target) {
    IrInstr *in = instr_new(m, b, IR_OP_CAST, target, true);
    in->u.cast.op = op;
    in->u.cast.value = value;
    in->u.cast.target = target;
    return in->result;
}

IrValue *ir_build_call(IrModule *m, IrBlock *b, const char *callee,
                       IrValue **args, size_t nargs, IrType *ret_type,
                       bool variadic) {
    bool has_result = ret_type && ret_type->kind != IR_TYPE_VOID;
    IrInstr *in = instr_new(m, b, IR_OP_CALL, ret_type, has_result);
    in->u.call.callee = etcc_arena_strdup(m->arena, callee);
    in->u.call.nargs = nargs;
    in->u.call.variadic = variadic;
    if (nargs) {
        in->u.call.args = etcc_arena_alloc(m->arena,
                                          nargs * sizeof(*in->u.call.args),
                                          _Alignof(IrValue *));
        memcpy(in->u.call.args, args, nargs * sizeof(*args));
    }
    return has_result ? in->result : NULL;
}

bool ir_validate(IrModule *m, EtccDiagnostics *diag) {
    bool ok = true;
    for (IrFunction *f = m->functions; f; f = f->next) {
        if (!f->is_definition) continue;
        if (!f->entry) {
            etcc_diag_error_global(diag, "IR: function '%s' has no entry block",
                                   f->name);
            ok = false;
            continue;
        }
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (b->term == IR_TERM_NONE) {
                etcc_diag_error_global(diag,
                                       "IR: block '%s' in '%s' has no terminator",
                                       b->name, f->name);
                ok = false;
            }
        }
    }
    return ok;
}

const char *ir_type_name(const IrType *t, char *buf, size_t size) {
    if (!t) return "void";
    switch (t->kind) {
    case IR_TYPE_VOID: return "void";
    case IR_TYPE_I8: return "i8";
    case IR_TYPE_I16: return "i16";
    case IR_TYPE_I32: return "i32";
    case IR_TYPE_I64: return "i64";
    case IR_TYPE_PTR: snprintf(buf, size, "ptr"); return buf;
    case IR_TYPE_STRUCT:
        snprintf(buf, size, "{ %zu bytes }", t->size);
        return buf;
    case IR_TYPE_ARRAY: {
        char inner[64];
        const char *ename =
            t->elem ? ir_type_name(t->elem, inner, sizeof(inner)) : "void";
        snprintf(buf, size, "[%llu x %s]", (unsigned long long)t->count, ename);
        return buf;
    }
    default: return "<ir-type>";
    }
}
