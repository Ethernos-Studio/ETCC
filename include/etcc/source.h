/*
 * Ethernos Tiny C Compiler (ETCC)
 * source.h - source files and preprocessed source maps.
 */
#ifndef ETCC_SOURCE_H
#define ETCC_SOURCE_H

#include "etcc/common.h"

typedef struct EtccSourceFile {
    char *path;          /* display path, stable for the process lifetime */
    char *canonical;     /* canonical path used for #pragma once */
    char *data;          /* NUL-terminated file contents */
    size_t len;
} EtccSourceFile;

typedef struct EtccLoc {
    const EtccSourceFile *file;
    uint32_t line;       /* 1-based */
    uint32_t col;        /* 1-based */
} EtccLoc;

#define ETCC_LOC_NONE ((EtccLoc){NULL, 0, 0})

typedef struct EtccSourceSeg {
    size_t offset;              /* offset in the preprocessed buffer */
    const EtccSourceFile *file;
    uint32_t line;              /* 1-based line in file */
} EtccSourceSeg;

typedef struct EtccSourceMap {
    EtccSourceSeg *segs;
    size_t len;
    size_t cap;
    const EtccSourceFile *fallback;
} EtccSourceMap;

void etcc_source_map_init(EtccSourceMap *map, const EtccSourceFile *fallback);
bool etcc_source_map_add(EtccSourceMap *map, size_t offset,
                         const EtccSourceFile *file, uint32_t line);
EtccLoc etcc_source_map_lookup(const EtccSourceMap *map, size_t offset);

/* Returns NULL and fills `err` (static string) on failure. */
char *etcc_read_file(const char *path, size_t *out_len, const char **err);

#endif /* ETCC_SOURCE_H */
