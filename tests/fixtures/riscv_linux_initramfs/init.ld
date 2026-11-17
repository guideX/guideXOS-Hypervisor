OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
}

SECTIONS
{
    . = 0x00010000;
    .text : ALIGN(4) {
        KEEP(*(.text.init))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
