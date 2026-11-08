#include "etcc/backend.h"

#include <stdio.h>
#include <string.h>

static const EtccBackendEntry backends[] = {
    {"llvm", etcc_backend_llvm_create},
};

const EtccBackendEntry *etcc_backend_find(const char *name) {
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (strcmp(backends[i].name, name) == 0) return &backends[i];
    }
    return NULL;
}

void etcc_backend_print_available(FILE *out) {
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++)
        fprintf(out, "  %s\n", backends[i].name);
}
