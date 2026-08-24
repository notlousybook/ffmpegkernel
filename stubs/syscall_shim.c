/* syscall_shim.c - minimal libc for FFmpeg on bare-metal x86_64.
 * Every public symbol is WEAK: your kernel overrides by defining the same name
 * (UART console, real heap, VFS, PIT clock). DEFAULTS do raw Linux syscalls so
 * the binary also runs under qemu-x86_64 / native Linux to validate FFmpeg
 * before kernel wiring. Bare metal ultimately needs only write(), _exit(),
 * optionally read/lseek/close/fstat/clock_gettime.
 * RAM disk: set rd_base / rd_size to your initrd before open().            */
#include "shim.h"

#define WEAK __attribute__((weak))
typedef __UINT64_TYPE__ u64;

static long sys1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sys3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
void WEAK _exit(int c) { sys1(231, c); for (;;) __asm__("hlt"); }
void WEAK exit(int c)  { _exit(c); }
void WEAK abort(void)  { write(2, "SHIM: abort()\n", 14); _exit(134); }
long WEAK write(int fd, const void *buf, unsigned long n)   /* console: override me */
    { return sys3(1, fd, (long)buf, n); }

static int errno_val;
int *WEAK __errno_location(void) { return &errno_val; }

/* ---------------------------------------------------- heap: pool + freelist */
#ifndef HEAP_POOL_BYTES
#define HEAP_POOL_BYTES (256u << 20)      /* demand-zero bss; tune freely   */
#endif
static unsigned char heap_pool[HEAP_POOL_BYTES] __attribute__((aligned(64)));
static size_t heap_top;                   /* bump frontier                  */
/* Block header: 64B total so every payload stays 64B-aligned (SSE-safe).
 * magic lets free() catch double-frees / foreign pointers loudly instead of
 * silently corrupting the freelist.                                       */
#define H_MAGIC 0x5EAF00DC0DE00001ull
#define H_FREED 0x5EAF00DC0DE000F0ull
struct hdr { size_t sz; size_t magic; struct hdr *nxt; char pad[40]; };
static struct hdr *free_list;

/* SMP: one global heap spinlock (allocator ops are short; contention is
 * low because worker count <= cores-1).                                  */
