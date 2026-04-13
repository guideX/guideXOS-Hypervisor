/**
 * minimal_kernel.s
 * 
 * Minimal IA-64 kernel for guideXOS Hypervisor
 * 
 * This kernel demonstrates:
 * - Proper IA-64 entry point
 * - Console output via MMIO
 * - Clean halt instruction
 * 
 * Memory map:
 * - Code starts at 0xE000000000100000 (kernel virtual space)
 * - Console at 0x1000 (MMIO)
 * - Stack at 0xE000000000200000
 */

.text
.align 16                           // IA-64 bundles must be 16-byte aligned
.global _start
.type _start, @function

/**
 * Kernel entry point
 * 
 * Entry conditions (set by hypervisor):
 * - PSR.ic = 0 (interruption collection disabled)
 * - PSR.i = 0 (interrupts disabled)
 * - PSR.dt = 0 (data translation disabled - physical mode)
 * - PSR.it = 0 (instruction translation disabled)
 * - PSR.cpl = 0 (kernel privilege level)
 * - r12 (sp) = initial kernel stack
 * - ar.bsp = initial backing store pointer
 */
_start:
    // Bundle 0: Initialize stack and backing store
    {
        .mmi
        mov r12 = 0xE000000000200000    // Set kernel stack pointer
        mov r13 = 0                      // Clear r13 (thread pointer)
        mov r14 = 0                      // Clear r14
    }
    
    // Bundle 1: Initialize application registers
    {
        .mmi
        mov ar.rsc = 0                   // Disable RSE during setup
        mov ar.bspstore = 0xE000000000180000  // Set backing store
        mov r15 = 0
    }
    
    // Bundle 2: Configure RSE
    {
        .mmi
        mov ar.rnat = 0                  // Clear NaT collection
        mov r16 = 0x3                    // RSE mode: eager, kernel privilege
        mov r17 = 0
    }
    
    // Bundle 3: Enable RSE
    {
        .mmi
        mov ar.rsc = r16                 // Enable RSE with configuration
        mov r18 = 0x1000                 // Console MMIO address
        mov r19 = 0
    }
    
    // Bundle 4: Write "H" to console
    {
        .mmi
        mov r20 = 'H'                    // ASCII 'H'
        mov r21 = 0
        mov r22 = 0
    }
    
    // Bundle 5: Store to console
    {
        .mmi
        st1 [r18] = r20                  // Write byte to console
        mov r20 = 'e'                    // ASCII 'e'
        adds r18 = 1, r18                // Move to next console position
    }
    
    // Bundle 6: Write "e"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 'l'
        adds r18 = 1, r18
    }
    
    // Bundle 7: Write first "l"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 'l'
        adds r18 = 1, r18
    }
    
    // Bundle 8: Write second "l"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 'o'
        adds r18 = 1, r18
    }
    
    // Bundle 9: Write "o"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = ' '
        adds r18 = 1, r18
    }
    
    // Bundle 10: Write space
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 'I'
        adds r18 = 1, r18
    }
    
    // Bundle 11: Write "I"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 'A'
        adds r18 = 1, r18
    }
    
    // Bundle 12: Write "A"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = '-'
        adds r18 = 1, r18
    }
    
    // Bundle 13: Write "-"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = '6'
        adds r18 = 1, r18
    }
    
    // Bundle 14: Write "6"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = '4'
        adds r18 = 1, r18
    }
    
    // Bundle 15: Write "4"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = '!'
        adds r18 = 1, r18
    }
    
    // Bundle 16: Write "!"
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 0x0A                   // ASCII newline
        adds r18 = 1, r18
    }
    
    // Bundle 17: Write newline
    {
        .mmi
        st1 [r18] = r20
        mov r20 = 0
        mov r18 = 0
    }
    
    // Bundle 18: Halt the CPU
    {
        .mib
        mov r0 = 0                       // Nop
        mov r0 = 0                       // Nop
        br.sptk halt                     // Branch to halt
    }

/**
 * Halt function - infinite loop
 */
.align 16
halt:
    {
        .mib
        mov r0 = 0                       // Nop
        mov r0 = 0                       // Nop
        br.sptk halt                     // Branch to self (infinite loop)
    }

.size _start, . - _start

/**
 * Kernel signature
 * This can be checked by the validator to ensure it's our minimal kernel
 */
.section .rodata
.align 8
.global kernel_signature
kernel_signature:
    .ascii "GUIDEXOS_MINIMAL_IA64_KERNEL_V1"
    .byte 0

.global kernel_version
kernel_version:
    .word 1                              // Major version
    .word 0                              // Minor version
    .word 0                              // Patch version
