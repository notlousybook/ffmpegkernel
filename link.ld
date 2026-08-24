/* link.ld — flat freestanding x86_64 ELF for the FFmpeg validation image.
 * Load address 0x400000: works under qemu-x86_64 user-mode, native Linux
 * (for quick sanity runs) AND as a `-kernel` payload for kernels that load
 * ELF64 directly.  __bss_start/__bss_end are exposed so a raw-binary kernel
 * loader can zero BSS before jumping to _start.                            */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4096)
    {
        *(.boot.text .boot.text.*)      /* PVH entry + trampoline          */
        *(.text .text.*)
    }

    /* QEMU -kernel needs this PT_NOTE to find our 32-bit PVH entry */
    .note.Xen.pvh : { KEEP(*(.note.Xen.pvh)) }

    .rodata : ALIGN(4096)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data .data.*)
        *(.got .got.*)
        *(.init_array .init_array.*)
        *(.fini_array .fini_array.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : ALIGN(4096)
    {
        *(.bss .bss.*)
        *(COMMON)
    }
    __bss_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)      /* no unwinding on bare metal */
    }
}
