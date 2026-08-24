/* ============================================================================
 * smp/apboot.c - enumerate CPUs via fw_cfg, park the trampoline at phys
 * 0x7000, and bring APs up one by one with the INIT-SIPI-SIPI dance.
 * LAPIC helpers (EOI / IPI / TSC-deadline) live here too.
 * ==========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "kern.h"
#include "../stubs/shim.h"

#define LAPIC_BASE   0xFEE00000UL
#define LAPIC_ID     0x020
#define LAPIC_EOI    0x0B0
#define LAPIC_SPUR   0x0F0
#define LAPIC_ICRLO  0x300
#define LAPIC_ICRHI  0x310
#define LAPIC_LVT_T  0x320

static inline void outb_(unsigned short p, unsigned char v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw_(unsigned short p, unsigned short v)
{ __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline unsigned char inb_(unsigned short p)
{ unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

extern struct kcpu kcpus[SMP_MAX_CPU];
volatile int kncpu_online = 1;

/* ---- trampoline blob (linked flat at VMA 0x7000, copied verbatim) ------ */
__asm__(
    ".section .rodata,\"a\"\n"
    ".global tramp_bin_start\n"
    "tramp_bin_start:\n"
    ".incbin \"smp/tramp.bin\"\n"
    ".global tramp_bin_end\n"
    "tramp_bin_end:\n"
    ".previous\n");
extern const unsigned char tramp_bin_start[];
extern const unsigned char tramp_bin_end[];

/* provided by core.c */
void ap_entry(unsigned long cpu_idx);

/* ---- LAPIC ------------------------------------------------------------- */
static volatile unsigned int *lapic(unsigned off)
    { return (volatile unsigned int *)(LAPIC_BASE + off); }

int g_x2apic;                              /* SeaBIOS may leave x2APIC on */

static void msr_write(unsigned idx, unsigned lo, unsigned hi)
{ __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(idx)); }
static unsigned long long msr_read(unsigned idx)
{ unsigned lo, hi; __asm__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(idx));
  return ((unsigned long long)hi << 32) | lo; }

int lapic_present(void)
{
    unsigned a, b, c, d;
    __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (d >> 9) & 1;                       /* CPUID.01H:EDX.APIC        */
}

void lapic_enable(void)
{
    unsigned long long apic_msr = msr_read(0x1B);
    g_x2apic = (int)((apic_msr >> 10) & 1);
    if (!g_x2apic && !((apic_msr >> 11) & 1)) {
        apic_msr |= 1ULL << 11;                /* APIC global enable        */
        msr_write(0x1B, (unsigned)apic_msr, (unsigned)(apic_msr >> 32));
    }
    if (g_x2apic) {
        unsigned long long svr = msr_read(0x80F);
        msr_write(0x80F, (unsigned)(svr | ((1u << 8) | 0x3F)),
                  (unsigned)(svr >> 32));
        /* program LVT Timer for TSC-deadline: vec 0x60, mode 10b, not masked */
        unsigned long long lvt = msr_read(0x832);
        lvt = (lvt & ~0xFFULL) | 0x60ULL | (2ULL << 17);
        lvt &= ~(1ULL << 16); /* clear mask */
        msr_write(0x832, (unsigned)lvt, (unsigned)(lvt>>32));
        /* divide config 0x3E0 = 1 (not used for TSC-deadline but set) */
        msr_write(0x83E, 0xB, 0);
    } else {
        *lapic(LAPIC_SPUR) |= (1u << 8) | 0x3F;/* enable, spurious vec      */
        /* LVT Timer: TSC-deadline mode, vector SMP_TIMER_VEC, not masked */
        *lapic(LAPIC_LVT_T) = (*lapic(LAPIC_LVT_T) & ~0xFF) | 0x60 | (2u << 17);
        *lapic(LAPIC_LVT_T) &= ~(1u << 16);
        *lapic(0x3E0) = 0xB; /* DCR Divide */
    }
}

void lapic_eoi(void)
{
    if (g_x2apic) { msr_write(0x80B, 0, 0); return; }
    *lapic(LAPIC_EOI) = 0;
}

void ipi_one(unsigned dest, unsigned cmd)
{
    if (g_x2apic) {
        msr_write(0x830, cmd, dest);       /* ICR: low=cmd high=dest         */
        return;
    }
    *lapic(LAPIC_ICRHI) = dest << 24;
    *lapic(LAPIC_ICRLO) = cmd;
}

void ipi_all_excl_self(unsigned cmd)
{
    if (g_x2apic) {
        msr_write(0x830, cmd | (3u << 18), 0);
        return;
    } else {
        *lapic(LAPIC_ICRHI) = 0;
        *lapic(LAPIC_ICRLO) = cmd | (3u << 18);
        return;
    }
}

unsigned lapic_id(void)
{
    if (g_x2apic) return (unsigned)msr_read(0x802);
    return (*lapic(LAPIC_ID) >> 24) & 0xFF;
}

/* TSC-deadline timer: LVT entry mode=010b, vector SMP_TIMER_VEC            */
static int tsc_deadline_ok(void)
{
    unsigned a, b, c, d;
    __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(6));
    return (a >> 2) & 1;                       /* leaf 6: bit2 TSC_DEADLINE */
}

int smp_tsc_deadline_ok;

void lapic_timer_arm_tscdeadline(unsigned long long ticks)
{
    { unsigned lo_ = (unsigned)ticks, hi_ = (unsigned)(ticks >> 32);
      __asm__ volatile("wrmsr" :: "a"(lo_), "d"(hi_), "c"(0x6E0)); }
}

/* ---- fw_cfg nb_cpus ----------------------------------------------------- */
static unsigned short fwcfg_nb_cpus(void)
{
    /* NB_CPUS is a pre-convention key: stored LITTLE-endian               */
    outw_(0x510, 0x0005);                      /* FW_CFG_NB_CPUS            */
    { unsigned char b0 = inb_(0x511), b1 = inb_(0x511);
      return (unsigned short)((b1 << 8) | b0); }
}

/* ---- AP launch ---------------------------------------------------------- */
static void patch16(void *p, unsigned short v)
    { *(volatile unsigned short *)p = v; }
static void patch64(void *p, unsigned long long v)
    { *(volatile unsigned long long *)p = v; }

static inline void pause_cpu(void) { __asm__ volatile("pause"); }

void smp_bringup(void)
{
    if (!lapic_present()) { write(1, "[smp] no LAPIC\n", 15); return; }
    lapic_enable();
    smp_tsc_deadline_ok = tsc_deadline_ok();
    write(1, smp_tsc_deadline_ok
             ? "\n[smp] LAPIC ok, TSC-deadline ok\n"
             : "\n[smp] LAPIC ok, no TSC-deadline (PIT fallback)\n",
          32);
    { char c='5'; write(1,&c,1); }

    unsigned short nb = fwcfg_nb_cpus();
    outw_(0x510, 0x000F);
    { unsigned char m0 = inb_(0x511), m1 = inb_(0x511);
      unsigned short mx = (unsigned short)((m1 << 8) | m0);   /* LE */
      if (mx > nb) nb = mx; }
    int want = (int)nb;
    if (want <= 0 || want > SMP_MAX_CPU) want = 1;
    write(1, "[smp] booting cpus=", 19);
    { char c = '0' + (char)want; write(1, &c, 1); write(1, "\n", 1); }

    /* copy trampoline to low memory once                                   */
    unsigned long sz = (unsigned long)(tramp_bin_end - tramp_bin_start);
    unsigned char *dst = (unsigned char *)TRAMP_PHYS;
    for (unsigned long i = 0; i < sz; i++) dst[i] = tramp_bin_start[i];
    { char m='0'+ (char)(sz & 0xff); }  /* hush unused */

    /* BSP slot                                                             */
    kcpus[0].id = lapic_id();
    kcpus[0].online = 1;

    unsigned long cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));


    for (int i = 1; i < want; i++) {
        /* per-AP parameter block                                           */
        patch64((void *)(TRAMP_PHYS + 0x100),
                (unsigned long long)(unsigned long)&ap_entry);
        patch64((void *)(TRAMP_PHYS + 0x108),
                (unsigned long long)(unsigned long)(kcpus[i].ap_stack
                        + sizeof kcpus[i].ap_stack));
        patch64((void *)(TRAMP_PHYS + 0x110), (unsigned long long)i);
        patch64((void *)(TRAMP_PHYS + 0x118), (unsigned long long)cr3);
        /* GDT descriptor copy: limit+base straight from pvh.S gdt_desc     */
        {
            extern unsigned char gdt_desc[];
            patch16((void *)(TRAMP_PHYS + 0x1f0),
                    *(unsigned short *)(const void *)gdt_desc);
            patch64((void *)(TRAMP_PHYS + 0x1f2),
                    *(unsigned long long *)(const void *)(gdt_desc + 2));
        }

        kcpus[i].online = 0;

        /* INIT -> (deassert) -> SIPI x2                                    */
        ipi_all_excl_self(0x00004500);          /* INIT, delivery 101 */
        for (volatile int k = 0; k < 200000; k++) pause_cpu();
        ipi_all_excl_self(0);                   /* level deassert      */
        for (volatile int k = 0; k < 200000; k++) pause_cpu();
        for (int attempt = 0; attempt < 2 && !kcpus[i].online; attempt++) {
            ipi_all_excl_self(0x00004600 | TRAMP_VECTOR);   /* SIPI */
            for (volatile int k = 0; k < 50000 && !kcpus[i].online; k++)
                pause_cpu();
        }
        /* wait for online flag                                             */
        unsigned long spins = 400000000ul;
        while (!kcpus[i].online && --spins) pause_cpu();
        if (!kcpus[i].online) { write(1, "[smp] ap timeout\n", 17); break; }
        { char c = '0' + (char)i; write(1, "[smp] AP ", 9); write(1, &c, 1); write(1, " online\n", 8); }
    }
    kncpu_online = want;
}

/* C-callable EOI for the asm stubs (x2APIC-aware)                          */
void lapic_eoi_c(void);
void lapic_eoi_c(void)
{
    if (g_x2apic) { msr_write(0x80B, 0, 0); return; }
    *lapic(LAPIC_EOI) = 0;
}
