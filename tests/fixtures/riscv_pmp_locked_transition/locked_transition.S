.option push
.option norvc
.option norelax

.equ GUEST_BASE,          0x80000000
.equ RESULT_BASE,         0x80002000
.equ NAPOT_TARGET,        0x80010000
.equ TOR_TARGET,          0x80011000
.equ TARGET_VALUE,        0x6c6f636b6564706d
.equ RESULT_PHASE,         8
.equ RESULT_FAULT_COUNT,  24
.equ RESULT_FAULT_LOG,    32
.equ RESULT_NAPOT_ADDR,  128
.equ RESULT_PMCFG_READ,  136
.equ RESULT_TOR_LOW_READ,144
.equ RESULT_TOR_HIGH_READ,152
.equ RESULT_RECOVERY_ALT,160
.equ RESULT_RECOVERY_ADDR,168
.equ RESULT_RECOVERY_CFG,176
.equ RESULT_RECOVERY_VALUE,184
.equ RESULT_LOCKED_COMPLETE,0x4758504d4c4f434b
.equ RESULT_RECOVERY_COMPLETE,0x4758504d52535431

.section .text.boot,"ax",@progbits
.globl _start
.type _start,@function
_start:
	li t0, RESULT_BASE
	ld t1, RESULT_PHASE(t0)
	li t2, 1
	beq t1, t2, recovery_start

	# Five hardware entries cover executable code, a locked NAPOT target,
	# writable guest results, and a locked TOR target with a protected predecessor.
	li t0, ((GUEST_BASE >> 2) | 0x3ff)
	csrw pmpaddr0, t0
	li t0, (NAPOT_TARGET >> 2)
	csrw pmpaddr1, t0
	li t0, ((RESULT_BASE >> 2) | 0x1ff)
	csrw pmpaddr2, t0
	li t0, (TOR_TARGET >> 2)
	csrw pmpaddr3, t0
	li t0, ((TOR_TARGET + 8) >> 2)
	csrw pmpaddr4, t0
	# On RV64, pmpcfg0 covers entries 0-7 in a 64-bit CSR. Entry 4 is
	# the locked TOR region, so its config byte belongs at bit 32.
	li t0, 0x88001b981d
	csrw pmpcfg0, t0
	sfence.vma zero, zero

	# M-mode CSR writes attempt to clear both locks and move both ranges.
	li t0, ((NAPOT_TARGET + 0x100) >> 2)
	csrw pmpaddr1, t0
	li t0, 0x001b001d
	csrw pmpcfg0, t0
	li t0, ((TOR_TARGET + 0x100) >> 2)
	csrw pmpaddr3, t0
	li t0, ((TOR_TARGET + 0x108) >> 2)
	csrw pmpaddr4, t0
	sfence.vma zero, zero

	li t0, RESULT_BASE
	csrr t1, pmpaddr1
	sd t1, RESULT_NAPOT_ADDR(t0)
	csrr t1, pmpcfg0
	sd t1, RESULT_PMCFG_READ(t0)
	csrr t1, pmpaddr3
	sd t1, RESULT_TOR_LOW_READ(t0)
	csrr t1, pmpaddr4
	sd t1, RESULT_TOR_HIGH_READ(t0)

	li t0, (1 << 5)
	csrw medeleg, t0
	la t0, supervisor_trap
	csrw stvec, t0
	la t0, supervisor_locked_start
	csrw mepc, t0
	csrr t0, mstatus
	li t1, 0xffffffffffffe7ff
	and t0, t0, t1
	li t1, 0x800
	or t0, t0, t1
	csrw mstatus, t0
	mret

supervisor_locked_start:
	li t0, NAPOT_TARGET
	ld t1, 0(t0)
	j supervisor_fail

supervisor_tor_load:
	li t0, TOR_TARGET
	ld t1, 0(t0)
	j supervisor_fail

