/* ============================================================================
 * stubs/libm_shim.c — software libm fallbacks (weak)
 *
 * FFmpeg's static closure references ~35 libm functions even when the actual
 * H.264 decode path executes almost none of them. These are compact,
 * accuracy-"good-enough" implementations whose ONLY job is to satisfy the
 * final freestanding link. Override any of them from your kernel if you have
 * an FPU-friendly libm. Compile this file with -fno-math-errno so GCC's
 * builtins expand to plain SSE instructions.
 * ==========================================================================*/
#include "shim.h"

/* double-precision core declared up-front; float wrappers below may call it */
double atan2(double, double), ceil(double), floor(double), cos(double),
       sin(double), tan(double), pow(double, double), exp(double),
       log(double), log2(double), log10(double), exp2(double),
       fabs(double), fmod(double, double), cbrt(double),
       hypot(double, double);
float sinf(float), cosf(float);

typedef __UINT64_TYPE__ u64;
#define PINF  bitsd(0x7ff0000000000000ULL)
#define PNAN  bitsd(0x7ff8000000000000ULL)

static inline u64    dbits(double x)     { union { double d; u64 u; } v; v.d = x; return v.u; }
static inline double bitsd(u64 u)        { union { double d; u64 u; } v; v.u = u; return v.d; }
static inline int    dexpfield(u64 u)    { return (int)((u >> 52) & 0x7ff); }

double sqrt(double x) {
    double r; __asm__("sqrtsd %1,%0" : "=x"(r) : "x"(x)); return r;
}
float sqrtf(float x) {
    float r; __asm__("sqrtss %1,%0" : "=x"(r) : "x"(x)); return r;
}
double fabs(double x) { return bitsd(dbits(x) & 0x7fffffffffffffffULL); }
double copysign(double x, double y) {
    return bitsd((dbits(x) & 0x7fffffffffffffffULL) |
                 (dbits(y) & 0x8000000000000000ULL));
}

static double p2(int e) {                       /* exact 2^e, |e|<=1000      */
    return bitsd(((u64)(e + 1023)) << 52);
}
double scalbn(double x, int n) {
    while (n >  1000) { x *= p2(1000); n -= 1000; }
    while (n < -1000) { x *= p2(-1000); n += 1000; }
    return x * p2(n);
}
double ldexp(double x, int n) { return scalbn(x, n); }
double frexp(double x, int *e) {
    u64 u = dbits(x); int ex = dexpfield(u);
    if (ex == 0 || ex == 0x7ff) { *e = 0; return x; }
    *e = ex - 1022;
    return bitsd((u & 0x800fffffffffffffULL) | ((u64)1022 << 52));
}
double trunc(double x) {
    u64 u = dbits(x); int ex = dexpfield(u);
    if (ex < 0x3ff) return bitsd(u & 0x8000000000000000ULL);   /* |x|<1   */
    if (ex >= 0x433) return x;                                 /* integral */
    return bitsd(u & ~(((u64)1 << (0x433 - ex)) - 1));
}
double floor(double x) {
    double t = trunc(x);
    return (x < 0 && t != x) ? t - 1 : t;
}
double ceil(double x) {
    double t = trunc(x);
    return (x > 0 && t != x) ? t + 1 : t;
}
double round(double x) {
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += copysign(1.0, x);
    return t;
}
double rint(double x) { return round(x); }
long lrint(double x)  { return (long)rint(x); }
int  lrintf(float x)  { return (int)(x >= 0 ? x + 0.5f : x - 0.5f); }
long long llrint(double x) { return (long long)rint(x); }
long long llrintf(float x) { return (long long)(x >= 0 ? x + 0.5f : x - 0.5f); }
double fmax(double a, double b) { return a > b ? a : b; }
double fmin(double a, double b) { return a < b ? a : b; }

double fmod(double x, double y) {
    if (y == 0.0) return PNAN;
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;
    int ex, ey; frexp(ax, &ex); frexp(ay, &ey);
    double s = scalbn(ay, ex - ey);
    for (int guard = 0; ax >= ay && guard++ < 2200; ) {
        if (ax < s) s = scalbn(s, -1);
        else ax -= s;
    }
    return x < 0 ? -ax : ax;
}

