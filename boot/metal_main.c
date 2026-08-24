/* ============================================================================
 * boot/metal_main.c - bare-metal entry: parse hvm_start_info, wire the RAM
 * disk from the QEMU -initrd module, then run the same decode harness.
 * ==========================================================================*/
#include "../stubs/shim.h"

extern unsigned char  *rd_base;         /* weak globals in syscall_shim.c   */
extern unsigned long   rd_size;

int kmain(void);                        /* test_harness.c                   */
void metal_main(unsigned long start_info_phys);

struct hvm_start_info {
    unsigned int  magic;                /* 0x336ec578                       */
    unsigned int  version;
    unsigned int  flags;
    unsigned int  nr_modules;
    unsigned long long modlist_paddr;
    unsigned long long cmdline_paddr;
    unsigned long long memmap_paddr;
    unsigned int  memmap_entries;
    unsigned int  pad;
} __attribute__((packed));
struct hvm_modlist_entry {
    unsigned long long paddr;
    unsigned long long size;
    unsigned long long cmdline_paddr;
    unsigned long long reserved;
} __attribute__((packed));

static inline void outw_(unsigned short p, unsigned short v)
    { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline unsigned char inb_(unsigned short p)
    { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* fw_cfg (ports 0x510/0x511): still populated by QEMU even for PVH boots */
static unsigned long long fwcfg_u64(unsigned short sel)
{
    unsigned long long v = 0;
    outw_(0x510, sel);
    for (int i = 0; i < 8; i++) v = (v << 8) | inb_(0x511);   /* big-endian */
    return v;
}

extern void intr_init(void);            /* boot/intr.c                     */
extern void metal_time_init(void);      /* RTC-calibrated TSC rate         */
extern unsigned long long tsc_hz(void);
extern void smp_bringup(void);          /* smp/apboot.c                    */
extern void sched_init_bsp(void);       /* smp/core.c                      */
extern int  kncpu_online;

void metal_main(unsigned long sip)
{
    { unsigned char bb='M'; __asm__ volatile("outb %0,%1"::"a"(bb),"Nd"(0x3f8)); }
    write(1, "\n[bare-metal] PVH entry OK\n", 28);

#define STEP_BANNER(c) do { char _c=(c); write(1, "[step] ", 7); \
                            write(1, &_c, 1); write(1, "\n", 1); } while (0)

    const struct hvm_start_info *si = (const void *)sip;
    int wired = 0;
    printf("[dbg] hvm magic=%08x nr_mod=%u modlist=%llx\n",
           si->magic, si->nr_modules,
           (unsigned long long)si->modlist_paddr);
    { unsigned char *p=(unsigned char*)sip;
      printf("[dbg] raw @%lx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
             sip, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
      printf("[dbg] raw+16: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
             p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23],p[24],p[25],p[26],p[27],p[28],p[29],p[30],p[31]);
    }
    if (si->magic == 0x336ec578u && si->nr_modules > 0 &&
        si->modlist_paddr) {
        const struct hvm_modlist_entry *mod =
            (const void *)(unsigned long)si->modlist_paddr;
        printf("[dbg] mod0 paddr=%llx size=%llx\n",
               (unsigned long long)mod[0].paddr, (unsigned long long)mod[0].size);
        if (mod[0].size) {
            rd_base = (unsigned char *)(unsigned long)mod[0].paddr;
            rd_size = (unsigned long)mod[0].size;
            wired = 1;
            printf("[bare-metal] initrd module: base=%p size=%lu\n",
                   rd_base, rd_size);
        }
    }
    if (!wired) {                                    /* fw_cfg fallback    */
        unsigned long long a = fwcfg_u64(0x11);      /* FW_CFG_INITRD_ADDR */
        unsigned long long s = fwcfg_u64(0x12);      /* FW_CFG_INITRD_SIZE */
        printf("[dbg] fw_cfg initrd addr=%llx size=%llx\n", a, s);
        if (a && s) {
            rd_base = (unsigned char *)a; rd_size = (unsigned long)s;
            wired = 1;
            printf("[bare-metal] initrd fw_cfg: base=%llx size=%llu\n", a, s);
        }
    }
    if (!wired)
        printf("[bare-metal] no initrd provided; harness will use its "
               "embedded copy\n");

    STEP_BANNER('A');
    intr_init();                        /* IDT + PIC + PIT: hlt sleepers   */
    STEP_BANNER('B');
    metal_time_init();                  /* calibrate TSC against RTC 1 s   */
    printf("[cal] g_hz=%llu\n", (unsigned long long)tsc_hz());

    /* raw-TSC probe mode: "tscprobe" in cmdline spins a fixed cycle count
     * between two serial markers - host measures true guest TSC rate      */
    if (si->cmdline_paddr) {
        const char *cl = (const void *)(unsigned long)si->cmdline_paddr;
        int probe = 0;
        for (const char *p = cl; *p; p++) {           /* strstr, no libc   */
            const char *q = p, *r = "tscprobe";
            while (*r && *q == *r) { q++; r++; }
            if (!*r) { probe = 1; break; }
        }
        if (probe) {
            write(1, "\nPROBE_A\n", 9);
            unsigned long long t0 = __builtin_ia32_rdtsc();
            while (__builtin_ia32_rdtsc() - t0 < 20000000000ULL) {}
            write(1, "\nPROBE_B\n", 9);
            _exit(0);
        }
    }
    STEP_BANNER('C');
    sched_init_bsp();                   /* CPU0 pseudo-thread              */
    STEP_BANNER('D');
    smp_bringup();                      /* INIT/SIPI the APs               */
    STEP_BANNER('E');

    int rc = kmain();
    _exit(rc);
}
