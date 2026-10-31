OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text : ALIGN(4) {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = 0x80008000;
    .signature : ALIGN(8) { KEEP(*(.signature)) }
}
