#include "rt_internal.h"

extern int main(int argc, char **argv);

__declspec(dllimport) char *__stdcall GetCommandLineA(void);

static char *next_arg(char **cursor) {
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        *cursor = p;
        return NULL;
    }

    char *out = (char *)malloc(strlen(p) + 1);
    if (!out) {
        *cursor = p + strlen(p);
        return NULL;
    }
    char *o = out;
    int in_quotes = 0;

    while (*p) {
        char c = *p;
        if (c == '"') {
            in_quotes = !in_quotes;
            p++;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t')) break;
        if (c == '\\' && p[1] == '"') {
            *o++ = '"';
            p += 2;
            continue;
        }
        *o++ = c;
        p++;
    }
    *o = '\0';
    *cursor = p;
    return out;
}

static void build_args(int *argc_out, char ***argv_out) {
    *argc_out = 0;
    *argv_out = NULL;

    char *cmdline = GetCommandLineA();
    if (!cmdline) return;
    rt_size n = strlen(cmdline);
    char *copy = (char *)malloc(n + 1);
    if (!copy) return;
    memcpy(copy, cmdline, n + 1);

    int cap = 8;
    int argc = 0;
    char **argv = (char **)malloc((rt_size)cap * sizeof(char *));
    if (!argv) {
        free(copy);
        return;
    }

    char *p = copy;
    char *arg;
    while ((arg = next_arg(&p)) != NULL) {
        if (argc + 2 > cap) {
            cap *= 2;
            char **grown = (char **)realloc(argv, (rt_size)cap * sizeof(char *));
            if (!grown) {
                free(copy);
                free(argv);
                return;
            }
            argv = grown;
        }
        argv[argc++] = arg;
    }
    argv[argc] = NULL;
    free(copy);

    *argc_out = argc;
    *argv_out = argv;
}

__declspec(noreturn) void etcc_entry(void) {
    int argc = 0;
    char **argv = NULL;
    build_args(&argc, &argv);
    int status = main(argc, argv);
    ExitProcess((rt_u32)status);
}
