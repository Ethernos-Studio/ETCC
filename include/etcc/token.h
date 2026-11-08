/*
 * Ethernos Tiny C Compiler (ETCC)
 * token.h - lexical tokens.
 */
#ifndef ETCC_TOKEN_H
#define ETCC_TOKEN_H

#include "etcc/source.h"

typedef enum EtccTokenKind {
    TK_EOF = 0,
    TK_INVALID,

    TK_IDENT,
    TK_INT_LIT,
    TK_CHAR_LIT,
    TK_STRING_LIT,

    /* type / storage keywords */
    TK_KW_AUTO,
    TK_KW_BREAK,
    TK_KW_CASE,
    TK_KW_CHAR,
    TK_KW_CONST,
    TK_KW_CONTINUE,
    TK_KW_DEFAULT,
    TK_KW_DO,
    TK_KW_DOUBLE,
    TK_KW_ELSE,
    TK_KW_ENUM,
    TK_KW_EXTERN,
    TK_KW_FLOAT,
    TK_KW_FOR,
    TK_KW_GOTO,
    TK_KW_IF,
    TK_KW_INLINE,
    TK_KW_INT,
    TK_KW_LONG,
    TK_KW_REGISTER,
    TK_KW_RESTRICT,
    TK_KW_RETURN,
    TK_KW_SHORT,
    TK_KW_SIGNED,
    TK_KW_SIZEOF,
    TK_KW_STATIC,
    TK_KW_STRUCT,
    TK_KW_SWITCH,
    TK_KW_TYPEDEF,
    TK_KW_UNION,
    TK_KW_UNSIGNED,
    TK_KW_VOID,
    TK_KW_VOLATILE,
    TK_KW_WHILE,
    TK_KW_BOOL,

    /* punctuators */
    TK_LPAREN,      /* ( */
    TK_RPAREN,      /* ) */
    TK_LBRACE,      /* { */
    TK_RBRACE,      /* } */
    TK_LBRACKET,    /* [ */
    TK_RBRACKET,    /* ] */
    TK_SEMICOLON,   /* ; */
    TK_COMMA,       /* , */
    TK_DOT,         /* . */
    TK_ARROW,       /* -> */
    TK_ELLIPSIS,    /* ... */
    TK_QUESTION,    /* ? */
    TK_COLON,       /* : */

    TK_PLUS,        /* + */
    TK_MINUS,       /* - */
    TK_STAR,        /* * */
    TK_SLASH,       /* / */
    TK_PERCENT,     /* % */
    TK_AMP,         /* & */
    TK_PIPE,        /* | */
    TK_CARET,       /* ^ */
    TK_TILDE,       /* ~ */
    TK_BANG,        /* ! */
    TK_ASSIGN,      /* = */
    TK_LT,          /* < */
    TK_GT,          /* > */

    TK_PLUS_PLUS,
    TK_MINUS_MINUS,
    TK_PLUS_ASSIGN,
    TK_MINUS_ASSIGN,
    TK_STAR_ASSIGN,
    TK_SLASH_ASSIGN,
    TK_PERCENT_ASSIGN,
    TK_AMP_ASSIGN,
    TK_PIPE_ASSIGN,
    TK_CARET_ASSIGN,
    TK_SHL,
    TK_SHR,
    TK_SHL_ASSIGN,
    TK_SHR_ASSIGN,
    TK_EQ,
    TK_NE,
    TK_LE,
    TK_GE,
    TK_AND_AND,
    TK_OR_OR,

    TK_KIND_COUNT
} EtccTokenKind;

typedef struct EtccToken {
    EtccTokenKind kind;
    EtccLoc loc;
    union {
        struct {
            const char *data;
            size_t len;
        } ident;
        struct {
            uint64_t value;
            bool is_unsigned;
            bool is_long;
        } int_lit;
        struct {
            int64_t value;
        } char_lit;
        struct {
            const char *data; /* decoded bytes */
            size_t len;       /* does not include implicit NUL */
        } string_lit;
    } v;
} EtccToken;

const char *etcc_token_kind_name(EtccTokenKind kind);
EtccTokenKind etcc_keyword_kind(const char *s, size_t len);
bool etcc_token_is_type_keyword(EtccTokenKind kind);
bool etcc_token_is_storage_keyword(EtccTokenKind kind);

#endif /* ETCC_TOKEN_H */
