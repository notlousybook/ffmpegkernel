/* ============================================================================
 * boot/metal_overrides.c - STRONG (non-weak) kernel-side implementations that
 * override the weak defaults in stubs/syscall_shim.c at link time. This is the
 * reference for wiring the shim into a real kernel:
 *   write()        -> COM1 UART polling
 *   _exit()        -> isa-debug-exit port + hlt
 * clock_gettime/gettimeofday already work: the shim's TSC default is fine on
 * real hardware / KVM.
 * ==========================================================================*/
#include "../stubs/shim.h"

static inline void outb_(unsigned short p, unsigned char v)
    { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline unsigned char inb_(unsigned short p)
    { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

extern int smp_tsc_deadline_ok;
void lapic_timer_arm_tscdeadline(unsigned long long ticks);

long write(int fd, const void *buf, unsigned long n)
{
    (void)fd;
    static volatile unsigned long con_lock;
    const unsigned char *p = buf;
    for (;;) {
        unsigned long expected = 0;
        if (__atomic_compare_exchange_n(&con_lock, &expected, 1ul, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        __asm__ volatile("pause");
    }
    for (unsigned long i = 0; i < n; i++) {
        while (!(inb_(0x3fd) & 0x20)) {}          /* LSR.THRE              */
        outb_(0x3f8, p[i]);                       /* COM1 data             */
    }
    __atomic_store_n(&con_lock, 0ul, __ATOMIC_RELEASE);
    return (long)n;
}

void _exit(int code)
{
    outb_(0xf4, (unsigned char)code);             /* isa-debug-exit device */
    for (;;) __asm__("hlt");
}

/* ---- calibrated time: RTC-measured TSC rate makes playback/bench honest -- */
typedef unsigned long long u64t;
#define TSC_HZ_DEFAULT 3498920000ull   /* i3-4150 @3.49892GHz: KVM passes
                                        * guest TSC through unscaled     */
static u64t g_hz = TSC_HZ_DEFAULT;
u64t tsc_hz(void) { return g_hz; }

static inline u64t tsc_ns(void)
{
    /* split sec/rem: rdtsc()*1e9 would wrap a u64 after ~5.3 s @3.5 GHz */
    u64t t = __builtin_ia32_rdtsc();
    u64t s = t / g_hz, r = t % g_hz;   /* r*1e9 < ~5.8e18: no overflow   */
    return s * 1000000000ull + r * 1000000000ull / g_hz;
}

static unsigned char cmos_rd(unsigned char reg)
{
    outb_(0x70, reg);
    return inb_(0x71);
}
static void rtc_wait_update_done(void)
{
    while (cmos_rd(0x0A) & 0x80) {}   /* UIP: regs invalid while set     */
}

void metal_time_init(void)
{
    /* Preferred: kvmclock pvclock MSR pairing - KVM gives us an exact
     * (tsc_timestamp, system_time) pair, so hz = dtsc*1e9/dns is exact
     * regardless of PIT/RTC emulation quirks.                            */
    {
        unsigned a, b, c, d;
        __asm__ volatile("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x40000001u));          /* KVM features     */
        if (a & (1u << 0)) {                           /* CLOCKSOURCE      */
            static struct {                            /* pvclock vcpu info*/
                unsigned version, pad;
                unsigned long long tsc_timestamp, system_time;
                unsigned mul; char shift; char flags[3];
            } __attribute__((packed)) kc __attribute__((aligned(32)));
            unsigned long long pa = (unsigned long long)&kc;
            unsigned lo = (unsigned)(pa | 1), hi = (unsigned)(pa >> 32);
            __asm__ volatile("wrmsr"
                             : : "c"(0x12u), "a"(lo), "d"(hi)); /* KVM_SYSTEM_TIME */
            unsigned long long t1t, s1, t2t, s2, anchor = __builtin_ia32_rdtsc();
            #define KC_SNAP(tt, ss) do { \
                unsigned v1, v2; \
                do { \
                    v1 = kc.version; \
                    __asm__ volatile("" ::: "memory"); \
                    tt = kc.tsc_timestamp; ss = kc.system_time; \
                    __asm__ volatile("" ::: "memory"); \
                    v2 = kc.version; \
                } while ((v1 & 1u) || v1 != v2); \
            } while (0)
            KC_SNAP(t1t, s1);
            if (!s1) {                                 /* not live yet     */
                while (__builtin_ia32_rdtsc() - anchor < 100000000ULL)
                    inb_(0x3fd);                       /* force VMEXITs so */
                KC_SNAP(t1t, s1);                      /* KVM refreshes it */
            }
            /* 5 estimates; pvclock area is only refreshed on VMEXIT, and
             * early boot barely exits - poke a serial-status port (an
             * exiting IO) inside every window to keep pairs fresh.       */
            unsigned long long est[5]; int n = 0;
            for (int k = 0; k < 5 && s1; k++) {
                unsigned long long ta = __builtin_ia32_rdtsc();
                unsigned long long sa;
                KC_SNAP(t2t, sa);
                do { inb_(0x3fd); }
                while (__builtin_ia32_rdtsc() - ta < 40000000ULL);
                KC_SNAP(t2t, s2);
                if (s2 > sa && t2t > t1t && s2 > s1) {
                    unsigned long long hz =
                        (t2t - t1t) * 1000000000ull / (s2 - s1);
                    if (hz > 100000000ull && hz < 20000000000ull)
                        est[n++] = hz;
                }
            }
            for (int i = 1; i < n; i++) {              /* insertion sort   */
                unsigned long long kk = est[i]; int j = i - 1;
                while (j >= 0 && est[j] > kk) { est[j+1] = est[j]; j--; }
                est[j+1] = kk;
            }
            if (n >= 3) {
                unsigned long long med = est[n/2];
                int cluster = 0;
                for (int i = 0; i < n; i++)
                    if (est[i] * 10ull <= med * 11ull &&
                        est[i] * 11ull >= med * 10ull)
                        cluster++;
                if (cluster >= 3 && med > 2800000000ull &&
                    med < 4200000000ull) { g_hz = med; return; }
            }
        }
    }

    /* Fallback (TCG/no-KVM): calibrate against the CMOS RTC: sample the
     * seconds register right after one update completes and again right
     * after the NEXT completes - that window is one wall-clock second.   */
    rtc_wait_update_done();
    {   /* RTC: timestamp two consecutive seconds rollovers = exactly 1s.
         * Bounded so a broken RTC can never hang boot; the plausibility
         * band rejects short/garbage windows.                               */
        u64t deadline = __builtin_ia32_rdtsc() + 6000000000ull;
        unsigned char prev = cmos_rd(0x00), cur;
        u64t t0 = 0;
        for (;;) {                                /* first rollover      */
            cur = cmos_rd(0x00);
            if (cur != prev) { t0 = __builtin_ia32_rdtsc(); break; }
            prev = cur;
            if (__builtin_ia32_rdtsc() > deadline) break;
        }
        if (t0) for (;;) {                        /* second rollover     */
            cur = cmos_rd(0x00);
            if (cur != prev) {
                u64t hz = __builtin_ia32_rdtsc() - t0;
                if (hz > 2800000000ull && hz < 4200000000ull) {
                    g_hz = hz;
                    return;
                }
                break;
            }
            prev = cur;
            if (__builtin_ia32_rdtsc() - t0 > 5000000000ull) break;
        }
    }
pit_fallback:
    /* fallback: median of 5 PIT ch2 windows (~50 ms each)                  */
    u64t samples[5];
    for (int r = 0; r < 5; r++) {
        outb_(0x43, 0xB0);                        /* ch2, lo/hi, mode 0    */
        unsigned short cnt = 59659;               /* ~50 ms @ 1.193182 MHz */
        outb_(0x42, cnt & 0xFF);
        outb_(0x42, cnt >> 8);
        u64t a = __builtin_ia32_rdtsc();
        unsigned char v = inb_(0x61);
        outb_(0x61, (unsigned char)((v & ~0x02) | 0x01));   /* gate on     */
        while (!(inb_(0x61) & 0x20)) {}                     /* wait OUT2   */
        u64t b = __builtin_ia32_rdtsc();
        outb_(0x61, (unsigned char)(v & ~0x03));            /* gate off    */
        samples[r] = (b - a) * 1193182ull / cnt;
    }
    for (int i = 1; i < 5; i++) {                 /* insertion sort         */
        u64t k = samples[i]; int j = i - 1;
        while (j >= 0 && samples[j] > k) { samples[j+1] = samples[j]; j--; }
        samples[j+1] = k;
    }
    if (samples[2] > 2800000000ull && samples[2] < 4200000000ull)
        g_hz = samples[2];
}

int clock_gettime(int clk, struct timespec *tp)
{
    if (!tp) return -1; (void)clk;
    u64t ns = tsc_ns();
    tp->tv_sec = (time_t)(ns / 1000000000ull);
    tp->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
int gettimeofday(struct timeval *tv, void *tz)
{
    if (!tv) return -1; (void)tz;
    u64t ns = tsc_ns();
    tv->tv_sec = (time_t)(ns / 1000000000ull);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ull) / 1000ull);
    return 0;
}
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    u64t want = (u64t)req->tv_sec * 1000000000ull + (u64t)req->tv_nsec;
    u64t dl = tsc_ns() + want;
    /* hybrid: hlt until 2ms before deadline, then busy-wait for precision */
    if (dl > 2000000ULL) {
        u64t early = dl - 2000000ULL;
        if (smp_tsc_deadline_ok) {
            unsigned long long ticks = __builtin_ia32_rdtsc()
                                     + (early - tsc_ns()) * tsc_hz() / 1000000000ull;
            lapic_timer_arm_tscdeadline(ticks);
            __asm__ volatile("sti\n\thlt\n\tcli");
        } else {
            while (tsc_ns() < early) __asm__ volatile("sti\n\thlt\n\tcli");
        }
    }
    while (tsc_ns() < dl) __asm__ volatile("pause");
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
