.option push
.option norvc
.option norelax

.equ RESULT_BASE,       0x83e00000
.equ SIGNATURE_BASE,    0x83e00100
.equ FAULT_LOG,         0x83e00200
.equ GUEST_STACK_TOP,   0x83fffff0
.equ EXPECTED_FDT,      0x82200000
.equ FIRMWARE_BASE,     0x80000000
.equ UART0_BASE,        0x10000000
.equ ROOT_TABLE,        0x83c00000
.equ L1_CODE_TABLE,     0x83c01000
.equ L1_ALIAS_TABLE,    0x83c02000
.equ L0_ALIAS_TABLE,    0x83c03000
.equ VA_FIRMWARE,       0x40000000
.equ VA_FIRMWARE_ALIAS, 0x40001000
.equ VA_PAGE_FAULT,     0x40002000
.equ COMPLETE_WORD,     0x434f4d504c455445
.equ SIGNATURE_MAGIC,   0x47584f5352495356

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP
	addi sp, sp, -16
	li t0, 0x6a09e667f3bcc909
	sd t0, 0(sp)
	ld s6, 0(sp)
	addi sp, sp, 16

	csrr t0, sstatus
	li s10, 1
	bnez s0, fail
	li t0, EXPECTED_FDT
	bne s1, t0, fail

	lla t0, initialized_word
	ld s7, 0(t0)
	li t1, 0x1122334455667788
	bne s7, t1, fail
	li t0, 6
	li t1, 7
	mul s4, t0, t1
	li t2, 42
	bne s4, t2, fail
	lla t0, work_slot
	sd s4, 0(t0)
	ld s5, 0(t0)
	bne s5, s4, fail

	la t0, supervisor_trap
	csrw stvec, t0
	li t0, FAULT_LOG
	sd zero, 0(t0)

	# Bare physical firmware accesses must trap to this S-mode handler.
	la t0, resume_bare_load
	csrw sscratch, t0
	li t1, FIRMWARE_BASE
	ld t2, 0(t1)
	j fail
resume_bare_load:
	la t0, resume_bare_store
	csrw sscratch, t0
	li t1, FIRMWARE_BASE
	li t2, 0x12345678
	sd t2, 0(t1)
	j fail
resume_bare_store:
	la t0, resume_bare_fetch
	csrw sscratch, t0
	li t0, FIRMWARE_BASE
	jalr t0
	j fail
resume_bare_fetch:

	# Authorized S-mode UART output proves an allowed platform MMIO region.
	li t0, UART0_BASE
	la t1, guest_message
uart_write_loop:
	lbu t2, 0(t1)
	beqz t2, uart_write_done
	sb t2, 0(t0)
	addi t1, t1, 1
	j uart_write_loop
uart_write_done:

	# Build page tables in guest RAM. VA 0x80200000 maps the guest's 2 MiB
	# code/data superpage; VA 0x83e00000 maps its stack and result area.
	li t0, ROOT_TABLE
	li t1, 2048
clear_tables:
	sd zero, 0(t0)
	addi t0, t0, 8
	addi t1, t1, -1
	bnez t1, clear_tables

	li t0, ROOT_TABLE
	li t1, L1_CODE_TABLE
	srli t1, t1, 2
	ori t1, t1, 1
	sd t1, 16(t0)                 # VPN[2]=2
	li t1, L1_ALIAS_TABLE
	srli t1, t1, 2
	ori t1, t1, 1
	sd t1, 8(t0)                  # VPN[2]=1

	li t0, L1_CODE_TABLE
	li t1, 0x80200000
	srli t1, t1, 2
	ori t1, t1, 0xcf
	sd t1, 8(t0)                  # code/data 2 MiB leaf
	li t1, 0x83e00000
	srli t1, t1, 2
	ori t1, t1, 0xcf
	sd t1, 248(t0)                # stack/result 2 MiB leaf

	li t0, L1_ALIAS_TABLE
	li t1, L0_ALIAS_TABLE
	srli t1, t1, 2
	ori t1, t1, 1
	sd t1, 0(t0)

	li t0, L0_ALIAS_TABLE
	li t1, FIRMWARE_BASE
	srli t1, t1, 2
	ori t1, t1, 0xcf
	sd t1, 0(t0)                  # valid Sv39 R/W/X mapping, PMP denies final PA
	sd t1, 8(t0)                  # second VA alias to the same physical page
	li t1, FIRMWARE_BASE
	srli t1, t1, 2
	ori t1, t1, 0x49              # valid execute-only leaf: load must page-fault
	sd t1, 16(t0)

	li t0, ROOT_TABLE
	srli t0, t0, 12
	li t1, 8
	slli t1, t1, 60
	or t0, t0, t1
	csrw satp, t0
	sfence.vma zero, zero

	# A stack round trip after paging proves the current guest stack mapping.
	addi sp, sp, -8
	li t0, 0x13579bdf
	sd t0, 0(sp)
	ld t1, 0(sp)
	bne t1, t0, fail
	addi sp, sp, 8

	# Sv39 final physical reads and fetches still obey PMP.
	la t0, resume_sv39_load
	csrw sscratch, t0
	li t1, VA_FIRMWARE
	ld t2, 0(t1)
	j fail
