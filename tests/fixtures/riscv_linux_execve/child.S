    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax
    .equ kExecMarkerLength, 46
    .equ kFailureExit, 99

    .globl _start
    .type _start,@function
_start:
    /* Linux RISC-V ELF entry ABI: sp -> argc, argv[], NULL, envp[], NULL, auxv. */
    mv      s0, sp
    ld      t0, 0(s0)
    li      t1, 2
    bne     t0, t1, .Lbad_image
    addi    s1, s0, 8              /* argv[0] */

    ld      a0, 0(s1)
    la      a1, expected_path
    call    .Lstring_equal
    bnez    a0, .Lbad_image

    ld      a0, 8(s1)
    la      a1, expected_argument
    call    .Lstring_equal
    bnez    a0, .Lbad_image
    ld      t0, 16(s1)             /* argv[2] must be the terminating NULL. */
    bnez    t0, .Lbad_image

    addi    s2, s0, 32             /* envp[0], after two argv strings + NULL. */
    ld      a0, 0(s2)
    la      a1, expected_environment
    call    .Lstring_equal
    bnez    a0, .Lbad_image
    ld      t0, 8(s2)              /* exactly one environment entry */
    bnez    t0, .Lbad_image

    .globl riscv28_child_argv_validated
riscv28_child_argv_validated:
    /* The exec image still has the original Linux PID and parent relationship. */
    li      a7, 172                 /* __NR_getpid */
    ecall
    li      t0, 21
    bne     a0, t0, .Lbad_image
    mv      s3, a0
    li      a7, 173                 /* __NR_getppid */
    ecall
    li      t0, 1
    bne     a0, t0, .Lbad_image

    /* Open /dev/ttyS0 from the new process image using Linux openat. */
    li      a0, -100                /* AT_FDCWD */
    la      a1, serial_path
    li      a2, 1                   /* O_WRONLY */
    li      a3, 0
    li      a7, 56                  /* __NR_openat */
    ecall
    blt     a0, zero, .Lbad_image
    mv      s4, a0

    mv      a0, s4
    la      a1, exec_marker
    li      a2, kExecMarkerLength
    li      a7, 64                  /* __NR_write */
    ecall
    li      t0, kExecMarkerLength
    bne     a0, t0, .Lbad_image

    li      a0, 42
    li      a7, 93                  /* __NR_exit */
    .globl riscv28_child_exit_ecall
riscv28_child_exit_ecall:
    ecall
    j       .Lbad_image             /* exit must not return to U-mode */

.Lbad_image:
    li      a0, kFailureExit
    li      a7, 93
    ecall
    j       .Lbad_image

.Lstring_equal:
    lbu     t0, 0(a0)
    lbu     t1, 0(a1)
    bne     t0, t1, .Lstring_bad
    beqz    t0, .Lstring_good
    addi    a0, a0, 1
    addi    a1, a1, 1
    j       .Lstring_equal
.Lstring_bad:
    li      a0, 1
    ret
.Lstring_good:
    li      a0, 0
    ret
    .size _start, .-_start

    .section .rodata,"a",@progbits
serial_path:
    .asciz "/dev/ttyS0"
expected_path:
    .asciz "/bin/child"
expected_argument:
    .asciz "execve-riscv28"
expected_environment:
    .asciz "GUIDEXOS_RISCV28=present"
exec_marker:
    .ascii "guideXOS RISCV-28: new ELF argv/env ok pid=21\n"
exec_marker_end:

    .section .data,"aw",@progbits
    .balign 8
    .globl child_private_word
child_private_word:
    .dword 0
