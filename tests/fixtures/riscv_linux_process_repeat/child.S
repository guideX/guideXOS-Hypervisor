    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax

    .globl _start
    .type _start,@function
_start:
    /* Linux RISC-V ELF entry ABI: sp -> argc, argv[], NULL, envp[], NULL, auxv. */
    mv      s0, sp
    ld      t0, 0(s0)
    li      t1, 2
    bne     t0, t1, .Lbad_image
    addi    s1, s0, 8
    ld      a0, 0(s1)
    la      a1, expected_path
    call    .Lstring_equal
    bnez    a0, .Lbad_image
    ld      a0, 8(s1)
    la      a1, expected_argument
    call    .Lstring_equal
    bnez    a0, .Lbad_image
    ld      t0, 16(s1)
    bnez    t0, .Lbad_image

    addi    s2, s0, 32
    ld      a0, 0(s2)
    la      a1, expected_environment
    call    .Lstring_equal
    bnez    a0, .Lbad_image
    ld      t0, 8(s2)
    bnez    t0, .Lbad_image

    .globl riscv29_child_argv_validated
riscv29_child_argv_validated:
    li      a7, 172                 /* __NR_getpid */
    ecall
    li      t0, 2
    bltu    a0, t0, .Lbad_image
    mv      s3, a0
    li      a7, 173                 /* __NR_getppid */
    ecall
    li      t0, 1
    bne     a0, t0, .Lbad_image

    li      a0, -100                /* AT_FDCWD */
    la      a1, serial_path
    li      a2, 1                   /* O_WRONLY */
    li      a3, 0
    li      a7, 56                  /* __NR_openat */
    ecall
    blt     a0, zero, .Lbad_image
    mv      s4, a0

    mv      a0, s4
    la      a1, success_marker
    li      a2, 44
    li      a7, 64                  /* __NR_write */
    ecall
    li      t0, 44
    bne     a0, t0, .Lbad_image

    li      a0, 42
    li      a7, 93                  /* __NR_exit */
    .globl riscv29_child_exit_ecall
riscv29_child_exit_ecall:
    ecall
    j       .Lbad_image

.Lbad_image:
    li      a0, 99
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
    .asciz "execve-riscv29"
expected_environment:
    .asciz "GUIDEXOS_RISCV29=present"
success_marker:
    .ascii "guideXOS RISCV-29: first child ELF executed\n"
success_marker_end:
