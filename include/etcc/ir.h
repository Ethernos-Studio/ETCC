/*
 * Ethernos Tiny C Compiler (ETCC)
 * ir.h - backend-independent typed IR.
 *
 * The IR is intentionally small and fully typed.  A backend translates it
 * to a concrete machine representation; the LLVM backend serializes it to
 * textual LLVM IR and invokes clang to produce a COFF object.
 */
#ifndef ETCC_IR_H
#define ETCC_IR_H

#include "etcc/common.h"
#include "etcc/diag.h"

typedef enum IrTypeKind {
    IR_TYPE_VOID,
    IR_TYPE_I8,
    IR_TYPE_I16,
    IR_TYPE_I32,
    IR_TYPE_I64,
    IR_TYPE_PTR,
    IR_TYPE_ARRAY,
} IrTypeKind;

typedef struct IrType IrType;
struct IrType {
    IrTypeKind kind;
    size_t size;
    size_t align;
    bool is_signed;
    IrType *elem;     /* pointer pointee / array element */
    uint64_t count;   /* array length */
};

typedef struct IrValue IrValue;
typedef struct IrGlobal IrGlobal;
typedef struct IrFunction IrFunction;
typedef struct IrBlock IrBlock;
typedef struct IrInstr IrInstr;
typedef struct IrModule IrModule;

typedef enum IrValueKind {
    IR_VALUE_CONST_INT,
    IR_VALUE_CONST_NULL,
    IR_VALUE_GLOBAL,
    IR_VALUE_FUNCTION,
    IR_VALUE_INSTRUCTION,
    IR_VALUE_PARAM,
} IrValueKind;

struct IrValue {
    IrType *type;
    int id;
    IrValueKind kind;
    union {
        uint64_t i64;
        IrGlobal *global;
        IrFunction *function;
    } as;
};

typedef enum IrBinOp {
    IR_BIN_ADD,
    IR_BIN_SUB,
    IR_BIN_MUL,
    IR_BIN_SDIV,
    IR_BIN_UDIV,
    IR_BIN_SREM,
    IR_BIN_UREM,
    IR_BIN_SHL,
    IR_BIN_LSHR,
    IR_BIN_ASHR,
    IR_BIN_AND,
    IR_BIN_OR,
    IR_BIN_XOR,
} IrBinOp;

typedef enum IrCmpOp {
    IR_CMP_EQ,
    IR_CMP_NE,
    IR_CMP_SLT,
    IR_CMP_SLE,
    IR_CMP_SGT,
    IR_CMP_SGE,
    IR_CMP_ULT,
    IR_CMP_ULE,
    IR_CMP_UGT,
    IR_CMP_UGE,
} IrCmpOp;

typedef enum IrCastOp {
    IR_CAST_TRUNC,
    IR_CAST_ZEXT,
    IR_CAST_SEXT,
    IR_CAST_BITCAST,
    IR_CAST_PTRTOINT,
    IR_CAST_INTTOPTR,
} IrCastOp;

typedef enum IrOp {
    IR_OP_ALLOCA,
    IR_OP_LOAD,
    IR_OP_STORE,
    IR_OP_GEP,
    IR_OP_BIN,
    IR_OP_CMP,
    IR_OP_CAST,
    IR_OP_CALL,
} IrOp;

struct IrInstr {
    IrOp op;
    IrType *type;    /* result type, or NULL for void/no-result */
    IrValue *result; /* NULL for void/no-result */
    IrInstr *next;

    union {
        struct {
            IrType *alloc_type;
        } alloca;
        struct {
            IrValue *ptr;
        } load;
        struct {
            IrValue *ptr;
            IrValue *value;
        } store;
        struct {
            IrValue *ptr;
            IrType *elem;
            IrValue *index;
        } gep;
        struct {
            IrBinOp op;
            IrValue *lhs;
            IrValue *rhs;
        } bin;
        struct {
            IrCmpOp op;
            IrValue *lhs;
            IrValue *rhs;
        } cmp;
        struct {
            IrCastOp op;
            IrValue *value;
            IrType *target;
        } cast;
        struct {
            const char *callee;
            IrValue **args;
            size_t nargs;
            bool variadic;
        } call;
    } u;
};

