.option push
.option norvc
.option norelax

.equ RESULT_BASE,        0x83e00000
.equ SIGNATURE_BASE,     0x83e00100
.equ FAULT_COUNT,        (RESULT_BASE + 0x200)
.equ FAULT_LOG,          (RESULT_BASE + 0x208)
.equ DOMAIN_STATE,       (RESULT_BASE + 0x400)
.equ TIMER_MILESTONE,    DOMAIN_STATE
.equ TIMER_DEADLINE_LOG, (DOMAIN_STATE + 8)
.equ UART0_RX_LOG,       (DOMAIN_STATE + 16)
.equ UART1_RX_LOG,       (DOMAIN_STATE + 24)
.equ EXTERNAL_COUNT,     (DOMAIN_STATE + 32)
.equ TIMER_COUNT,        (DOMAIN_STATE + 40)
.equ GUEST_STACK_TOP,    0x83fffff0
.equ EXPECTED_FDT,       0x82200000
.equ COMPLETE_WORD,      0x434f4d504c455445
.equ SIGNATURE_MAGIC,    0x47584f5352495356
.equ FIRMWARE_BASE,      0x80000000
.equ UART0_BASE,         0x10000000
.equ UART1_BASE,         0x1000a000
.equ PLIC_BASE,          0x0c000000
.equ PLIC_PRIORITY_12,   (PLIC_BASE + 48)
.equ PLIC_ENABLE_S,      (PLIC_BASE + 0x2080)
.equ PLIC_THRESHOLD_S,   (PLIC_BASE + 0x201000)
.equ PLIC_CLAIM_S,       (PLIC_BASE + 0x201004)
.equ MTIMECMP_BASE,      0x02004000
.equ MTIME_BASE,         0x0200bff8
.equ VA_FIRMWARE,        0x40000000
.equ VA_FIRMWARE_ALIAS,  0x40001000
.equ VA_AD_TEST,         0x40003000
.equ VA_MTIMECMP,        0x40004000
.equ VA_EXEC_ONLY,       0x40005000
.equ VA_NONIDENTITY,     0x40006000
.equ NONIDENTITY_PA,     0x83e01000
.equ NONIDENTITY_VALUE,  0xa55a1234deadbeef
.equ IDENTITY_ADDRESS,   0x83e02000
.equ IDENTITY_VALUE,     0x1d3d5a7b9cbed0f2
.equ SBI_BASE_EID,       0x10
.equ SBI_TIME_EID,       0x54494d45
.equ TIMER_DELTA,        30000
.equ EXPECTED_FAULTS,    16
.equ EXPECTED_SEIP,      0x8000000000000009
.equ EXPECTED_STIP,      0x8000000000000005
.equ SEIE_BIT,           0x200
.equ STIE_BIT,           0x20
.equ SIE_BIT,            0x2

.section .text.entry,"ax",@progbits
.globl _start
.type _start,@function
_start:
	mv s0, a0
	mv s1, a1
	li sp, GUEST_STACK_TOP
	li s2, SIGNATURE_BASE
	bnez s0, fail_hart
	li t0, EXPECTED_FDT
	bne s1, t0, fail_fdt
	li t0, SIGNATURE_MAGIC
	sd t0, 0(s2)
	li t0, 3
	sd t0, 8(s2)

	la t0, supervisor_trap
	csrw stvec, t0
	li t0, FAULT_COUNT
	sd zero, 0(t0)
	li t0, DOMAIN_STATE
	sd zero, 0(t0)
	sd zero, 8(t0)
	sd zero, 16(t0)
	sd zero, 24(t0)
	sd zero, 32(t0)
	sd zero, 40(t0)

	# Bare S-mode firmware checks: load, store, and instruction fetch.
	la t0, resume_fw_load
	csrw sscratch, t0
	li t1, FIRMWARE_BASE
	ld t2, 0(t1)
	j fail_access
