#include "etcc/diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void etcc_diag_init(EtccDiagnostics *d) {
    d->errors = 0;
    d->warnings = 0;
    d->notes = 0;
    d->max_errors = 25;
    d->show_source = true;
}

static void print_prefix(EtccLoc loc, const char *severity) {
    if (loc.file && loc.file->path) {
        fprintf(stderr, "%s:%u:%u: %s: ", loc.file->path, loc.line, loc.col,
                severity);
    } else {
        fprintf(stderr, "etcc: %s: ", severity);
    }
}

static void print_snippet(EtccLoc loc) {
    if (!loc.file || !loc.file->data || loc.line == 0) return;

    const char *data = loc.file->data;
    size_t len = loc.file->len;
    uint32_t line = 1;
    size_t start = 0;
    while (start < len && line < loc.line) {
        if (data[start] == '\n') line++;
        start++;
    }
    if (line != loc.line) return;

    size_t end = start;
    while (end < len && data[end] != '\n' && data[end] != '\r') end++;
    size_t n = end - start;
    if (n > 240) n = 240;

    fputs("    ", stderr);
    fwrite(data + start, 1, n, stderr);
    fputc('\n', stderr);
    fputs("    ", stderr);

    uint32_t col = loc.col ? loc.col : 1;
    for (uint32_t i = 1; i < col && i <= n; i++) {
        fputc(data[start + i - 1] == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);
}

static void vreport(EtccDiagnostics *d, EtccLoc loc, const char *severity,
                    const char *fmt, va_list ap) {
    print_prefix(loc, severity);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    if (d->show_source && loc.file) print_snippet(loc);
}

void etcc_diag_error(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...) {
    if (d->errors >= d->max_errors) return;
    d->errors++;
    if (d->errors == d->max_errors) {
        va_list ap;
        va_start(ap, fmt);
        vreport(d, loc, "error", fmt, ap);
        va_end(ap);
        fputs("etcc: fatal: too many errors, stopping\n", stderr);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vreport(d, loc, "error", fmt, ap);
    va_end(ap);
}

void etcc_diag_warning(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...) {
    d->warnings++;
    va_list ap;
    va_start(ap, fmt);
    vreport(d, loc, "warning", fmt, ap);
    va_end(ap);
}

void etcc_diag_note(EtccDiagnostics *d, EtccLoc loc, const char *fmt, ...) {
    d->notes++;
    va_list ap;
    va_start(ap, fmt);
    vreport(d, loc, "note", fmt, ap);
    va_end(ap);
}

void etcc_diag_error_global(EtccDiagnostics *d, const char *fmt, ...) {
    d->errors++;
    fputs("etcc: error: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
