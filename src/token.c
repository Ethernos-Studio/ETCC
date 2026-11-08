#include "etcc/token.h"

#include <string.h>

typedef struct {
    const char *name;
    EtccTokenKind kind;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    {"auto", TK_KW_AUTO},         {"break", TK_KW_BREAK},
    {"case", TK_KW_CASE},         {"char", TK_KW_CHAR},
    {"const", TK_KW_CONST},       {"continue", TK_KW_CONTINUE},
    {"default", TK_KW_DEFAULT},   {"do", TK_KW_DO},
    {"double", TK_KW_DOUBLE},     {"else", TK_KW_ELSE},
    {"enum", TK_KW_ENUM},         {"extern", TK_KW_EXTERN},
    {"float", TK_KW_FLOAT},       {"for", TK_KW_FOR},
    {"goto", TK_KW_GOTO},         {"if", TK_KW_IF},
    {"inline", TK_KW_INLINE},     {"int", TK_KW_INT},
    {"long", TK_KW_LONG},         {"register", TK_KW_REGISTER},
    {"restrict", TK_KW_RESTRICT}, {"return", TK_KW_RETURN},
    {"short", TK_KW_SHORT},       {"signed", TK_KW_SIGNED},
    {"sizeof", TK_KW_SIZEOF},     {"static", TK_KW_STATIC},
    {"struct", TK_KW_STRUCT},     {"switch", TK_KW_SWITCH},
    {"typedef", TK_KW_TYPEDEF},   {"union", TK_KW_UNION},
    {"unsigned", TK_KW_UNSIGNED}, {"void", TK_KW_VOID},
    {"volatile", TK_KW_VOLATILE}, {"while", TK_KW_WHILE},
    {"_Bool", TK_KW_BOOL},
};

const char *etcc_token_kind_name(EtccTokenKind kind) {
    switch (kind) {
    case TK_EOF: return "end of file";
    case TK_INVALID: return "invalid token";
    case TK_IDENT: return "identifier";
    case TK_INT_LIT: return "integer literal";
    case TK_CHAR_LIT: return "character literal";
    case TK_STRING_LIT: return "string literal";
    case TK_KW_AUTO: return "auto";
    case TK_KW_BREAK: return "break";
    case TK_KW_CASE: return "case";
    case TK_KW_CHAR: return "char";
    case TK_KW_CONST: return "const";
    case TK_KW_CONTINUE: return "continue";
    case TK_KW_DEFAULT: return "default";
    case TK_KW_DO: return "do";
    case TK_KW_DOUBLE: return "double";
    case TK_KW_ELSE: return "else";
    case TK_KW_ENUM: return "enum";
    case TK_KW_EXTERN: return "extern";
    case TK_KW_FLOAT: return "float";
    case TK_KW_FOR: return "for";
    case TK_KW_GOTO: return "goto";
    case TK_KW_IF: return "if";
    case TK_KW_INLINE: return "inline";
    case TK_KW_INT: return "int";
    case TK_KW_LONG: return "long";
    case TK_KW_REGISTER: return "register";
    case TK_KW_RESTRICT: return "restrict";
    case TK_KW_RETURN: return "return";
    case TK_KW_SHORT: return "short";
    case TK_KW_SIGNED: return "signed";
    case TK_KW_SIZEOF: return "sizeof";
    case TK_KW_STATIC: return "static";
    case TK_KW_STRUCT: return "struct";
    case TK_KW_SWITCH: return "switch";
    case TK_KW_TYPEDEF: return "typedef";
    case TK_KW_UNION: return "union";
    case TK_KW_UNSIGNED: return "unsigned";
    case TK_KW_VOID: return "void";
    case TK_KW_VOLATILE: return "volatile";
    case TK_KW_WHILE: return "while";
    case TK_KW_BOOL: return "_Bool";
    case TK_LPAREN: return "(";
    case TK_RPAREN: return ")";
    case TK_LBRACE: return "{";
    case TK_RBRACE: return "}";
    case TK_LBRACKET: return "[";
    case TK_RBRACKET: return "]";
    case TK_SEMICOLON: return ";";
    case TK_COMMA: return ",";
    case TK_DOT: return ".";
    case TK_ARROW: return "->";
    case TK_ELLIPSIS: return "...";
    case TK_QUESTION: return "?";
    case TK_COLON: return ":";
    case TK_PLUS: return "+";
    case TK_MINUS: return "-";
    case TK_STAR: return "*";
    case TK_SLASH: return "/";
    case TK_PERCENT: return "%";
    case TK_AMP: return "&";
    case TK_PIPE: return "|";
    case TK_CARET: return "^";
    case TK_TILDE: return "~";
    case TK_BANG: return "!";
    case TK_ASSIGN: return "=";
    case TK_LT: return "<";
    case TK_GT: return ">";
    case TK_PLUS_PLUS: return "++";
    case TK_MINUS_MINUS: return "--";
    case TK_PLUS_ASSIGN: return "+=";
    case TK_MINUS_ASSIGN: return "-=";
    case TK_STAR_ASSIGN: return "*=";
    case TK_SLASH_ASSIGN: return "/=";
    case TK_PERCENT_ASSIGN: return "%=";
    case TK_AMP_ASSIGN: return "&=";
    case TK_PIPE_ASSIGN: return "|=";
    case TK_CARET_ASSIGN: return "^=";
    case TK_SHL: return "<<";
    case TK_SHR: return ">>";
    case TK_SHL_ASSIGN: return "<<=";
    case TK_SHR_ASSIGN: return ">>=";
    case TK_EQ: return "==";
    case TK_NE: return "!=";
    case TK_LE: return "<=";
    case TK_GE: return ">=";
    case TK_AND_AND: return "&&";
    case TK_OR_OR: return "||";
    default: return "<token>";
    }
}

EtccTokenKind etcc_keyword_kind(const char *s, size_t len) {
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        size_t n = strlen(keywords[i].name);
        if (n == len && memcmp(keywords[i].name, s, n) == 0)
            return keywords[i].kind;
    }
    return TK_IDENT;
}

bool etcc_token_is_type_keyword(EtccTokenKind kind) {
    switch (kind) {
    case TK_KW_CHAR:
    case TK_KW_SHORT:
    case TK_KW_INT:
    case TK_KW_LONG:
    case TK_KW_FLOAT:
    case TK_KW_DOUBLE:
    case TK_KW_SIGNED:
    case TK_KW_UNSIGNED:
    case TK_KW_VOID:
    case TK_KW_BOOL:
    case TK_KW_STRUCT:
    case TK_KW_UNION:
    case TK_KW_ENUM:
    case TK_KW_CONST:
    case TK_KW_VOLATILE:
    case TK_KW_RESTRICT:
        return true;
    default:
        return false;
    }
}

bool etcc_token_is_storage_keyword(EtccTokenKind kind) {
    switch (kind) {
    case TK_KW_AUTO:
    case TK_KW_EXTERN:
    case TK_KW_REGISTER:
    case TK_KW_STATIC:
    case TK_KW_TYPEDEF:
        return true;
    default:
        return false;
    }
}
