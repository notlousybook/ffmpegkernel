/* ============================================================================
 * boot/pvh.S - QEMU "-kernel" entry for the freestanding FFmpeg image.
 *
 * QEMU boots ELF64 kernels only through the PVH protocol: it needs an ELFNOTE
 * named "Xen" with type XEN_ELFNOTE_PHYS32_ENTRY (18) holding our 32-bit
 * physical entry point. QEMU enters us there in flat 32-bit protected mode,
 * paging off, with %ebx = physical address of the hvm_start_info struct
 * (which lists the -initrd as module #0!). We switch on long mode and jump to
 * metal_main(), which wires rd_base/rd_size from that info.
 *
 * Page tables identity-map the first 1 GiB with 2 MiB pages - plenty for a
 * ~12 MB binary + 256 MB heap pool + initrd.
 * ==========================================================================*/

/* ---- PVH ELF note -------------------------------------------------------- */
.section .note.Xen.pvh, "a", @note
.align 4
.long 4                     /* namesz ("Xen\0")                            */
.long 8                     /* descsz                                      */
.long 18                    /* XEN_ELFNOTE_PHYS32_ENTRY                    */
.asciz "Xen"
.align 4
.quad pvh_entry

/* ---- 32-bit trampoline --------------------------------------------------- */
.section .boot.text, "ax"
.code32
.globl pvh_entry
pvh_entry:
    cli
    mov %ebx, start_info_ptr            /* save hvm_start_info phys addr   */
    lgdt gdt_desc
    ljmp $0x08, $prot32
prot32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x3f8, %edx                    /* banner 'P' on COM1               */
    mov $'P', %al
    out %al, %dx
    mov $stack32_top, %esp              /* temp stack BEFORE wiping bss    */

    /* Wipe .bss NOW, while still in flat 32-bit mode: linear == physical,
     * and paging is not on yet - so wiping cannot kill our page tables.    */
    mov $__bss_start, %edi              /* resolved at link time           */
    mov $__bss_end, %ecx
    sub %edi, %ecx
    shr $2, %ecx                        /* dword count                     */
    xor %eax, %eax
    rep stosl

    mov $0x3f8, %edx                    /* banner 'W': bss wiped           */
    mov $'W', %al
    out %al, %dx
    xor %eax, %eax                      /* zero all page tables            */
    mov $pt_pml4, %edi
    mov $16384 >> 2, %ecx               /* pml4+pdpt+pd+pd_ap = 4 pages    */
    rep stosl

    mov $(pt_pdpt + 3), %eax            /* PML4[0] -> PDPT                 */
    mov %eax, pt_pml4
    mov $(pt_pd + 3), %eax              /* PDPT[0] -> PD                   */
    mov %eax, pt_pdpt
    /* PDPT[3] -> pd_ap: window for LAPIC MMIO at phys 0xFEE00000          */
    mov $(pt_pd_ap + 3), %eax
    mov %eax, pt_pdpt + 3*8
    mov $pt_pd, %edi                    /* PD[i] = i*2MiB | PS|RW|P        */
    mov $(3 | 0x80), %eax
    mov $512, %ecx
1:
    mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b
    /* pd_ap[503] maps 0xFEE00000 (0xC0000000 + 503*2MiB)                  */
    mov $(0xFEE00000 | 3 | 0x80), %eax
    mov %eax, pt_pd_ap + 503*8

    mov $0x3f8, %edx                    /* banner 'T': tables built        */
    mov $'T', %al
    out %al, %dx
    mov %cr4, %eax
    or  $0x20, %eax                     /* CR4.PAE                         */
    mov %eax, %cr4
    mov $0xC0000080, %ecx
    rdmsr
    or  $0x100, %eax                    /* EFER.LME                        */
    wrmsr
    /* Enable FPU/SSE/AVX state so codec DSP can use SSE:                  */
    mov %cr4, %eax
    or  $((1 << 9) | (1 << 10) | (1 << 18)), %eax   /* OSFXSR|OSXMMEXCPT|OSXSAVE */
    mov %eax, %cr4
    xor %ecx, %ecx                      /* XSETBV(XCR0): x87|SSE|AVX       */
    xor %edx, %edx
    mov $0x7, %eax
    xsetbv

    mov $pt_pml4, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or  $0x80010001, %eax               /* CR0.PG | CR0.PE                 */
    mov %eax, %cr0
    ljmp $0x18, $long_mode

.code64
long_banner:
    .ascii ""                           /* placeholder                     */
long_mode:
    mov $'L', %al                      /* banner 'L': long mode reached     */
    mov $0x3f8, %dx
    out %al, %dx
    mov $0x10, %ax                     /* data sel is valid in long mode too */
    mov %ax, %ds
    mov %ax, %ss
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    xor %rbp, %rbp
    mov $boot_stack_top, %rsp
    mov $'C', %al                      /* banner 'C': calling metal_main     */
    mov $0x3f8, %dx
    out %al, %dx
    mov start_info_ptr(%rip), %rdi      /* arg = hvm_start_info phys addr  */
    call metal_main
1:
    hlt
    jmp 1b

.section .boot.data, "aw"
start_info_ptr: .long 0
.align 16
stack32_bottom: .zero 8192
stack32_top:                             /* lives in LOADED data: survives wipe */

.align 16
.globl gdt
gdt:
    .quad 0                             /* null                            */
    .quad 0x00CF9A000000FFFF            /* sel 0x08: code32                */
    .quad 0x00CF92000000FFFF            /* sel 0x10: data                  */
    .quad 0x00209A0000000000            /* sel 0x18: code64 (L=1)          */
.globl gdt_desc
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .quad gdt

.section .bss.boot, "aw", @nobits
.align 4096
.global pt_pml4
pt_pml4:     .space 4096
pt_pdpt:     .space 4096
pt_pd:       .space 4096
pt_pd_ap:    .space 4096
.align 16
boot_stack_bottom:
.space 131072                            /* 128 KiB boot stack             */
boot_stack_top:
