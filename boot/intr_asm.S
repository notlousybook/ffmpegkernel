/* boot/intr_asm.S - interrupt stubs (see intr.c)                           */
.code64
.text

/* Unexpected vector: just return.  Everything except IRQ0 is masked; an    */
/* iretq without EOI would latch the PIC level, but no other source fires.  */
.globl default_stub
default_stub:
    iretq

/* IRQ0 (PIT ~4 kHz): acknowledge and return.  Sleepers re-check their TSC  */
/* deadline after each wake - the ISR itself keeps no state and only        */
/* clobbers rax, which it saves.                                            */
.globl timer_stub
timer_stub:
    push %rax
    mov $0x20, %al
    out %al, $0x20                    /* EOI to master PIC               */
    pop %rax
    iretq

/* ---- SMP vectors: LAPIC EOI + iret (x2APIC-aware) ------------------------ */
.globl smp_ipi_stub                   /* resched kick                    */
smp_ipi_stub:
    push %rax
    push %rcx
    push %rdx
    call lapic_eoi_c
    pop %rdx
    pop %rcx
    pop %rax
    iretq

.globl smp_timer_stub                 /* TSC-deadline fired              */
smp_timer_stub:
    push %rax
    push %rcx
    push %rdx
    call lapic_eoi_c
    pop %rdx
    pop %rcx
    pop %rax
    iretq

.section .note.GNU-stack, "", @progbits
