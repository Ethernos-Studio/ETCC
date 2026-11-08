/*
 * Ethernos Tiny C Compiler (ETCC)
 * backend.h - pluggable code-generation backend interface.
 */
#ifndef ETCC_BACKEND_H
#define ETCC_BACKEND_H

#include "etcc/diag.h"
#include "etcc/ir.h"

#include <stdio.h>

typedef struct EtccBackend EtccBackend;

typedef struct EtccBackendOptions {
    const char *clang_path;
    bool verbose;
} EtccBackendOptions;

typedef struct EtccBackendOps {
    const char *name;
    const char *description;

    /* Emit a native COFF object for the Windows x64 target. */
    bool (*emit_object)(EtccBackend *b, const IrModule *m, const char *obj_path,
                        EtccDiagnostics *diag);

    /* Emit the backend's textual IR/assembly representation. */
    bool (*emit_ir)(EtccBackend *b, const IrModule *m, const char *path,
                    EtccDiagnostics *diag);

    void (*destroy)(EtccBackend *b);
} EtccBackendOps;

struct EtccBackend {
    const EtccBackendOps *ops;
    void *impl;
};

typedef EtccBackend *(*EtccBackendFactory)(EtccArena *arena,
                                           EtccDiagnostics *diag,
                                           const EtccBackendOptions *options);

typedef struct EtccBackendEntry {
    const char *name;
    EtccBackendFactory create;
} EtccBackendEntry;

const EtccBackendEntry *etcc_backend_find(const char *name);
void etcc_backend_print_available(FILE *out);

EtccBackend *etcc_backend_llvm_create(EtccArena *arena, EtccDiagnostics *diag,
                                      const EtccBackendOptions *options);

#endif /* ETCC_BACKEND_H */