resume_sv39_load:
	la t0, resume_sv39_alias
	csrw sscratch, t0
	li t1, VA_FIRMWARE_ALIAS
	ld t2, 0(t1)
	j fail
resume_sv39_alias:
	la t0, resume_sv39_fetch
	csrw sscratch, t0
	li t0, VA_FIRMWARE
	jalr t0
	j fail
resume_sv39_fetch:
	la t0, resume_page_fault
	csrw sscratch, t0
	li t1, VA_PAGE_FAULT
	ld t2, 0(t1)
	j fail
resume_page_fault:

	# Ask authentic OpenSBI for its Base version after the protection checks.
	li a0, 0
	li a1, 0
	li a6, 0
	li a7, 0x10
	ecall
	mv s2, a0
	mv s3, a1
	bnez s2, fail
	li t0, 0x03000000
	bne s3, t0, fail

	# Require all seven expected faults, in order, before reporting success.
	li t0, FAULT_LOG
	ld t1, 0(t0)
	li t2, 7
	bne t1, t2, fail

	# Copy the required message into guest RAM while Sv39 is active.
	li t0, RESULT_BASE
	la t1, guest_message
copy_message:
	lbu t2, 0(t1)
	sb t2, 0(t0)
	beqz t2, message_done
	addi t0, t0, 1
	addi t1, t1, 1
	j copy_message
message_done:

	# Versioned guest-authored result record. The completion word is written last.
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 2
	sd t1, 8(t0)
	sd zero, 16(t0)
	li t1, 7
	sd t1, 24(t0)
	sd s3, 32(t0)
	sd s4, 40(t0)
	sd s5, 48(t0)
	sd s6, 56(t0)
	sd s7, 64(t0)
	li t1, FIRMWARE_BASE
	sd t1, 72(t0)
	li t1, VA_FIRMWARE
	sd t1, 80(t0)
	li t1, VA_FIRMWARE_ALIAS
	sd t1, 88(t0)
	li t1, VA_PAGE_FAULT
	sd t1, 96(t0)
	sd s10, 104(t0)
	fence rw, w
	li t1, COMPLETE_WORD
	sd t1, 112(t0)
guest_done:
	wfi
	j guest_done

fail:
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 2
	sd t1, 8(t0)
	li t1, 1
	sd t1, 16(t0)
fail_wait:
	wfi
	j fail_wait

.globl supervisor_trap
supervisor_trap:
	csrr t0, scause
	csrr t1, stval
	csrr t2, sepc
	li t3, FAULT_LOG
	ld t4, 0(t3)
	li t5, 7
	bgeu t4, t5, trap_unexpected
	la t5, expected_causes
	slli t6, t4, 3
	add t5, t5, t6
	ld t5, 0(t5)
	bne t0, t5, trap_unexpected
	la t5, expected_tvals
	add t5, t5, t6
	ld t5, 0(t5)
	bne t1, t5, trap_unexpected

	# Each entry is {cause, tval, sepc}. Record the actual architectural trap.
	slli t5, t4, 4
	slli t6, t4, 3
	add t5, t5, t6
	addi t5, t5, 8
	add t5, t5, t3
	sd t0, 0(t5)
	sd t1, 8(t5)
	sd t2, 16(t5)
	addi t4, t4, 1
	sd t4, 0(t3)
	csrr t5, sscratch
	csrw sepc, t5
	sret

trap_unexpected:
	li t3, SIGNATURE_BASE
	li t4, 1
	sd t4, 16(t3)
	la t5, fail_wait
	csrw sepc, t5
	sret
.size _start, .-_start

.section .rodata.expected,"a",@progbits
.balign 8
expected_causes:
	.dword 5, 7, 1, 5, 5, 1, 13
expected_tvals:
	.dword FIRMWARE_BASE, FIRMWARE_BASE, FIRMWARE_BASE
	.dword VA_FIRMWARE, VA_FIRMWARE_ALIAS, VA_FIRMWARE, VA_PAGE_FAULT

.section .rodata.message,"a",@progbits
.balign 4
guest_message:
	.asciz "Hello from PMP on guideXOS"

.section .data.fixture,"aw",@progbits
.balign 8
initialized_word:
	.dword 0x1122334455667788
work_slot:
	.dword 0

.option pop