typedef enum IrTermKind {
    IR_TERM_NONE,
    IR_TERM_RET,
    IR_TERM_BR,
    IR_TERM_COND_BR,
} IrTermKind;

struct IrBlock {
    char *name;
    IrInstr *first;
    IrInstr *last;
    IrTermKind term;
    IrValue *ret_value;
    IrBlock *target;
    IrBlock *then_block;
    IrBlock *else_block;
    IrValue *cond;
    IrBlock *next;
};

struct IrGlobal {
    char *name;
    IrType *type;
    bool is_const;
    bool is_internal;
    bool is_external;
    bool is_string;
    const char *init_data; /* string bytes, including trailing NUL */
    size_t init_len;
    bool has_int_init;
    uint64_t init_int;
    bool has_ptr_init;
    IrGlobal *ptr_init;
    IrGlobal *next;
};

struct IrFunction {
    char *name;
    IrType *ret_type;
    IrType **param_types;
    size_t nparams;
    bool variadic;
    bool is_definition;
    bool is_internal;
    IrValue **params;
    IrBlock *entry;
    IrBlock *blocks;
    IrFunction *next;
};

struct IrModule {
    EtccArena *arena;
    IrType *void_type;
    IrType *i8;
    IrType *i16;
    IrType *i32;
    IrType *i64;
    IrGlobal *globals;
    IrFunction *functions;
    int next_value_id;
    int next_string_id;
};

IrModule *ir_module_create(EtccArena *arena);

IrType *ir_type_void(IrModule *m);
IrType *ir_type_i8(IrModule *m);
IrType *ir_type_i16(IrModule *m);
IrType *ir_type_i32(IrModule *m);
IrType *ir_type_i64(IrModule *m);
IrType *ir_type_ptr(IrModule *m, IrType *elem);
IrType *ir_type_array(IrModule *m, IrType *elem, uint64_t count);

IrValue *ir_const_int(IrModule *m, IrType *type, uint64_t value);
IrValue *ir_const_null(IrModule *m, IrType *pointer_type);
IrValue *ir_global_value(IrModule *m, IrGlobal *g);
IrValue *ir_function_value(IrModule *m, IrFunction *f);

IrGlobal *ir_global_create(IrModule *m, const char *name, IrType *type,
                           bool is_const, bool is_internal);
IrGlobal *ir_global_find(IrModule *m, const char *name);
IrGlobal *ir_string_literal(IrModule *m, const char *data, size_t len);

IrFunction *ir_function_create(IrModule *m, const char *name, IrType *ret,
                               IrType **param_types, size_t nparams,
                               bool variadic);
IrFunction *ir_function_find(IrModule *m, const char *name);
void ir_function_begin(IrModule *m, IrFunction *fn);

IrBlock *ir_block_create(IrModule *m, IrFunction *fn, const char *name);
void ir_block_set_ret(IrModule *m, IrBlock *b, IrValue *value);
void ir_block_set_br(IrModule *m, IrBlock *b, IrBlock *target);
void ir_block_set_cond_br(IrModule *m, IrBlock *b, IrValue *cond,
                          IrBlock *then_block, IrBlock *else_block);

IrValue *ir_build_alloca(IrModule *m, IrFunction *fn, IrType *type);
IrValue *ir_build_load(IrModule *m, IrBlock *b, IrValue *ptr);
void ir_build_store(IrModule *m, IrBlock *b, IrValue *ptr, IrValue *value);
IrValue *ir_build_gep(IrModule *m, IrBlock *b, IrValue *ptr, IrType *elem,
                      IrValue *index);
IrValue *ir_build_bin(IrModule *m, IrBlock *b, IrBinOp op, IrValue *lhs,
                      IrValue *rhs);
IrValue *ir_build_cmp(IrModule *m, IrBlock *b, IrCmpOp op, IrValue *lhs,
                      IrValue *rhs);
IrValue *ir_build_cast(IrModule *m, IrBlock *b, IrCastOp op, IrValue *value,
                       IrType *target);
IrValue *ir_build_call(IrModule *m, IrBlock *b, const char *callee,
                       IrValue **args, size_t nargs, IrType *ret_type,
                       bool variadic);

bool ir_validate(IrModule *m, EtccDiagnostics *diag);
const char *ir_type_name(const IrType *t, char *buf, size_t size);

#endif /* ETCC_IR_H */