static volatile unsigned long heap_lock;
static void heap_lock_take(void)
{
    for (;;) {
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(&heap_lock, &expected, 1ul, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        __asm__ volatile("pause");
    }
}
static void heap_lock_give(void)
{ __atomic_store_n(&heap_lock, 0ul, __ATOMIC_RELEASE); }

static void *heap_malloc_unlocked(size_t n) {
    if (!n) n = 1;
    n = (n + 63u) & ~(size_t)63;          /* 64B granularity                */
    for (struct hdr **pp = &free_list; *pp; pp = &(*pp)->nxt) {
        struct hdr *b = *pp;
        if (b->sz < n) continue;
        void *v = (char *)b + sizeof(struct hdr);
        b->magic = H_MAGIC;
        if (b->sz >= n + sizeof(struct hdr) + 64) {
            struct hdr *rem = (struct hdr *)((char *)b + sizeof(struct hdr) + n);
            rem->sz  = b->sz - n - sizeof(struct hdr);
            rem->nxt = b->nxt;
            b->sz = n;                    /* shrink served block            */
            *pp = rem;                    /* remainder replaces b           */
        } else {
            *pp = b->nxt;                 /* unlink whole block             */
        }
        return v;
    }
    if (heap_top + sizeof(struct hdr) + n > sizeof heap_pool) return 0;
    struct hdr *h = (struct hdr *)(heap_pool + heap_top);
    heap_top += sizeof(struct hdr) + n;
    h->sz = n; h->magic = H_MAGIC;
    return (char *)h + sizeof(struct hdr);
}

void *WEAK malloc(size_t n)
{
    heap_lock_take();
    void *p = heap_malloc_unlocked(n);
    heap_lock_give();
    return p;
}

/* raw allocator for kernel-internal use (thread stacks etc.)              */
int shim_heap_alloc(size_t n, void **out)
{
    heap_lock_take();
    void *p = heap_malloc_unlocked(n);
    heap_lock_give();
    if (!p) return -1;
    *out = p;
    return 0;
}
void WEAK free(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    unsigned char *b = (unsigned char *)h;
    if (b < heap_pool || b >= heap_pool + sizeof heap_pool ||
        h->magic == H_FREED || h->magic != H_MAGIC) {
        write(2, "SHIM: free() of bad/double pointer\n", 35);
        _exit(141);
    }
    heap_lock_take();
    h->magic = H_FREED;
    h->nxt = free_list; free_list = h;    /* LIFO recycle                   */
    heap_lock_give();
}
void *WEAK calloc(size_t a, size_t b) {
    size_t t; if (__builtin_mul_overflow(a, b, &t)) return 0;
    void *p = malloc(t); if (p) memset(p, 0, t); return p;
}
void *WEAK realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != H_MAGIC) {
        write(2, "SHIM: realloc() of bad pointer\n", 32);
        _exit(142);
    }
    size_t old = h->sz;
    heap_lock_take();
    void *q = heap_malloc_unlocked(n);
    if (!q) { heap_lock_give(); return 0; }
    memcpy(q, p, old < n ? old : n);
    /* free(p) body, already under the lock                                */
    h->magic = H_FREED;
    h->nxt = free_list; free_list = h;
    heap_lock_give();
    return q;
}
void *WEAK memalign(size_t align, size_t n) {
    if (align <= 64) return malloc(n);
    size_t req = n ? n : 1;
    size_t cap = (req + 63u) & ~(size_t)63;
    heap_lock_take();
    unsigned char *raw = heap_malloc_unlocked(cap + align + 64);
    heap_lock_give();
    if (!raw) return 0;
    unsigned char *al = raw + ((-(size_t)raw) & (align - 1));
    if (al - raw < 64) al += align;       /* keep room for embedded header */
    struct hdr *h = (struct hdr *)al - 1;
    h->sz = cap; h->magic = H_MAGIC;
    return al;
}
int WEAK posix_memalign(void **out, size_t align, size_t n)
    { return (*out = memalign(align, n)) ? 0 : -1; }
void *WEAK aligned_alloc(size_t a, size_t n) { return memalign(a, n); }

/* mmap: carve anonymous pages out of a side arena */
static unsigned char mmap_arena[8u << 20] __attribute__((aligned(4096)));
static size_t mmap_off;
void *WEAK mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    (void)addr;(void)prot;(void)flags;(void)fd;(void)off;
    if (mmap_off + len > sizeof mmap_arena) return (void *)-1;
    void *p = mmap_arena + mmap_off; mmap_off += len; return p;
}
void *WEAK mmap(void *a, size_t l, int p, int f, int fd, long o) { return mmap64(a,l,p,f,fd,o); }
int WEAK munmap(void *a, size_t l) { (void)a;(void)l; return 0; }
int WEAK brk(void *addr) { (void)addr; return 0; }

void *WEAK memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n >= 8) { *(u64 *)dd = *(const u64 *)ss; dd += 8; ss += 8; n -= 8; }
    while (n--) *dd++ = *ss++; return d;
}
void *WEAK memset(void *d, int c, size_t n) {
    unsigned char *dd = d; u64 v = (unsigned char)c * 0x0101010101010101ull;
    while (n >= 8) { *(u64 *)dd = v; dd += 8; n -= 8; }
    while (n--) *dd++ = (unsigned char)c; return d;
}
void *WEAK memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) return memcpy(d, s, n);
    dd += n; ss += n; while (n--) *--dd = *--ss; return d;
}
int WEAK memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n--; x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void *WEAK memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
size_t WEAK strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int WEAK strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int WEAK strncmp(const char *a, const char *b, size_t n) {
    for (; n--; a++, b++)
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    return 0;
}
char *WEAK strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)){} return r; }
void *WEAK strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (void *)s; if (!*s) return 0; }
}
void *WEAK strrchr(const char *s, int c) {
    const char *r = 0; do { if (*s == (char)c) r = s; } while (*s++);
    return (void *)r;
}
void *WEAK strstr(const char *h, const char *n) {
    if (!*n) return (void *)h;
    for (; *h; h++)
        for (const char *a = h, *b = n;; a++, b++) {
            if (!*b) return (void *)h;
            if (*a != *b) break;
        }
    return 0;
}
size_t WEAK strspn(const char *s, const char *set) {
    size_t n = 0; for (; *s && strchr(set, *s); s++) n++; return n;
}
size_t WEAK strcspn(const char *s, const char *set) {
    size_t n = 0; for (; *s && !strchr(set, *s); s++) n++; return n;
}

