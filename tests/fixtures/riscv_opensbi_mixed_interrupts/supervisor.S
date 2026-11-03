.option push
.option norvc
.option norelax

.equ RESULT_BASE,       0x83e00000
.equ SIGNATURE_BASE,    0x83e00100
.equ EVENT_LOG,         (SIGNATURE_BASE + 512)
.equ SNAPSHOT_LOG,      (SIGNATURE_BASE + 160)
.equ HOST_STAGE,        (RESULT_BASE + 0x220)
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
.equ PENDING_BOTH,      0x1400
.equ STIE_BIT,          0x20
.equ SEIE_BIT,          0x200
.equ SUPERVISOR_IRQS,   0x220
.equ SIE_BIT,           0x2
.equ EXPECTED_STIP,     0x8000000000000005
.equ EXPECTED_SEIP,     0x8000000000000009

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

.macro WAIT_EVENT value
 li t0, \value
1: bltu s3, t0, 1b
.endm
.macro WAIT_TIMER value
 li t0, \value
1: bltu s4, t0, 1b
.endm
.macro WAIT_EXTERNAL value
 li t0, \value
1: bltu s5, t0, 1b
.endm
.macro RECORD_SNAPSHOT
 li t0, SNAPSHOT_LOG
 li t1, 40
 mul t1, s7, t1
 add t0, t0, t1
 csrr t2, sip
 sd t2, 0(t0)
 csrr t2, sie
 sd t2, 8(t0)
 csrr t2, sstatus
 sd t2, 16(t0)
 auipc t2, 0
 sd t2, 24(t0)
 sd zero, 32(t0)
 addi s7, s7, 1
 sd s7, 56(s2)
.endm

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
 mv s0, a0
 mv s1, a1
 li sp, GUEST_STACK_TOP
 li s2, SIGNATURE_BASE
 li s3, 0                    # Total guest interrupt records
 li s4, 0                    # Timer handler entries
 li s5, 0                    # External handler entries
 li s6, SIE_BIT
 li s7, 0                    # Both-pending snapshots
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

 # Prove TIME is advertised by authentic OpenSBI.
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

 # Two independent UART receive paths and the real supervisor PLIC context.
 li t0, UART0_BASE
 li t1, 3
 sb t1, 3(t0)
 sb zero, 2(t0)
 li t1, 1
 sb t1, 1(t0)
 li t0, UART1_BASE
 li t1, 3
 sb t1, 3(t0)
 sb zero, 2(t0)
 li t1, 1
 sb t1, 1(t0)
 li t0, PLIC_PRIORITY_10
 li t1, 1
 sw t1, 0(t0)
 li t0, PLIC_PRIORITY_12
 li t1, 3
 sw t1, 0(t0)
 li t0, PLIC_ENABLE_S
 li t1, PENDING_BOTH
 sw t1, 0(t0)
 li t0, PLIC_THRESHOLD_S
 sw zero, 0(t0)
 li t0, SUPERVISOR_IRQS
 csrs sie, t0

 # A: STIP and SEIP both pending and locally enabled; SEI must win.
 csrc sstatus, s6
 ARM_TIMER
 SET_STAGE 1
1: csrr t0, sip
 li t1, SUPERVISOR_IRQS
 and t0, t0, t1
 bne t0, t1, 1b
 RECORD_SNAPSHOT
 csrs sstatus, s6
 WAIT_EVENT 2
 csrc sstatus, s6

 # B: STIP stays pending with STIE off while the UART interrupt is serviced.
 li t0, STIE_BIT
 csrc sie, t0
 li t0, SEIE_BIT
 csrs sie, t0
 ARM_TIMER
 SET_STAGE 2
 csrs sstatus, s6
 WAIT_EXTERNAL 2
 csrr t0, sip
 andi t0, t0, STIE_BIT
 beqz t0, fail_stip_lost
 csrr t0, sie
 andi t0, t0, STIE_BIT
 bnez t0, fail_stie_mask
 li t0, STIE_BIT
 csrs sie, t0
 WAIT_TIMER 2
 csrc sstatus, s6

 # C: pending UART/SEIP is locally masked; timer service preserves its byte.
 li t0, SEIE_BIT
 csrc sie, t0
 li t0, STIE_BIT
 csrs sie, t0
 ARM_TIMER
 SET_STAGE 3
 csrs sstatus, s6
 WAIT_TIMER 3
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
 WAIT_EXTERNAL 3
 csrc sstatus, s6

 # D: both sources arrive across a masked S-mode WFI; wake is not a trap.
 li t0, SUPERVISOR_IRQS
 csrs sie, t0
 csrc sstatus, s6
 ARM_TIMER
 SET_STAGE 4
 wfi
1: csrr t0, sip
 li t1, SUPERVISOR_IRQS
 and t0, t0, t1
 bne t0, t1, 1b
 RECORD_SNAPSHOT
 csrs sstatus, s6
 WAIT_EVENT 8
 csrc sstatus, s6

 # E: PLIC threshold masks UART0 while STI proceeds; lower it to release SEIP.
 li t0, PLIC_PRIORITY_10
 li t1, 1
 sw t1, 0(t0)
 li t0, PLIC_THRESHOLD_S
 li t1, 2
 sw t1, 0(t0)
 ARM_TIMER
 SET_STAGE 5
 csrs sstatus, s6
 WAIT_TIMER 5
 csrr t0, sip
 andi t0, t0, SEIE_BIT
 bnez t0, fail_threshold
 li t0, PLIC_PENDING
 lw t1, 0(t0)
 andi t1, t1, (1 << 10)
 beqz t1, fail_uart_pending
 li t0, UART0_BASE
 lbu t1, 5(t0)
 andi t1, t1, 1
 beqz t1, fail_uart_data
 li t0, PLIC_THRESHOLD_S
 sw zero, 0(t0)
 WAIT_EXTERNAL 5
 csrc sstatus, s6

 # F: device IER masks UART1; timer proceeds and later releases the same byte.
 li t0, UART1_BASE
 sb zero, 1(t0)
 ARM_TIMER
 SET_STAGE 6
 csrs sstatus, s6
 WAIT_TIMER 6
 li t0, UART1_BASE
 lbu t1, 5(t0)
 andi t1, t1, 1
 beqz t1, fail_uart_data
 li t1, 1
 sb t1, 1(t0)
 WAIT_EXTERNAL 6
 csrc sstatus, s6

 # G: three-way case. PLIC chooses source 12, then 10; SEI stays above STI.
 li t0, PLIC_PRIORITY_10
 li t1, 1
 sw t1, 0(t0)
 li t0, PLIC_PRIORITY_12
 li t1, 3
 sw t1, 0(t0)
 li t0, PLIC_THRESHOLD_S
 sw zero, 0(t0)
 li t0, SUPERVISOR_IRQS
 csrs sie, t0
 csrc sstatus, s6
 ARM_TIMER
 SET_STAGE 7
1: csrr t0, sip
 li t1, SUPERVISOR_IRQS
 and t0, t0, t1
 bne t0, t1, 1b
1: li t0, PLIC_PENDING
 lw t1, 0(t0)
 li t2, PENDING_BOTH
 and t1, t1, t2
 bne t1, t2, 1b
 RECORD_SNAPSHOT
 csrs sstatus, s6
 WAIT_EVENT 15
 csrc sstatus, s6

 # Stop the final deadline via the same authentic SBI TIME interface.
 li a0, -1
 li a6, 0
 li a7, SBI_TIME_EID
 ecall
 bnez a0, fail_sbi
 j write_completion

write_completion:
 sd s0, 16(s2)
 sd s1, 24(s2)
 sd s3, 32(s2)
 sd s4, 40(s2)
 sd s5, 48(s2)
 sd s7, 56(s2)
 csrr t0, satp
 sd t0, 64(s2)
 csrr t0, sip
 sd t0, 72(s2)
 csrr t0, sstatus
 sd t0, 80(s2)
 lla t0, mixed_message
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
 li t0, 13
 sd t0, 8(s2)
 fence rw, w
 li t0, COMPLETE_WORD
 sd t0, 128(s2)             # Completion marker is the final guest store.
guest_done:
 wfi
 j guest_done

/* The S-mode handler records cause/EPC/status and performs real device MMIO. */
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
 li t1, EXPECTED_SEIP
 beq t0, t1, service_external
 li t1, EXPECTED_STIP
 beq t0, t1, service_timer
 j fail_cause

service_external:
 li t2, EVENT_LOG
 slli t3, s3, 5
 add t6, t2, t3
 sd t0, 0(t6)
 csrr t4, sepc
 sd t4, 8(t6)
 csrr t4, sstatus
 sd t4, 16(t6)
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
 slli t5, t1, 8
 or t5, t5, t4
 sd t5, 24(t6)
 addi s3, s3, 1
 addi s5, s5, 1
 sd s3, 32(s2)
 sd s5, 48(s2)
 li t0, PLIC_CLAIM_S
 sw t1, 0(t0)                # Complete exactly the claimed PLIC source.
 j trap_return

service_timer:
 li t2, EVENT_LOG
 slli t3, s3, 5
 add t6, t2, t3
 sd t0, 0(t6)
 csrr t4, sepc
 sd t4, 8(t6)
 csrr t4, sstatus
 sd t4, 16(t6)
 rdtime t4
 sd t4, 24(t6)
 addi s3, s3, 1
 addi s4, s4, 1
 sd s3, 32(s2)
 sd s4, 40(s2)
 li t0, 7
 bgeu s4, t0, timer_disarm
 rdtime t0
 li t1, TIMER_DELTA
 add a0, t0, t1
 j timer_set
timer_disarm:
 li a0, -1
timer_set:
 li a6, 0
 li a7, SBI_TIME_EID
 ecall
 beqz a0, trap_return
 j fail_sbi

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

fail_hart:           li t0, 1; j guest_fail
fail_fdt:            li t0, 2; j guest_fail
fail_sbi:            li t0, 3; j guest_fail
fail_cause:          li t0, 4; j guest_fail
fail_claim:          li t0, 5; j guest_fail
fail_iir:            li t0, 6; j guest_fail
fail_stip_lost:      li t0, 7; j guest_fail
fail_stie_mask:      li t0, 8; j guest_fail
fail_uart_pending:   li t0, 9; j guest_fail
fail_uart_data:      li t0, 10; j guest_fail
fail_threshold:      li t0, 11; j guest_fail
guest_fail:
 sd t0, 136(s2)
fail_wait:
 wfi
 j fail_wait
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
mixed_message:
 .asciz "Hello from mixed IRQs on guideXOS"

.section .bss.page_tables,"aw",@nobits
.balign 4096
page_table_root:
 .zero 4096

.option pop
