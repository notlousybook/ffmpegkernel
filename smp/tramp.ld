OUTPUT_FORMAT(binary)
SECTIONS
{
    . = 0x7000;
    .rodata.tramp : { *(.rodata.tramp) }
    /DISCARD/ : { *(*) }
}
