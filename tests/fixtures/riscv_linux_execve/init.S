    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax
    .equ kParentMarkerLength, 38
    .equ kPreExecMarkerLength, 37
    .equ kParentSuccessLength, 43

    .globl _start
    .type _start,@function
_start:
    /* Linux starts /init as PID 1 in U-mode. */
    li      a7, 172                 /* __NR_getpid */
    ecall
    li      t0, 1
    bne     a0, t0, .Lfailure
    mv      s0, a0

    /* Open the UART-backed terminal through Linux; clone and exec inherit it. */
    li      a0, -100                /* AT_FDCWD */
    la      a1, serial_path
    li      a2, 1                   /* O_WRONLY */
    li      a3, 0
    li      a7, 56                  /* __NR_openat */
    ecall
    blt     a0, zero, .Lfailure
    mv      s1, a0

    mv      a0, s1
    la      a1, parent_marker
    li      a2, kParentMarkerLength
    li      a7, 64                  /* __NR_write */
    ecall
    li      t0, kParentMarkerLength
    bne     a0, t0, .Lfailure

    /* The child must retain its own private copy across execve. */
    la      t0, cow_value
    li      t1, 0x11223344
    sd      t1, 0(t0)

    /* CONFIG_CLONE_BACKWARDS: flags, stack, ptid, tls, ctid. */
    li      a0, 17                  /* SIGCHLD, no CLONE_VM or CLONE_THREAD */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, 220                 /* __NR_clone */
    .globl riscv28_clone_ecall
riscv28_clone_ecall:
    ecall
    .globl riscv28_clone_resume
riscv28_clone_resume:
    beqz    a0, .Lchild
    li      t0, 2
    bltu    a0, t0, .Lfailure
    mv      s2, a0                  /* real child PID returned by Linux */

    li      a7, 172
    ecall
    bne     a0, s0, .Lfailure

    /* wait4 writes into genuine PID 1 userspace memory. */
    mv      a0, s2
    la      a1, wait_status
    li      a2, 0
    li      a3, 0
    li      a7, 260                 /* __NR_wait4 */
    .globl riscv28_wait4_ecall
riscv28_wait4_ecall:
    ecall
    .globl riscv28_wait4_resume
riscv28_wait4_resume:
    bne     a0, s2, .Lfailure

    .globl riscv28_parent_cow_check
riscv28_parent_cow_check:
    la      t0, cow_value
    ld      t1, 0(t0)
    li      t2, 0x11223344
    bne     t1, t2, .Lfailure
    la      t0, wait_status
    lw      t1, 0(t0)
    li      t2, (42 << 8)
    bne     t1, t2, .Lfailure

    li      a0, -1
    la      a1, wait_status
    li      a2, 1                   /* WNOHANG */
    li      a3, 0
    li      a7, 260
    ecall
    li      t0, -10                 /* -ECHILD */
    bne     a0, t0, .Lfailure

    mv      a0, s1
    la      a1, parent_success
    li      a2, kParentSuccessLength
    li      a7, 64
    ecall
    li      t0, kParentSuccessLength
    bne     a0, t0, .Lfailure

    /* The harness stops after observing a bounded PID 1 continuation. */
.Lpid1_alive:
    li      a7, 172
    ecall
    bne     a0, s0, .Lfailure
    li      a7, 124                 /* __NR_sched_yield */
    ecall
    j       .Lpid1_alive

.Lchild:
    /* clone returned zero in the original image and a distinct task. */
    li      a7, 172
    ecall
    li      t0, 2
    bltu    a0, t0, .Lfailure
    mv      s2, a0
    li      a7, 173                 /* __NR_getppid */
    ecall
    bne     a0, s0, .Lfailure

    la      t0, cow_value
    ld      t1, 0(t0)
    li      t2, 0x11223344
    bne     t1, t2, .Lfailure
    li      t1, 0x55667788
    .globl riscv28_child_cow_store
riscv28_child_cow_store:
    sd      t1, 0(t0)
    .globl riscv28_child_cow_verify
riscv28_child_cow_verify:
    ld      t1, 0(t0)
    li      t2, 0x55667788
    bne     t1, t2, .Lfailure

    mv      a0, s1
    la      a1, pre_exec_marker
    li      a2, kPreExecMarkerLength
    li      a7, 64
    ecall
    li      t0, kPreExecMarkerLength
    bne     a0, t0, .Lfailure

    /* The pathname, argv, envp, and their strings are in mapped U-mode memory. */
    la      a0, child_path
    la      a1, child_argv
    la      a2, child_envp
    li      a7, 221                 /* __NR_execve */
    .globl riscv28_execve_ecall
riscv28_execve_ecall:
    ecall
    /* Linux returns here only on failure; successful exec starts a new ELF. */
    li      a0, 99
    li      a7, 93                  /* __NR_exit */
    ecall
    j       .Lfailure

.Lfailure:
    /* Keep PID 1 alive and boundedly observable on a guest assertion failure. */
    li      a7, 124
    ecall
    j       .Lfailure
    .size _start, .-_start

    .section .rodata,"a",@progbits
    .balign 8
serial_path:
    .asciz "/dev/ttyS0"
child_path:
    .asciz "/bin/child"
child_argv:
    .dword child_path
    .dword child_argument
    .dword 0
child_envp:
    .dword child_environment
    .dword 0
child_argument:
    .asciz "execve-riscv28"
child_environment:
    .asciz "GUIDEXOS_RISCV28=present"
parent_marker:
    .ascii "guideXOS RISCV-28: parent initialized\n"
parent_marker_end:
pre_exec_marker:
    .ascii "guideXOS RISCV-28: child before exec\n"
pre_exec_marker_end:
parent_success:
    .ascii "guideXOS RISCV-28: parent reaped status=42\n"
parent_success_end:

    .section .data,"aw",@progbits
    .balign 8
    .globl cow_value
cow_value:
    .dword 0
    .globl wait_status
wait_status:
    .word 0
