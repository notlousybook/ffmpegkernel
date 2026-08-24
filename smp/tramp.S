/* Boot the secondary CPUs: INIT/SIPI lands them here in 16-bit real mode
 * at TRAMP_PHYS (0x7000).  Linked with VMA 0x7000 so every absolute
 * reference is pre-baked; the blob is memcpy'd to 0x7000 at runtime and a
 * parameter block below carries per-AP pointers.  Reuses the BSP's GDT
 * and page tables (identity-mapped first GiB).                            */

.global tramp_start
.global tramp_end

.section .rodata.tramp,"ax"
tramp_start:
.code16
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x6f00, %sp                    /* tiny stack below the page     */
    lgdt 0x71f0                         /* 10-byte descriptor copy       */

    /* CR4: PAE + SSE/AVX state enables, matching the BSP                  */
    mov %cr4, %eax
    or  $(1 << 5), %eax                 /* PAE                           */
    or  $((1 << 9) | (1 << 10) | (1 << 18)), %eax   /* OSFXSR|OSXMM|XSAVE */
    mov %eax, %cr4
    xor %ecx, %ecx                      /* XCR0 = x87|SSE|AVX like BSP   */
    xor %edx, %edx
    mov $0x7, %eax
    xsetbv

    /* EFER.LME                                                            */
    mov $0xC0000080, %ecx
    rdmsr
    or  $(1 << 8), %eax
    wrmsr

    /* CR3 from param block                                                */
    mov $0x7118, %eax
    mov (%eax), %eax
    mov %eax, %cr3

    /* paging on                                                           */
    mov %cr0, %eax
    or  $0x80010001, %eax               /* PG | PE                       */
    mov %eax, %cr0

    ljmp $0x18, $tramp_long             /* selector 0x18 = code64        */

.code64
tramp_long:
    mov $0x7108, %eax                   /* per-AP stack top              */
    mov (%rax), %rsp
    mov $0x7110, %eax                   /* cpu index                     */
    mov (%rax), %edi
    mov $0x7100, %rax                   /* &ap_entry                     */
    mov (%rax), %rax
    call *%rax
1:  cli
    hlt
    jmp 1b

/* ---- parameter block (patched by BSP before each SIPI) ----------------- */
.org 0x100                             /* -> VA 0x7100                  */
tramp_ap_main:  .quad 0                 /* 0x7100 &ap_entry              */
tramp_stack:    .quad 0                 /* 0x7108 stack top              */
tramp_cpuidx:   .quad 0                 /* 0x7110 cpu index              */
tramp_cr3:      .quad 0                 /* 0x7118 page table root        */
.org 0x1f0                              /* -> VA 0x71f0                  */
tramp_gdtd:     .word 0                 /* limit                         */
                .quad 0                 /* GDT linear base               */
tramp_end:
