/*
 * Ethernos Tiny C Compiler (ETCC)
 * sema.h - semantic analysis and lowering to typed IR.
 */
#ifndef ETCC_SEMA_H
#define ETCC_SEMA_H

#include "etcc/ast.h"
#include "etcc/ir.h"

/*
 * Analyzes a translation unit and fills `module`.
 * Returns false if diagnostics with error severity were produced.
 */
bool etcc_sema_run(EtccAstNode *tu, EtccArena *arena, EtccDiagnostics *diag,
                   IrModule *module);

#endif /* ETCC_SEMA_H */
