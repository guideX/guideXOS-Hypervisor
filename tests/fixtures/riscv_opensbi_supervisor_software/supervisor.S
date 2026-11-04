.option push
.option norvc
.option norelax

.equ RESULT_BASE,       0x83e00000
.equ SIGNATURE_BASE,    0x83e00100
.equ EVENT_LOG,         (SIGNATURE_BASE + 512)
.equ SNAPSHOT_LOG,      (SIGNATURE_BASE + 160)
.equ HOST_STAGE,        (RESULT_BASE + 0x280)
.equ GUEST_STACK_TOP,   0x83fffff0
.equ EXPECTED_FDT,      0x82200000
.equ COMPLETE_WORD,     0x434f4d504c455445
.equ SIGNATURE_MAGIC,   0x47584f5352495356
.equ SBI_BASE_EID,      0x10
.equ SBI_TIME_EID,      0x54494d45
.equ TIMER_DELTA,       4096
.equ UART0_BASE,        0x10000000
.equ UART1_BASE,        0x1000a000
.equ PLIC_BASE,         0x0c000000
.equ PLIC_PRIORITY_10,  (PLIC_BASE + 40)
.equ PLIC_PRIORITY_12,  (PLIC_BASE + 48)
.equ PLIC_PENDING,      (PLIC_BASE + 0x1000)
.equ PLIC_ENABLE_S,     (PLIC_BASE + 0x2080)
.equ PLIC_THRESHOLD_S,  (PLIC_BASE + 0x201000)
.equ PLIC_CLAIM_S,      (PLIC_BASE + 0x201004)
.equ ALL_S_IRQ_BITS,    0x222
.equ SSIP_BIT,          0x2
.equ STIP_BIT,          0x20
.equ SEIP_BIT,          0x200
.equ SSIE_BIT,          0x2
.equ STIE_BIT,          0x20
.equ SEIE_BIT,          0x200
.equ SSTATUS_SIE_BIT,   0x2
.equ EXPECTED_SSI,      0x8000000000000001
.equ EXPECTED_STI,      0x8000000000000005
.equ EXPECTED_SEI,      0x8000000000000009

.macro ARM_TIMER
 rdtime t0
 li t1, TIMER_DELTA
 add t0, t0, t1
 mv a0, t0
 li a6, 0
 li a7, SBI_TIME_EID
 ecall
 beqz a0, 1f
 j fail_sbi
1:
.endm

.macro SET_STAGE value
 li t0, HOST_STAGE
 li t1, \value
 sd t1, 0(t0)
.endm

.macro WAIT_EVENTS value
 li t0, \value
1: bltu s3, t0, 1b
.endm

.macro WAIT_PENDING mask
 li t0, \mask
1: csrr t1, sip
 and t1, t1, t0
 bne t1, t0, 1b
.endm

.macro RECORD_SNAPSHOT phase
 li t0, SNAPSHOT_LOG
 slli t1, s7, 6
 add t0, t0, t1
 li t1, \phase
 sd t1, 0(t0)
 csrr t1, sip
 sd t1, 8(t0)
 csrr t1, sie
 sd t1, 16(t0)
 csrr t1, sstatus
 sd t1, 24(t0)
 auipc t1, 0
 sd t1, 32(t0)
 li t1, PLIC_PENDING
 lw t2, 0(t1)
 sd t2, 40(t0)
 csrr t1, satp
 sd t1, 48(t0)
 sd zero, 56(t0)
 addi s7, s7, 1
 sd s7, 64(s2)
