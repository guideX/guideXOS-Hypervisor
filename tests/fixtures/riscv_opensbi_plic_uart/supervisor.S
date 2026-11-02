.option push
.option norvc
.option norelax

.equ RESULT_BASE,        0x83e00000
.equ SIGNATURE_BASE,     0x83e00100
.equ GUEST_STACK_TOP,    0x83fffff0
.equ EXPECTED_FDT,       0x82200000
.equ COMPLETE_WORD,      0x434f4d504c455445
.equ SIGNATURE_MAGIC,    0x47584f5352495356
.equ UART_BASE,          0x10000000
.equ PLIC_BASE,          0x0c000000
.equ PLIC_PRIORITY_10,   (PLIC_BASE + 40)
.equ PLIC_ENABLE_S,      (PLIC_BASE + 0x2080)
.equ PLIC_THRESHOLD_S,   (PLIC_BASE + 0x201000)
.equ PLIC_CLAIM_S,       (PLIC_BASE + 0x201004)
.equ SEIE_BIT,           0x200
.equ SIE_BIT,            0x2
.equ EXPECTED_SEIP,      0x8000000000000009

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	# Authentic FW_JUMP enters S-mode with a0=hartid and a1=the FDT.
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP
	li t0, SIGNATURE_BASE
	sd zero, 0(t0)
	sd zero, 8(t0)
	sd zero, 16(t0)
	sd zero, 24(t0)
	sd zero, 32(t0)
	sd zero, 40(t0)
	sd zero, 48(t0)
	sd zero, 56(t0)
	sd zero, 64(t0)
	sd zero, 72(t0)
	sd zero, 80(t0)
	sd zero, 88(t0)
	sd zero, 96(t0)
	sd zero, 104(t0)
	sd zero, 112(t0)
	sd zero, 120(t0)
	sd zero, 128(t0)
	sd zero, 136(t0)
	li t0, RESULT_BASE
	sd zero, 0x200(t0)
	bnez s0, fail_hart
	li t0, EXPECTED_FDT
	bne s1, t0, fail_fdt

	# Identity map low MMIO and the 0x80000000 RAM window with Sv39 1 GiB leaves.
	lla t0, page_table_root
	li t1, 0x000000cf
	sd t1, 0(t0)                 # VA/PA [0, 1 GiB): includes UART and PLIC.
	li t1, 0x200000cf
	sd t1, 16(t0)                # Root VPN[2]=2 maps RAM at 0x80000000.
	lla t1, page_table_root
	srli t1, t1, 12
	li t2, 8
	slli t2, t2, 60
	or t1, t1, t2
	csrw satp, t1
	sfence.vma zero, zero

	lla t0, supervisor_external_entry
	csrw stvec, t0

	# Select the documented non-FIFO UART mode and enable receive-data interrupts.
	li t0, UART_BASE
	li t1, 3
	sb t1, 3(t0)                 # LCR: 8N1, DLAB clear.
	sb zero, 2(t0)               # FCR: FIFO disabled.
	li t1, 1
	sb t1, 1(t0)                 # IER: receive data available only.

	# OpenSBI initializes source priority to zero and clears both context banks.
	# Configure source 10 only for S-context 1, then remove the boot threshold.
	li t0, PLIC_PRIORITY_10
	li t1, 3
	sw t1, 0(t0)
	li t0, PLIC_ENABLE_S
	li t1, (1 << 10)
	sw t1, 0(t0)
	li t0, PLIC_THRESHOLD_S
	sw zero, 0(t0)

	# Enable local SEIE while leaving global SIE clear. The first pending byte
	# wakes WFI, remains pending through the global mask, then is delivered when
	# this guest instruction enables global SIE.
	li t0, SEIE_BIT
	csrs sie, t0
	csrr t1, sie
	and t1, t1, t0
	beqz t1, fail_seie
	li t0, RESULT_BASE
	li t1, 1
	sd t1, 0x200(t0)             # Host may inject event 1 after this handshake.
	wfi
	csrr t0, sip
	li t1, SEIE_BIT
	and t0, t0, t1
	beqz t0, fail_mask_pending
	li t0, SIGNATURE_BASE
	li t1, 1
	sd t1, 120(t0)               # Global-mask pending state was observed.
	li t0, SIE_BIT
	csrs sstatus, t0