resume_fw_load:
	la t0, resume_fw_store
	csrw sscratch, t0
	li t1, FIRMWARE_BASE
	li t2, 0x12345678
	sd t2, 0(t1)
	j fail_access
resume_fw_store:
	la t0, resume_fw_fetch
	csrw sscratch, t0
	li t0, FIRMWARE_BASE
	jalr t0
	j fail_access
resume_fw_fetch:

	# Establish an authentic OpenSBI TIME deadline, then try the denied comparator.
	li a0, 0
	li a1, 0
	li a6, 0
	li a7, SBI_BASE_EID
	ecall
	bnez a0, fail_sbi
	li t0, 0x03000000
	bne a1, t0, fail_sbi
	rdtime t0
	li t1, TIMER_DELTA
	add s4, t0, t1
	mv a0, s4
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, fail_sbi
	li t0, TIMER_DEADLINE_LOG
	sd s4, 0(t0)

	la t0, resume_mtimecmp_store
	csrw sscratch, t0
	li t1, MTIMECMP_BASE
	li t2, 0
	sd t2, 0(t1)
	j fail_access
resume_mtimecmp_store:
	la t0, resume_mtime_load
	csrw sscratch, t0
	li t1, MTIME_BASE
	ld t2, 0(t1)
	j fail_access
resume_mtime_load:

	# Unmatched device, out-of-RAM, unassigned, and out-of-window UART accesses.
	la t0, resume_device_load
	csrw sscratch, t0
	li t1, 0x1000b100
	lw t2, 0(t1)
	j fail_access
resume_device_load:
	la t0, resume_ram_end_load
	csrw sscratch, t0
	li t1, 0x84000000
	ld t2, 0(t1)
	j fail_access
resume_ram_end_load:
	la t0, resume_unassigned_load
	csrw sscratch, t0
	li t1, 0x40000000
	ld t2, 0(t1)
	j fail_access
resume_unassigned_load:
	la t0, resume_uart_window_store
	csrw sscratch, t0
	li t1, 0x1000a100
	sb zero, 0(t1)
	j fail_access
resume_uart_window_store:

	# The bad root remains readable and maps this handler, but points VPN[2]=1
	# at protected firmware. The walker must raise a PMP load access fault.
	li t0, 8
	slli t0, t0, 60
	li t1, (0x83c03000 >> 12)
	or t0, t0, t1
	csrw satp, t0
	sfence.vma zero, zero
	la t0, resume_page_table_load
	csrw sscratch, t0
	li t1, VA_FIRMWARE
	ld t2, 0(t1)
	j fail_access
resume_page_table_load:

	# Switch to preloaded guest page tables. They are S-readable and S-read-only.
	li t0, 8
	slli t0, t0, 60
	li t1, (0x83c00000 >> 12)
	or t0, t0, t1
	csrw satp, t0
	sfence.vma zero, zero

	# The A-bit update for this valid leaf reaches the read-only page-table page.
	la t0, resume_ad_update
	csrw sscratch, t0
	li t1, VA_AD_TEST
	ld t2, 0(t1)
	j fail_access
resume_ad_update:

	# Final translated accesses still obey PMP and preserve virtual stval.
	la t0, resume_sv39_firmware_load
	csrw sscratch, t0
	li t1, VA_FIRMWARE
	ld t2, 0(t1)
	j fail_access
resume_sv39_firmware_load:
	la t0, resume_sv39_alias_load
	csrw sscratch, t0
	li t1, VA_FIRMWARE_ALIAS
	ld t2, 0(t1)
	j fail_access
resume_sv39_alias_load:
	la t0, resume_sv39_firmware_fetch
	csrw sscratch, t0
	li t0, VA_FIRMWARE
	jalr t0
	j fail_access
resume_sv39_firmware_fetch:
	la t0, resume_page_fault
	csrw sscratch, t0
	li t1, VA_EXEC_ONLY
	ld t2, 0(t1)
	j fail_access
resume_page_fault:
	la t0, resume_sv39_mtimecmp_store
	csrw sscratch, t0
	li t1, VA_MTIMECMP
	sd zero, 0(t1)
	j fail_access