.endm

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
 mv s0, a0
 mv s1, a1
 li sp, GUEST_STACK_TOP
 li s2, SIGNATURE_BASE
 li s3, 0                    # Guest event count.
 li s4, 0                    # Supervisor timer entries.
 li s5, 0                    # Supervisor external entries.
 li s6, 0                    # Supervisor software entries.
 li s7, 0                    # Three-way guest snapshots.
 bnez s0, fail_hart
 li t0, EXPECTED_FDT
 bne s1, t0, fail_fdt
 sd s0, 16(s2)
 sd s1, 24(s2)

 # Identity map low MMIO and 0x80000000 RAM with Sv39 1 GiB leaves.
 lla t0, page_table_root
 li t1, 0x000000cf
 sd t1, 0(t0)
 li t1, 0x200000cf
 sd t1, 16(t0)
 lla t1, page_table_root
 srli t1, t1, 12
 li t2, 8
 slli t2, t2, 60
 or t1, t1, t2
 csrw satp, t1
 sfence.vma zero, zero
 lla t0, supervisor_trap_entry
 csrw stvec, t0

 # Confirm the real OpenSBI Base and TIME extensions before scheduling MTIP.
 li a0, 0
 li a1, 0
 li a6, 0
 li a7, SBI_BASE_EID
 ecall
 bnez a0, fail_sbi
 li t0, 0x03000000
 bne a1, t0, fail_sbi
 li a0, SBI_TIME_EID
 li a6, 3
 li a7, SBI_BASE_EID
 ecall
 bnez a0, fail_sbi
 li t0, 1
 bne a1, t0, fail_sbi

 # Enable real receive interrupts and the S-mode PLIC context.
 li t0, UART0_BASE
 li t1, 3
 sb t1, 3(t0)
 li t1, 1
 sb t1, 1(t0)
 li t0, UART1_BASE
 li t1, 3
 sb t1, 3(t0)
 li t1, 1
 sb t1, 1(t0)
 li t0, PLIC_PRIORITY_10
 li t1, 1
 sw t1, 0(t0)
 li t0, PLIC_PRIORITY_12
 li t1, 3
 sw t1, 0(t0)
 li t0, PLIC_ENABLE_S
 li t1, (1 << 10) | (1 << 12)
 sw t1, 0(t0)
 li t0, PLIC_THRESHOLD_S
 sw zero, 0(t0)

 # Architectural SSIP ownership proof: SSIP is set by a guest CSRRS while
 # SSIE remains clear, and its pending bit remains visible in sip.
 csrc sstatus, SSTATUS_SIE_BIT
 csrw sie, zero
 li t0, SSIP_BIT
 csrs sip, t0
 csrr t1, sip
 and t1, t1, t0
 beqz t1, fail_ssip_set
 csrr t1, sie
 and t1, t1, t0
 bnez t1, fail_ssie_gate

 # First WFI starts with no locally enabled interrupt. The host injects both
 # UART sources and advances only virtual time after this instruction waits.
 ARM_TIMER
 SET_STAGE 1
 wfi
 li t0, ALL_S_IRQ_BITS
 csrs sie, t0
 WAIT_PENDING ALL_S_IRQ_BITS
 RECORD_SNAPSHOT 1
 # WFI with all three pending while global SIE is masked must not trap and
 # need not remain asleep. The harness checks the instruction boundary.
 wfi
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 4
 csrc sstatus, SSTATUS_SIE_BIT

 # Three-way round 2: one UART0 source, guest SSIP and authentic STIP.
 li t0, SSIP_BIT
 csrs sip, t0
 ARM_TIMER
 SET_STAGE 2
 WAIT_PENDING ALL_S_IRQ_BITS
 RECORD_SNAPSHOT 2
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 7
 csrc sstatus, SSTATUS_SIE_BIT

 # Three-way round 3 uses UART1, retaining the same interrupt identities.
 li t0, SSIP_BIT
 csrs sip, t0
 ARM_TIMER
 SET_STAGE 3
 WAIT_PENDING ALL_S_IRQ_BITS
 RECORD_SNAPSHOT 3
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 10
 csrc sstatus, SSTATUS_SIE_BIT

 # SSIE masking: SEI and STI run while the original SSIP remains pending.
 li t0, SSIE_BIT
 csrc sie, t0
 li t0, SSIP_BIT
 csrs sip, t0
 ARM_TIMER
 SET_STAGE 4
 WAIT_PENDING (SEIP_BIT | STIP_BIT)
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 12
 csrr t1, sip
 and t1, t1, SSIP_BIT
 beqz t1, fail_ssip_lost
 csrs sie, SSIE_BIT
 WAIT_EVENTS 13
 csrc sstatus, SSTATUS_SIE_BIT

 # STIE masking: SEI and SSI run, and STIP is delivered after STIE returns.
 li t0, STIE_BIT
 csrc sie, t0
 li t0, SSIP_BIT
 csrs sip, t0
 ARM_TIMER
 SET_STAGE 5
 WAIT_PENDING ALL_S_IRQ_BITS
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 15
 csrr t1, sip
 and t1, t1, STIP_BIT
 beqz t1, fail_stip_lost
 li t0, STIE_BIT
 csrs sie, t0
 WAIT_EVENTS 16
 csrc sstatus, SSTATUS_SIE_BIT

 # SEIE masking: SSI and STI run while the UART byte and PLIC source persist.
 li t0, SEIE_BIT
 csrc sie, t0
 li t0, SSIP_BIT
 csrs sip, t0
 ARM_TIMER
 SET_STAGE 6
 WAIT_PENDING ALL_S_IRQ_BITS
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 18
 li t0, PLIC_PENDING
 lw t1, 0(t0)
 andi t1, t1, (1 << 10)
 beqz t1, fail_uart_pending
 li t0, UART0_BASE
 lbu t1, 5(t0)
 andi t1, t1, 1
 beqz t1, fail_uart_data
 li t0, SEIE_BIT
 csrs sie, t0
 WAIT_EVENTS 19
 csrc sstatus, SSTATUS_SIE_BIT

 # Second real WFI has no local supervisor enable. Only the authentic M-mode
 # timer can wake it; OpenSBI forwards MTIP into STIP before the S-mode STI.
 csrw sie, zero
 ARM_TIMER
 SET_STAGE 7
 wfi
 li t0, STIE_BIT
 csrs sie, t0
 li t0, SSTATUS_SIE_BIT
 csrs sstatus, t0
 WAIT_EVENTS 20
 csrc sstatus, SSTATUS_SIE_BIT

 # Final guest-owned completion summary and message.
 sd s3, 32(s2)
 sd s4, 40(s2)
 sd s5, 48(s2)
 sd s6, 56(s2)
 sd s7, 64(s2)
 csrr t0, satp
 sd t0, 72(s2)
 sd zero, 136(s2)
 lla t0, supervisor_message
 li t1, UART0_BASE
 li t2, RESULT_BASE
