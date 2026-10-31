OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);       /* R-X */
    rodata PT_LOAD FLAGS(4);     /* R-- */
    data PT_LOAD FLAGS(6);       /* RW- */
    stack PT_GNU_STACK FLAGS(6); /* Non-executable guest stack. */
}

SECTIONS
{
    . = 0x80200000;
    .text : ALIGN(4)
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text

    . = 0x80400000;
    .rodata : ALIGN(16)
    {
        KEEP(*(.rodata.message))
        *(.rodata .rodata.*)
    } :rodata

    . = 0x80600000;
    .data : ALIGN(8)
    {
        *(.data .data.*)
    } :data
    .bss (NOLOAD) : ALIGN(8)
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
