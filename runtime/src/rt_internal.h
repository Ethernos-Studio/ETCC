/*
 * ETCC runtime - minimal freestanding Windows x64 C library.
 * No UCRT, no MSVC CRT. Only kernel32 imports.
 */
#ifndef ETCC_RT_INTERNAL_H
#define ETCC_RT_INTERNAL_H

typedef unsigned char rt_u8;
typedef unsigned short rt_u16;
typedef unsigned int rt_u32;
typedef unsigned long long rt_u64;
typedef signed long long rt_i64;
typedef unsigned long long rt_size;
typedef void *rt_handle;

typedef __builtin_va_list rt_va_list;
#define rt_va_start(ap, last) __builtin_va_start(ap, last)
#define rt_va_arg(ap, type) __builtin_va_arg(ap, type)
#define rt_va_end(ap) __builtin_va_end(ap)
#define rt_va_copy(dst, src) __builtin_va_copy(dst, src)

#define RT_STD_OUTPUT_HANDLE ((rt_u32)-11)
#define RT_STD_ERROR_HANDLE ((rt_u32)-12)
#define RT_HEAP_ZERO_MEMORY 0x00000008UL
#define NULL ((void *)0)
#define EOF (-1)

__declspec(dllimport) rt_handle __stdcall GetStdHandle(rt_u32);
__declspec(dllimport) int __stdcall WriteFile(rt_handle, const void *, rt_u32,
                                              rt_u32 *, void *);
__declspec(dllimport) __declspec(noreturn) void __stdcall
ExitProcess(rt_u32);
__declspec(dllimport) rt_handle __stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(rt_handle, rt_u32, rt_size);
__declspec(dllimport) void *__stdcall HeapReAlloc(rt_handle, rt_u32, void *,
                                                  rt_size);
__declspec(dllimport) int __stdcall HeapFree(rt_handle, rt_u32, void *);

/* Provided by string.c. */
rt_size strlen(const char *s);
void *memcpy(void *dst, const void *src, rt_size n);
void *memset(void *dst, int c, rt_size n);
int memcmp(const void *a, const void *b, rt_size n);

/* Provided by stdlib.c. */
void *malloc(rt_size size);
void *realloc(void *ptr, rt_size size);
void free(void *ptr);
void exit(int status);
void abort(void);

#endif /* ETCC_RT_INTERNAL_H */
