// AARCH64-13 bare-metal fixture. The focused host test assembles the same
// instructions with the project's verified word encoders because this
// checkout has no AArch64 cross-assembler. Entry X0 is the generated DTB PA.
.text
.balign 4
.global _start
_start:
    ldr     w1, [x0]                 // Read FDT big-endian magic bytes.
    and     w2, w1, #0xffff          // Check low 16-bit signature 0x0dd0.
    cmp     w2, #0x0dd0
    b.ne    fail
    movz    w3, #1                   // Guest-side discovery success record.
    b       done
fail:
    movz    w3, #0
done:
    nop

// Separate UART fixture used to qualify real PL011 MMIO stores after the
// platform entry smoke. It intentionally tests the fixed-address device path;
// the current guest fixture does not yet parse and use the FDT UART node.
.global _uart_start
_uart_start:
    movz    x10, #0
    movk    x10, #0x0900, lsl #16
    movz    w11, #0x70
    str     w11, [x10, #0x2c]
    movz    w11, #0x301
    str     w11, [x10, #0x30]
    movz    w11, #0x4f
    str     w11, [x10]
    movz    w11, #0x4b
    str     w11, [x10]
    nop
