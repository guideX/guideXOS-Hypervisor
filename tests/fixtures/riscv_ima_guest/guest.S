.option push
.option norvc
.option norelax

.equ ROOT_TABLE,       0x8000A000
.equ MID_TABLE,        0x8000B000
.equ LEAF_TABLE,       0x8000C000
.equ COUNTER_PA,       0x80009000
.equ READONLY_PA,      0x8000D000
.equ FAULT_RECORD,     0x8000E000
.equ STACK_TOP,        0x80010000
.equ COUNTER_ALIAS,    0x40009000
.equ READONLY_ALIAS,   0x4000D000

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
    li sp, STACK_TOP
    la t0, machine_trap
    csrw mtvec, t0

    # Set up an identity 1 GiB leaf and a separate Sv39 subtree used to map
    # the counter through a different virtual address.
    li t0, ROOT_TABLE
    li t1, MID_TABLE
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1
    sd t1, 8(t0)                  # root[1] -> mid table
    li t1, 0x200000CF
    sd t1, 16(t0)                 # root[2] -> identity 1 GiB RWX leaf

    li t0, MID_TABLE
    li t1, LEAF_TABLE
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1
    sd t1, 0(t0)                  # mid[0] -> leaf table

    li t0, LEAF_TABLE
    li t1, 0x20002407             # VA 0x40009000 -> PA counter, RW
    sd t1, 72(t0)
    li t1, 0x20003403             # VA 0x4000D000 -> PA read-only, R
    sd t1, 104(t0)

    li t0, READONLY_PA
    li t1, 55
    sd t1, 0(t0)

    li t0, ROOT_TABLE
    srli t0, t0, 12
    li t1, 0x8000000000000000
    or t0, t0, t1
    csrw satp, t0
    sfence.vma x0, x0

    la t0, supervisor_entry
    csrw mepc, t0
    li t0, 0x800                 # MPP=S
    csrw mstatus, t0
    mret
.size _start, .-_start

.align 2
.type machine_trap,@function
machine_trap:
    csrr t0, mcause
    li t1, 15
    bne t0, t1, 1f
    csrr t0, mtval
    li t1, READONLY_ALIAS
    bne t0, t1, 1f
    li t0, 15
    j 2f
1:
    csrr t0, mcause
2:
    li t1, FAULT_RECORD
    sd t0, 0(t1)
    csrr t0, mtval
    sd t0, 8(t1)
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    mret
.size machine_trap, .-machine_trap

.align 2
.type supervisor_entry,@function
supervisor_entry:
    li sp, STACK_TOP
    li s0, 0                      # result status
    li s2, 0                      # successful SC status
    li s3, 0                      # failed SC status
    li t0, COUNTER_PA
    sd zero, 0(t0)

    # Program A: genuine RV64M arithmetic, including 64-bit and W forms.
    li t0, 6
    li t1, 7
    mul t2, t0, t1
    li t3, 42
    bne t2, t3, guest_failure
    li t0, -7
    li t1, 3
    div t2, t0, t1
    li t3, -2
    bne t2, t3, guest_failure
    rem t2, t0, t1
    li t3, -1
    bne t2, t3, guest_failure
    li t0, 0x7fffffff
    li t1, 2
    mulw t2, t0, t1
    li t3, -2
    bne t2, t3, guest_failure
    li t0, 0xffffffff
    li t1, 2
    divuw t2, t0, t1
    li t3, 0x7fffffff
    bne t2, t3, guest_failure
    li t0, -1
    mulhu t2, t0, t0
    li t3, -2
    bne t2, t3, guest_failure
    li t0, -1
    li t1, 0x8000000000000000
    mulhsu t2, t0, t1
    li t3, -1
    bne t2, t3, guest_failure

    # Program B: AMO counter, successful SC, a real failed SC after an
    # overlapping guest store, then a bounded retry loop with no conflict.
    li t0, COUNTER_PA
    li t1, 1
    amoadd.d.aq t2, t1, (t0)
    bnez t2, guest_failure
    ld t3, 0(t0)
    li t4, 1
    bne t3, t4, guest_failure

1:
    lr.d.aq t2, (t0)
    addi t2, t2, 5
    sc.d.rl s2, t2, (t0)
    bnez s2, 1b
    ld t3, 0(t0)
    li t4, 6
    bne t3, t4, guest_failure

    lr.d.aq t2, (t0)
    sd t2, 0(t0)                  # overlapping write invalidates reservation
    li t3, 0x1234
    sc.d.aqrl s3, t3, (t0)
    beqz s3, guest_failure
    ld t4, 0(t0)
    li t5, 6
    bne t4, t5, guest_failure     # failed SC must leave memory unchanged

2:
    lr.d.aq t2, (t0)
    addi t2, t2, 1
    sc.d.rl t3, t2, (t0)
    bnez t3, 2b

    # Program C: use a virtual alias of the counter and then deliberately
    # fault an SC against a read-only Sv39 leaf PTE.
    li t0, COUNTER_ALIAS
    li t1, 1
    amoadd.d.aqrl s4, t1, (t0)
    li t2, 7
    bne s4, t2, guest_failure
    li t0, COUNTER_PA
    ld s5, 0(t0)
    li t2, 8
    bne s5, t2, guest_failure

    li t0, READONLY_ALIAS
    li t1, 0x1234
    sc.d.aqrl t2, t1, (t0)        # expected StorePageFault, even without LR
    li t0, READONLY_ALIAS
    ld t1, 0(t0)
    li t2, 55
    bne t1, t2, guest_failure
    li t0, FAULT_RECORD
    ld t1, 0(t0)
    li t2, 15
    bne t1, t2, guest_failure
    ld t1, 8(t0)
    li t2, READONLY_ALIAS
    bne t1, t2, guest_failure
    j write_signature

guest_failure:
    li s0, 1

write_signature:
    la t0, guest_signature
    li t1, 0x47584F53
    sd t1, 0(t0)
    li t1, 2
    sd t1, 8(t0)
    sd s0, 16(t0)
    li t1, 42
    sd t1, 24(t0)
    li t1, COUNTER_PA
    ld t2, 0(t1)
    sd t2, 32(t0)
    sd s2, 40(t0)
    sd s3, 48(t0)
    sd s4, 56(t0)
    sd s5, 64(t0)
    li t1, FAULT_RECORD
    ld t2, 0(t1)
    sd t2, 72(t0)
    ld t2, 8(t1)
    sd t2, 80(t0)
    fence rw, w
    li t1, 0x434F4D50
    sd t1, 88(t0)                 # completion flag is committed last
3:
    j 3b
.size supervisor_entry, .-supervisor_entry

.section .signature,"aw",@progbits
.balign 8
.globl guest_signature
guest_signature:
    .space 96

.option pop
