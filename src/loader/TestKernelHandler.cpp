#include "TestKernelHandler.h"
#include "logger.h"
#include <cstring>

namespace ia64 {

bool TestKernelHandler::IsTestKernel(const uint8_t* data, size_t size) {
    if (size < 8) return false;
    
    // Check for "GUIDEXOS" magic
    const char* magic = "GUIDEXOS";
    return memcmp(data, magic, 8) == 0;
}

bool TestKernelHandler::ExecuteTestKernel(const uint8_t* data, size_t size, uint16_t* framebuffer) {
    if (!IsTestKernel(data, size)) {
        LOG_ERROR("Not a valid test kernel");
        return false;
    }
    
    LOG_INFO("Executing test kernel");
    LOG_INFO("  Magic: GUIDEXOS");
    
    // Skip header (magic + version + entry point = 16 bytes)
    size_t offset = 16;
    
    // Process commands
    while (offset < size) {
        Command cmd = static_cast<Command>(data[offset++]);
        
        switch (cmd) {
            case Command::CLEAR_SCREEN: {
                if (offset >= size) return false;
                uint8_t color = data[offset++];
                ClearScreen(framebuffer, color);
                LOG_INFO("  Command: Clear screen (color=0x" + 
                        std::to_string((int)color) + ")");
                break;
            }
            
            case Command::WRITE_STRING: {
                if (offset + 4 >= size) return false;
                uint8_t x = data[offset++];
                uint8_t y = data[offset++];
                uint8_t color = data[offset++];
                uint8_t length = data[offset++];
                
                if (offset + length > size) return false;
                
                WriteString(framebuffer, x, y, color, 
                           reinterpret_cast<const char*>(&data[offset]), length);
                offset += length;
                
                std::string text(reinterpret_cast<const char*>(&data[offset - length]), length);
                LOG_INFO("  Command: Write string at (" + std::to_string(x) + "," + 
                        std::to_string(y) + "): \"" + text + "\"");
                break;
            }
            
            case Command::DRAW_BOX: {
                if (offset + 4 >= size) return false;
                uint8_t x1 = data[offset++];
                uint8_t y1 = data[offset++];
                uint8_t x2 = data[offset++];
                uint8_t y2 = data[offset++];
                uint8_t color = data[offset++];
                
                DrawBox(framebuffer, x1, y1, x2, y2, color);
                LOG_INFO("  Command: Draw box (" + std::to_string(x1) + "," + 
                        std::to_string(y1) + ")-(" + std::to_string(x2) + "," + 
                        std::to_string(y2) + ")");
                break;
            }
            
            case Command::END: {
                LOG_INFO("  Command: End of kernel");
                LOG_INFO("Test kernel execution completed successfully!");
                return true;
            }
            
            default: {
                LOG_ERROR("Unknown command: 0x" + std::to_string((int)cmd));
                return false;
            }
        }
    }
    
    LOG_WARN("Test kernel ended without END command");
    return true;
}

void TestKernelHandler::ClearScreen(uint16_t* framebuffer, uint8_t color) {
    uint16_t fill = (color << 8) | ' ';
    for (int i = 0; i < 80 * 25; i++) {
        framebuffer[i] = fill;
    }
}

void TestKernelHandler::WriteString(uint16_t* framebuffer, int x, int y, 
                                    uint8_t color, const char* text, int length) {
    if (y < 0 || y >= 25) return;
    
    for (int i = 0; i < length && (x + i) < 80; i++) {
        if (x + i >= 0) {
            framebuffer[y * 80 + x + i] = (color << 8) | text[i];
        }
    }
}

void TestKernelHandler::DrawBox(uint16_t* framebuffer, int x1, int y1, 
                                int x2, int y2, uint8_t color) {
    // Draw horizontal lines
    for (int x = x1; x <= x2; x++) {
        if (x >= 0 && x < 80) {
            if (y1 >= 0 && y1 < 25) {
                framebuffer[y1 * 80 + x] = (color << 8) | (x == x1 || x == x2 ? '+' : '-');
            }
            if (y2 >= 0 && y2 < 25 && y2 != y1) {
                framebuffer[y2 * 80 + x] = (color << 8) | (x == x1 || x == x2 ? '+' : '-');
            }
        }
    }
    
    // Draw vertical lines
    for (int y = y1 + 1; y < y2; y++) {
        if (y >= 0 && y < 25) {
            if (x1 >= 0 && x1 < 80) {
                framebuffer[y * 80 + x1] = (color << 8) | '|';
            }
            if (x2 >= 0 && x2 < 80) {
                framebuffer[y * 80 + x2] = (color << 8) | '|';
            }
        }
    }
}

} // namespace ia64
