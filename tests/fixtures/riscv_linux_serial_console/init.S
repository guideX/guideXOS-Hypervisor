    .option norvc
    .section .text
    .align 2
    .globl _start
    .type _start, @function
_start:
    li s3, 0
    li a7, 172                    # Linux getpid
    ecall
    li t0, 1
    bne a0, t0, guest_failure

    li a0, -100                   # AT_FDCWD
    la a1, serial_path
    li a2, 2                      # O_RDWR
    li a3, 0
    li a7, 56                     # openat
    .globl riscv30_openat_ecall
riscv30_openat_ecall:
    ecall
    .globl riscv30_openat_resume
riscv30_openat_resume:
    bltz a0, guest_failure
    mv s0, a0
    la s1, input_buffer

.globl riscv30_command_loop
riscv30_command_loop:
    mv a0, s0
    la a1, prompt
    li a2, 10
    li a7, 64                     # write
    .globl riscv30_prompt_write_ecall
riscv30_prompt_write_ecall:
    ecall

    mv a0, s0
    mv a1, s1
    li a2, 64                     # bounded line buffer
    li a7, 63                     # read
    .globl riscv30_read_ecall
riscv30_read_ecall:
    ecall
    .globl riscv30_read_resume
riscv30_read_resume:
    blez a0, riscv30_command_loop
    mv s2, a0
    add t0, s1, s2
    addi t0, t0, -1
    lbu t1, 0(t0)
    li t2, 10
    bne t1, t2, riscv30_command_loop  # never execute a partial line

    li t0, 5                      # ping\n
    bne s2, t0, check_status
    lbu t1, 0(s1)
    li t2, 'p'
    bne t1, t2, check_status
    lbu t1, 1(s1)
    li t2, 'i'
    bne t1, t2, check_status
    lbu t1, 2(s1)
    li t2, 'n'
    bne t1, t2, check_status
    lbu t1, 3(s1)
    li t2, 'g'
    bne t1, t2, check_status
    j riscv30_command_ping

check_status:
    li t0, 7                      # status\n
    bne s2, t0, check_echo
    lbu t1, 0(s1)
    li t2, 's'
    bne t1, t2, check_echo
    lbu t1, 1(s1)
    li t2, 't'
    bne t1, t2, check_echo
    lbu t1, 2(s1)
    li t2, 'a'
    bne t1, t2, check_echo
    lbu t1, 3(s1)
    li t2, 't'
    bne t1, t2, check_echo
    lbu t1, 4(s1)
    li t2, 'u'
    bne t1, t2, check_echo
    lbu t1, 5(s1)
    li t2, 's'
    bne t1, t2, check_echo
    j riscv30_command_status

check_echo:
    li t0, 11                     # echo hello\n
    bne s2, t0, check_quit
    lbu t1, 0(s1)
    li t2, 'e'
    bne t1, t2, check_quit
    lbu t1, 1(s1)
    li t2, 'c'
    bne t1, t2, check_quit
    lbu t1, 2(s1)
    li t2, 'h'
    bne t1, t2, check_quit
    lbu t1, 3(s1)
    li t2, 'o'
    bne t1, t2, check_quit
    lbu t1, 4(s1)
    li t2, ' '
    bne t1, t2, check_quit
    lbu t1, 5(s1)
    li t2, 'h'
    bne t1, t2, check_quit
    lbu t1, 6(s1)
    li t2, 'e'
    bne t1, t2, check_quit
    lbu t1, 7(s1)
    li t2, 'l'
    bne t1, t2, check_quit
    lbu t1, 8(s1)
    li t2, 'l'
    bne t1, t2, check_quit
    lbu t1, 9(s1)
    li t2, 'o'
    bne t1, t2, check_quit
    j riscv30_command_echo

check_quit:
    li t0, 5                      # quit\n
    bne s2, t0, riscv30_command_unknown
    lbu t1, 0(s1)
    li t2, 'q'
    bne t1, t2, riscv30_command_unknown
    lbu t1, 1(s1)
    li t2, 'u'
    bne t1, t2, riscv30_command_unknown
    lbu t1, 2(s1)
    li t2, 'i'
    bne t1, t2, riscv30_command_unknown
    lbu t1, 3(s1)
    li t2, 't'
    bne t1, t2, riscv30_command_unknown
    j riscv30_command_quit

    .globl riscv30_command_ping
riscv30_command_ping:
    la a1, response_pong
    li a2, 5
    j riscv30_response_write

    .globl riscv30_command_status
riscv30_command_status:
    la a1, response_status
    li a2, 13
    j riscv30_response_write

    .globl riscv30_command_echo
riscv30_command_echo:
    la a1, response_hello
    li a2, 6
    j riscv30_response_write

    .globl riscv30_command_unknown
riscv30_command_unknown:
    la a1, response_unknown
    li a2, 16
    j riscv30_response_write

    .globl riscv30_command_quit
riscv30_command_quit:
    li s3, 1
    la a1, response_quit
    li a2, 18
    j riscv30_response_write

riscv30_response_write:
    mv a0, s0
    li a7, 64
    .globl riscv30_response_write_ecall
riscv30_response_write_ecall:
    ecall
    j riscv30_command_after_response

riscv30_command_after_response:
    # The quit acknowledgement is followed by a live, bounded yield loop.
    beq s3, zero, riscv30_command_loop
.globl riscv30_quit_yield_loop
riscv30_quit_yield_loop:
    li a7, 124                    # sched_yield
    ecall
    j riscv30_quit_yield_loop

guest_failure:
    li a0, 1
    la a1, failure_message
    li a2, 22
    li a7, 64
    ecall
guest_failure_wait:
    li a7, 124
    ecall
    j guest_failure_wait

    .section .rodata
serial_path:
    .asciz "/dev/ttyS0"
prompt:
    .ascii "guideXOS> "
prompt_end:
response_pong:
    .ascii "pong\n"
response_pong_end:
response_status:
    .ascii "status:pid=1\n"
response_status_end:
response_hello:
    .ascii "hello\n"
response_hello_end:
response_unknown:
    .ascii "unknown command\n"
response_unknown_end:
response_quit:
    .ascii "quit acknowledged\n"
response_quit_end:
failure_message:
    .ascii "RISCV-30 init failure\n"
failure_message_end:

    .section .bss
    .align 4
input_buffer:
    .zero 64
