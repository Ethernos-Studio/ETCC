#include "rt_internal.h"

void *malloc(rt_size size) {
    if (size == 0) size = 1;
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void *calloc(rt_size count, rt_size size) {
    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }
    if (count > (rt_u64)-1 / size) return NULL;
    return HeapAlloc(GetProcessHeap(), RT_HEAP_ZERO_MEMORY, count * size);
}

void *realloc(void *ptr, rt_size size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return HeapReAlloc(GetProcessHeap(), 0, ptr, size);
}

void free(void *ptr) {
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}

void exit(int status) {
    ExitProcess((rt_u32)status);
}

void abort(void) {
    ExitProcess(3);
}