struct sink { char *buf; size_t cap, len; };
static void emit(struct sink *o, const char *s, size_t n) {
    while (n--) { if (o->len + 1 < o->cap) o->buf[o->len] = *s; o->len++; s++; }
}
static void putch(struct sink *o, char c) { emit(o, &c, 1); }
static void num(struct sink *o, u64 v, int neg, unsigned base,
                const char *dig, int w, int zero) {
    char t[24]; int i = 0;
    do { t[i++] = dig[v % base]; v /= base; } while (v);
    if (zero && neg && w > i + 1) { putch(o,'-'); neg = 0; }
    for (int p = w - i - (neg ? 1 : 0); p > 0; p--)
        putch(o, zero ? '0' : ' ');
    if (neg) putch(o, '-');
    while (i--) putch(o, t[i]);
}
int WEAK vsnprintf(char *str, size_t n, const char *fmt, __builtin_va_list ap) {
    struct sink o = { str, n ? n : 1, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, fmt, 1); continue; }
        fmt++;
        int w = 0, zero = 0, prec = -1;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') w = w * 10 + (*fmt++ - '0');
        if (*fmt == '*') { w = __builtin_va_arg(ap, int); fmt++; }
        if (*fmt == '.') { fmt++; prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        const char *lm = fmt;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' ||
               *fmt == 'j' || *fmt == 't') fmt++;
        int big = (fmt - lm) >= 2 || *lm == 'z' || *lm == 'j';
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = big ? __builtin_va_arg(ap, long long)
                              : __builtin_va_arg(ap, int);
            num(&o, v < 0 ? -(u64)v : (u64)v, v < 0, 10, "0123456789", w, zero);
            break; }
        case 'u': case 'x': case 'X': {
            u64 v = big ? __builtin_va_arg(ap, u64)
                        : __builtin_va_arg(ap, unsigned);
            num(&o, v, 0, *fmt == 'u' ? 10 : 16,
                *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef", w, zero);
            break; }
        case 'p': putch(&o,'0'); putch(&o,'x');
                  num(&o, (u64)__builtin_va_arg(ap, void *), 0, 16,
                      "0123456789abcdef", 0, 0); break;
        case 's': { const char *s = __builtin_va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    int sl = (int)strlen(s);
                    if (prec >= 0 && sl > prec) sl = prec;
                    for (int p = w - sl; p > 0; p--) putch(&o, ' ');
                    emit(&o, s, sl); break; }
        case 'c': putch(&o, (char)__builtin_va_arg(ap, int)); break;
        case '%': putch(&o, '%'); break;
        default:  putch(&o,'%'); putch(&o,*fmt); break;   /* %f etc: literal */
        }
    }
    if (n) str[o.len < n ? o.len : n - 1] = 0;
    return (int)o.len;
}
#define FMT_BODY(outfd) { \
    char b[1024]; __builtin_va_list a; __builtin_va_start(a, fmt); \
    int r = vsnprintf(b, sizeof b, fmt, a); __builtin_va_end(a); \
    if (r > (int)sizeof b - 1) r = (int)sizeof b - 1; write(outfd, b, (size_t)r); return r; }
int  WEAK snprintf(char *s, size_t n, const char *fmt, ...)
    { __builtin_va_list a; __builtin_va_start(a, fmt);
      int r = vsnprintf(s, n, fmt, a); __builtin_va_end(a); return r; }
int  WEAK printf(const char *fmt, ...)              FMT_BODY(1)
int  WEAK fprintf(FILE *fh, const char *fmt, ...)   FMT_BODY(fh ? fh->fd : 2)
int WEAK vprintf(const char *fmt, __builtin_va_list a) {
    char b[1024]; int r = vsnprintf(b, sizeof b, fmt, a);
    write(1, b, (size_t)r); return r; }
