    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax
    .globl _start
    .type _start,@function
_start:
    /* Validate the real Linux getpid result for init (PID 1). */
    li      a7, 172                 /* __NR_getpid, Linux RISC-V UAPI */
    ecall
    li      t0, 1
    bne     a0, t0, .Lyield

    /* Open the real serial TTY behind the configured UART console. */
    li      a0, -100                /* AT_FDCWD */
    la      a1, serial_path
    li      a2, 1                   /* O_WRONLY */
    li      a3, 0
    li      a7, 56                  /* __NR_openat, Linux RISC-V UAPI */
    ecall
    blt     a0, zero, .Lyield

    /* Use the descriptor returned by Linux openat for the proof write. */
    la      a1, proof_message
    li      a2, 49                  /* proof_message bytes, without a newline */
    li      a7, 64                  /* __NR_write, Linux RISC-V UAPI */
    ecall
    li      t0, 49
    bne     a0, t0, .Lyield

.Lyield:
    /* Keep PID 1 alive. The harness stops after observing a real yield. */
    li      a7, 124                 /* __NR_sched_yield, Linux RISC-V UAPI */
    ecall
    j       .Lyield
    .size _start, .-_start

    .section .rodata,"a",@progbits
    .balign 4
serial_path:
    .asciz "/dev/ttyS0"
proof_message:
.ascii "guideXOS RISCV-26: authentic U-mode /init reached"
