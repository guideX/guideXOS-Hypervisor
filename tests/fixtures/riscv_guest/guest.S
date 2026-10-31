    .option norvc
    .option norelax
    .attribute arch, "rv64i2p1_zicsr2p0"

    .equ CSR_MSTATUS, 0x300
    .equ CSR_MTVEC,   0x305
    .equ CSR_MSCRATCH,0x340
    .equ CSR_MEPC,    0x341
    .equ CSR_MCAUSE,  0x342
    .equ MSTATUS_MPP_S, 0x800

    .section .text.boot,"ax",@progbits
    .globl _start
    .type _start,@function
_start:
    /* M-mode reset starts at 0x80000000 with paging disabled. */
    lla     sp, __machine_stack_top
    csrw    CSR_MSCRATCH, sp
    lla     t0, machine_trap_vector
    csrw    CSR_MTVEC, t0
    lla     t0, supervisor_entry
    csrw    CSR_MEPC, t0
    li      t0, MSTATUS_MPP_S
    csrw    CSR_MSTATUS, t0
    j       machine_mret
    .size _start, .-_start

    .section .text.handoff,"ax",@progbits
    .globl machine_mret
    .type machine_mret,@function
machine_mret:
    mret
    .size machine_mret, .-machine_mret

    .section .text.trap,"ax",@progbits
    .globl machine_trap_vector
    .type machine_trap_vector,@function
machine_trap_vector:
    /* Swap to the M stack and preserve temporary registers per SBI ABI. */
    csrrw   sp, CSR_MSCRATCH, sp
    addi    sp, sp, -16
    sd      t0, 0(sp)
    sd      t1, 8(sp)
    /* The only expected trap is the supervisor SBI ECALL (cause 9). */
    csrr    t0, CSR_MCAUSE
    li      t1, 9
    bne     t0, t1, machine_unexpected_trap
    csrr    t0, CSR_MEPC
    addi    t0, t0, 4
    csrw    CSR_MEPC, t0
    ld      t0, 0(sp)
    ld      t1, 8(sp)
    addi    sp, sp, 16
    csrrw   sp, CSR_MSCRATCH, sp
    mret

machine_unexpected_trap:
    /* Unexpected traps remain in a visible, non-privileged-progress loop. */
    wfi
    j       machine_unexpected_trap
    .size machine_trap_vector, .-machine_trap_vector

    .section .text.supervisor,"ax",@progbits
    .globl supervisor_entry
    .type supervisor_entry,@function
supervisor_entry:
    lla     sp, __supervisor_stack_top
    addi    sp, sp, -16
    sd      ra, 8(sp)

    /* Exercise arithmetic, a real JAL/JALR call-return, and stack memory. */
    addi    a0, zero, 20
    addi    a1, zero, 22
    jal     ra, compute_result
    sd      a0, 0(sp)
    ld      t0, 0(sp)
    addi    t1, zero, 42
    bne     t0, t1, supervisor_failure

    /* Exercise the production S-mode ECALL -> M-mode SBI Base path. */
    addi    a6, zero, 0
    addi    a7, zero, 0x10
    ecall
    bne     a0, zero, supervisor_failure
    lui     t2, 0x2000
    bne     a1, t2, supervisor_failure

    /* Zero the full signature, then populate every field through guest stores. */
    lla     t2, __signature_start
    sd      zero, 0(t2)
    sd      zero, 8(t2)
    sd      zero, 16(t2)
    sd      zero, 24(t2)
    sd      zero, 32(t2)
    sd      zero, 40(t2)
    sd      zero, 48(t2)
    sd      zero, 56(t2)
    sd      zero, 64(t2)
    li      t3, 0x47584f53
    sd      t3, 0(t2)
    addi    t3, zero, 1
    sd      t3, 8(t2)
    sd      zero, 16(t2)
    sd      t0, 24(t2)

    lla     t3, guest_message
    addi    t4, t2, 32
supervisor_copy_message:
    lbu     t5, 0(t3)
    sb      t5, 0(t4)
    addi    t3, t3, 1
    addi    t4, t4, 1
    bne     t5, zero, supervisor_copy_message

    ld      ra, 8(sp)
    addi    sp, sp, 16

    /* Completion is the final guest-memory write and is written last. */
    li      t6, 0x434f4d50
    sd      t6, 64(t2)
    j       supervisor_done

supervisor_failure:
    /* No completion marker is written on any failed guest-side check. */
    wfi
    j       supervisor_failure

supervisor_done:
    j       supervisor_done

compute_result:
    add     a0, a0, a1
    jalr    zero, 0(ra)
    .size supervisor_entry, .-supervisor_entry

    .section .rodata.message,"a",@progbits
    .balign 4
guest_message:
    .asciz "Hello from guideXOS RISC-V"