int WEAK vfprintf(FILE *fh, const char *fmt, __builtin_va_list a) {
    char b[1024]; int r = vsnprintf(b, sizeof b, fmt, a);
    write(fh ? fh->fd : 2, b, (size_t)r); return r; }

/* stdio objects: what FFmpeg's default logger touches */
static FILE _sin = {0}, _sout = {1}, _serr = {2};
FILE *const stdin  = &_sin;
FILE *const stdout = &_sout;
FILE *const stderr = &_serr;
int WEAK fputs(const char *s, FILE *f) { return write(f ? f->fd : 1, s, strlen(s)) < 0 ? -1 : 0; }
size_t WEAK fwrite(const void *p, size_t sz, size_t n, FILE *f)
    { return (size_t)write(f ? f->fd : 1, p, sz * n) / (sz ? sz : 1); }
int WEAK fclose(FILE *f) { (void)f; return 0; }
size_t WEAK fread(void *p, size_t sz, size_t n, FILE *f)
    { long r = read(f ? f->fd : 0, p, sz * n); return r > 0 ? (size_t)r / (sz ? sz : 1) : 0; }
void *WEAK fdopen(int fd, const char *m)
    { static FILE t[4]; static int nt; (void)m;
      return nt < 4 ? (t[nt].fd = fd, &t[nt++]) : 0; }
int WEAK setvbuf(FILE *f, char *b, int m, size_t n) { (void)f;(void)b;(void)m;(void)n; return 0; }
char *WEAK getenv(const char *n) { (void)n; return 0; }
int WEAK isatty(int fd) { (void)fd; return 0; }
int WEAK fcntl64(int fd, int cmd, ...) { (void)fd;(void)cmd; return 0; }
int WEAK mkdir(const char *p, mode_t m) { (void)p;(void)m; return -1; }
int WEAK mkstemp64(char *t) { (void)t; return -1; }
int WEAK __xpg_strerror_r(int e, char *b, size_t l) { snprintf(b,l,"error %d",e); return 0; }
int WEAK abs(int x) { return x < 0 ? -x : x; }
long long WEAK llabs(long long x) { return x < 0 ? -x : x; }

unsigned char *rd_base = 0;               /* kernel sets these post-initrd  */
unsigned long  rd_size = 0;
#define NFD 8
#define FD0 3
static struct { unsigned char used; unsigned long pos; } ft[NFD];

int WEAK open64(const char *path, int flags, ...) {
    (void)path;(void)flags;               /* every name maps to the ramdisk */
    for (int i = 0; i < NFD; i++)
        if (!ft[i].used) { ft[i].used = 1; ft[i].pos = 0; return FD0 + i; }
    return -1;
}
int WEAK open(const char *p, int f, ...) { return open64(p, f); }
long WEAK read(int fd, void *buf, unsigned long count) {
    if (fd < FD0 || fd >= FD0 + NFD || !ft[fd - FD0].used) return 0;
    unsigned long pos = ft[fd - FD0].pos;
    unsigned long avail = rd_size > pos ? rd_size - pos : 0;
    unsigned long n = count < avail ? count : avail;
    if (rd_base && n) memcpy(buf, rd_base + pos, n);
    ft[fd - FD0].pos = pos + n;
    return (long)n;
}
off_t WEAK lseek64(int fd, long long off, int whence) {
    if (fd < FD0 || fd >= FD0 + NFD || !ft[fd - FD0].used) return -1;
    unsigned long pos = ft[fd - FD0].pos;
    long long np = whence == 0 ? off : whence == 1 ? (long long)pos + off
                                                   : (long long)rd_size + off;
    if (np < 0) np = 0;
    if ((unsigned long)np > rd_size) np = rd_size;
    ft[fd - FD0].pos = (unsigned long)np;
    return (off_t)np;
}
off_t WEAK lseek(int fd, off_t o, int w) { return lseek64(fd, o, w); }
int WEAK close(int fd) { if (fd >= FD0 && fd < FD0 + NFD) ft[fd - FD0].used = 0; return 0; }
int WEAK fstat64(int fd, struct stat *st) {
    if (!st) return -1;
    memset(st, 0, sizeof *st);
    st->st_mode  = 0100644;               /* S_IFREG | rw-r--r--           */
    st->st_nlink = 1;
    st->st_size  = (off_t)rd_size;
    st->st_blksize = 4096;
    st->st_blocks = (long)(rd_size / 512);
    (void)fd; return 0;
}
int WEAK fstat(int fd, struct stat *st) { return fstat64(fd, st); }

