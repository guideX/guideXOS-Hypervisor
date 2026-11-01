.option push
.option norvc
.option norelax

.equ RESULT_BASE,       0x83e00000
.equ SIGNATURE_BASE,    0x83e00100
.equ GUEST_STACK_TOP,   0x83fffff0
.equ EXPECTED_FDT,      0x82200000
.equ COMPLETE_WORD,     0x434f4d504c455445
.equ SIGNATURE_MAGIC,   0x47584f5352495356

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	# OpenSBI FW_JUMP enters here in S-mode with a0=hartid and a1=FDT.
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP

	# A legal SSTATUS read and the successful SBI call below prove S-mode.
	csrr t0, sstatus
	li s10, 1

	# Validate the single-hart contract and the aligned FDT handoff pointer.
	bnez s0, fail_hart
	andi t0, s1, 7
	bnez t0, fail_fdt
	li t0, EXPECTED_FDT
	bne s1, t0, fail_fdt

	# Validate initialized data and the loader-zeroed BSS region.
	lla t0, initialized_word
	ld s6, 0(t0)
	li t1, 0x1122334455667788
	bne s6, t1, fail_data
	lla t0, zero_probe
	ld t1, 0(t0)
	bnez t1, fail_bss
	li s7, 1

	# Run guest arithmetic and a memory round trip.
	li t0, 6
	li t1, 7
	mul s4, t0, t1
	li t2, 42
	bne s4, t2, fail_arithmetic
	lla t0, work_slot
	sd s4, 0(t0)
	ld s5, 0(t0)
	bne s5, s4, fail_arithmetic

	# Ask the running OpenSBI firmware for its SBI specification version.
	li a0, 0
	li a1, 0
	li a6, 0
	li a7, 0x10
	ecall
	mv s2, a0
	mv s3, a1
	bnez s2, fail_sbi
	li t0, 0x03000000
	bne s3, t0, fail_sbi

	# Copy the guest-authored completion message into guest RAM.
	lla t0, guest_message
	li t1, RESULT_BASE
	li s9, 0
1:
	lbu t2, 0(t0)
	sb t2, 0(t1)
	beqz t2, 2f
	addi s9, s9, 1
	addi t0, t0, 1
	addi t1, t1, 1
	j 1b
2:

	# Write the versioned result record. The completion word is last.
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 1
	sd t1, 8(t0)
	sd zero, 16(t0)
	sd s0, 24(t0)
	sd s1, 32(t0)
	sd s2, 40(t0)
	sd s3, 48(t0)
	sd s4, 56(t0)
	sd s5, 64(t0)
	sd s6, 72(t0)
	sd s7, 80(t0)
	sd s10, 88(t0)
	sd s9, 96(t0)
	fence rw, w
	li t1, COMPLETE_WORD
	sd t1, 112(t0)
3:
	wfi
	j 3b

fail_hart:
	li s8, 1
	j failure
fail_fdt:
	li s8, 2
	j failure
fail_data:
	li s8, 3
	j failure
fail_bss:
	li s8, 4
	j failure
fail_arithmetic:
	li s8, 5
	j failure
fail_sbi:
	li s8, 6
failure:
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 1
	sd t1, 8(t0)
	sd s8, 16(t0)
	sd s0, 24(t0)
	sd s1, 32(t0)
	sd s2, 40(t0)
	sd s3, 48(t0)
	sd s4, 56(t0)
	sd s5, 64(t0)
	sd s6, 72(t0)
	sd s7, 80(t0)
	sd s10, 88(t0)
4:
	wfi
	j 4b
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
guest_message:
	.asciz "Hello from OpenSBI on guideXOS"

.section .data.fixture,"aw",@progbits
.balign 8
initialized_word:
	.dword 0x1122334455667788
work_slot:
	.dword 0

.section .bss.fixture,"aw",@nobits
.balign 8
zero_probe:
	.zero 8

.option pop
