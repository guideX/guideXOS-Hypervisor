.option push
.option norvc
.option norelax

.equ RESULT_BASE,        0x83e00000
.equ SIGNATURE_BASE,     0x83e00100
.equ EVENT_LOG,          (SIGNATURE_BASE + 512)
.equ GUEST_STACK_TOP,    0x83fffff0
.equ EXPECTED_FDT,       0x82200000
.equ COMPLETE_WORD,      0x434f4d504c455445
.equ SIGNATURE_MAGIC,    0x47584f5352495356
.equ UART0_BASE,         0x10000000
.equ UART1_BASE,         0x1000a000
.equ PLIC_BASE,          0x0c000000
.equ PLIC_PRIORITY_10,   (PLIC_BASE + 40)
.equ PLIC_PRIORITY_12,   (PLIC_BASE + 48)
.equ PLIC_PENDING,       (PLIC_BASE + 0x1000)
.equ PLIC_ENABLE_S,      (PLIC_BASE + 0x2080)
.equ PLIC_THRESHOLD_S,   (PLIC_BASE + 0x201000)
.equ PLIC_CLAIM_S,       (PLIC_BASE + 0x201004)
.equ PENDING_BOTH,       0x1400
.equ SEIE_BIT,           0x200
.equ SIE_BIT,            0x2
.equ EXPECTED_SEIP,      0x8000000000000009

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP
	li s2, SIGNATURE_BASE
	li s3, 0
	li s6, SIE_BIT
	sd zero, 32(s2)
	sd zero, 40(s2)
	sd zero, 48(s2)
	sd zero, 56(s2)
	sd zero, 64(s2)
	sd zero, 72(s2)
	sd zero, 80(s2)
	sd zero, 88(s2)
	sd zero, 96(s2)
	sd zero, 104(s2)
	sd zero, 112(s2)
	sd zero, 120(s2)
	sd zero, 128(s2)
	sd zero, 136(s2)
	li t0, RESULT_BASE
	sd zero, 0x200(t0)
	bnez s0, fail_hart
	li t0, EXPECTED_FDT
	bne s1, t0, fail_fdt

	# Identity-map low MMIO and the 0x80000000 RAM window with Sv39 1 GiB leaves.
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
	lla t0, supervisor_external_entry
	csrw stvec, t0

	# Configure two independent non-FIFO 16550 receive paths.
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

	# Enable sources 10/12 in S context 1 and install the trap gate.
	li t0, PLIC_ENABLE_S
	li t1, PENDING_BOTH
	sw t1, 0(t0)
	li t0, PLIC_THRESHOLD_S
	sw zero, 0(t0)
	li t0, SEIE_BIT
	csrs sie, t0

	li s4, 1
	jal ra, run_pair
	li s4, 2
	jal ra, run_pair
	li s4, 3
	jal ra, run_pair
	li s4, 4
	jal ra, run_pair
	li s4, 5
	jal ra, run_pair

	# Emit the required user-visible line through UART0, the OpenSBI console.
	lla t0, irq_message
	li t1, UART0_BASE
	li t3, RESULT_BASE
write_message_loop:
	lbu t2, 0(t0)
	sb t2, 0(t3)
	beqz t2, write_signature
	sb t2, 0(t1)
	addi t0, t0, 1
	addi t3, t3, 1
	j write_message_loop

write_signature:
	sd s0, 16(s2)
	sd s1, 24(s2)
	sd s3, 32(s2)
	li t1, SIGNATURE_MAGIC
	sd t1, 0(s2)
	li t1, 12
	sd t1, 8(s2)
	li t1, 5
	sd t1, 40(s2)               # Five simultaneous-pending snapshots.
	fence rw, w
	li t1, COMPLETE_WORD
	sd t1, 128(s2)              # Completion word is the final guest store.
guest_done:
	wfi
	j guest_done

/* s4 selects the scenario; host injects one byte per published handshake. */
run_pair:
	csrc sstatus, s6            # Keep global delivery masked until both pending.
	li t0, PLIC_PRIORITY_10
	li t1, 1
	li t2, 3
	li t3, 0
	li t4, 2
	beq s4, t4, priorities_reverse
	li t4, 3
	beq s4, t4, priorities_equal
	li t4, 4
	beq s4, t4, priorities_threshold
	j priorities_ready
priorities_reverse:
	li t1, 3
	li t2, 1
	j priorities_ready
priorities_equal:
	li t1, 2
	li t2, 2
	j priorities_ready
priorities_threshold:
	li t3, 2
priorities_ready:
	sw t1, 0(t0)
	li t0, PLIC_PRIORITY_12
	sw t2, 0(t0)
	li t0, PLIC_THRESHOLD_S
	sw t3, 0(t0)
	li t0, 5
	bne s4, t0, local_interrupt_ready
	li t0, SEIE_BIT
	csrc sie, t0                 # Phase 5 tests local SEIE masking separately.