resume_sv39_mtimecmp_store:

	# The 1 GiB identity leaf grants this permitted physical RAM address too.
	li t1, IDENTITY_ADDRESS
	li t2, IDENTITY_VALUE
	sd t2, 0(t1)
	ld t3, 0(t1)
	bne t2, t3, fail_access

	# This permitted RAM window also works through a nonidentity Sv39 leaf.
	li t1, VA_NONIDENTITY
	li t2, NONIDENTITY_VALUE
	sd t2, 0(t1)
	ld t3, 0(t1)
	bne t2, t3, fail_access
	li t0, SIGNATURE_MAGIC
	li t1, TIMER_MILESTONE
	sd t0, 0(t1)

	# Authorized UART0 receive and transmit operations use guest MMIO instructions.
	li t0, UART0_BASE
	lbu t1, 0(t0)
	li t2, UART0_RX_LOG
	sd t1, 0(t2)
	la t1, domain_message
uart0_transmit:
	lbu t2, 0(t1)
	beqz t2, uart0_transmit_done
	sb t2, 0(t0)
	addi t1, t1, 1
	j uart0_transmit
uart0_transmit_done:

	# Configure the S-context PLIC, then enable the UART1 receive interrupt.
	li t0, PLIC_PRIORITY_12
	li t1, 3
	sw t1, 0(t0)
	li t0, PLIC_ENABLE_S
	li t1, (1 << 12)
	sw t1, 0(t0)
	li t0, PLIC_THRESHOLD_S
	sw zero, 0(t0)
	li t0, UART1_BASE
	li t1, 1
	sb t1, 1(t0)
	li t0, (SEIE_BIT | STIE_BIT)
	csrs sie, t0
	li t0, SIE_BIT
	csrs sstatus, t0

wait_for_interrupts:
	li t0, EXTERNAL_COUNT
	ld t0, 0(t0)
	li t1, 1
	bltu t0, t1, wait_for_interrupts
	li t0, TIMER_COUNT
	ld t0, 0(t0)
	bltu t0, t1, wait_for_interrupts

	# Completion record is guest-authored; the marker is written last.
	sd s0, 16(s2)
	sd s1, 24(s2)
	li t0, 3
	sd t0, 8(s2)
	sd zero, 16(s2)
	li t0, EXPECTED_FAULTS
	sd t0, 24(s2)
	sd s4, 32(s2)
	li t0, UART0_RX_LOG
	ld t0, 0(t0)
	sd t0, 40(s2)
	li t0, UART1_RX_LOG
	ld t0, 0(t0)
	sd t0, 48(s2)
	li t0, EXTERNAL_COUNT
	ld t0, 0(t0)
	sd t0, 56(s2)
	li t0, TIMER_COUNT
	ld t0, 0(t0)
	sd t0, 64(s2)
	li t0, 15
	sd t0, 72(s2)               # 11 explicit regions plus four inherited machine-only windows.
	csrr t0, satp
	sd t0, 80(s2)
	csrr t0, sip
	sd t0, 88(s2)
	li t0, VA_NONIDENTITY
	sd t0, 96(s2)
	li t0, NONIDENTITY_VALUE
	sd t0, 104(s2)
	fence rw, w
	li t0, COMPLETE_WORD
	sd t0, 112(s2)
guest_done:
	wfi
	j guest_done

supervisor_trap:
	addi sp, sp, -112
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
	bltz t0, supervisor_interrupt

	# Synchronous fault: compare architectural cause/stval, retain sepc, and
	# resume only at the guest-selected recovery label in sscratch.
	li t3, FAULT_COUNT
	ld t4, 0(t3)
	li t5, EXPECTED_FAULTS
	bgeu t4, t5, trap_unexpected
	slli t6, t4, 3
	la t5, expected_causes
	add t5, t5, t6
	ld t5, 0(t5)
	bne t0, t5, trap_unexpected
	csrr t1, stval
	la t5, expected_tvals
	add t5, t5, t6
	ld t5, 0(t5)
	bne t1, t5, trap_unexpected
	csrr t2, sepc
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
	j trap_return

