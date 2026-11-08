#include "etcc/backend.h"

#include "etcc/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    EtccArena *arena;
    EtccDiagnostics *diag;
    const char *clang;
    const char *llc;
    bool verbose;
} LlvmBackend;

typedef struct {
    FILE *f;
    const IrModule *m;
    EtccDiagnostics *diag;
} LlvmEmitter;

static void llvm_type(FILE *f, const IrType *t) {
    if (!t) {
        fputs("void", f);
        return;
    }
    switch (t->kind) {
    case IR_TYPE_VOID: fputs("void", f); break;
    case IR_TYPE_I8: fputs("i8", f); break;
    case IR_TYPE_I16: fputs("i16", f); break;
    case IR_TYPE_I32: fputs("i32", f); break;
    case IR_TYPE_I64: fputs("i64", f); break;
    case IR_TYPE_PTR: fputs("ptr", f); break;
    case IR_TYPE_STRUCT:
        fprintf(f, "[%zu x i8]", t->size);
        break;
    case IR_TYPE_ARRAY:
        fprintf(f, "[%llu x ", (unsigned long long)t->count);
        llvm_type(f, t->elem);
        fputc(']', f);
        break;
    default: fputs("i32", f); break;
    }
}

static uint64_t mask_constant(const IrType *t, uint64_t v) {
    size_t size = t ? t->size : 8;
    if (size >= 8) return v;
    if (size == 0) return 0;
    uint64_t mask = (1ull << (size * 8)) - 1;
    return v & mask;
}

static void llvm_operand(FILE *f, const IrValue *v) {
    if (!v) {
        fputs("null", f);
        return;
    }
    switch (v->kind) {
    case IR_VALUE_CONST_INT:
        fprintf(f, "%llu",
                (unsigned long long)mask_constant(v->type, v->as.i64));
        break;
    case IR_VALUE_CONST_NULL:
        fputs("null", f);
        break;
    case IR_VALUE_GLOBAL:
        fprintf(f, "@%s", v->as.global->name);
        break;
    case IR_VALUE_FUNCTION:
        fprintf(f, "@%s", v->as.function->name);
        break;
    case IR_VALUE_PARAM:
        fprintf(f, "%%p%d", v->id);
        break;
    case IR_VALUE_INSTRUCTION:
        fprintf(f, "%%t%d", v->id);
        break;
    }
}

static void llvm_typed_value(FILE *f, const IrValue *v) {
    llvm_type(f, v ? v->type : NULL);
    fputc(' ', f);
    llvm_operand(f, v);
}

