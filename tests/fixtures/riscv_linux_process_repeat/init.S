    .section .text.init,"ax",@progbits
    .option norvc
    .option norelax

    .globl _start
    .type _start,@function
_start:
    li      a7, 172                 /* __NR_getpid */
    ecall
    li      t0, 1
    bne     a0, t0, .Lparent_failure
    mv      s0, a0

    /* Use the guest UART via Linux's existing 16550 terminal. */
    li      a0, -100                /* AT_FDCWD */
    la      a1, serial_path
    li      a2, 1                   /* O_WRONLY */
    li      a3, 0
    li      a7, 56                  /* __NR_openat */
    ecall
    blt     a0, zero, .Lparent_failure
    mv      s1, a0

    mv      a0, s1
    la      a1, parent_marker
    li      a2, 38
    li      a7, 64
    ecall
    li      t0, 38
    bne     a0, t0, .Lparent_failure

    la      t0, cow_value
    li      t1, 0x11223344
    sd      t1, 0(t0)
    la      t0, private_sentinel
    li      t1, 0x29aabbcc
    sw      t1, 0(t0)

    /* First genuine clone: child performs successful execve and exits 42. */
    li      a0, 17                  /* SIGCHLD, private address space */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, 220                 /* __NR_clone */
    .globl riscv29_clone_first_ecall
riscv29_clone_first_ecall:
    ecall
    .globl riscv29_clone_first_resume
riscv29_clone_first_resume:
    beqz    a0, .Lfirst_child
    li      t0, 2
    bltu    a0, t0, .Lparent_failure
    mv      s2, a0

    li      a7, 172
    ecall
    bne     a0, s0, .Lparent_failure
    mv      a0, s2
    la      a1, wait_status_first
    li      a2, 0
    li      a3, 0
    li      a7, 260                 /* __NR_wait4 */
    .globl riscv29_wait_first_ecall
riscv29_wait_first_ecall:
    ecall
    .globl riscv29_wait_first_resume
riscv29_wait_first_resume:
    bne     a0, s2, .Lparent_failure
    .globl riscv29_parent_cow_check
riscv29_parent_cow_check:
    la      t0, wait_status_first
    lw      t1, 0(t0)
    li      t2, (42 << 8)
    bne     t1, t2, .Lparent_failure
    la      t0, cow_value
    ld      t1, 0(t0)
    li      t2, 0x11223344
    bne     t1, t2, .Lparent_failure

    mv      a0, s1
    la      a1, first_reaped_marker
    li      a2, 46
    li      a7, 64
    ecall
    li      t0, 46
    bne     a0, t0, .Lparent_failure

    /* Second independent clone exercises failure recovery in its old image. */
    li      a0, 17
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, 220
    .globl riscv29_clone_second_ecall
riscv29_clone_second_ecall:
    ecall
    .globl riscv29_clone_second_resume
riscv29_clone_second_resume:
    beqz    a0, .Lsecond_child
    li      t0, 2
    bltu    a0, t0, .Lparent_failure
    mv      s3, a0
    li      a7, 172
    ecall
    bne     a0, s0, .Lparent_failure

    mv      a0, s3
    la      a1, wait_status_second
    li      a2, 0
    li      a3, 0
    li      a7, 260
    .globl riscv29_wait_second_ecall
riscv29_wait_second_ecall:
    ecall
    .globl riscv29_wait_second_resume
riscv29_wait_second_resume:
    bne     a0, s3, .Lparent_failure
    la      t0, wait_status_second
    lw      t1, 0(t0)
    li      t2, (43 << 8)
    bne     t1, t2, .Lparent_failure
    la      t0, private_sentinel
    lw      t1, 0(t0)
    li      t2, 0x29aabbcc
    bne     t1, t2, .Lparent_failure

    mv      a0, s1
    la      a1, second_reaped_marker
    li      a2, 47
    li      a7, 64
    ecall
    li      t0, 47
    bne     a0, t0, .Lparent_failure

    li      a0, -1
    la      a1, wait_status_empty
    li      a2, 1                   /* WNOHANG */
    li      a3, 0
    li      a7, 260
    .globl riscv29_wait_no_children_ecall
riscv29_wait_no_children_ecall:
    ecall
    li      t0, -10                 /* -ECHILD */
    bne     a0, t0, .Lparent_failure

    mv      a0, s1
    la      a1, all_reaped_marker
    li      a2, 43
    li      a7, 64
    ecall
    li      t0, 43
    bne     a0, t0, .Lparent_failure

.Lpid1_alive:
    li      a7, 172
    ecall
    bne     a0, s0, .Lparent_failure
    li      a7, 124                 /* __NR_sched_yield */
    ecall
    j       .Lpid1_alive

.Lfirst_child:
    li      a7, 172
    ecall
    li      t0, 2
    bltu    a0, t0, .Lchild_failure
    mv      s2, a0
    li      a7, 173                 /* __NR_getppid */
    ecall
    bne     a0, s0, .Lchild_failure

    la      t0, cow_value
    ld      t1, 0(t0)
    li      t2, 0x11223344
    bne     t1, t2, .Lchild_failure
    li      t1, 0x55667788
    .globl riscv29_child_cow_store
riscv29_child_cow_store:
    sd      t1, 0(t0)
    .globl riscv29_child_cow_verify
