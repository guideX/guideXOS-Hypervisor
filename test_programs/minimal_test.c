/**
 * Minimal IA-64 Test Program
 * 
 * This is a simple test program that can be used to verify the hypervisor
 * is working. It writes a test pattern to memory that can be displayed
 * in the framebuffer.
 * 
 * To compile:
 * If you have an IA-64 cross-compiler:
 *   ia64-linux-gnu-gcc -nostdlib -nodefaultlibs -static -Ttext=0x100000 \
 *     -o minimal_test.elf minimal_test.c -lgcc
 * 
 * If you don't have a cross-compiler, see the pre-built binary or
 * use the mock framebuffer test mode.
 */

// Framebuffer base address (configurable in VM config)
#define FRAMEBUFFER_BASE 0xA0000
#define FRAMEBUFFER_WIDTH 80
#define FRAMEBUFFER_HEIGHT 25

// Simple putchar function that writes to framebuffer
static void fb_putchar(int x, int y, char c, unsigned char color) {
    volatile unsigned short* fb = (volatile unsigned short*)FRAMEBUFFER_BASE;
    fb[y * FRAMEBUFFER_WIDTH + x] = (color << 8) | c;
}

// Write a string to framebuffer
static void fb_write_string(int x, int y, const char* str, unsigned char color) {
    int i = 0;
    while (str[i] && x + i < FRAMEBUFFER_WIDTH) {
        fb_putchar(x + i, y, str[i], color);
        i++;
    }
}

// Clear the screen
static void fb_clear(unsigned char color) {
    volatile unsigned short* fb = (volatile unsigned short*)FRAMEBUFFER_BASE;
    unsigned short fill = (color << 8) | ' ';
    
    for (int i = 0; i < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; i++) {
        fb[i] = fill;
    }
}

// Entry point
void _start(void) {
    // Clear screen with blue background
    fb_clear(0x1F); // White on blue
    
    // Draw a border
    for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
        fb_putchar(x, 0, '=', 0x1F);
        fb_putchar(x, FRAMEBUFFER_HEIGHT - 1, '=', 0x1F);
    }
    
    for (int y = 1; y < FRAMEBUFFER_HEIGHT - 1; y++) {
        fb_putchar(0, y, '|', 0x1F);
        fb_putchar(FRAMEBUFFER_WIDTH - 1, y, '|', 0x1F);
    }
    
    // Write title
    fb_write_string(25, 2, "guideXOS IA-64 Hypervisor", 0x1E); // Yellow on blue
    fb_write_string(28, 3, "Test Program v1.0", 0x1F);
    
    // Write status messages
    fb_write_string(10, 6, "CPU: IA-64 (Itanium) Architecture", 0x0F);
    fb_write_string(10, 7, "Status: Running", 0x0A); // Green
    fb_write_string(10, 8, "Entry Point: 0x100000", 0x0F);
    
    // Write test pattern
    fb_write_string(10, 11, "Testing CPU execution...", 0x0F);
    fb_write_string(10, 12, "[OK] CPU is executing instructions", 0x0A);
    fb_write_string(10, 13, "[OK] Memory access working", 0x0A);
    fb_write_string(10, 14, "[OK] Framebuffer output working", 0x0A);
    
    // Success message
    fb_write_string(20, 18, "All systems operational!", 0x0E); // Yellow
    
    // Show a blinking cursor
    int counter = 0;
    while (1) {
        // Simple delay loop
        for (volatile int i = 0; i < 10000000; i++) {
            // Do nothing
        }
        
        // Toggle cursor
        if (counter % 2 == 0) {
            fb_putchar(50, 20, '_', 0x0F);
        } else {
            fb_putchar(50, 20, ' ', 0x0F);
        }
        counter++;
    }
}

// Dummy functions to satisfy linker
void __main(void) {}
void __do_global_ctors(void) {}
void __do_global_dtors(void) {}
