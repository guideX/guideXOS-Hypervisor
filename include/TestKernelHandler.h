#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace ia64 {

/**
 * Test Kernel Handler
 * 
 * Handles special test kernels that contain display commands
 * instead of actual IA-64 instructions. This allows testing
 * the hypervisor without needing an IA-64 cross-compiler.
 */
class TestKernelHandler {
public:
    /**
     * Check if a loaded binary is a test kernel
     * @param data Pointer to binary data
     * @param size Size of binary data
     * @return true if this is a recognized test kernel
     */
    static bool IsTestKernel(const uint8_t* data, size_t size);
    
    /**
     * Execute a test kernel
     * This interprets the test kernel commands and updates the framebuffer
     * 
     * @param data Pointer to kernel data
     * @param size Size of kernel data
     * @param framebuffer Pointer to framebuffer (80x25 VGA text mode)
     * @return true if execution succeeded
     */
    static bool ExecuteTestKernel(const uint8_t* data, size_t size, uint16_t* framebuffer);
    
private:
    // Command opcodes
    enum class Command : uint8_t {
        CLEAR_SCREEN = 0x01,
        WRITE_STRING = 0x02,
        DRAW_BOX = 0x03,
        END = 0xFF
    };
    
    static void ClearScreen(uint16_t* framebuffer, uint8_t color);
    static void WriteString(uint16_t* framebuffer, int x, int y, uint8_t color, 
                           const char* text, int length);
    static void DrawBox(uint16_t* framebuffer, int x1, int y1, int x2, int y2, uint8_t color);
};

} // namespace ia64
