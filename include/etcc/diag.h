/*
 * Ethernos Tiny C Compiler (ETCC)
 * diag.h - diagnostics with source locations.
 */
#ifndef ETCC_DIAG_H
#define ETCC_DIAG_H

#include "etcc/source.h"

typedef struct EtccDiagnostics {
    int errors;
    int warnings;
    int notes;
    int max_errors;
    bool show_source;
} EtccDiagnostics;

void etcc_diag_init(EtccDiagnostics *d);
void etcc_diag_error(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...);
void etcc_diag_warning(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...);
void etcc_diag_note(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...);
void etcc_diag_error_global(EtccDiagnostics *d, const char *fmt, ...);

#endif /* ETCC_DIAG_H */
