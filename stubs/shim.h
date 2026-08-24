/* stubs/shim.h - shared declarations for the bare-metal FFmpeg shim.
 * Self-contained: includes NO host headers, so it is safe to include from
 * both the freestanding shim itself and the kernel / test harness.       */
#ifndef SHIM_H
#define SHIM_H

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define SHIM_SSIZE   long
#define SHIM_OFFT    long
typedef SHIM_SSIZE      ssize_t;
typedef SHIM_OFFT       off_t;
typedef long long       off64_t;
typedef unsigned int    mode_t;
typedef unsigned int    uid_t;
typedef unsigned int    gid_t;
typedef unsigned long   dev_t;
typedef unsigned long   ino_t;
typedef long            time_t;
typedef long            suseconds_t;

struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval  { time_t tv_sec; suseconds_t tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

/* x86_64 glibc-compatible layout (offset of st_size = 48) */
struct stat {
    dev_t st_dev;        /* 0  */
    ino_t st_ino;        /* 8  */
    mode_t st_mode;      /* 16 */
    unsigned int st_nlink;/*20 */
    uid_t st_uid;        /* 24 */
    gid_t st_gid;        /* 28 */
    unsigned int __pad0; /* 32 */
    dev_t st_rdev;       /* 40 */
    off_t st_size;       /* 48 */
    long st_blksize;     /* 56 */
    long st_blocks;      /* 64 */
    struct timespec st_atim, st_mtim, st_ctim;
    long __unused[3];
};

typedef struct _FILE FILE;
struct _FILE { int fd; };
extern FILE *const stdin, *const stdout, *const stderr;

/* --- libc surface provided by stubs/syscall_shim.c (all weak) ------------- */
void  *malloc(size_t); void free(void *);
void  *calloc(size_t, size_t); void *realloc(void *, size_t);
void  *memcpy(void *, const void *, size_t);
void  *memset(void *, int, size_t);
void  *memmove(void *, const void *, size_t);
int    memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int    strcmp(const char *, const char *), strncmp(const char *, const char *, size_t);
char  *strcpy(char *, const char *);
void  *strchr(const char *, int), *strrchr(const char *, int), *strstr(const char *, const char *);
long   write(int, const void *, unsigned long);
long   read(int, void *, unsigned long);
off_t  lseek(int, off_t, int);
int    open(const char *, int, ...), close(int);
int    fstat(int, struct stat *);
int    clock_gettime(int, struct timespec *);
int    nanosleep(const struct timespec *, struct timespec *);
void   _exit(int);

int vsnprintf(char *str, size_t n, const char *fmt, __builtin_va_list ap);
int snprintf(char *str, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);

#endif /* SHIM_H */