riscv29_child_cow_verify:
    ld      t1, 0(t0)
    li      t2, 0x55667788
    bne     t1, t2, .Lchild_failure

    mv      a0, s1
    la      a1, first_pre_exec_marker
    li      a2, 43
    li      a7, 64
    ecall
    li      t0, 43
    bne     a0, t0, .Lchild_failure

    la      a0, child_path
    la      a1, child_argv
    la      a2, child_envp
    li      a7, 221                 /* __NR_execve */
    .globl riscv29_execve_success_ecall
riscv29_execve_success_ecall:
    ecall
    li      a0, 99
    li      a7, 93
    ecall
    j       .Lchild_failure

.Lsecond_child:
    li      a7, 172
    ecall
    li      t0, 2
    bltu    a0, t0, .Lchild_failure
    mv      s2, a0
    li      a7, 173
    ecall
    bne     a0, s0, .Lchild_failure

    la      t0, private_sentinel
    lw      t1, 0(t0)
    li      t2, 0x29aabbcc
    bne     t1, t2, .Lchild_failure
    li      t1, 0x29998877
    .globl riscv29_private_sentinel_store
riscv29_private_sentinel_store:
    sw      t1, 0(t0)
    .globl riscv29_private_sentinel_verify
riscv29_private_sentinel_verify:
    lw      t1, 0(t0)
    li      t2, 0x29998877
    bne     t1, t2, .Lchild_failure

    mv      a0, s1
    la      a1, second_pre_exec_marker
    li      a2, 51
    li      a7, 64
    ecall
    li      t0, 51
    bne     a0, t0, .Lchild_failure

    la      a0, missing_path
    la      a1, missing_argv
    la      a2, missing_envp
    li      a7, 221                 /* __NR_execve */
    .globl riscv29_execve_failure_ecall
riscv29_execve_failure_ecall:
    ecall
    .globl riscv29_execve_failure_resume
riscv29_execve_failure_resume:
    li      t0, -2                  /* Linux -ENOENT */
    bne     a0, t0, .Lchild_failure

    li      a7, 172
    ecall
    bne     a0, s2, .Lchild_failure
    li      a7, 173
    ecall
    bne     a0, s0, .Lchild_failure
    la      t0, private_sentinel
    lw      t1, 0(t0)
    li      t2, 0x29998877
    bne     t1, t2, .Lchild_failure

    /* Exercise the original child stack after the failed execve return. */
    addi    sp, sp, -16
    .globl riscv29_recovery_stack_store
riscv29_recovery_stack_store:
    sd      s2, 0(sp)
    .globl riscv29_recovery_stack_load
riscv29_recovery_stack_load:
    ld      t0, 0(sp)
    .globl riscv29_recovery_stack_restore
riscv29_recovery_stack_restore:
    addi    sp, sp, 16
    bne     t0, s2, .Lchild_failure

    mv      a0, s1
    la      a1, recovery_marker
    li      a2, 61
    li      a7, 64
    ecall
    li      t0, 61
    bne     a0, t0, .Lchild_failure

    li      a0, 43
    li      a7, 93
    .globl riscv29_child_second_exit_ecall
riscv29_child_second_exit_ecall:
    ecall
    j       .Lchild_failure

.Lparent_failure:
    li      a7, 124
    ecall
    j       .Lparent_failure
.Lchild_failure:
    li      a7, 124
    ecall
    j       .Lchild_failure
    .size _start, .-_start

    .section .rodata,"a",@progbits
serial_path:
    .asciz "/dev/ttyS0"
child_path:
    .asciz "/bin/child"
missing_path:
    .asciz "/bin/riscv29-missing"
child_argv:
    .dword child_path
    .dword child_argument
    .dword 0
child_envp:
    .dword child_environment
    .dword 0
missing_argv:
    .dword missing_path
    .dword missing_argument
    .dword 0
missing_envp:
    .dword missing_environment
    .dword 0
child_argument:
    .asciz "execve-riscv29"
child_environment:
    .asciz "GUIDEXOS_RISCV29=present"
missing_argument:
    .asciz "expect-ENOENT"
missing_environment:
    .asciz "GUIDEXOS_RISCV29_FAILURE=present"
parent_marker:
    .ascii "guideXOS RISCV-29: parent initialized\n"
parent_marker_end:
first_pre_exec_marker:
    .ascii "guideXOS RISCV-29: first child before exec\n"
first_pre_exec_marker_end:
second_pre_exec_marker:
    .ascii "guideXOS RISCV-29: second child before failed exec\n"
second_pre_exec_marker_end:
first_reaped_marker:
    .ascii "guideXOS RISCV-29: first child reaped code=42\n"
first_reaped_marker_end:
recovery_marker:
    .ascii "guideXOS RISCV-29: failed execve recovered in original image\n"
recovery_marker_end:
second_reaped_marker:
    .ascii "guideXOS RISCV-29: second child reaped code=43\n"
second_reaped_marker_end:
all_reaped_marker:
    .ascii "guideXOS RISCV-29: both children collected\n"
all_reaped_marker_end:

    .section .data,"aw",@progbits
    .balign 8
    .globl cow_value
cow_value:
    .dword 0
    .globl private_sentinel
    .globl riscv29_private_sentinel
private_sentinel:
riscv29_private_sentinel:
    .word 0
wait_status_first:
    .word 0
wait_status_second:
    .word 0
wait_status_empty:
    .word 0
