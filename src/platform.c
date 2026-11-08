#include "etcc/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

bool etcc_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool is_sep(char c) { return c == '/' || c == '\\'; }

char *etcc_path_dirname(EtccArena *a, const char *path) {
    if (!path || !*path) return etcc_arena_strdup(a, ".");
    size_t len = strlen(path);
    size_t i = len;
    while (i > 0 && !is_sep(path[i - 1])) i--;
    if (i == 0) return etcc_arena_strdup(a, ".");
    if (i == 1) return etcc_arena_strndup(a, path, 1);
    if (i == 3 && path[1] == ':') return etcc_arena_strndup(a, path, 3);
    while (i > 1 && is_sep(path[i - 1])) i--;
    return etcc_arena_strndup(a, path, i);
}

char *etcc_path_join(EtccArena *a, const char *dir, const char *name) {
    if (!name) name = "";
    if (name[0] && (is_sep(name[0]) || (name[1] == ':' && name[0] && name[2])))
        return etcc_arena_strdup(a, name);
    if (!dir || !*dir || strcmp(dir, ".") == 0)
        return etcc_arena_strdup(a, name);

    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    bool need_sep = dl > 0 && !is_sep(dir[dl - 1]);
    char *p = etcc_arena_alloc(a, dl + (need_sep ? 1 : 0) + nl + 1, 1);
    memcpy(p, dir, dl);
    size_t off = dl;
    if (need_sep) p[off++] = '\\';
    memcpy(p + off, name, nl);
    p[off + nl] = '\0';
    return p;
}

const char *etcc_path_filename(const char *path) {
    if (!path) return "";
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (is_sep(*p)) base = p + 1;
    }
    return base;
}

char *etcc_path_canonical(EtccArena *a, const char *path) {
#ifdef _WIN32
    char *full = _fullpath(NULL, path, 0);
    if (full) {
        char *copy = etcc_arena_strdup(a, full);
        free(full);
        return copy;
    }
#endif
    return etcc_arena_strdup(a, path);
}

char *etcc_get_executable_dir(EtccArena *a) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return etcc_arena_strdup(a, ".");
    return etcc_path_dirname(a, buf);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return etcc_arena_strdup(a, ".");
    buf[n] = '\0';
    return etcc_path_dirname(a, buf);
#endif
}

char *etcc_make_temp_path(EtccArena *a, const char *extension) {
#ifdef _WIN32
    char dir[4096];
    DWORD n = GetCurrentDirectoryA((DWORD)sizeof(dir), dir);
    if (n == 0 || n >= sizeof(dir)) {
        char temp[MAX_PATH + 1];
        DWORD t = GetTempPathA(MAX_PATH, temp);
        if (t == 0 || t > MAX_PATH) strcpy(dir, ".");
        else strcpy(dir, temp);
    }
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    const char *dir = ".";
    unsigned long pid = (unsigned long)getpid();
#endif
    static unsigned long counter = 0;
    counter++;
    char name[128];
    snprintf(name, sizeof(name), ".etcc-%lu-%lu.%s", pid, counter,
             extension && *extension ? extension : "tmp");
    return etcc_path_join(a, dir, name);
}

char *etcc_find_program(EtccArena *a, const char *name) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = SearchPathA(NULL, name, ".exe", (DWORD)sizeof(buf), buf, NULL);
    if (n > 0 && n < sizeof(buf)) return etcc_arena_strdup(a, buf);

    char *path = getenv("PATH");
    if (path) {
        const char *p = path;
        while (*p) {
            const char *end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len && len < 4000) {
                char candidate[4096];
                memcpy(candidate, p, len);
                candidate[len] = '\0';
                char *joined = etcc_path_join(a, candidate, name);
                if (etcc_file_exists(joined)) return joined;
                char *with_ext = etcc_arena_printf(a, "%s.exe", joined);
                if (etcc_file_exists(with_ext)) return with_ext;
            }
            if (!end) break;
            p = end + 1;
        }
    }
    return NULL;
#else
    (void)a;
    (void)name;
    return NULL;
#endif
}

bool etcc_run_process(char *const argv[], bool verbose) {
    if (verbose) {
        fputs("etcc: run:", stderr);
        for (size_t i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
        fputc('\n', stderr);
    }
#ifdef _WIN32
    EtccStrBuf cmd;
    etcc_sb_init(&cmd);
    for (size_t i = 0; argv[i]; i++) {
        if (i) etcc_sb_append_char(&cmd, ' ');
        const char *arg = argv[i];
        bool need_quotes = (*arg == '\0') || strpbrk(arg, " \t\n\v\"") != NULL;
        if (!need_quotes) {
            etcc_sb_append_cstr(&cmd, arg);
            continue;
        }
        etcc_sb_append_char(&cmd, '"');
        size_t backslashes = 0;
        for (const char *p = arg;; p++) {
            if (*p == '\\') {
                backslashes++;
                continue;
            }
            if (*p == '"') {
                for (size_t k = 0; k < backslashes * 2 + 1; k++)
                    etcc_sb_append_char(&cmd, '\\');
                etcc_sb_append_char(&cmd, '"');
                backslashes = 0;
                continue;
            }
            if (*p == '\0') {
                for (size_t k = 0; k < backslashes * 2; k++)
                    etcc_sb_append_char(&cmd, '\\');
                break;
            }
            for (size_t k = 0; k < backslashes; k++)
                etcc_sb_append_char(&cmd, '\\');
            backslashes = 0;
            etcc_sb_append_char(&cmd, *p);
        }
        etcc_sb_append_char(&cmd, '"');
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    BOOL created = CreateProcessA(NULL, cmd.data, NULL, NULL, TRUE, 0, NULL,
                                  NULL, &si, &pi);
    if (!created) {
        DWORD err = GetLastError();
        fprintf(stderr,
                "etcc: error: could not execute '%s' (Windows error %lu)\n",
                argv[0], (unsigned long)err);
        etcc_sb_free(&cmd);
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    etcc_sb_free(&cmd);
    return code == 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return false;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

void etcc_remove_file_if_exists(const char *path) {
    if (!path || !*path) return;
#ifdef _WIN32
    DeleteFileA(path);
#else
    remove(path);
#endif
}
