/*
 * Ethernos Tiny C Compiler (ETCC)
 * lexer.h - tokenizer with object-like macro expansion.
 */
#ifndef ETCC_LEXER_H
#define ETCC_LEXER_H

#include "etcc/pp.h"
#include "etcc/token.h"

typedef struct EtccMacroFrame EtccMacroFrame;

typedef struct EtccLexer {
    EtccArena *arena;
    EtccDiagnostics *diag;
    const EtccMacroTable *macros;
    const EtccSourceMap *map;

    const char *data;
    size_t len;
    size_t pos;

    EtccMacroFrame *frames;
    const char *active[128];
    size_t nactive;
} EtccLexer;

void etcc_lexer_init(EtccLexer *lx, EtccArena *arena, EtccDiagnostics *diag,
                     const char *data, size_t len, const EtccSourceMap *map,
                     const EtccMacroTable *macros);
EtccToken etcc_lexer_next(EtccLexer *lx);

#endif /* ETCC_LEXER_H */
