/* ============================================================================
 * stubs/scanf_shim.c — compact sscanf subset (weak)
 *
 * libavutil/opt.c and a few mov/h264 SEI parsers call sscanf(); glibc maps
 * those call sites to __isoc99_sscanf, so both names are provided here.
 * Supported verbs cover every format string actually present in this FFmpeg
 * build:  %d %i %x %X %u %c %% , '*' suppression, field widths, [...]
 * scansets (e.g. "%*1[:/]"). Anything else stops scanning safely.
 * ==========================================================================*/
#include "shim.h"

#define WS(s) while (*(s) == ' ' || ((*(s)) >= '\t' && (*(s)) <= '\r')) (s)++

static int scan_int(const char **sp, long long *out, int base) {
    const char *s = *sp; int neg = 0;
    long long acc = 0;
    WS(s);
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if (base == 16 && s[0] == '0' && (s[1] | 32) == 'x' &&
        (((s[2] >= '0' && s[2] <= '9')) || ((s[2]|32) >= 'a' && (s[2]|32) <= 'f')))
        s += 2;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && (*s|32) >= 'a' && (*s|32) <= 'f')
                                    d = (*s|32) - 'a' + 10;
        else break;
        acc = acc * base + d;
    }
    if (s == *sp || (neg && s == *sp + 1)) return 0;   /* no digits */
    *sp = s;
    *out = neg ? -acc : acc;
    return 1;
}

static int vscan(const char *s, const char *fmt, __builtin_va_list ap)
{
    int nass = 0;

    for (; *fmt; fmt++) {
        if (*fmt == ' ') { WS(s); continue; }
        if (*fmt != '%') {
            if (*s != *fmt) goto done;
            s++; continue;
        }
        fmt++;
        int supp = 0, w = 0;
        if (*fmt == '*') { supp = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') w = w * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'd': case 'i': {
            long long v;
            if (!scan_int(&s, &v, 10)) goto done;
            if (!supp) *__builtin_va_arg(ap, int *) = (int)v;
            break;
        }
        case 'x': case 'X': {
            long long v;
            if (!scan_int(&s, &v, 16)) goto done;
            if (!supp) *__builtin_va_arg(ap, unsigned *) = (unsigned)v;
            break;
        }
        case 'u': {
            long long v;
            if (!scan_int(&s, &v, 10)) goto done;
            if (!supp) *__builtin_va_arg(ap, unsigned *) = (unsigned)v;
            break;
        }
        case 'c': {
            int n = w > 0 ? w : 1;
            char *out = supp ? 0 : __builtin_va_arg(ap, char *);
            while (n--) {
                if (!*s) return nass;
                if (out) *out++ = *s;
                s++;
            }
            break;
        }
        case '[': {                       /* scanset                        */
            fmt++;
            int invert = (*fmt == '^');
            if (invert) fmt++;
            unsigned char set[256] = {0};
            if (*fmt == ']') { set[(unsigned char)']'] = 1; fmt++; }
            while (*fmt && *fmt != ']') set[(unsigned char)*fmt++] = 1;
            while (*fmt && *fmt != ']') fmt++;
            if (!*fmt) goto done;         /* unterminated scanset           */
            int cnt = 0, lim = w > 0 ? w : 1 << 30;
            char *out = supp ? 0 : __builtin_va_arg(ap, char *);
            while (*s && set[(unsigned char)*s] != invert && cnt < lim) {
                if (out) *out++ = *s;
                s++; cnt++;
            }
            if (!cnt) goto done;
            if (out) *out = 0;
            break;
        }
        case '%':
            if (*s != '%') goto done;
            s++; break;
        default:
            goto done;                    /* unsupported verb               */
        }
        if (!supp) nass++;
    }
done:
    return nass;
}
#undef WS

int __isoc99_sscanf(const char *s, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int r = vscan(s, fmt, ap); __builtin_va_end(ap);
    return r;
}

int sscanf(const char *s, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int r = vscan(s, fmt, ap); __builtin_va_end(ap);
    return r;
}
