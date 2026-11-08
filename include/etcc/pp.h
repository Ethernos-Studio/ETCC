/*
 * Ethernos Tiny C Compiler (ETCC)
 * pp.h - C preprocessor (includes, conditionals, object-like macros).
 */
#ifndef ETCC_PP_H
#define ETCC_PP_H

#include "etcc/common.h"
#include "etcc/diag.h"
#include "etcc/source.h"

typedef struct EtccMacro {
    char *name;
    char *body;
    size_t body_len;
    bool function_like;
} EtccMacro;

typedef struct EtccMacroTable {
    EtccMacro *items;
    size_t len;
    size_t cap;
} EtccMacroTable;

const EtccMacro *etcc_macro_find(const EtccMacroTable *t, const char *name,
                                 size_t len);
bool etcc_macro_define(EtccArena *a, EtccMacroTable *t, const char *name,
                       size_t name_len, const char *body, size_t body_len,
                       bool function_like);
bool etcc_macro_undef(EtccMacroTable *t, const char *name, size_t len);

typedef struct EtccPP {
    EtccArena *arena;
    EtccDiagnostics *diag;

    EtccMacroTable macros;
    ETCC_VEC(char *) include_dirs;

    EtccStrBuf out;
    EtccSourceMap map;

    EtccSourceFile **files;
    size_t nfiles;
    size_t cap_files;

    ETCC_VEC(char *) once_files;

    void *conds;
    size_t nconds;
    size_t cap_conds;

    int depth;
    bool failed;
} EtccPP;

void etcc_pp_init(EtccPP *pp, EtccArena *arena, EtccDiagnostics *diag);
void etcc_pp_add_include_dir(EtccPP *pp, const char *dir);
bool etcc_pp_run_file(EtccPP *pp, const char *path);
bool etcc_pp_run_buffer(EtccPP *pp, const char *name, const char *data,
                        size_t len);
const char *etcc_pp_text(const EtccPP *pp, size_t *len);
void etcc_pp_destroy(EtccPP *pp);

#endif /* ETCC_PP_H */
