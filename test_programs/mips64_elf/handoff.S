.set noreorder
.set noat
.text
.section .text.start,"ax",@progbits
.globl _start
.ent _start
_start:
    # argc = 2, and prove the branch delay slot retired on the not-taken path.
    daddiu  $t0, $zero, 2
    bne     $a0, $t0, fail
    daddiu  $s0, $zero, 0x55

    # QEMU Malta's 32-bit PROM pointers sign-extend into 64-bit registers.
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x2000
    bne     $a1, $t0, fail
    nop
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x2008
    bne     $a2, $t0, fail
    nop
    daddiu  $t0, $zero, 0x0400
    dsll    $t0, $t0, 16
    bne     $a3, $t0, fail
    nop
    daddiu  $t0, $zero, 0x55
    bne     $s0, $t0, fail
    nop

    # argv[0] and argv[1] are 32-bit table entries, sign-extended by LW.
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x2040
    lw      $t1, 0($a1)
    bne     $t1, $t0, fail
    nop
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x2140
    lw      $t1, 4($a1)
    bne     $t1, $t0, fail
    nop
    lbu     $t2, 0($t1)
    daddiu  $t0, $zero, 'c'
    bne     $t2, $t0, fail
    nop
    lbu     $t2, 7($t1)
    daddiu  $t0, $zero, '='
    bne     $t2, $t0, fail
    nop
    lbu     $t2, 8($t1)
    daddiu  $t0, $zero, 't'
    bne     $t2, $t0, fail
    nop

    # The environment starts after the two arguments, contains memsize/value,
    # ememsize/value, modetty0/value, and an explicit NULL pointer.
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x2240
    lw      $t1, 0($a2)
    bne     $t1, $t0, fail
    nop
    lbu     $t2, 0($t1)
    daddiu  $t0, $zero, 'm'
    bne     $t2, $t0, fail
    nop
    lw      $t1, 4($a2)
    lbu     $t2, 0($t1)
    daddiu  $t0, $zero, '6'
    bne     $t2, $t0, fail
    nop
    lbu     $t2, 1($t1)
    daddiu  $t0, $zero, '7'
    bne     $t2, $t0, fail
    nop
    lbu     $t2, 7($t1)
    daddiu  $t0, $zero, '4'
    bne     $t2, $t0, fail
    nop
    lbu     $t2, 8($t1)
    bne     $t2, $zero, fail
    nop
    lw      $t1, 24($a2)
    bne     $t1, $zero, fail
    nop

    # Confirm zero-filled BSS and writable initialized data.
    lui     $t0, %hi(bss_word)
    daddiu  $t0, $t0, %lo(bss_word)
    lw      $t1, 0($t0)
    bne     $t1, $zero, fail
    nop
    lui     $t1, 0x4758
    ori     $t1, $t1, 0x4f53
    sw      $t1, 0($t0)
    lw      $t2, 0($t0)
    bne     $t2, $t1, fail
    nop

    lui     $t0, %hi(data_word)
    daddiu  $t0, $t0, %lo(data_word)
    lw      $t1, 0($t0)
    lui     $t2, 0x1122
    ori     $t2, $t2, 0x3344
    bne     $t1, $t2, fail
    nop
    lui     $t1, 0x6677
    ori     $t1, $t1, 0x8899
    sw      $t1, 0($t0)
    lw      $t2, 0($t0)
    bne     $t2, $t1, fail
    nop

    # Emit the success message via the guest's actual Malta NS16550 mapping.
    lui     $t0, 0xbf00
    ori     $t0, $t0, 0x0900
    lui     $t1, %hi(success_message)
    daddiu  $t1, $t1, %lo(success_message)
print_loop:
    lbu     $a0, 0($t1)
    beq     $a0, $zero, print_done
    nop
    sb      $a0, 0($t0)
    daddiu  $t1, $t1, 1
    beq     $zero, $zero, print_loop
    nop
print_done:
    # Store a completion word through CKSEG0 and remain in a bounded idle loop.
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x4000
    lui     $t1, 0x4758
    ori     $t1, $t1, 0x4f53
    sw      $t1, 0($t0)
pass_idle:
    beq     $zero, $zero, pass_idle
    nop

fail:
    lui     $t0, 0x8000
    ori     $t0, $t0, 0x4000
    daddiu  $t1, $zero, 0xbad
    sw      $t1, 0($t0)
fail_idle:
    beq     $zero, $zero, fail_idle
    nop
.end _start

.section .rodata,"a",@progbits
.align 3
success_message:
    .asciz "guideXOS MIPS64 ELF handoff OK\r\n"

.section .data,"aw",@progbits
.align 2
.globl data_word
data_word:
    .word 0x11223344

.section .bss,"aw",@nobits
.align 2
.globl bss_word
bss_word:
    .space 4
