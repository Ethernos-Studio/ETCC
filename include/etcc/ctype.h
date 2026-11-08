/*
 * Ethernos Tiny C Compiler (ETCC)
 * ctype.h - C type representation (Windows LLP64 data model).
 */
#ifndef ETCC_CTYPE_H
#define ETCC_CTYPE_H

#include "etcc/common.h"

typedef enum EtccCTypeKind {
    CT_VOID,
    CT_BOOL,
    CT_CHAR,
    CT_SHORT,
    CT_INT,
    CT_LONG,
    CT_LLONG,
    CT_FLOAT,
    CT_DOUBLE,
    CT_PTR,
    CT_ARRAY,
    CT_FUNC,
    CT_STRUCT,
    CT_UNION,
} EtccCTypeKind;

typedef struct EtccCType EtccCType;
typedef struct EtccField EtccField;

struct EtccField {
    const char *name;
    EtccCType *type;
    size_t offset;
    EtccField *next;
};

struct EtccCType {
    EtccCTypeKind kind;
    size_t size;
    size_t align;
    bool is_unsigned;
    bool is_const;

    EtccCType *base;       /* pointer/array element, or function result */
    uint64_t array_len;    /* CT_ARRAY; 0 means incomplete */

    EtccCType **params;    /* CT_FUNC */
    size_t nparams;
    bool variadic;

    const char *tag;       /* CT_STRUCT/CT_UNION tag, or NULL */
    EtccField *fields;     /* CT_STRUCT/CT_UNION members */
    bool is_complete;      /* CT_STRUCT/CT_UNION definition seen */
    const void *identity;  /* stable identity for tag compatibility */
};

EtccCType *etcc_type_builtin(EtccCTypeKind kind);
EtccCType *etcc_type_ptr(EtccArena *a, EtccCType *base);
EtccCType *etcc_type_array(EtccArena *a, EtccCType *base, uint64_t len);
EtccCType *etcc_type_func(EtccArena *a, EtccCType *result, EtccCType **params,
                          size_t nparams, bool variadic);
EtccCType *etcc_type_struct(EtccArena *a, EtccCTypeKind kind,
                            const char *tag);
EtccCType *etcc_type_copy(EtccArena *a, const EtccCType *t);

EtccField *etcc_field_new(EtccArena *a, const char *name, EtccCType *type);
bool etcc_type_define(EtccArena *a, EtccCType *t, EtccField *fields);
EtccField *etcc_type_find_field(const EtccCType *t, const char *name,
                                size_t name_len);

bool etcc_type_equal(const EtccCType *a, const EtccCType *b);
bool etcc_type_is_integer(const EtccCType *t);
bool etcc_type_is_arithmetic(const EtccCType *t);
bool etcc_type_is_scalar(const EtccCType *t);
bool etcc_type_is_pointer(const EtccCType *t);
bool etcc_type_is_aggregate(const EtccCType *t);
bool etcc_type_is_signed(const EtccCType *t);
int etcc_type_rank(const EtccCType *t);
EtccCType *etcc_type_decay(const EtccCType *t);
EtccCType *etcc_type_common_arithmetic(EtccCType *a, EtccCType *b);
size_t etcc_type_sizeof(const EtccCType *t);
size_t etcc_type_alignof(const EtccCType *t);
const char *etcc_type_format(const EtccCType *t, char *buf, size_t size);

#endif /* ETCC_CTYPE_H */
