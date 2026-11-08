#ifndef ETCC_STDIO_H
#define ETCC_STDIO_H

#include <stddef.h>

#define EOF (-1)

int puts(const char *s);
int printf(const char *fmt, ...);
int putchar(int c);
int fputs(const char *s, void *stream);
int fflush(void *stream);

#endif /* ETCC_STDIO_H */
