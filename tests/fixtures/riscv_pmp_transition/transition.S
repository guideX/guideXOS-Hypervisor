.option push
.option norvc
.option norelax

.equ DATA_WORD,       0x80010000
.equ RESULT_BASE,     0x80002000
.equ RESULT_MAGIC,    0x4758504d50545231
.equ EXPECTED_WORD,   0xfeedcafe11223344
.equ TRANSITION_EID,  0x16

.section .text.boot,"ax",@progbits
.globl _start
.type _start,@function
_start:
	# Entry 0: S-mode RX for the 8 KiB machine and supervisor code window.
	li t0, ((0x80000000 >> 2) | 0x3ff)
	csrw pmpaddr0, t0
	# Entry 1: initially deny the exact 8-byte data word.
	li t0, (DATA_WORD >> 2)
	csrw pmpaddr1, t0
	# Entry 2: guest-writable result page.
	li t0, ((RESULT_BASE >> 2) | 0x1ff)
	csrw pmpaddr2, t0
	li t0, 0x001b181d
	csrw pmpcfg0, t0
	sfence.vma zero, zero

	la t0, machine_trap
	csrw mtvec, t0
	li t0, (1 << 5)
	csrw medeleg, t0
	la t0, supervisor_start
	csrw mepc, t0
	csrr t0, mstatus
	li t1, 0xffffffffffffe7ff
	and t0, t0, t1
	li t1, 0x800
	or t0, t0, t1
	csrw mstatus, t0
	mret

machine_trap:
	csrr t0, mcause
	li t1, 9
	bne t0, t1, machine_fail
	li t1, TRANSITION_EID
	bne a7, t1, machine_fail
	li t1, 1
	beq a0, t1, machine_grant
	li t1, 2
	beq a0, t1, machine_revoke
	j machine_fail

machine_grant:
	li t0, 0x001b191d
	csrw pmpcfg0, t0
	sfence.vma zero, zero
	li t0, RESULT_BASE
	li t1, 1
	sd t1, 48(t0)
	li a0, 0
	j machine_return

machine_revoke:
	li t0, 0x001b181d
	csrw pmpcfg0, t0
	sfence.vma zero, zero
	li t0, RESULT_BASE
	li t1, 2
	sd t1, 48(t0)
	li a0, 0
	j machine_return

machine_fail:
	li t0, RESULT_BASE
	li t1, 1
	sd t1, 16(t0)
	li a0, -1
machine_return:
	csrr t0, mepc
	addi t0, t0, 4
	csrw mepc, t0
	mret

supervisor_start:
	la t0, supervisor_trap
	csrw stvec, t0

supervisor_initial_load:
	li t0, DATA_WORD
	ld t1, 0(t0)
	j supervisor_fail

supervisor_grant_request:
	li a7, TRANSITION_EID
	li a0, 1
	ecall
	bnez a0, supervisor_fail

supervisor_retry_after_grant:
	li t0, DATA_WORD
	ld t1, 0(t0)
	li t2, EXPECTED_WORD
	bne t1, t2, supervisor_fail
	li t0, RESULT_BASE
	sd t1, 32(t0)

	li a7, TRANSITION_EID
	li a0, 2
	ecall
	bnez a0, supervisor_fail

supervisor_retry_after_revoke:
	li t0, DATA_WORD
	ld t1, 0(t0)
	j supervisor_fail

supervisor_trap:
	csrr t0, scause
	li t1, 5
	bne t0, t1, supervisor_fail
	csrr t1, stval
	li t2, DATA_WORD
	bne t1, t2, supervisor_fail
	li t2, RESULT_BASE
	ld t3, 24(t2)
	li t4, 2
	bgeu t3, t4, supervisor_fail
	slli t4, t3, 4
	slli t5, t3, 3
	add t4, t4, t5
	addi t4, t4, 64
	add t4, t4, t2
	sd t0, 0(t4)
	sd t1, 8(t4)
	csrr t5, sepc
	sd t5, 16(t4)
	addi t3, t3, 1
	sd t3, 24(t2)
	li t4, 1
	beq t3, t4, first_access_fault
	la t5, supervisor_done
	csrw sepc, t5
	sret
first_access_fault:
	la t5, supervisor_grant_request
	csrw sepc, t5
	sret

supervisor_done:
	li t0, RESULT_BASE
	ld t1, 24(t0)
	li t2, 2
	bne t1, t2, supervisor_fail
	li t1, RESULT_MAGIC
	sd t1, 0(t0)
	li t1, 1
	sd t1, 8(t0)
	li t1, 0
	sd t1, 16(t0)
	fence rw, w
	li t1, 0x434f4d504c455445
	sd t1, 120(t0)
supervisor_wait:
	wfi
	j supervisor_wait

supervisor_fail:
	li t0, RESULT_BASE
	li t1, 1
	sd t1, 16(t0)
supervisor_fail_wait:
	wfi
	j supervisor_fail_wait

.size _start, .-_start
.option pop