local_interrupt_ready:
	ld s5, 32(s2)               # Event-log start index for this scenario.
	li t0, RESULT_BASE
	li t2, 10
	mul t1, s4, t2
	addi t1, t1, 1
	sd t1, 0x200(t0)            # Host may inject the first byte.
	li t1, 5
	beq s4, t1, pending_after_first
	wfi                          # First eligible PLIC request wakes WFI.
pending_after_first:
	li t0, RESULT_BASE
	li t2, 10
	mul t1, s4, t2
	addi t1, t1, 2
	sd t1, 0x200(t0)            # Host injects the other UART before unmask.
wait_both_pending:
	li t0, PLIC_PENDING
	lw t1, 0(t0)
	li t2, PENDING_BOTH
	and t1, t1, t2
	bne t1, t2, wait_both_pending
	addi t0, s4, -1
	slli t0, t0, 3
	add t0, s2, t0
	addi t0, t0, 160
	li t1, PENDING_BOTH
	sd t1, 0(t0)                 # Guest records both pending before any claim.

	li t0, 5
	bne s4, t0, enable_global
	# Local SEIE is separately disabled while both device conditions are pending.
	csrr t1, sip
	li t2, SEIE_BIT
	and t1, t1, t2
	beqz t1, fail_local_pending
	csrr t1, sie
	and t1, t1, t2
	bnez t1, fail_local_mask
	csrs sie, t2
enable_global:
	csrs sstatus, s6

	li t0, 4
	bne s4, t0, wait_pair_complete
	# With threshold 2, source 10 stays pending while source 12 is serviced.
	addi t0, s5, 1
wait_threshold_source:
	bltu s3, t0, wait_threshold_source
	csrc sstatus, s6
	li t0, PLIC_PENDING
	lw t1, 0(t0)
	andi t1, t1, (1 << 10)
	beqz t1, fail_threshold_pending
	li t0, UART0_BASE
	lbu t1, 5(t0)
	andi t1, t1, 1
	beqz t1, fail_uart_isolation
	li t0, PLIC_THRESHOLD_S
	sw zero, 0(t0)                # Release original source 10 condition.
	csrs sstatus, s6

wait_pair_complete:
	addi t0, s5, 2
wait_pair_loop:
	bltu s3, t0, wait_pair_loop
	csrc sstatus, s6
	ret

/* S-mode handles both source IDs and performs all device/controller MMIO. */
supervisor_external_entry:
	addi sp, sp, -64
	sd t0, 0(sp)
	sd t1, 8(sp)
	sd t2, 16(sp)
	sd t3, 24(sp)
	sd t4, 32(sp)
	sd t5, 40(sp)
	sd t6, 48(sp)
	csrr t0, scause
	li t1, EXPECTED_SEIP
	bne t0, t1, fail_interrupt
	li t0, PLIC_CLAIM_S
	lw t1, 0(t0)
	beqz t1, fail_claim_zero
	li t2, 10
	beq t1, t2, service_uart0
	li t2, 12
	bne t1, t2, fail_claim_id
	li t3, UART1_BASE
	j service_source
service_uart0:
	li t3, UART0_BASE
service_source:
	lbu t4, 2(t3)
	li t5, 4
	bne t4, t5, fail_iir
	lbu t4, 0(t3)                 # RBR acknowledges only this UART.
	li t5, EVENT_LOG
	slli t6, s3, 4
	add t5, t5, t6
	sd t1, 0(t5)
	sd t4, 8(t5)
	addi s3, s3, 1
	sd s3, 32(s2)
	li t0, PLIC_CLAIM_S
	sw t1, 0(t0)                  # Complete only the claimed source.
	ld t0, 0(sp)
	ld t1, 8(sp)
	ld t2, 16(sp)
	ld t3, 24(sp)
	ld t4, 32(sp)
	ld t5, 40(sp)
	ld t6, 48(sp)
	addi sp, sp, 64
	sret

fail_hart:                 li t1, 1; j fail
fail_fdt:                  li t1, 2; j fail
fail_interrupt:            li t1, 3; j fail
fail_claim_zero:           li t1, 4; j fail
fail_claim_id:             li t1, 5; j fail
fail_iir:                  li t1, 6; j fail
fail_local_pending:        li t1, 7; j fail
fail_local_mask:           li t1, 8; j fail
fail_threshold_pending:    li t1, 9; j fail
fail_uart_isolation:       li t1, 10; j fail
fail:
	sd t1, 136(s2)
fail_wait:
	wfi
	j fail_wait
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
irq_message:
	.asciz "Hello from dual UART IRQ on guideXOS"

.section .bss.page_tables,"aw",@nobits
.balign 4096
page_table_root:
	.zero 4096

.option pop
