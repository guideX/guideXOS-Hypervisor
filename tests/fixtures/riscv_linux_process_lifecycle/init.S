    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax
    .equ kParentMarkerLength, 37
    .equ kChildMarkerLength, 28
    .equ kParentSuccessMarkerLength, 42
    .globl _start
    .type _start,@function
_start:
    /* Linux starts this ELF as PID 1 in genuine U-mode. */
    li      a7, 172                 /* __NR_getpid */
    ecall
    li      t0, 1
    bne     a0, t0, .Lfailure
    mv      s0, a0                  /* Keep PID 1 across Linux syscalls. */

    /* Open the UART-backed tty through Linux and retain the inherited fd. */
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

    /* Both fork-like tasks inherit this initialized private data page. */
    la      t0, cow_value
    li      t1, 0x11223344
    sd      t1, 0(t0)

    /* RISC-V v6.12 CONFIG_CLONE_BACKWARDS order: flags, stack, ptid, tls, ctid. */
    li      a0, 17                  /* SIGCHLD; no CLONE_VM or CLONE_THREAD. */
    li      a1, 0                   /* Fork-like child uses inherited stack. */
    li      a2, 0                   /* No parent_tid pointer. */
    li      a3, 0                   /* No TLS change. */
    li      a4, 0                   /* No child_tid pointer. */
    li      a7, 220                 /* __NR_clone */
    .globl riscv27_clone_ecall
riscv27_clone_ecall:
    ecall
    .globl riscv27_clone_resume
riscv27_clone_resume:
    beqz    a0, .Lchild
    li      t0, 2
    bltu    a0, t0, .Lfailure       /* Parent must receive a positive child PID. */
    mv      s2, a0

    /* Keep PID 1 identifiable when its blocked wait4 call resumes. */
    li      a7, 172                 /* __NR_getpid */
    ecall
    bne     a0, s0, .Lfailure

    /* wait4 blocks while the child performs bounded yields and exits. */
    mv      a0, s2                  /* child pid */
    la      a1, wait_status         /* genuine parent U-mode memory */
    li      a2, 0                   /* options */
    li      a3, 0                   /* rusage */
    li      a7, 260                 /* __NR_wait4 */
    .globl riscv27_wait4_ecall
riscv27_wait4_ecall:
    ecall
    .globl riscv27_wait4_resume
riscv27_wait4_resume:
    bne     a0, s2, .Lfailure
    /* Child has exited: parent now proves its COW value stayed unchanged. */
    .globl riscv27_parent_cow_check
riscv27_parent_cow_check:
    la      t0, cow_value
    ld      t1, 0(t0)
    li      t2, 0x11223344
    bne     t1, t2, .Lfailure
    la      t0, wait_status
    lw      t1, 0(t0)
    li      t2, (37 << 8)
    bne     t1, t2, .Lfailure

    /* PID 1 created exactly one child and has now reaped it. */
    li      a0, -1                  /* any child */
    la      a1, wait_status
    li      a2, 1                   /* WNOHANG */
    li      a3, 0
    li      a7, 260                 /* __NR_wait4 */
    .globl riscv27_wait4_no_child_ecall
riscv27_wait4_no_child_ecall:
    ecall
    li      t0, -10                 /* -ECHILD */
    bne     a0, t0, .Lfailure

    mv      a0, s1
    la      a1, parent_success_marker
    li      a2, kParentSuccessMarkerLength
    li      a7, 64                  /* __NR_write */
    ecall
    li      t0, kParentSuccessMarkerLength
    bne     a0, t0, .Lfailure

    /* PID 1 remains alive; the harness stops after this genuine yield returns. */
.Lpid1_alive:
    li      a7, 172                 /* __NR_getpid */
    ecall
    bne     a0, s0, .Lfailure
    li      a7, 124                 /* __NR_sched_yield */
    ecall
    j       .Lpid1_alive

.Lchild:
    /* The same clone ECALL resumes with a0=0 in a distinct task context. */
    li      a7, 172                 /* __NR_getpid */
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
    bne     t1, t2, .Lfailure       /* Child initially inherits parent's value. */
    li      t1, 0x55667788
    .globl riscv27_child_cow_store
riscv27_child_cow_store:
    sd      t1, 0(t0)
    .globl riscv27_child_cow_verify
riscv27_child_cow_verify:
    ld      t1, 0(t0)
    li      t2, 0x55667788
    bne     t1, t2, .Lfailure

    mv      a0, s1                  /* File descriptor is inherited at clone. */
    la      a1, child_marker
    li      a2, kChildMarkerLength
    li      a7, 64                  /* __NR_write */
    ecall
    li      t0, kChildMarkerLength
    bne     a0, t0, .Lfailure

    /* Keep the child runnable long enough for the parent to enter wait4. */
    li      s3, 8
.Lchild_yield:
    li      a7, 124                 /* __NR_sched_yield */
    ecall
    addi    s3, s3, -1
    bnez    s3, .Lchild_yield

    li      a0, 37
    li      a7, 93                  /* __NR_exit */
    .globl riscv27_child_exit_ecall
riscv27_child_exit_ecall:
    ecall
    j       .Lfailure               /* Linux exit must not return. */

.Lfailure:
    /* Keep PID 1 bounded and observable if any guest assertion fails. */
    li      a7, 124                 /* __NR_sched_yield */
    ecall
    j       .Lfailure
    .size _start, .-_start

    .section .rodata,"a",@progbits
    .balign 4
serial_path:
    .asciz "/dev/ttyS0"
parent_marker:
    .ascii "guideXOS RISCV-27: parent initialized\n"
parent_marker_end:
child_marker:
    .ascii "guideXOS RISCV-27: child ran\n"
child_marker_end:
parent_success_marker:
    .ascii "guideXOS RISCV-27: parent reaped status=37\n"
parent_success_marker_end:

    .section .data,"aw",@progbits
    .balign 8
    .globl cow_value
cow_value:
    .dword 0
    .globl wait_status
wait_status:
    .word 0