supervisor_interrupt:
	li t1, EXPECTED_SEIP
	beq t0, t1, service_external
	li t1, EXPECTED_STIP
	beq t0, t1, service_timer
	j trap_unexpected

service_external:
	li t0, PLIC_CLAIM_S
	lw t1, 0(t0)
	li t2, 12
	bne t1, t2, trap_unexpected
	mv t3, t1
	li t0, UART1_BASE
	lbu t1, 2(t0)
	li t2, 4
	bne t1, t2, trap_unexpected
	lbu t1, 0(t0)
	li t2, 0x5a
	bne t1, t2, trap_unexpected
	li t0, UART1_RX_LOG
	sd t1, 0(t0)
	li t0, EXTERNAL_COUNT
	ld t1, 0(t0)
	addi t1, t1, 1
	sd t1, 0(t0)
	li t0, PLIC_CLAIM_S
	sw t3, 0(t0)
	j trap_return

service_timer:
	li t0, TIMER_COUNT
	ld t1, 0(t0)
	addi t1, t1, 1
	sd t1, 0(t0)
	li a0, -1
	li a6, 0
	li a7, SBI_TIME_EID
	ecall
	bnez a0, trap_unexpected
	j trap_return

trap_unexpected:
	li t0, SIGNATURE_BASE
	li t1, 1
	sd t1, 136(t0)
	la t0, fail_wait
	csrw sepc, t0

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
	addi sp, sp, 112
	sret

fail_hart:   li t0, 1; j guest_fail
fail_fdt:    li t0, 2; j guest_fail
fail_sbi:    li t0, 3; j guest_fail
fail_access: li t0, 4; j guest_fail
guest_fail:
	sd t0, 136(s2)
fail_wait:
	wfi
	j fail_wait
.size _start, .-_start

.section .rodata.expected,"a",@progbits
.balign 8
expected_causes:
	.dword 5, 7, 1, 7, 5, 5, 5, 5, 7, 5, 5, 5, 5, 1, 13, 7
expected_tvals:
	.dword 0x80000000, 0x80000000, 0x80000000, 0x02004000
	.dword 0x0200bff8, 0x1000b100, 0x84000000, 0x40000000
	.dword 0x1000a100, 0x40000000, 0x40003000, 0x40000000
	.dword 0x40001000, 0x40000000, 0x40005000, 0x40004000

.section .rodata.message,"a",@progbits
.balign 4
domain_message:
	.asciz "RISCV-16 restricted domain: UART and PLIC authorized"

.section .page_tables,"a",@progbits
.balign 4096
.globl page_table_root
page_table_root:
	.dword 0x000000cf
	.dword ((0x83c01000 >> 2) | 1)
	.dword 0x200000cf
	.zero (4096 - 24)

.balign 4096
page_table_alias_l1:
	.dword ((0x83c02000 >> 2) | 1)
	.zero (4096 - 8)

.balign 4096
page_table_alias_l0:
	.dword ((0x80000000 >> 2) | 0xcf)
	.dword ((0x80000000 >> 2) | 0xcf)
	.dword ((0x80000000 >> 2) | 0xcf)
	.dword ((0x80200000 >> 2) | 0x0f) /* A=0: protected A-bit update must fail. */
	.dword ((0x02004000 >> 2) | 0xcf)
	.dword ((0x80200000 >> 2) | 0x49) /* Execute-only leaf: load page-faults. */
	.dword ((NONIDENTITY_PA >> 2) | 0xcf)
	.zero (4096 - 56)

.balign 4096
page_table_bad_root:
	.dword 0x000000cf
	.dword ((0x80000000 >> 2) | 1) /* Next-level table is in denied firmware. */
	.dword 0x200000cf
	.zero (4096 - 24)

.option pop