static void llvm_escape_bytes(FILE *f, const char *data, size_t len) {
    fputc('c', f);
    fputc('"', f);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7F)
            fprintf(f, "\\%02X", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void emit_global(LlvmEmitter *e, const IrGlobal *g) {
    FILE *f = e->f;
    if (g->is_string) {
        fprintf(f, "@%s = private unnamed_addr constant ", g->name);
        llvm_type(f, g->type);
        fputc(' ', f);
        llvm_escape_bytes(f, g->init_data, g->init_len);
        fputs(", align 1\n", f);
        return;
    }

    fprintf(f, "@%s = ", g->name);
    if (g->is_external) {
        fputs("external ", f);
        if (g->is_const) fputs("constant ", f);
        else fputs("global ", f);
        llvm_type(f, g->type);
        fputc('\n', f);
        return;
    }
    if (g->is_internal) fputs("internal ", f);
    if (g->is_const) fputs("constant ", f);
    else fputs("global ", f);

    if (g->has_bytes_init) {
        fprintf(f, "[%zu x i8] ", g->init_len);
        llvm_escape_bytes(f, g->init_data, g->init_len);
        if (g->type->align > 1) fprintf(f, ", align %zu", g->type->align);
        fputc('\n', f);
        return;
    }

    llvm_type(f, g->type);
    fputc(' ', f);

    if (g->has_ptr_init) {
        if (g->ptr_init) fprintf(f, "@%s", g->ptr_init->name);
        else fputs("null", f);
    } else if (g->has_int_init) {
        fprintf(f, "%llu", (unsigned long long)mask_constant(g->type,
                                                             g->init_int));
    } else {
        switch (g->type->kind) {
        case IR_TYPE_ARRAY:
        case IR_TYPE_STRUCT:
            fputs("zeroinitializer", f);
            break;
        case IR_TYPE_PTR:
            fputs("null", f);
            break;
        default:
            fputs("0", f);
            break;
        }
    }

    if (g->type->align > 1) fprintf(f, ", align %zu", g->type->align);
    fputc('\n', f);
}

static void emit_function_signature(FILE *f, const IrFunction *fn,
                                    bool definition) {
    if (definition) {
        fputs("define ", f);
        if (fn->is_internal) fputs("internal ", f);
    } else {
        fputs("declare ", f);
    }
    llvm_type(f, fn->ret_type);
    fprintf(f, " @%s(", fn->name);

    for (size_t i = 0; i < fn->nparams; i++) {
        if (i) fputs(", ", f);
        llvm_type(f, fn->param_types[i]);
        if (definition) fprintf(f, " %%p%d", fn->params[i]->id);
    }
    if (fn->variadic) {
        if (fn->nparams) fputs(", ", f);
        fputs("...", f);
    }
    fputc(')', f);
}

static void emit_instruction(LlvmEmitter *e, const IrInstr *in) {
    FILE *f = e->f;
    switch (in->op) {
    case IR_OP_ALLOCA:
        fprintf(f, "  %%t%d = alloca ", in->result->id);
        llvm_type(f, in->u.alloca.alloc_type);
        if (in->u.alloca.alloc_type->align > 1)
            fprintf(f, ", align %zu", in->u.alloca.alloc_type->align);
        fputc('\n', f);
        break;

    case IR_OP_LOAD:
        fprintf(f, "  %%t%d = load ", in->result->id);
        llvm_type(f, in->type);
        fputs(", ptr ", f);
        llvm_operand(f, in->u.load.ptr);
        if (in->type->align > 1) fprintf(f, ", align %zu", in->type->align);
        fputc('\n', f);
        break;

    case IR_OP_STORE:
        fputs("  store ", f);
        llvm_typed_value(f, in->u.store.value);
        fputs(", ptr ", f);
        llvm_operand(f, in->u.store.ptr);
        if (in->u.store.value->type->align > 1)
            fprintf(f, ", align %zu", in->u.store.value->type->align);
        fputc('\n', f);
        break;

    case IR_OP_GEP:
        fprintf(f, "  %%t%d = getelementptr inbounds ", in->result->id);
        llvm_type(f, in->u.gep.elem);
        fputs(", ptr ", f);
        llvm_operand(f, in->u.gep.ptr);
        fputs(", ", f);
        llvm_typed_value(f, in->u.gep.index);
        fputc('\n', f);
        break;

    case IR_OP_BIN: {
        const char *op = "add";
        switch (in->u.bin.op) {
        case IR_BIN_ADD: op = "add"; break;
        case IR_BIN_SUB: op = "sub"; break;
        case IR_BIN_MUL: op = "mul"; break;
        case IR_BIN_SDIV: op = "sdiv"; break;
        case IR_BIN_UDIV: op = "udiv"; break;
        case IR_BIN_SREM: op = "srem"; break;
        case IR_BIN_UREM: op = "urem"; break;
        case IR_BIN_SHL: op = "shl"; break;
        case IR_BIN_LSHR: op = "lshr"; break;
        case IR_BIN_ASHR: op = "ashr"; break;
        case IR_BIN_AND: op = "and"; break;
        case IR_BIN_OR: op = "or"; break;
        case IR_BIN_XOR: op = "xor"; break;
        }
        fprintf(f, "  %%t%d = %s ", in->result->id, op);
        llvm_typed_value(f, in->u.bin.lhs);
        fputs(", ", f);
        llvm_operand(f, in->u.bin.rhs);
        fputc('\n', f);
        break;
    }

    case IR_OP_CMP: {
        const char *pred = "eq";
        switch (in->u.cmp.op) {
        case IR_CMP_EQ: pred = "eq"; break;
        case IR_CMP_NE: pred = "ne"; break;
        case IR_CMP_SLT: pred = "slt"; break;
        case IR_CMP_SLE: pred = "sle"; break;
        case IR_CMP_SGT: pred = "sgt"; break;
        case IR_CMP_SGE: pred = "sge"; break;
        case IR_CMP_ULT: pred = "ult"; break;
        case IR_CMP_ULE: pred = "ule"; break;
        case IR_CMP_UGT: pred = "ugt"; break;
        case IR_CMP_UGE: pred = "uge"; break;
        }
        fprintf(f, "  %%c%d = icmp %s ", in->result->id, pred);
        llvm_typed_value(f, in->u.cmp.lhs);
        fputs(", ", f);
        llvm_operand(f, in->u.cmp.rhs);
        fputc('\n', f);
        fprintf(f, "  %%t%d = zext i1 %%c%d to ", in->result->id,
                in->result->id);
        llvm_type(f, in->type);
        fputc('\n', f);
        break;
    }

    case IR_OP_CAST: {
        const char *op = "bitcast";
        switch (in->u.cast.op) {
        case IR_CAST_TRUNC: op = "trunc"; break;
        case IR_CAST_ZEXT: op = "zext"; break;
        case IR_CAST_SEXT: op = "sext"; break;
        case IR_CAST_BITCAST: op = "bitcast"; break;
        case IR_CAST_PTRTOINT: op = "ptrtoint"; break;
        case IR_CAST_INTTOPTR: op = "inttoptr"; break;
        }
        fprintf(f, "  %%t%d = %s ", in->result->id, op);
        llvm_typed_value(f, in->u.cast.value);
        fputs(" to ", f);
        llvm_type(f, in->u.cast.target);
        fputc('\n', f);
        break;
    }

    case IR_OP_CALL:
        if (in->result) fprintf(f, "  %%t%d = ", in->result->id);
        else fputs("  ", f);
        fputs("call ", f);
        llvm_type(f, in->type);
        fprintf(f, " @%s(", in->u.call.callee);
        for (size_t i = 0; i < in->u.call.nargs; i++) {
            if (i) fputs(", ", f);
            llvm_typed_value(f, in->u.call.args[i]);
        }
        fputc(')', f);
        fputc('\n', f);
        break;
    }
}

static void emit_block(LlvmEmitter *e, const IrBlock *b) {
    FILE *f = e->f;
    fprintf(f, "%s:\n", b->name ? b->name : "bb");
    for (const IrInstr *in = b->first; in; in = in->next)
        emit_instruction(e, in);

    switch (b->term) {
    case IR_TERM_RET:
        if (b->ret_value) {
            fputs("  ret ", f);
            llvm_typed_value(f, b->ret_value);
            fputc('\n', f);
        } else {
            fputs("  ret void\n", f);
        }
        break;
    case IR_TERM_BR:
        fprintf(f, "  br label %%%s\n", b->target->name);
        break;
    case IR_TERM_COND_BR:
        fprintf(f, "  %%t%d.bool = icmp ne i32 ", b->cond->id);
        llvm_operand(f, b->cond);
        fputs(", 0\n", f);
        fprintf(f, "  br i1 %%t%d.bool, label %%%s, label %%%s\n",
                b->cond->id, b->then_block->name, b->else_block->name);
        break;
    case IR_TERM_NONE:
        fputs("  unreachable\n", f);
        break;
    }
}

static void emit_function(LlvmEmitter *e, const IrFunction *fn) {
    emit_function_signature(e->f, fn, true);
    fputs(" {\n", e->f);
    for (const IrBlock *b = fn->blocks; b; b = b->next)
        emit_block(e, b);
    fputs("}\n\n", e->f);
}

static void emit_module(LlvmEmitter *e) {
    const IrModule *m = e->m;
    FILE *f = e->f;

    fputs("; generated by Ethernos Tiny C Compiler (ETCC)\n", f);
    fputs("target triple = \"x86_64-pc-windows-msvc\"\n\n", f);

    for (const IrGlobal *g = m->globals; g; g = g->next)
        emit_global(e, g);
    if (m->globals) fputc('\n', f);

    for (const IrFunction *fn = m->functions; fn; fn = fn->next) {
        if (!fn->is_definition) {
            emit_function_signature(f, fn, false);
            fputs("\n", f);
        }
    }
    fputc('\n', f);

    for (const IrFunction *fn = m->functions; fn; fn = fn->next) {
        if (fn->is_definition) emit_function(e, fn);
    }
}

static bool write_ir_file(LlvmEmitter *e, const char *path,
                          EtccDiagnostics *diag) {
    if (strcmp(path, "-") == 0) {
        e->f = stdout;
        emit_module(e);
        fflush(stdout);
        return true;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        etcc_diag_error_global(diag, "cannot create IR file '%s'", path);
        return false;
    }
    e->f = f;
    emit_module(e);
    fclose(f);
    return true;
}

static bool llvm_emit_ir(EtccBackend *b, const IrModule *m, const char *path,
                         EtccDiagnostics *diag) {
    LlvmBackend *lb = b->impl;
    (void)lb;
    LlvmEmitter e = {.f = NULL, .m = m, .diag = diag};
    return write_ir_file(&e, path, diag);
}

static bool llvm_assemble_object(LlvmBackend *lb, const char *ir_path,
                                     const char *obj_path) {
    const char *clang = lb->clang && *lb->clang ? lb->clang : "clang";

    if (lb->llc && *lb->llc) {
        char *llc[10];
        int n = 0;
        llc[n++] = (char *)lb->llc;
        llc[n++] = "-mtriple=x86_64-pc-windows-msvc";
        llc[n++] = "-filetype=obj";
        llc[n++] = "-O0";
        llc[n++] = "-o";
        llc[n++] = (char *)obj_path;
        llc[n++] = (char *)ir_path;
        llc[n] = NULL;
        if (etcc_run_process(llc, lb->verbose)) return true;
    }

    /*
     * Calling clang's cc1 frontend directly avoids the clang driver's extra
     * process startup and argument translation.  IR -> COFF does not need
     * driver features.  This is an internal interface, so fall back to the
     * public driver command if a future LLVM changes the cc1 options.
     */
    char *cc1[20];
    int n = 0;
    cc1[n++] = (char *)clang;
    cc1[n++] = "-cc1";
    cc1[n++] = "-triple";
    cc1[n++] = "x86_64-pc-windows-msvc";
    cc1[n++] = "-emit-obj";
    cc1[n++] = "-O0";
    cc1[n++] = "-ffreestanding";
    cc1[n++] = "-stack-protector";
    cc1[n++] = "0";
    cc1[n++] = "-o";
    cc1[n++] = (char *)obj_path;
    cc1[n++] = (char *)ir_path;
    cc1[n] = NULL;

    if (etcc_run_process(cc1, lb->verbose)) return true;

    char *driver[20];
    n = 0;
    driver[n++] = (char *)clang;
    driver[n++] = "--target=x86_64-pc-windows-msvc";
    driver[n++] = "-c";
    driver[n++] = "-O0";
    driver[n++] = "-ffreestanding";
    driver[n++] = "-fno-stack-protector";
    driver[n++] = "-fno-asynchronous-unwind-tables";
    driver[n++] = "-Wno-override-module";
    driver[n++] = "-x";
    driver[n++] = "ir";
    driver[n++] = (char *)ir_path;
    driver[n++] = "-o";
    driver[n++] = (char *)obj_path;
    driver[n] = NULL;
    return etcc_run_process(driver, lb->verbose);
}

static bool llvm_emit_object(EtccBackend *b, const IrModule *m,
                             const char *obj_path, EtccDiagnostics *diag) {
    LlvmBackend *lb = b->impl;
    char *ir_path = etcc_make_temp_path(lb->arena, "ll");
    if (lb->verbose) fprintf(stderr, "etcc: LLVM IR temp file: %s\n", ir_path);
    LlvmEmitter e = {.f = NULL, .m = m, .diag = diag};
    if (!write_ir_file(&e, ir_path, diag)) {
        etcc_remove_file_if_exists(ir_path);
        return false;
    }

    bool ok = llvm_assemble_object(lb, ir_path, obj_path);
    etcc_remove_file_if_exists(ir_path);
    if (!ok) {
        etcc_diag_error_global(diag,
                               "LLVM backend failed to assemble IR into '%s'",
                               obj_path);
        return false;
    }
    if (!etcc_file_exists(obj_path)) {
        etcc_diag_error_global(diag,
                               "LLVM backend produced no object file '%s'",
                               obj_path);
        return false;
    }
    return true;
}

static void llvm_destroy(EtccBackend *b) {
    (void)b;
}

static const EtccBackendOps llvm_ops = {
    .name = "llvm",
    .description = "LLVM textual IR -> COFF object via clang",
    .emit_object = llvm_emit_object,
    .emit_ir = llvm_emit_ir,
    .destroy = llvm_destroy,
};

EtccBackend *etcc_backend_llvm_create(EtccArena *arena, EtccDiagnostics *diag,
                                      const EtccBackendOptions *options) {
    LlvmBackend *lb = etcc_arena_alloc(arena, sizeof(*lb), _Alignof(LlvmBackend));
    memset(lb, 0, sizeof(*lb));
    lb->arena = arena;
    lb->diag = diag;
    lb->clang = etcc_arena_strdup(arena,
                                  options && options->clang_path
                                      ? options->clang_path
                                      : "clang");
    lb->llc = options && options->llc_path
                  ? etcc_arena_strdup(arena, options->llc_path)
                  : NULL;
    lb->verbose = options && options->verbose;

    EtccBackend *b = etcc_arena_alloc(arena, sizeof(*b), _Alignof(EtccBackend));
    b->ops = &llvm_ops;
    b->impl = lb;
    return b;
}