/* --- exponential / logarithmic family ------------------------------------- */
double exp2(double x) {
    static const double c[7] = {           /* minimax fit of 2^t, t in [0,1) */
        0.00001525273380755879, 0.00015403530393381635,
        0.0013333558146428443,  0.009618129107628477,
        0.05550410866482158,    0.2402265069591007,
        0.6931471805599453 };
    if (x > 1100.0) return PINF;
    if (x < -1150.0) return 0.0;
    int i = (int)floor(x);
    double f = x - i, p = 1.0;
    for (int k = 0; k < 7; k++) p = p * f + c[k];
    return scalbn(p, i);
}
double exp(double x)  { return exp2(x * 1.4426950408889634); }
double log2(double x) {
    if (x <= 0) return (x == 0) ? -PINF : PNAN;
    int e; double m = frexp(x, &e) * 2;            /* m in [1,2), adj e--   */
    double z = (m - 1) / (m + 1), z2 = z * z;
    double atanhz = z * (1 + z2 * (1.0/3 + z2 * (1.0/5 + z2 * (1.0/7 + z2 * (1.0/9 + z2 * (1.0/11))))));
    return (e - 1) + atanhz * 2.8853900817779268;  /* 2/ln2                 */
}
double log(double x)  { return log2(x) * 0.6931471805599453; }
double log10(double x){ return log2(x) * 0.30102999566398114; }
double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y < 0 ? PINF : 0;
    if (x < 0) {
        long long yi = (long long)y;
        if ((double)yi != y) return PNAN;          /* non-integral power    */
        x = -x;
        return exp2(y * log2(x)) * ((yi & 1) ? -1 : 1);
    }
    return exp2(y * log2(x));
}

/* --- trigonometric family -------------------------------------------------- */
#define PI        3.14159265358979323846
#define HALF_PI   1.57079632679489661923
static void sinccos(double x, double *so, double *co) {
    x = fmod(x, 2 * PI);                          /* coarse reduce          */
    double t = x, s = x, c = 1.0, term_s = x, term_c = 1.0;
    for (int k = 1; k <= 25; k++) {               /* alternating series     */
        term_s *= -x * x / ((2 * k) * (2 * k + 1));
        term_c *= -x * x / ((2 * k - 1) * (2 * k));
        s += term_s; c += term_c;
        (void)t;
    }
    *so = s; *co = c;
}
double sin(double x)  { double s, c; sinccos(x, &s, &c); return s; }
double cos(double x)  { double s, c; sinccos(x, &s, &c); return c; }
double tan(double x)  { double s, c; sinccos(x, &s, &c); return s / c; }
double atan(double x) {
    double a = fabs(x); int rec = 0;
    if (a > 1) { a = 1 / a; rec = 1; }
    for (int k = 0; k < 4; k++)                   /* half-angle shrink x4   */
        a = a / (1 + sqrt(1 + a * a));
    double a2 = a * a;
    double r = a * (1 + a2 * (-1.0/3 + a2 * (1.0/5 + a2 * (-1.0/7 + a2 * (1.0/9)))));
    for (int k = 0; k < 4; k++) r *= 2;           /* undo half-angle        */
    if (rec) r = HALF_PI - r;
    return copysign(r, x);
}
double atan2(double y, double x) {
    if (x == 0) return copysign(HALF_PI, y);
    double r = atan(fabs(y) / fabs(x));
    if (x < 0) r = (y >= 0) ? PI - r : r - PI;
    else r = copysign(r, y);
    return r;
}
double asin(double x) {
    if (x < -1 || x > 1) return PNAN;
    return atan(x / sqrt(1 - x * x));
}
double acos(double x) { return x < -1 || x > 1 ? PNAN : HALF_PI - asin(x); }
double sinh(double x) { double e = exp(x); return (e - 1 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) * 0.5; }
double tanh(double x) {
    if (x > 20) return 1; if (x < -20) return -1;
    double e2 = exp(2 * x); return (e2 - 1) / (e2 + 1);
}
/* --- float wrappers demanded by libswscale -------------------------------- */
void sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void sincosf(float x, float *s, float *c)   { *s = sinf(x); *c = cosf(x); }
float  atan2f(float y, float x)      { return (float)atan2(y, x); }
float  ceilf(float x)                { return (float)ceil(x); }
float  floorf(float x)               { return (float)floor(x); }
float  cosf(float x)                 { return (float)cos(x); }
float  sinf(float x)                 { return (float)sin(x); }
float  tanf(float x)                 { return (float)tan(x); }
float  powf(float x, float y)        { return (float)pow(x, y); }
float  expf(float x)                 { return (float)exp(x); }
float  logf(float x)                 { return (float)log(x); }
float  log2f(float x)                { return (float)log2(x); }
float  log10f(float x)               { return (float)log10(x); }
float  exp2f(float x)                { return (float)exp2(x); }
float  fabsf(float x)                { return (float)fabs(x); }
float  fmodf(float x, float y)       { return (float)fmod(x, y); }
float  hypotf(float a, float b)      { return (float)hypot(a, b); }
double cbrt(double x)                { return x >= 0 ? pow(x, 1.0 / 3.0)
                                                  : -pow(-x, 1.0 / 3.0); }
float  fmaxf(float a, float b)       { return a > b ? a : b; }
float  fminf(float a, float b)       { return a < b ? a : b; }
long   labs(long x)                  { return x < 0 ? -x : x; }

int mprotect(void *a, unsigned long l, int p) { (void)a;(void)l;(void)p; return 0; }

double hypot(double a, double b) {
    a = fabs(a); b = fabs(b);
    if (a < b) { double t = a; a = b; b = t; }
    if (a == 0) return b;
    return a * sqrt(1 + (b / a) * (b / a));
}
