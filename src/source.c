#include "etcc/source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void etcc_source_map_init(EtccSourceMap *map, const EtccSourceFile *fallback) {
    map->segs = NULL;
    map->len = 0;
    map->cap = 0;
    map->fallback = fallback;
}

bool etcc_source_map_add(EtccSourceMap *map, size_t offset,
                         const EtccSourceFile *file, uint32_t line) {
    if (map->len == map->cap) {
        size_t ncap = map->cap ? map->cap * 2 : 32;
        EtccSourceSeg *p = realloc(map->segs, ncap * sizeof(*p));
        if (!p) return false;
        map->segs = p;
        map->cap = ncap;
    }
    map->segs[map->len++] = (EtccSourceSeg){offset, file, line};
    return true;
}

EtccLoc etcc_source_map_lookup(const EtccSourceMap *map, size_t offset) {
    if (!map || map->len == 0) {
        return (EtccLoc){map ? map->fallback : NULL, 1, (uint32_t)(offset + 1)};
    }

    size_t lo = 0, hi = map->len;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (map->segs[mid].offset <= offset) lo = mid;
        else hi = mid;
    }
    if (map->segs[0].offset > offset) {
        return (EtccLoc){map->segs[0].file, map->segs[0].line, 1};
    }
    const EtccSourceSeg *seg = &map->segs[lo];
    size_t delta = offset - seg->offset;
    if (delta > UINT32_MAX - 1) delta = UINT32_MAX - 1;
    return (EtccLoc){seg->file, seg->line, (uint32_t)(delta + 1)};
}

char *etcc_read_file(const char *path, size_t *out_len, const char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = errno == ENOENT ? "file not found" : strerror(errno);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *err = "cannot seek file";
        return NULL;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        *err = "cannot determine file size";
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        *err = "cannot seek file";
        return NULL;
    }

    size_t len = (size_t)end;
    char *data = malloc(len + 1);
    if (!data) {
        fclose(f);
        *err = "out of memory";
        return NULL;
    }
    if (len && fread(data, 1, len, f) != len) {
        free(data);
        fclose(f);
        *err = "cannot read file";
        return NULL;
    }
    fclose(f);
    data[len] = '\0';

    /* Skip a UTF-8 BOM. */
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        memmove(data, data + 3, len - 3 + 1);
        len -= 3;
    }

    *out_len = len;
    return data;
}