#ifndef TSC_HZ
#define TSC_HZ 2500000000ull                /* guess; override or weak-replace */
#endif
static inline u64 rdtsc(void) { return __builtin_ia32_rdtsc(); }
static u64 ns_now(void) { return (u64)((unsigned __int128)rdtsc() * 1000000000ull / TSC_HZ); }

int WEAK clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) return -1;
    (void)clk_id;                         /* REALTIME/MONOTONIC alike       */
    u64 ns = ns_now();
    tp->tv_sec = (time_t)(ns / 1000000000ull);
    tp->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
int WEAK gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; if (!tv) return -1;
    u64 ns = ns_now();
    tv->tv_sec = (time_t)(ns / 1000000000ull);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ull) / 1000ull);
    return 0;
}
int WEAK nanosleep(const struct timespec *req, struct timespec *rem) {
    u64 want = (u64)req->tv_sec * 1000000000ull + (u64)req->tv_nsec;
    u64 dl = ns_now() + want;
    while (ns_now() < dl) __asm__("pause");           /* busy spin          */
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
long WEAK clock(void) { return (long)(ns_now() / 1000ull); } /* CLOCKS_PER_SEC=1e6 */

/* proleptic Gregorian calendar */
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static struct tm *civil_to_tm(long days, long sod, struct tm *tm) {
    days += 719468;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    tm->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    unsigned m = mp + (mp < 10 ? 3 : -9);
    tm->tm_year = (int)(y + (m <= 2) - 1900);
    tm->tm_mon  = (int)m - 1;
    tm->tm_hour = (int)(sod / 3600);
    tm->tm_min  = (int)((sod % 3600) / 60);
    tm->tm_sec  = (int)(sod % 60);
    tm->tm_wday = (int)((days + 4) % 7);              /* 1970-01-01 = Thu  */
    tm->tm_yday = (int)doy;
    tm->tm_isdst = 0;
    return tm;
}
struct tm *WEAK gmtime_r(const time_t *tp, struct tm *tm) {
    long s = (long)*tp, days = s / 86400, sod = s % 86400;
    if (sod < 0) { sod += 86400; days--; }
    return civil_to_tm(days, sod, tm);
}
struct tm *WEAK localtime_r(const time_t *tp, struct tm *tm) { return gmtime_r(tp, tm); } /* UTC */
time_t WEAK mktime(struct tm *tm) {
    return days_from_civil(tm->tm_year + 1900L, (unsigned)tm->tm_mon + 1,
                           (unsigned)tm->tm_mday) * 86400L +
           tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}
size_t WEAK strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    for (; *fmt && n < max; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        fmt++;
        char b[16]; int v = -1, w = 2;
        switch (*fmt) {
        case 'Y': v = tm->tm_year + 1900; w = 4; goto num;
        case 'm': v = tm->tm_mon + 1;  goto num;
        case 'd': v = tm->tm_mday;     goto num;
        case 'H': v = tm->tm_hour;     goto num;
        case 'M': v = tm->tm_min;      goto num;
        case 'S': v = tm->tm_sec;      goto num;
        num:      snprintf(b, sizeof b, "%0*d", w, v); goto cat;
        case '%': b[0]='%'; b[1]=0;    goto cat;
        default:  continue;
        }
    cat: for (char *p = b; *p && n < max; p++) s[n++] = *p;
    }
    if (n < max) s[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- misc glue */
void WEAK qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    char *b = base;
    for (size_t i = 1; i < n; i++)                 /* insertion sort       */
        for (size_t j = i; j && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
            for (size_t k = 0; k < sz; k++) {
                char t = b[(j - 1) * sz + k];
                b[(j - 1) * sz + k] = b[j * sz + k];
                b[j * sz + k] = t;
            }
}
unsigned long WEAK __stack_chk_guard = 0x5eaf00dc5eaf00dcull;
void WEAK __stack_chk_fail(void) { write(2, "stack chk fail\n", 15); _exit(134); }

int WEAK __sched_cpucount(size_t setsize, const unsigned long *set) {
    int n = 0;
    for (size_t i = 0; i * sizeof *set < setsize; i++)
        n += __builtin_popcountl(set[i]);
    return n;
}
int WEAK sched_getaffinity(int pid, size_t sz, unsigned long *mask) {
    (void)pid;                                       /* report one CPU     */
    if (sz < sizeof *mask) return -1;
    memset(mask, 0, sz); mask[0] = 1; return 0;
}

static const char *skip_ws_sign(const char *s, int *neg) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    *neg = (*s == '-');
    if (*s == '+' || *s == '-') s++;
    return s;
}
/* LP64: long == long long, so one core serves the whole strtol family */
long long WEAK strtoll(const char *np, char **end, int base) {
    int neg = 0;
    const char *p = skip_ws_sign(np, &neg), *s;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x')
        { base = 16; p += (p[2] != 0) ? 2 : 1; }
    else if (base == 0) base = (*p == '0') ? 8 : 10;
    unsigned long long acc = 0; int any = 0;
    for (s = p;; s++) {
        int d; unsigned c = (unsigned char)*s;
        if (c - '0' < 10u)             d = c - '0';
        else if ((c | 32) - 'a' < 26u) d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        any = 1; acc = acc * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char *)(any ? s : np);
    return neg ? -(long long)acc : (long long)acc;
}
long WEAK strtol(const char *n, char **e, int b) { return strtoll(n, e, b); }
unsigned long WEAK strtoul(const char *n, char **e, int b) { return (unsigned long)strtoll(n, e, b); }
unsigned long long WEAK strtoull(const char *n,char **e,int b){return (unsigned long long)strtoll(n,e,b);}
/* drm stubs for hwcontext_drm when -march pulls it in without libdrm   */
void *WEAK drmGetVersion(int fd) { (void)fd; return 0; }
void  WEAK drmFreeVersion(void *v) { (void)v; }
void *WEAK drmGetMagic(int fd, unsigned *m) { (void)fd;(void)m; return 0; }
int   WEAK drmIoctl(int fd, unsigned long r, void *a) { (void)fd;(void)r;(void)a; return -1; }
int   WEAK ioctl(int fd, unsigned long r, ...) { (void)fd;(void)r; return -1; }
void *WEAK iconv_open(const char *a, const char *b) { (void)a;(void)b; return (void*)-1; }
int    WEAK iconv_close(void *c) { (void)c; return 0; }
unsigned long WEAK iconv(void *cd, char **in, unsigned long *il, char **out, unsigned long *ol)
       { (void)cd;(void)in;(void)il;(void)out;(void)ol; return (unsigned long)-1; }

double WEAK strtod(const char *s, char **e) {
    const char *start = s;
    while (*s == ' ') s++;
    int neg = (*s == '-'); if (*s == '+' || *s == '-') s++;
    double m = 0; int ex = 0, nd = 0;
    for (; *s >= '0' && *s <= '9'; s++, nd++)
        if (m < 1e17) m = m * 10 + (*s - '0'); else ex++;
    if (*s == '.') for (s++; *s >= '0' && *s <= '9'; s++, nd++)
        if (m < 1e17) { m = m * 10 + (*s - '0'); ex--; }
    if (!nd) { if (e) *e = (char *)start; return 0; }
    int E = ex;
    if ((*s | 32) == 'e') {
        const char *sv = s; s++; int en = 0, eneg = (*s == '-');
        if (*s == '+' || *s == '-') s++;
        if (*s >= '0' && *s <= '9') {
            for (; *s >= '0' && *s <= '9'; s++) en = en * 10 + (*s - '0');
            E += eneg ? -en : en;
        } else s = sv;
    }
    for (; E >= 16; E -= 16) m *= 1e16;
    for (; E <= -16; E += 16) m *= 1e-16;
    while (E-- > 0) m *= 10;
    while (E++ < 0) m /= 10;
    if (e) *e = (char *)s;
    return neg ? -m : m;
}
