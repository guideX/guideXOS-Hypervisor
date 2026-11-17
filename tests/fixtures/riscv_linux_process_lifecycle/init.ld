OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x00010000;
    .text : ALIGN(4) {
        KEEP(*(.text.init))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text
    . = ALIGN(0x1000);
    .data : ALIGN(8) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data
    .bss : ALIGN(8) {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