after_first_interrupt:
	# SRET from event 1 returns here with SEIE masked. Poll guest-visible LSR,
	# verify sip.SEIP remains pending, then locally re-enable the interrupt.
	li t0, RESULT_BASE
	poll_second_byte:
	li t1, UART_BASE
	lbu t2, 5(t1)
	andi t2, t2, 1
	beqz t2, poll_second_byte
	csrr t2, sip
	li t3, SEIE_BIT
	and t2, t2, t3
	beqz t2, fail_local_mask_pending
	li t0, SIGNATURE_BASE
	li t1, 3
	sd t1, 120(t0)               # Global and local mask evidence.
	li t0, SEIE_BIT
	csrs sie, t0

after_second_interrupt:
	li t0, RESULT_BASE
	li t1, 4
	sd t1, 0x200(t0)             # Host may inject event 3 after this handshake.
wait_third_interrupt:
	wfi
	li t0, SIGNATURE_BASE
	ld t1, 32(t0)
	li t2, 3
	bgeu t1, t2, write_completion
	j wait_third_interrupt

/*
 * This real S-mode trap handler checks architectural cause 9, claims source
 * 10, reads the UART IIR and RBR, records the byte and claim ID, completes the
 * PLIC request, and returns with SRET. No host callback services the guest.
 */
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
	li t5, SIGNATURE_BASE
	ld t2, 32(t5)
	li t3, 3
	bgeu t2, t3, fail_extra_interrupt
	beqz t2, first_interrupt_state
	li t3, 1
	beq t2, t3, second_interrupt_state
	j third_interrupt_state

first_interrupt_state:
	sd t0, 88(t5)
	csrr t3, sepc
	sd t3, 96(t5)
	csrr t3, stval
	sd t3, 104(t5)
	csrr t3, sstatus
	sd t3, 112(t5)
	li t4, 0
	j service_interrupt
second_interrupt_state:
	li t4, 1
	j service_interrupt
third_interrupt_state:
	li t4, 2

service_interrupt:
	li t0, UART_BASE
	lbu t1, 2(t0)                 # IIR: 0x04 means receive data available.
	li t2, 4
	bne t1, t2, fail_iir
	li t3, PLIC_CLAIM_S
	lw t1, 0(t3)
	li t2, 10
	bne t1, t2, fail_claim
	lbu t2, 0(t0)                 # RBR read clears UART DR and deasserts IRQ.
	slli t4, t4, 3
	addi t3, t5, 40
	add t3, t3, t4
	sd t2, 0(t3)
	addi t3, t5, 64
	add t3, t3, t4
	sd t1, 0(t3)
	ld t2, 32(t5)
	addi t2, t2, 1
	sd t2, 32(t5)
	li t3, PLIC_CLAIM_S
	sw t1, 0(t3)                 # Completion is guest-issued after RBR.

	li t3, 1
	beq t2, t3, first_interrupt_done
	li t3, 2
	beq t2, t3, second_interrupt_done
	j interrupt_return

first_interrupt_done:
	li t0, SEIE_BIT
	csrc sie, t0
	li t0, RESULT_BASE
	li t1, 2
	sd t1, 0x200(t0)
	j interrupt_return
second_interrupt_done:
	li t0, RESULT_BASE
	li t1, 3
	sd t1, 0x200(t0)

interrupt_return:
	ld t0, 0(sp)
	ld t1, 8(sp)
	ld t2, 16(sp)
	ld t3, 24(sp)
	ld t4, 32(sp)
	ld t5, 40(sp)
	ld t6, 48(sp)
	addi sp, sp, 64
	sret

write_completion:
	# Send the requested text through the same UART THR used by OpenSBI.
	lla t0, irq_message
	li t1, UART_BASE
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
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 11
	sd t1, 8(t0)
	sd s0, 16(t0)
	sd s1, 24(t0)
	li t1, 3
	sd t1, 32(t0)
	fence rw, w
	li t1, COMPLETE_WORD
	sd t1, 128(t0)               # Versioned completion word is written last.
guest_done:
	wfi
	j guest_done

fail_hart:
	li t1, 1
	j fail
fail_fdt:
	li t1, 2
	j fail
fail_seie:
	li t1, 3
	j fail
fail_mask_pending:
	li t1, 4
	j fail
fail_local_mask_pending:
	li t1, 5
	j fail
fail_interrupt:
	li t1, 6
	j fail
fail_iir:
	li t1, 7
	j fail
fail_claim:
	li t1, 8
	j fail
fail_extra_interrupt:
	li t1, 9
fail:
	li t0, SIGNATURE_BASE
	sd t1, 136(t0)
fail_wait:
	wfi
	j fail_wait
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
irq_message:
	.asciz "Hello from OpenSBI UART IRQ on guideXOS"

.section .bss.page_tables,"aw",@nobits
.balign 4096
page_table_root:
	.zero 4096

.option pop
