#include "etcc/ast.h"

#include <string.h>

EtccAstNode *etcc_ast_new(EtccArena *a, EtccAstKind kind, EtccLoc loc) {
    EtccAstNode *n = etcc_arena_alloc(a, sizeof(*n), _Alignof(EtccAstNode));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->loc = loc;
    return n;
}

const char *etcc_ast_kind_name(EtccAstKind kind) {
    switch (kind) {
    case AST_TRANSLATION_UNIT: return "translation-unit";
    case AST_FUNC_DEF: return "function-definition";
    case AST_DECL: return "declaration";
    case AST_PARAM: return "parameter";
    case AST_COMPOUND: return "compound-statement";
    case AST_RETURN: return "return";
    case AST_IF: return "if";
    case AST_WHILE: return "while";
    case AST_DO: return "do";
    case AST_FOR: return "for";
    case AST_BREAK: return "break";
    case AST_CONTINUE: return "continue";
    case AST_EXPR_STMT: return "expression-statement";
    case AST_EMPTY: return "empty-statement";
    case AST_INT_LIT: return "integer-literal";
    case AST_CHAR_LIT: return "character-literal";
    case AST_STRING_LIT: return "string-literal";
    case AST_IDENT: return "identifier";
    case AST_CALL: return "call";
    case AST_BINARY: return "binary-expression";
    case AST_UNARY: return "unary-expression";
    case AST_ASSIGN: return "assignment";
    case AST_COND: return "conditional-expression";
    case AST_CAST: return "cast";
    case AST_INDEX: return "index";
    case AST_MEMBER: return "member";
    case AST_SIZEOF: return "sizeof";
    case AST_COMMA: return "comma";
    default: return "<ast>";
    }
}
