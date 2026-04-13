/**
 * minimal_kernel.c
 * 
 * Minimal IA-64 kernel for guideXOS Hypervisor
 * Demonstrates proper kernel boot and console output
 */

#include <stdint.h>

// Console MMIO address
#define CONSOLE_MMIO 0x1000

// Kernel signature for validation
const char kernel_signature[] __attribute__((section(".rodata"))) = 
    "GUIDEXOS_MINIMAL_IA64_KERNEL_V1";

const uint32_t kernel_version[] __attribute__((section(".rodata"))) = {1, 0, 0};

/**
 * Write a character to the console
 */
static inline void console_putc(char c) {
    volatile uint8_t* console = (volatile uint8_t*)CONSOLE_MMIO;
    *console = c;
}

/**
 * Write a string to the console
 */
static void console_puts(const char* str) {
    while (*str) {
        console_putc(*str++);
    }
}

/**
 * Halt the CPU
 */
static inline void halt(void) {
    while (1) {
        // In a real kernel, this would be a halt instruction
        // For now, infinite loop
        __asm__ volatile("nop");
    }
}

/**
 * Kernel entry point
 * 
 * This is called by the hypervisor after loading the kernel.
 * At this point:
 * - We're in kernel mode (CPL=0)
 * - Physical addressing is enabled
 * - Stack is set up
 * - Backing store is initialized
 */
void _start(void) __attribute__((section(".text.start")));

void _start(void) {
    // Print boot message
    console_puts("Hello IA-64!\n");
    console_puts("guideXOS Minimal Kernel v1.0.0\n");
    console_puts("Boot successful!\n");
    console_puts("\n");
    console_puts("System Information:\n");
    console_puts("  Architecture: IA-64\n");
    console_puts("  Privilege Level: 0 (Kernel)\n");
    console_puts("  Addressing Mode: Physical\n");
    console_puts("\n");
    console_puts("Kernel halted.\n");
    
    // Halt the system
    halt();
    
    // Should never reach here
    __builtin_unreachable();
}

/**
 * Emergency panic handler
 * Called if something goes catastrophically wrong
 */
void __attribute__((section(".text"))) kernel_panic(const char* message) {
    console_puts("\n*** KERNEL PANIC ***\n");
    console_puts(message);
    console_puts("\nSystem halted.\n");
    halt();
}

/**
 * Stack canary check (if using stack protection)
 */
void __attribute__((noreturn)) __stack_chk_fail(void) {
    kernel_panic("Stack smashing detected!");
}
