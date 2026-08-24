/* ============================================================================
 * boot/intr.c - minimal interrupt plumbing so nanosleep() can "sti; hlt"
 * instead of busy-spinning the vCPU.  A spinning sleeper starves QEMU's UI
 * thread on small hosts and makes paced playback stutter.
 *
 *   IDT: 256 gates -> default_stub (bare iretq); vector 0x20 (IRQ0 after
 *   remap) -> timer_stub (EOI + iretq).  Sleepers poll their TSC deadline
 *   after every wake, so the ISR keeps no state.
 *   PIC: master remapped to 0x20..0x27, slave to 0x28..0x2f; only IRQ0 is
 *   unmasked.  PIT ch0: mode-2 rate generator, divisor 298 (~4004 Hz).
 * ==========================================================================*/
#include <stdint.h>

struct idt_gate {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t rsvd;
} __attribute__((packed));

static struct idt_gate idt[256] __attribute__((aligned(16)));
static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;

void timer_stub(void);                 /* in intr_asm.S                   */
void default_stub(void);
void smp_ipi_stub(void);
void smp_timer_stub(void);

static void set_gate(int v, const void *fn)
{
    unsigned long f = (unsigned long)fn;
    idt[v].off_lo    = (uint16_t)f;
    idt[v].sel       = 0x18;          /* code64 segment from pvh.S       */
    idt[v].ist       = 0;
    idt[v].type_attr = 0x8E;          /* present | ring0 | int gate      */
    idt[v].off_mid   = (uint16_t)(f >> 16);
    idt[v].off_hi    = (uint32_t)(f >> 32);
    idt[v].rsvd      = 0;
}

static inline void outb_(unsigned short p, unsigned char v)
{ __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }

void intr_init(void)
{
    for (int i = 0; i < 256; i++) set_gate(i, default_stub);
    set_gate(0x20, timer_stub);
    set_gate(0x50, smp_ipi_stub);          /* resched IPI                    */
    set_gate(0x60, smp_timer_stub);        /* LAPIC TSC-deadline             */

    idtr.limit = sizeof idt - 1;
    idtr.base  = (unsigned long)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));

    /* remap the PICs away from CPU exception vectors                       */
    outb_(0x20, 0x11);                /* ICW1: init | ICW4 needed        */
    outb_(0x21, 0x20);                /* ICW2: master base vector 0x20   */
    outb_(0x21, 0x04);                /* ICW3: slave on cascade IRQ2     */
    outb_(0x21, 0x01);                /* ICW4: 8086 mode                 */
    outb_(0xA0, 0x11);
    outb_(0xA1, 0x28);                /* slave base vector 0x28          */
    outb_(0xA1, 0x02);                /* cascade identity                */
    outb_(0xA1, 0x01);
    outb_(0x21, 0xFE);                /* mask all but IRQ0               */
    outb_(0xA1, 0xFF);

    /* PIT ch0 rate generator ~4004 Hz: wakes hlt sleepers every ~250 us    */
    outb_(0x43, 0x34);                /* ch0, lo/hi access, mode 2       */
    outb_(0x40, 298 & 0xFF);
    outb_(0x40, 298 >> 8);
}
