OUTPUT_ARCH(mips)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);    /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);  /* PF_R */
    data PT_LOAD FLAGS(6);    /* PF_R | PF_W */
}

SECTIONS
{
    . = 0xffffffff80010000;
    .text : AT(ADDR(.text) & 0x1fffffff) {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : AT(ADDR(.rodata) & 0x1fffffff) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data : AT(ADDR(.data) & 0x1fffffff) {
        *(.data .data.*)
    } :data
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.MIPS.options)
        *(.comment)
        *(.pdr)
        *(.reginfo)
    }
}