supervisor_trap:
	csrr t0, scause
	li t1, 5
	bne t0, t1, supervisor_fail
	csrr t1, stval
	li t2, RESULT_BASE
	ld t3, RESULT_FAULT_COUNT(t2)
	li t4, 2
	bgeu t3, t4, supervisor_fail
	slli t4, t3, 4
	slli t5, t3, 3
	add t4, t4, t5
	addi t4, t4, RESULT_FAULT_LOG
	add t4, t4, t2
	sd t0, 0(t4)
	sd t1, 8(t4)
	csrr t5, sepc
	sd t5, 16(t4)
	addi t3, t3, 1
	sd t3, RESULT_FAULT_COUNT(t2)
	li t4, 1
	beq t3, t4, supervisor_first_fault
	la t5, supervisor_locked_done
	csrw sepc, t5
	sret

supervisor_first_fault:
	la t5, supervisor_tor_load
	csrw sepc, t5
	sret

supervisor_locked_done:
	li t0, RESULT_BASE
	ld t1, RESULT_FAULT_COUNT(t0)
	li t2, 2
	bne t1, t2, supervisor_fail
	li t1, 1
	sd t1, RESULT_PHASE(t0)
	li t1, 1
	sd t1, 16(t0)
	li t1, 0
	sw t1, 20(t0)
	fence rw, w
	li t1, RESULT_LOCKED_COMPLETE
	sd t1, 0(t0)
	sd t1, 120(t0)
supervisor_locked_wait:
	wfi
	j supervisor_locked_wait

supervisor_fail:
	li t0, RESULT_BASE
	li t1, 1
	sw t1, 20(t0)
supervisor_fail_wait:
	wfi
	j supervisor_fail_wait

recovery_start:
	li t0, ((GUEST_BASE >> 2) | 0x3ff)
	csrw pmpaddr0, t0
	li t0, (NAPOT_TARGET >> 2)
	csrw pmpaddr1, t0
	li t0, ((RESULT_BASE >> 2) | 0x1ff)
	csrw pmpaddr2, t0
	li t0, (TOR_TARGET >> 2)
	csrw pmpaddr3, t0
	li t0, ((TOR_TARGET + 8) >> 2)
	csrw pmpaddr4, t0
	li t0, 0x081b181d
	csrw pmpcfg0, t0
	sfence.vma zero, zero

	# The reset-cleared entry accepts actual M-mode address and config updates.
	li t0, ((NAPOT_TARGET + 0x100) >> 2)
	csrw pmpaddr1, t0
	csrr t1, pmpaddr1
	li t2, RESULT_BASE
	sd t1, RESULT_RECOVERY_ALT(t2)
	li t0, (NAPOT_TARGET >> 2)
	csrw pmpaddr1, t0
	li t0, 0x081b191d
	csrw pmpcfg0, t0
	sfence.vma zero, zero
	csrr t1, pmpaddr1
	sd t1, RESULT_RECOVERY_ADDR(t2)
	csrr t1, pmpcfg0
	sd t1, RESULT_RECOVERY_CFG(t2)

	li t0, (1 << 5)
	csrw medeleg, t0
	la t0, recovery_trap
	csrw stvec, t0
	la t0, supervisor_recovery_start
	csrw mepc, t0
	csrr t0, mstatus
	li t1, 0xffffffffffffe7ff
	and t0, t0, t1
	li t1, 0x800
	or t0, t0, t1
	csrw mstatus, t0
	mret

recovery_trap:
	li t0, RESULT_BASE
	li t1, 2
	sd t1, 20(t0)
recovery_fail_wait:
	wfi
	j recovery_fail_wait

supervisor_recovery_start:
	li t0, NAPOT_TARGET
	ld t1, 0(t0)
	li t2, TARGET_VALUE
	bne t1, t2, recovery_fail
	li t0, RESULT_BASE
	sd t1, RESULT_RECOVERY_VALUE(t0)
	li t1, RESULT_RECOVERY_COMPLETE
	fence rw, w
	sd t1, 192(t0)
supervisor_recovery_wait:
	wfi
	j supervisor_recovery_wait

recovery_fail:
	li t0, RESULT_BASE
	li t1, 3
	sd t1, 20(t0)
recovery_guest_wait:
	wfi
	j recovery_guest_wait

.size _start, .-_start
.option pop
