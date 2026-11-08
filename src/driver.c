#include "etcc/backend.h"
#include "etcc/diag.h"
#include "etcc/ir.h"
#include "etcc/parser.h"
#include "etcc/platform.h"
#include "etcc/pp.h"
#include "etcc/sema.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define ETCC_VERSION "0.1.0"

typedef struct {
    char *input;
    char *output;
    bool compile_only;
    bool emit_ir;
    bool preprocess_only;
    bool verbose;
    bool save_temps;
    const char *backend_name;
    const char *clang;
    const char *lld_link;
    const char *kernel32;
    const char *runtime_lib;
    const char *runtime_include;
    ETCC_VEC(char *) include_dirs;
    ETCC_VEC(char *) defines;
    ETCC_VEC(char *) undefs;
} Options;

typedef struct {
    EtccArena *arena;
    EtccDiagnostics *diag;
    const char *clang;
    const char *lld_link;
    const char *kernel32;
    const char *runtime_lib;
    const char *runtime_include;
    bool verbose;
} Toolchain;

static void usage(FILE *out) {
    fputs(
        "Ethernos Tiny C Compiler (ETCC) " ETCC_VERSION "\n"
        "\n"
        "Usage: etcc [options] <input.c>\n"
        "\n"
        "Output:\n"
        "  -o <file>              output file (default: input name with .exe)\n"
        "  -c                     compile to a COFF object, do not link\n"
        "  -S, --emit-llvm        emit backend textual IR, do not assemble\n"
        "  -E, --emit-pp          emit preprocessed source only\n"
        "\n"
        "Code generation:\n"
        "  --backend <name>       backend to use (default: llvm)\n"
        "  --clang <path>         clang executable used by the LLVM backend\n"
        "  --lld-link <path>      lld-link executable\n"
        "  --kernel32 <path>      kernel32.lib import library\n"
        "  --runtime-lib <path>   ETCC runtime library\n"
        "  --runtime-include <d>  ETCC runtime include directory\n"
        "\n"
        "Preprocessing:\n"
        "  -I <dir>               add an include search directory\n"
        "  -D <name>[=<value>]    define an object-like macro\n"
        "  -U <name>              undefine a macro\n"
        "\n"
        "Diagnostics:\n"
        "  -v, --verbose          show commands and toolchain paths\n"
        "  --save-temps           keep intermediate object files\n"
        "  --version              print version and exit\n"
        "  --help                 show this help\n",
        out);
}

static char *replace_extension(EtccArena *a, const char *path,
                               const char *new_ext) {
    const char *base = etcc_path_filename(path);
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) n = (size_t)(dot - base);
    char *name = etcc_arena_alloc(a, n + strlen(new_ext) + 1, 1);
    memcpy(name, base, n);
    strcpy(name + n, new_ext);
    return name;
}

