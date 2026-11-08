/*
 * Ethernos Tiny C Compiler (ETCC)
 * parser.h - recursive-descent parser for the supported C subset.
 */
#ifndef ETCC_PARSER_H
#define ETCC_PARSER_H

#include "etcc/ast.h"
#include "etcc/pp.h"

/*
 * Parses preprocessed source text. The returned node is always an
 * AST_TRANSLATION_UNIT; on errors it may contain partial declarations.
 */
EtccAstNode *etcc_parse_translation_unit(EtccArena *arena,
                                         EtccDiagnostics *diag,
                                         const char *text, size_t len,
                                         const EtccSourceMap *map,
                                         const EtccMacroTable *macros);

#endif /* ETCC_PARSER_H */
