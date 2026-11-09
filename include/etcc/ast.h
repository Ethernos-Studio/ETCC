/*
 * Ethernos Tiny C Compiler (ETCC)
 * ast.h - syntax tree.
 */
#ifndef ETCC_AST_H
#define ETCC_AST_H

#include "etcc/ctype.h"
#include "etcc/token.h"

typedef enum EtccStorageClass {
    STORAGE_NONE = 0,
    STORAGE_STATIC,
    STORAGE_EXTERN,
    STORAGE_TYPEDEF,
    STORAGE_AUTO,
    STORAGE_REGISTER,
} EtccStorageClass;

typedef enum EtccAstKind {
    AST_TRANSLATION_UNIT,
    AST_FUNC_DEF,
    AST_DECL,
    AST_PARAM,
    AST_COMPOUND,
    AST_RETURN,
    AST_IF,
    AST_WHILE,
    AST_DO,
    AST_FOR,
    AST_SWITCH,
    AST_CASE,
    AST_DEFAULT,
    AST_LABEL,
    AST_GOTO,
    AST_BREAK,
    AST_CONTINUE,
    AST_EXPR_STMT,
    AST_EMPTY,
    AST_INIT_LIST,
    AST_ENUM_CONST,

    AST_INT_LIT,
    AST_CHAR_LIT,
    AST_STRING_LIT,
    AST_IDENT,
    AST_CALL,
    AST_BINARY,
    AST_UNARY,
    AST_ASSIGN,
    AST_COND,
    AST_CAST,
    AST_INDEX,
    AST_MEMBER,
    AST_SIZEOF,
    AST_COMMA,
} EtccAstKind;

typedef struct EtccAstNode EtccAstNode;

struct EtccAstNode {
    EtccAstKind kind;
    EtccLoc loc;
    EtccCType *type;
    EtccAstNode *next; /* linked list of declarations/statements/args */

    union {
        struct {
            EtccAstNode *decls;
        } tu;

        struct {
            char *name;
            EtccCType *type;
            EtccAstNode *params;
            EtccAstNode *body;
            EtccStorageClass storage;
        } func;

        struct {
            char *name;
            EtccCType *type;
            EtccStorageClass storage;
            EtccAstNode *init;
            bool is_definition;
        } decl;

        struct {
            char *name;
            EtccCType *type;
        } param;

        struct {
            EtccAstNode *stmts;
        } compound;

        struct {
            EtccAstNode *expr;
        } ret;

        struct {
            EtccAstNode *cond;
            EtccAstNode *then_branch;
            EtccAstNode *else_branch;
        } if_;

        struct {
            EtccAstNode *cond;
            EtccAstNode *body;
        } while_;

        struct {
            EtccAstNode *body;
            EtccAstNode *cond;
        } do_;

        struct {
            EtccAstNode *init;
            EtccAstNode *cond;
            EtccAstNode *step;
            EtccAstNode *body;
        } for_;

        struct {
            EtccAstNode *cond;
            EtccAstNode *body;
        } switch_;

        struct {
            EtccAstNode *value;
            EtccAstNode *body;
        } case_;

        struct {
            EtccAstNode *body;
        } default_;

        struct {
            const char *name;
            size_t len;
            EtccAstNode *body;
        } label;

        struct {
            const char *name;
            size_t len;
        } goto_;

        struct {
            const char *name;
            size_t len;
            int64_t value;
        } enum_const;

        struct {
            EtccAstNode *expr;
        } expr_stmt;

        struct {
            EtccAstNode *items;
        } init_list;

        struct {
            uint64_t value;
            bool is_unsigned;
            int long_count; /* 0, 1 (L), 2 (LL) */
            bool is_decimal;
        } int_lit;

        struct {
            int64_t value;
        } char_lit;

        struct {
            const char *data;
            size_t len;
        } string_lit;

        struct {
            const char *name;
            size_t len;
        } ident;

        struct {
            EtccAstNode *callee;
            EtccAstNode *args;
        } call;

        struct {
            EtccTokenKind op;
            EtccAstNode *lhs;
            EtccAstNode *rhs;
        } binary;

        struct {
            EtccTokenKind op;
            EtccAstNode *operand;
            bool postfix;
        } unary;

        struct {
            EtccTokenKind op;
            EtccAstNode *target;
            EtccAstNode *value;
        } assign;

        struct {
            EtccAstNode *cond;
            EtccAstNode *then_expr;
            EtccAstNode *else_expr;
        } cond;

        struct {
            EtccCType *target;
            EtccAstNode *operand;
        } cast;

        struct {
            EtccAstNode *base;
            EtccAstNode *index;
        } index;

        struct {
            EtccAstNode *base;
            const char *field;
            size_t field_len;
            bool arrow;
        } member;

        struct {
            EtccCType *type; /* sizeof(type) when non-NULL */
            EtccAstNode *operand;
        } sizeof_;

        struct {
            EtccAstNode *lhs;
            EtccAstNode *rhs;
        } comma;
    } u;
};

EtccAstNode *etcc_ast_new(EtccArena *a, EtccAstKind kind, EtccLoc loc);
const char *etcc_ast_kind_name(EtccAstKind kind);

#endif /* ETCC_AST_H */