#ifdef _WIN32
static char *find_windows_sdk_kernel32(EtccArena *a) {
    const char *roots[2];
    roots[0] = getenv("ProgramFiles(x86)");
    roots[1] = getenv("ProgramFiles");
    char *best = NULL;

    for (int r = 0; r < 2; r++) {
        if (!roots[r] || !*roots[r]) continue;
        char pattern[4096];
        snprintf(pattern, sizeof(pattern), "%s\\Windows Kits\\10\\Lib\\*",
                 roots[r]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
                continue;
            char candidate[4096];
            snprintf(candidate, sizeof(candidate),
                     "%s\\Windows Kits\\10\\Lib\\%s\\um\\x64\\kernel32.lib",
                     roots[r], fd.cFileName);
            if (!etcc_file_exists(candidate)) continue;
            if (!best || strcmp(fd.cFileName, best) > 0) {
                free(best);
                best = etcc_xstrdup(candidate);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!best) return NULL;
    char *result = etcc_arena_strdup(a, best);
    free(best);
    return result;
}
#endif

static char *find_runtime_lib(EtccArena *a, const char *exe_dir,
                              const char *override) {
    if (override && *override) {
        if (etcc_file_exists(override)) return etcc_arena_strdup(a, override);
        return NULL;
    }
    const char *env = getenv("ETCC_RUNTIME_LIB");
    if (env && *env && etcc_file_exists(env))
        return etcc_arena_strdup(a, env);

    const char *candidates[] = {
        "etccrt.lib",
        "runtime\\etccrt.lib",
        "runtime\\build\\etccrt.lib",
        "..\\runtime\\build\\etccrt.lib",
        "..\\lib\\etccrt.lib",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *p = etcc_path_join(a, exe_dir, candidates[i]);
        if (etcc_file_exists(p)) return p;
    }
    char *cwd = etcc_arena_strdup(a, ".");
    char *p = etcc_path_join(a, cwd, "runtime\\build\\etccrt.lib");
    if (etcc_file_exists(p)) return p;
    p = etcc_path_join(a, cwd, "build\\runtime\\etccrt.lib");
    if (etcc_file_exists(p)) return p;
    return NULL;
}

static char *find_runtime_include(EtccArena *a, const char *exe_dir,
                                  const char *runtime_lib,
                                  const char *override) {
    if (override && *override) {
        if (etcc_file_exists(etcc_path_join(a, override, "stdio.h")))
            return etcc_arena_strdup(a, override);
        return NULL;
    }
    const char *env = getenv("ETCC_RUNTIME_INCLUDE");
    if (env && *env && etcc_file_exists(etcc_path_join(a, env, "stdio.h")))
        return etcc_arena_strdup(a, env);

    const char *candidates[] = {
        "runtime\\include",
        "..\\runtime\\include",
        "..\\..\\runtime\\include",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *p = etcc_path_join(a, exe_dir, candidates[i]);
        if (etcc_file_exists(etcc_path_join(a, p, "stdio.h"))) return p;
    }
    if (runtime_lib) {
        char *dir = etcc_path_dirname(a, runtime_lib);
        char *p = etcc_path_join(a, dir, "include");
        if (etcc_file_exists(etcc_path_join(a, p, "stdio.h"))) return p;
    }
    char *cwd = etcc_arena_strdup(a, ".");
    char *p = etcc_path_join(a, cwd, "runtime\\include");
    if (etcc_file_exists(etcc_path_join(a, p, "stdio.h"))) return p;
    return NULL;
}

static bool discover_toolchain(Options *opt, Toolchain *tc) {
    EtccArena *a = tc->arena;
    tc->clang = opt->clang && *opt->clang
                    ? etcc_arena_strdup(a, opt->clang)
                    : (getenv("ETCC_CLANG") && *getenv("ETCC_CLANG")
                           ? etcc_arena_strdup(a, getenv("ETCC_CLANG"))
                           : etcc_arena_strdup(a, "clang"));
    tc->lld_link = opt->lld_link && *opt->lld_link
                       ? etcc_arena_strdup(a, opt->lld_link)
                       : (getenv("ETCC_LLD_LINK") && *getenv("ETCC_LLD_LINK")
                              ? etcc_arena_strdup(a, getenv("ETCC_LLD_LINK"))
                              : etcc_arena_strdup(a, "lld-link"));

    if (opt->kernel32 && *opt->kernel32)
        tc->kernel32 = etcc_arena_strdup(a, opt->kernel32);
    else if (getenv("ETCC_KERNEL32_LIB") && *getenv("ETCC_KERNEL32_LIB"))
        tc->kernel32 = etcc_arena_strdup(a, getenv("ETCC_KERNEL32_LIB"));
#ifdef _WIN32
    else
        tc->kernel32 = find_windows_sdk_kernel32(a);
#endif

    char *exe_dir = etcc_get_executable_dir(a);
    tc->runtime_lib = find_runtime_lib(a, exe_dir, opt->runtime_lib);
    tc->runtime_include =
        find_runtime_include(a, exe_dir, tc->runtime_lib, opt->runtime_include);
    tc->verbose = opt->verbose;

    if (tc->verbose) {
        fprintf(stderr, "etcc: clang           = %s\n", tc->clang);
        fprintf(stderr, "etcc: lld-link        = %s\n", tc->lld_link);
        fprintf(stderr, "etcc: kernel32.lib    = %s\n",
                tc->kernel32 ? tc->kernel32 : "<not found>");
        fprintf(stderr, "etcc: runtime library = %s\n",
                tc->runtime_lib ? tc->runtime_lib : "<not found>");
        fprintf(stderr, "etcc: runtime include = %s\n",
                tc->runtime_include ? tc->runtime_include : "<not found>");
    }
    return true;
}

static bool parse_args(int argc, char **argv, Options *opt, EtccArena *a,
                       EtccDiagnostics *diag) {
    (void)a;
    memset(opt, 0, sizeof(*opt));
    opt->backend_name = "llvm";

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            exit(0);
        }
        if (strcmp(arg, "--version") == 0) {
            printf("etcc " ETCC_VERSION " (Windows x64, LLVM backend)\n");
            exit(0);
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opt->verbose = true;
            continue;
        }
        if (strcmp(arg, "-c") == 0) {
            opt->compile_only = true;
            continue;
        }
        if (strcmp(arg, "-S") == 0 || strcmp(arg, "--emit-llvm") == 0 ||
            strcmp(arg, "--emit-ir") == 0) {
            opt->emit_ir = true;
            continue;
        }
        if (strcmp(arg, "-E") == 0 || strcmp(arg, "--emit-pp") == 0) {
            opt->preprocess_only = true;
            continue;
        }
        if (strcmp(arg, "--save-temps") == 0) {
            opt->save_temps = true;
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (++i >= argc) {
                etcc_diag_error_global(diag, "missing argument for -o");
                return false;
            }
            opt->output = argv[i];
            continue;
        }
        if (strncmp(arg, "-o", 2) == 0 && arg[2]) {
            opt->output = (char *)arg + 2;
            continue;
        }
        if (strcmp(arg, "-I") == 0) {
            if (++i >= argc) {
                etcc_diag_error_global(diag, "missing argument for -I");
                return false;
            }
            char *dir = etcc_xstrdup(argv[i]);
            if (!ETCC_VEC_PUSH(opt->include_dirs, dir)) {
                free(dir);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "-I", 2) == 0 && arg[2]) {
            char *dir = etcc_xstrdup(arg + 2);
            if (!ETCC_VEC_PUSH(opt->include_dirs, dir)) {
                free(dir);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "-D") == 0) {
            if (++i >= argc) {
                etcc_diag_error_global(diag, "missing argument for -D");
                return false;
            }
            char *def = etcc_xstrdup(argv[i]);
            if (!ETCC_VEC_PUSH(opt->defines, def)) {
                free(def);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "-D", 2) == 0 && arg[2]) {
            char *def = etcc_xstrdup(arg + 2);
            if (!ETCC_VEC_PUSH(opt->defines, def)) {
                free(def);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "-U") == 0) {
            if (++i >= argc) {
                etcc_diag_error_global(diag, "missing argument for -U");
                return false;
            }
            char *name = etcc_xstrdup(argv[i]);
            if (!ETCC_VEC_PUSH(opt->undefs, name)) {
                free(name);
                return false;
            }
            continue;
        }
        if (strncmp(arg, "-U", 2) == 0 && arg[2]) {
            char *name = etcc_xstrdup(arg + 2);
            if (!ETCC_VEC_PUSH(opt->undefs, name)) {
                free(name);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "--backend") == 0) {
            if (++i >= argc) {
                etcc_diag_error_global(diag, "missing argument for --backend");
                return false;
            }
            opt->backend_name = argv[i];
            continue;
        }
        if (strncmp(arg, "--backend=", 10) == 0) {
            opt->backend_name = arg + 10;
            continue;
        }
#define TAKE_OPT(name, field)                                                 \
    if (strcmp(arg, name) == 0) {                                             \
        if (++i >= argc) {                                                    \
            etcc_diag_error_global(diag, "missing argument for %s", name);    \
            return false;                                                     \
        }                                                                     \
        opt->field = argv[i];                                                 \
        continue;                                                             \
    }
        TAKE_OPT("--clang", clang)
        TAKE_OPT("--lld-link", lld_link)
        TAKE_OPT("--kernel32", kernel32)
        TAKE_OPT("--runtime-lib", runtime_lib)
        TAKE_OPT("--runtime-include", runtime_include)
#undef TAKE_OPT

        if (strcmp(arg, "--") == 0) {
            if (++i < argc && !opt->input) opt->input = argv[i];
            i = argc;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            /* Accept common no-op flags so build systems can pass them. */
            if (strncmp(arg, "-O", 2) == 0 || strcmp(arg, "-g") == 0 ||
                strcmp(arg, "-Wall") == 0 || strcmp(arg, "-Wextra") == 0 ||
                strncmp(arg, "-std=", 5) == 0 || strcmp(arg, "-pedantic") == 0)
                continue;
            etcc_diag_error_global(diag, "unknown option '%s'", arg);
            return false;
        }
        if (opt->input) {
            etcc_diag_error_global(diag,
                                   "multiple input files are not supported yet");
            return false;
        }
        opt->input = (char *)arg;
    }

    if (!opt->input) {
        etcc_diag_error_global(diag, "no input file");
        usage(stderr);
        return false;
    }
    return true;
}

