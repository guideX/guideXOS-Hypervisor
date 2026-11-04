OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    stack PT_GNU_STACK FLAGS(6);
}
SECTIONS
{
    . = 0x80200000;
    .text : ALIGN(4) { KEEP(*(.text.entry)) *(.text .text.*) } :text
    .rodata : ALIGN(16) { *(.rodata.expected) KEEP(*(.rodata.message)) *(.rodata .rodata.*) } :rodata
    . = 0x80210000;
    .data : ALIGN(8) { *(.data .data.*) } :data
    .bss (NOLOAD) : ALIGN(8) { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