1:
 lbu t3, 0(t0)
 sb t3, 0(t2)
 beqz t3, 2f
 sb t3, 0(t1)
 addi t0, t0, 1
 addi t2, t2, 1
 j 1b
2:
 li t0, SIGNATURE_MAGIC
 sd t0, 0(s2)
 li t0, 14
 sd t0, 8(s2)
 fence rw, w
 li t0, COMPLETE_WORD
 sd t0, 128(s2)             # Completion is the final guest store.
guest_done:
 j guest_done

/* Each event is a guest-written 64-byte record:
 * sequence, scause, sepc, sip, sie, sstatus, source/ack evidence, auxiliary.
 */
supervisor_trap_entry:
 addi sp, sp, -96
 sd t0, 0(sp)
 sd t1, 8(sp)
 sd t2, 16(sp)
 sd t3, 24(sp)
 sd t4, 32(sp)
 sd t5, 40(sp)
 sd t6, 48(sp)
 sd a0, 56(sp)
 sd a1, 64(sp)
 sd a6, 72(sp)
 sd a7, 80(sp)
 csrr t0, scause
 addi t1, s3, 1
 slli t2, s3, 6
 li t3, EVENT_LOG
 add t6, t3, t2
 sd t1, 0(t6)
 sd t0, 8(t6)
 csrr t1, sepc
 sd t1, 16(t6)
 csrr t1, sip
 sd t1, 24(t6)
 csrr t1, sie
 sd t1, 32(t6)
 csrr t1, sstatus
 sd t1, 40(t6)
 sd zero, 48(t6)
 sd zero, 56(t6)
 li t1, EXPECTED_SEI
 beq t0, t1, service_external
 li t1, EXPECTED_SSI
 beq t0, t1, service_software
 li t1, EXPECTED_STI
 beq t0, t1, service_timer
 j fail_cause

