OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64K
}

SECTIONS
{
    .text.boot ORIGIN(RAM) : ALIGN(4)
    {
        KEEP(*(.text.boot))
    } > RAM
    ASSERT(SIZEOF(.text.boot) <= 0x40, "M-mode startup exceeds fixed handoff slot")

    .text.handoff ORIGIN(RAM) + 0x40 : ALIGN(4)
    {
        KEEP(*(.text.handoff))
    } > RAM
    ASSERT(SIZEOF(.text.handoff) == 4, "MRET handoff slot must contain one instruction")

    .text.trap ORIGIN(RAM) + 0x80 : ALIGN(4)
    {
        KEEP(*(.text.trap))
    } > RAM

    .text.supervisor ORIGIN(RAM) + 0x100 : ALIGN(4)
    {
        KEEP(*(.text.supervisor))
    } > RAM
    ASSERT(ADDR(.text.supervisor) + SIZEOF(.text.supervisor) <= ORIGIN(RAM) + 0x1000,
           "supervisor code overlaps fixed read-only message region")

    .rodata ORIGIN(RAM) + 0x1000 : ALIGN(4)
    {
        KEEP(*(.rodata.message))
    } > RAM

    __image_start = ORIGIN(RAM);
    __image_end = ADDR(.rodata) + SIZEOF(.rodata);
    __signature_start = ORIGIN(RAM) + 0x8000;
    __supervisor_stack_top = ORIGIN(RAM) + 0xC000;
    __machine_stack_top = ORIGIN(RAM) + 0xE000;

    ASSERT(__image_end <= __signature_start, "guest image overlaps completion signature")
    ASSERT(__signature_start + 72 <= ORIGIN(RAM) + LENGTH(RAM),
           "completion signature outside guest RAM")
    ASSERT(__supervisor_stack_top < __machine_stack_top,
           "supervisor and machine stack placement invalid")

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
