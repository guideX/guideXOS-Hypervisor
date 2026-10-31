.option push
.option norvc
.option norelax

.equ GUEST_OUTPUT,  0x80010000
.equ GUEST_TRAP_LOG,0x80011000
.equ GUEST_STACK_TOP, 0x80FFFFF0
.equ GUEST_SIGNATURE,0x80010100
.equ MSTATUS_MPP_S, 0x800

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
    # The ELF entry starts in M-mode with paging Bare and hart ID zero.
    li sp, GUEST_STACK_TOP
    la t0, machine_trap
    csrw mtvec, t0
    la t0, supervisor_entry
    csrw mepc, t0
    li t0, MSTATUS_MPP_S
    csrw mstatus, t0
    mret
.size _start, .-_start

.align 2
.type machine_trap,@function
machine_trap:
    csrr t0, mcause
    li t1, 9                       # Expected S-mode ECALL (SBI Base).
    bne t0, t1, unexpected_trap
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    mret

unexpected_trap:
    li t1, GUEST_TRAP_LOG
    sd t0, 0(t1)
    csrr t0, mtval
    sd t0, 8(t1)
    ebreak
.size machine_trap, .-machine_trap

.align 2
.type supervisor_entry,@function
supervisor_entry:
    li s0, 0                       # Result status, zero means success.

    # Verify initialized .data and loader-zeroed .bss before touching either.
    la t0, initialized_word
    ld s1, 0(t0)
    li t1, 0x1122334455667788
    bne s1, t1, guest_failure

    la t0, zero_probe
    ld s2, 0(t0)
    bnez s2, guest_failure
    li s10, 1                      # .bss was zero before guest writes.

    # Genuine RV64M multiply.
    li t0, 6
    li t1, 7
    mul s5, t0, t1
    li t1, 42
    bne s5, t1, guest_failure

    # Genuine RV64A atomic update of a word initially supplied by .bss.
    la t0, atomic_counter
    li t1, 1
    amoadd.d.aqrl s3, t1, (t0)
    ld s4, 0(t0)
    bnez s3, guest_failure
    li t1, 1
    bne s4, t1, guest_failure

    # Write/read the guest-owned stack, then call and return from a function.
    addi sp, sp, -16
    li t0, 0x12345678
    sd t0, 0(sp)
    ld s7, 0(sp)
    addi sp, sp, 16
    li t1, 0x12345678
    bne s7, t1, guest_failure

    li a0, 41
    call add_one
    mv s6, a0
    li t1, 42
    bne s6, t1, guest_failure

    # Exercise the existing SBI Base firmware boundary from S-mode.
    li a7, 0x10                    # SBI_EXT_BASE
    li a6, 0                       # get_spec_version
    ecall
    mv s8, a0                      # SBI error
    mv s9, a1                      # SBI specification version
    bnez s8, guest_failure
    li t1, 0x02000000
    bne s9, t1, guest_failure

    # Copy the exact message to the reserved guest output page.
    la t0, guest_message
    li t1, GUEST_OUTPUT
1:
    lbu t2, 0(t0)
    sb t2, 0(t1)
    addi t0, t0, 1
    addi t1, t1, 1
    bnez t2, 1b
    j write_signature

guest_failure:
    li s0, 1

write_signature:
    li t0, GUEST_SIGNATURE
    li t1, 0x4758454C46380001
    sd t1, 0(t0)                   # "GXELF8" plus format marker.
    li t1, 1
    sd t1, 8(t0)                   # Signature version.
    sd s0, 16(t0)                  # Status.
    sd s5, 24(t0)                  # RV64M product.
    sd s3, 32(t0)                  # AMO previous counter.
    sd s4, 40(t0)                  # AMO final counter.
    sd s1, 48(t0)                  # Initialized .data.
    sd s10, 56(t0)                 # .bss zero check.
    sd s7, 64(t0)                  # Stack round-trip.
    sd s6, 72(t0)                  # Function return.
    sd s8, 80(t0)                  # SBI error.
    sd s9, 88(t0)                  # SBI spec version.
    li t1, 23
    sd t1, 96(t0)                  # Message byte count, not including NUL.
    fence rw, w
    li t1, 0x434F4D504C455445      # "COMPLETE".
    sd t1, 112(t0)                 # Written last by guest instructions.
2:
    j 2b
.size supervisor_entry, .-supervisor_entry

.type add_one,@function
add_one:
    addi a0, a0, 1
    ret
.size add_one, .-add_one

.section .rodata.message,"a",@progbits
.balign 4
guest_message:
    .asciz "Hello from RISC-V ELF64"

.section .data.fixture,"aw",@progbits
.balign 8
.globl initialized_word
initialized_word:
    .dword 0x1122334455667788

.section .bss.fixture,"aw",@nobits
.balign 8
.globl zero_probe
zero_probe:
    .zero 8
.globl atomic_counter
atomic_counter:
    .zero 8

.option pop
