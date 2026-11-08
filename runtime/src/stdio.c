#include "rt_internal.h"

typedef struct {
    char *data;
    rt_size len;
    rt_size cap;
    int failed;
} RtOut;

static int out_reserve(RtOut *o, rt_size extra) {
    if (o->failed) return 0;
    if (extra > (rt_u64)-1 - o->len - 1) {
        o->failed = 1;
        return 0;
    }
    rt_size need = o->len + extra + 1;
    if (need <= o->cap) return 1;
    rt_size cap = o->cap ? o->cap : 128;
    while (cap < need) {
        if (cap > (rt_u64)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *p = (char *)realloc(o->data, cap);
    if (!p) {
        o->failed = 1;
        return 0;
    }
    o->data = p;
    o->cap = cap;
    return 1;
}

static void out_bytes(RtOut *o, const char *s, rt_size n) {
    if (!n) return;
    if (!out_reserve(o, n)) return;
    memcpy(o->data + o->len, s, n);
    o->len += n;
}

static void out_char(RtOut *o, char c) {
    out_bytes(o, &c, 1);
}

static void out_pad(RtOut *o, rt_size n, char c) {
    while (n--) out_char(o, c);
}

static int rt_write(rt_u32 which, const void *data, rt_size n) {
    rt_handle h = GetStdHandle(which);
    const char *p = (const char *)data;
    while (n) {
        rt_u32 chunk = n > 0x7FFFFFFF ? 0x7FFFFFFF : (rt_u32)n;
        rt_u32 written = 0;
        if (!WriteFile(h, p, chunk, &written, NULL)) return 0;
        if (written == 0) return 0;
        p += written;
        n -= written;
    }
    return 1;
}

int puts(const char *s) {
    if (!s) return EOF;
    rt_size n = strlen(s);
    if (!rt_write(RT_STD_OUTPUT_HANDLE, s, n)) return EOF;
    if (!rt_write(RT_STD_OUTPUT_HANDLE, "\n", 1)) return EOF;
    return 0;
}

int putchar(int c) {
    char ch = (char)c;
    if (!rt_write(RT_STD_OUTPUT_HANDLE, &ch, 1)) return EOF;
    return (unsigned char)ch;
}

int fputs(const char *s, void *stream) {
    (void)stream;
    if (!s) return EOF;
    return rt_write(RT_STD_OUTPUT_HANDLE, s, strlen(s)) ? 0 : EOF;
}

int fflush(void *stream) {
    (void)stream;
    return 0;
}

static const char *base_digits = "0123456789abcdef";
static const char *base_digits_upper = "0123456789ABCDEF";

static void format_unsigned(RtOut *o, rt_u64 value, unsigned base, int upper,
                            int width, int precision, int left, int zero,
                            const char *prefix, int prefix_len) {
    (void)upper;
    char digits[64];
    int nd = 0;
    if (value == 0) {
        digits[nd++] = '0';
    } else {
        const char *table = upper ? base_digits_upper : base_digits;
        while (value) {
            digits[nd++] = table[value % base];
            value /= base;
        }
    }

    int zeros = 0;
    if (precision > nd) zeros = precision - nd;
    if (zero && !left && precision < 0 && width > nd + zeros + prefix_len)
        zeros = width - nd - prefix_len;

    int total = nd + zeros + prefix_len;
    int pad = width > total ? width - total : 0;

    if (!left) out_pad(o, (rt_size)pad, ' ');
    if (prefix_len) out_bytes(o, prefix, (rt_size)prefix_len);
    out_pad(o, (rt_size)zeros, '0');
    /* Digits were generated least-significant first. */
    for (int i = nd - 1; i >= 0; i--) out_char(o, digits[i]);
    if (left) out_pad(o, (rt_size)pad, ' ');
}

int printf(const char *fmt, ...) {
    if (!fmt) return -1;

    RtOut o;
    o.data = NULL;
    o.len = 0;
    o.cap = 0;
    o.failed = 0;

    rt_va_list ap;
    rt_va_start(ap, fmt);

    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            const char *start = p;
            while (*p && *p != '%') p++;
            out_bytes(&o, start, (rt_size)(p - start));
            continue;
        }
        p++;
        if (*p == '%') {
            out_char(&o, '%');
            p++;
            continue;
        }

        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else if (*p == '0') zero = 1;
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = rt_va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
        }

        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = rt_va_arg(ap, int);
                if (precision < 0) precision = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }

        int length = 0; /* 0=int, 1=long, 2=long long, 3=size, 4=short, 5=char */
        if (*p == 'h') {
            p++;
            if (*p == 'h') {
                length = 5;
                p++;
            } else {
                length = 4;
            }
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') {
                length = 2;
                p++;
            } else {
                length = 1;
            }
        } else if (*p == 'z') {
            length = 3;
            p++;
        } else if (*p == 'j') {
            length = 2;
            p++;
        } else if (*p == 't') {
            length = 3;
            p++;
        }

        char conv = *p;
        if (!conv) break;

        char prefix[3];
        int prefix_len = 0;

        switch (conv) {
        case 'd':
        case 'i': {
            rt_i64 sv;
            if (length == 2) sv = rt_va_arg(ap, rt_i64);
            else if (length == 1) sv = (long)rt_va_arg(ap, long);
            else if (length == 4) sv = (short)rt_va_arg(ap, int);
            else if (length == 5) sv = (signed char)rt_va_arg(ap, int);
            else if (length == 3) sv = (rt_i64)rt_va_arg(ap, rt_size);
            else sv = rt_va_arg(ap, int);

            rt_u64 uv;
            if (sv < 0) {
                prefix[prefix_len++] = '-';
                uv = (rt_u64)(-(sv + 1)) + 1;
            } else {
                if (plus) prefix[prefix_len++] = '+';
                else if (space) prefix[prefix_len++] = ' ';
                uv = (rt_u64)sv;
            }
            format_unsigned(&o, uv, 10, 0, width, precision, left, zero, prefix, prefix_len);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            rt_u64 uv;
            if (length == 2) uv = rt_va_arg(ap, rt_u64);
            else if (length == 1) uv = (unsigned long)rt_va_arg(ap, unsigned long);
            else if (length == 4) uv = (unsigned short)rt_va_arg(ap, unsigned int);
            else if (length == 5) uv = (unsigned char)rt_va_arg(ap, unsigned int);
            else if (length == 3) uv = rt_va_arg(ap, rt_size);
            else uv = rt_va_arg(ap, unsigned int);

            unsigned base = conv == 'o' ? 8 : (conv == 'u' ? 10 : 16);
            if (alt && conv == 'x' && uv) {
                prefix[prefix_len++] = '0';
                prefix[prefix_len++] = 'x';
            } else if (alt && conv == 'X' && uv) {
                prefix[prefix_len++] = '0';
                prefix[prefix_len++] = 'X';
            } else if (alt && conv == 'o') {
                prefix[prefix_len++] = '0';
            }
            format_unsigned(&o, uv, base, conv == 'X', width, precision, left, zero, prefix, prefix_len);
            break;
        }
        case 'p': {
            rt_u64 uv = (rt_u64)(rt_size)rt_va_arg(ap, void *);
            prefix[0] = '0';
            prefix[1] = 'x';
            format_unsigned(&o, uv, 16, 0, width, precision, left, zero, prefix, 2);
            break;
        }
        case 'c': {
            char ch = (char)rt_va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left) out_pad(&o, (rt_size)pad, ' ');
            out_char(&o, ch);
            if (left) out_pad(&o, (rt_size)pad, ' ');
            break;
        }
        case 's': {
            const char *s = rt_va_arg(ap, const char *);
            if (!s) s = "(null)";
            rt_size n = strlen(s);
            if (precision >= 0 && (rt_size)precision < n)
                n = (rt_size)precision;
            int pad = width > (int)n ? width - (int)n : 0;
            if (!left) out_pad(&o, (rt_size)pad, ' ');
            out_bytes(&o, s, n);
            if (left) out_pad(&o, (rt_size)pad, ' ');
            break;
        }
        case 'n':
            /* Not supported; consume nothing. */
            break;
        default:
            out_char(&o, '%');
            out_char(&o, conv);
            break;
        }
        p++;
    }

    rt_va_end(ap);

    int result = -1;
    if (!o.failed) {
        result = (int)o.len;
        if (!rt_write(RT_STD_OUTPUT_HANDLE, o.data, o.len)) result = -1;
    }
    free(o.data);
    return result;
}