service_external:
 li t0, PLIC_CLAIM_S
 lw t1, 0(t0)
 li t2, 10
 beq t1, t2, external_uart0
 li t2, 12
 bne t1, t2, fail_claim
 li t3, UART1_BASE
 j external_read
external_uart0:
 li t3, UART0_BASE
external_read:
 lbu t4, 2(t3)
 li t5, 4
 bne t4, t5, fail_iir
 lbu t4, 0(t3)
 slli t5, t1, 32
 or t5, t5, t4
 sd t5, 48(t6)
 addi s5, s5, 1
 addi s3, s3, 1
 sd s3, 32(s2)
 sd s5, 48(s2)
 li t0, PLIC_CLAIM_S
 sw t1, 0(t0)                # Complete only the claimed source.
 li t0, PLIC_PENDING
 lw t1, 0(t0)
 sd t1, 56(t6)
 j trap_return

service_software:
 csrr t1, sip
 sd t1, 48(t6)
 li t2, SSIP_BIT
 csrc sip, t2                # Guest acknowledges the local SSIP request.
 csrr t3, sip
 sd t3, 56(t6)
 and t4, t3, t2
 bnez t4, fail_ssip_ack
 xor t4, t1, t3
 not t5, t2
 and t4, t4, t5
 bnez t4, fail_ssip_cross_clear
 addi s6, s6, 1
 addi s3, s3, 1
 sd s3, 32(s2)
 sd s6, 56(s2)
 j trap_return

service_timer:
 rdtime t4
 sd t4, 48(t6)
 addi s4, s4, 1
 li t5, 7
 bgeu s4, t5, timer_disarm
 rdtime t4
 li t5, TIMER_DELTA
 add t4, t4, t5
 j timer_set
timer_disarm:
 li t4, -1
timer_set:
 sd t4, 56(t6)
 mv a0, t4
 li a6, 0
 li a7, SBI_TIME_EID
 ecall
 beqz a0, 1f
 j fail_sbi
1:
 addi s3, s3, 1
 sd s3, 32(s2)
 sd s4, 40(s2)

trap_return:
 ld t0, 0(sp)
 ld t1, 8(sp)
 ld t2, 16(sp)
 ld t3, 24(sp)
 ld t4, 32(sp)
 ld t5, 40(sp)
 ld t6, 48(sp)
 ld a0, 56(sp)
 ld a1, 64(sp)
 ld a6, 72(sp)
 ld a7, 80(sp)
 addi sp, sp, 96
 sret

fail_hart:              li t0, 1; j guest_fail
fail_fdt:               li t0, 2; j guest_fail
fail_sbi:               li t0, 3; j guest_fail
fail_cause:             li t0, 4; j guest_fail
fail_claim:             li t0, 5; j guest_fail
fail_iir:               li t0, 6; j guest_fail
fail_ssip_set:          li t0, 7; j guest_fail
fail_ssie_gate:         li t0, 8; j guest_fail
fail_ssip_lost:         li t0, 9; j guest_fail
fail_stip_lost:         li t0, 10; j guest_fail
fail_uart_pending:      li t0, 11; j guest_fail
fail_uart_data:         li t0, 12; j guest_fail
fail_ssip_ack:          li t0, 13; j guest_fail
fail_ssip_cross_clear:  li t0, 14; j guest_fail
guest_fail:
 sd t0, 136(s2)
fail_wait:
 wfi
 j fail_wait
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
supervisor_message:
 .asciz "Hello from SEI SSI STI on guideXOS"

.section .bss.page_tables,"aw",@nobits
.balign 4096
page_table_root:
 .zero 4096

.option pop