static void add_define(EtccPP *pp, const char *def) {
    const char *eq = strchr(def, '=');
    size_t n = eq ? (size_t)(eq - def) : strlen(def);
    const char *body = eq ? eq + 1 : "1";
    etcc_macro_define(pp->arena, &pp->macros, def, n, body, strlen(body), false);
}

static void add_builtin_macros(EtccPP *pp) {
    static const char *defs[][2] = {
        {"__ETCC__", "1"},
        {"__STDC__", "1"},
        {"__STDC_VERSION__", "201112L"},
        {"_WIN32", "1"},
        {"_WIN64", "1"},
        {"_M_X64", "100"},
        {"__x86_64__", "1"},
        {"__amd64__", "1"},
    };
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        etcc_macro_define(pp->arena, &pp->macros, defs[i][0],
                          strlen(defs[i][0]), defs[i][1], strlen(defs[i][1]),
                          false);
    }
}

static bool write_text_file(const char *path, const char *data, size_t len) {
    FILE *f = strcmp(path, "-") == 0 ? stdout : fopen(path, "wb");
    if (!f) return false;
    if (len) fwrite(data, 1, len, f);
    if (f != stdout) fclose(f);
    return true;
}

int main(int argc, char **argv) {
    EtccArena arena;
    etcc_arena_init(&arena);
    EtccDiagnostics diag;
    etcc_diag_init(&diag);

    Options opt;
    if (!parse_args(argc, argv, &opt, &arena, &diag)) {
        return 2;
    }

    Toolchain tc;
    memset(&tc, 0, sizeof(tc));
    tc.arena = &arena;
    tc.diag = &diag;
    discover_toolchain(&opt, &tc);

    char *out_path = opt.output;
    if (!out_path) {
        if (opt.preprocess_only) out_path = "-";
        else if (opt.compile_only) out_path = replace_extension(&arena, opt.input, ".obj");
        else if (opt.emit_ir) out_path = replace_extension(&arena, opt.input, ".ll");
        else out_path = replace_extension(&arena, opt.input, ".exe");
    }

    EtccPP pp;
    etcc_pp_init(&pp, &arena, &diag);
    add_builtin_macros(&pp);

    for (size_t i = 0; i < opt.include_dirs.len; i++)
        etcc_pp_add_include_dir(&pp, opt.include_dirs.items[i]);
    if (tc.runtime_include)
        etcc_pp_add_include_dir(&pp, tc.runtime_include);
    for (size_t i = 0; i < opt.defines.len; i++)
        add_define(&pp, opt.defines.items[i]);
    for (size_t i = 0; i < opt.undefs.len; i++)
        etcc_macro_undef(&pp.macros, opt.undefs.items[i],
                         strlen(opt.undefs.items[i]));

    const char *input = etcc_path_canonical(&arena, opt.input);
    if (!etcc_pp_run_file(&pp, input)) {
        etcc_pp_destroy(&pp);
        return 1;
    }
    if (diag.errors) {
        etcc_pp_destroy(&pp);
        return 1;
    }

    size_t pp_len = 0;
    const char *pp_text = etcc_pp_text(&pp, &pp_len);

    if (opt.preprocess_only) {
        bool ok = write_text_file(out_path, pp_text, pp_len);
        if (!ok) etcc_diag_error_global(&diag, "cannot write '%s'", out_path);
        etcc_pp_destroy(&pp);
        return ok ? 0 : 1;
    }

    EtccAstNode *tu = etcc_parse_translation_unit(&arena, &diag, pp_text, pp_len,
                                                  &pp.map, &pp.macros);
    if (diag.errors) {
        etcc_pp_destroy(&pp);
        return 1;
    }

    IrModule *m = ir_module_create(&arena);
    if (!etcc_sema_run(tu, &arena, &diag, m)) {
        etcc_pp_destroy(&pp);
        return diag.errors ? 1 : 70;
    }

    const EtccBackendEntry *entry = etcc_backend_find(opt.backend_name);
    if (!entry) {
        etcc_diag_error_global(&diag, "unknown backend '%s'", opt.backend_name);
        etcc_backend_print_available(stderr);
        etcc_pp_destroy(&pp);
        return 2;
    }
    EtccBackendOptions bopts = {.clang_path = tc.clang, .verbose = tc.verbose};
    EtccBackend *backend = entry->create(&arena, &diag, &bopts);
    if (!backend) {
        etcc_pp_destroy(&pp);
        return 70;
    }

    bool ok = false;
    if (opt.emit_ir) {
        ok = backend->ops->emit_ir(backend, m, out_path, &diag);
    } else if (opt.compile_only) {
        ok = backend->ops->emit_object(backend, m, out_path, &diag);
    } else {
        if (!tc.runtime_lib) {
            etcc_diag_error_global(
                &diag,
                "cannot find the ETCC runtime library (etccrt.lib); "
                "set ETCC_RUNTIME_LIB or build the runtime");
        } else if (!tc.runtime_include) {
            etcc_diag_error_global(
                &diag,
                "cannot find the ETCC runtime headers; set "
                "ETCC_RUNTIME_INCLUDE");
        } else if (!tc.kernel32) {
            etcc_diag_error_global(
                &diag,
                "cannot find kernel32.lib; set ETCC_KERNEL32_LIB or install "
                "the Windows SDK");
        } else {
            char *obj_path = etcc_make_temp_path(&arena, "obj");
            if (backend->ops->emit_object(backend, m, obj_path, &diag)) {
                char *link_argv[16];
                int n = 0;
                link_argv[n++] = (char *)tc.lld_link;
                link_argv[n++] = "/nologo";
                link_argv[n++] = "/entry:etcc_entry";
                link_argv[n++] = "/subsystem:console";
                link_argv[n++] = "/nodefaultlib";
                link_argv[n++] = obj_path;
                link_argv[n++] = (char *)tc.runtime_lib;
                link_argv[n++] = (char *)tc.kernel32;
                link_argv[n++] = "/out:";
                char *outarg = etcc_arena_printf(&arena, "/out:%s", out_path);
                link_argv[n - 1] = outarg;
                link_argv[n] = NULL;
                ok = etcc_run_process(link_argv, tc.verbose);
                if (!ok)
                    etcc_diag_error_global(&diag, "linker failed for '%s'",
                                           out_path);
            }
            if (!opt.save_temps) etcc_remove_file_if_exists(obj_path);
        }
    }

    backend->ops->destroy(backend);
    etcc_pp_destroy(&pp);
    return ok && diag.errors == 0 ? 0 : 1;
}
