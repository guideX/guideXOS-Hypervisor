.option push
.option norvc
.option norelax

.equ RESULT_BASE,       0x83e00000
.equ SIGNATURE_BASE,    0x83e00100
.equ GUEST_STACK_TOP,   0x83fffff0
.equ EXPECTED_FDT,      0x82200000
.equ COMPLETE_WORD,     0x434f4d504c455445
.equ SIGNATURE_MAGIC,   0x47584f5352495356
.equ SBI_BASE_EID,      0x10
.equ SBI_BASE_GET_SPEC, 0
.equ SBI_BASE_PROBE,    3
.equ SBI_TIME_EID,      0x54494d45
.equ TIMER_DELTA,       10000
.equ STIE_BIT,          0x20
.equ SIE_BIT,           0x2
.equ EXPECTED_STIP,     0x8000000000000005

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	# Authentic FW_JUMP enters S-mode with a0=hartid and a1=FDT.
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP
	li t0, SIGNATURE_BASE
	sd zero, 112(t0)
	bnez s0, fail_hart
	li t0, EXPECTED_FDT
	bne s1, t0, fail_fdt

	# Build an identity-mapped Sv39 1 GiB leaf for RAM and enable paging.
	lla t0, page_table_root
	li t1, 0x200000cf            # PA 0x80000000, V/R/W/X/A/D, supervisor-only
	sd t1, 16(t0)                # root VPN[2] = 2
	li t1, 0x80210000
	srli t1, t1, 12
	li t2, 8
	slli t2, t2, 60
	or t1, t1, t2
	csrw satp, t1
	sfence.vma zero, zero

	# Query SBI Base and ask genuine OpenSBI whether TIME is implemented.
	li a0, 0
	li a1, 0
	li a6, SBI_BASE_GET_SPEC
	li a7, SBI_BASE_EID
	ecall
	bnez a0, fail_base
	mv s2, a1
	li t0, 0x03000000
	bne s2, t0, fail_base
	li a0, SBI_TIME_EID
	li a6, SBI_BASE_PROBE
	li a7, SBI_BASE_EID
	ecall
	bnez a0, fail_time_probe
	li t0, 1
	bne a1, t0, fail_time_probe
	mv s3, a1

	# Install a real S-mode timer handler and enable delegated STIP delivery.
	lla t0, supervisor_timer_entry
	csrw stvec, t0
	li t0, STIE_BIT
	csrs sie, t0

	# The unsupported TIME FID must return NOT_SUPPORTED and do no work.
	li a0, 0x1234
	li a6, 1
	li a7, SBI_TIME_EID
	ecall
	li t0, -2
	bne a0, t0, fail_time_probe

	# An already-expired deadline remains eligible for immediate MTIP service.
	# SIE is clear, so the resulting STIP stays masked until a future rearm.
	rdtime t0
	addi a0, t0, -1
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_set_timer

	# Arm a future deadline, then reprogram it before it expires. SBI TIME must
	# clear pending STIP while both SIE and STIE are masked.
	li t0, STIE_BIT
	csrc sie, t0
	rdtime t0
	li t1, TIMER_DELTA
	add t0, t0, t1
	li t2, SIGNATURE_BASE
	mv a0, t0
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_set_timer
	rdtime t0
	li t1, TIMER_DELTA
	add t0, t0, t1
	li t2, SIGNATURE_BASE
	sd t0, 56(t2)
	mv a0, t0
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_set_timer
	csrr t0, sip
	andi t0, t0, STIE_BIT
	bnez t0, fail_clear_stip
	li t0, STIE_BIT
	csrs sie, t0
	li t0, SIE_BIT
	csrs sstatus, t0

wait_for_timer:
	wfi
	li t0, SIGNATURE_BASE
	ld t1, 48(t0)
	li t2, 3
	bltu t1, t2, wait_for_timer
	j write_completion

/*
 * OpenSBI raises STIP by a machine-mode CSR operation. This handler records
 * architectural state, counts only real S-mode trap entries, and independently
 * schedules the next absolute deadline through another SBI TIME ECALL.
 */
supervisor_timer_entry:
	addi sp, sp, -64
	sd t0, 0(sp)
	sd t1, 8(sp)
	sd t2, 16(sp)
	sd t3, 24(sp)
	sd a0, 32(sp)
	sd a1, 40(sp)
	sd a6, 48(sp)
	sd a7, 56(sp)

	csrr t0, scause
	li t1, EXPECTED_STIP
	bne t0, t1, fail_interrupt
	li t1, SIGNATURE_BASE
	sd t0, 80(t1)
	csrr t2, sepc
	sd t2, 88(t1)
	csrr t3, sstatus
	sd t3, 96(t1)
	ld t0, 48(t1)
	addi t0, t0, 1
	sd t0, 48(t1)
	li t2, STIE_BIT
	csrc sie, t2
	li t2, 3
	bgeu t0, t2, disarm_timer

	# Store the next deadline at +64 for event two and +72 for event three.
	rdtime t2
	li t3, TIMER_DELTA
	add t2, t2, t3
	slli t3, t0, 3
	addi t3, t3, 56
	add t3, t1, t3
	sd t2, 0(t3)
	mv a0, t2
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_set_timer
	j timer_rearm_complete

disarm_timer:
	# SBI defines UINT64_MAX as a practical timer disable sentinel.
	li a0, -1
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_set_timer

timer_rearm_complete:
	csrr t0, sip
	andi t0, t0, STIE_BIT
	bnez t0, fail_clear_stip
	li t0, STIE_BIT
	csrs sie, t0

timer_return:
	ld t0, 0(sp)
	ld t1, 8(sp)
	ld t2, 16(sp)
	ld t3, 24(sp)
	ld a0, 32(sp)
	ld a1, 40(sp)
	ld a6, 48(sp)
	ld a7, 56(sp)
	addi sp, sp, 64
	sret

write_completion:
	# The message and full versioned evidence are written by the guest.
	lla t0, timer_message
	li t1, RESULT_BASE
1:
	lbu t2, 0(t0)
	sb t2, 0(t1)
	beqz t2, 2f
	addi t0, t0, 1
	addi t1, t1, 1
	j 1b
2:
	li t0, SIGNATURE_BASE
	li t1, SIGNATURE_MAGIC
	sd t1, 0(t0)
	li t1, 10
	sd t1, 8(t0)
	sd s0, 16(t0)
	sd s1, 24(t0)
	sd s2, 32(t0)
	sd s3, 40(t0)
	li t1, 3
	sd t1, 48(t0)
	rdtime t1
	sd t1, 104(t0)
	fence rw, w
	li t1, COMPLETE_WORD
	sd t1, 112(t0)
done:
	wfi
	j done

fail_hart:
	li t1, 1
	j fail
fail_fdt:
	li t1, 2
	j fail
fail_base:
	li t1, 3
	j fail
fail_time_probe:
	li t1, 4
	j fail
fail_set_timer:
	li t1, 5
	j fail
fail_interrupt:
	li t1, 6
	j fail
fail_clear_stip:
	li t1, 7
fail:
	li t0, SIGNATURE_BASE
	sd t1, 120(t0)
fail_wait:
	wfi
	j fail_wait
.size _start, .-_start

.section .rodata.message,"a",@progbits
.balign 4
timer_message:
	.asciz "Hello from OpenSBI TIMER on guideXOS"

.section .bss.page_tables,"aw",@nobits
.balign 4096
page_table_root:
	.zero 4096

.option pop
