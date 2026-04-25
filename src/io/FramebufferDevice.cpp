#include "FramebufferDevice.h"
#include <cstring>
#include <algorithm>
#include <cctype>

namespace ia64 {

FramebufferDevice::FramebufferDevice(uint64_t baseAddress, size_t width, size_t height)
    : baseAddress_(baseAddress)
    , width_(width)
    , height_(height)
    , pitch_(width * kBytesPerPixel)
    , bufferSize_(pitch_ * height)
    , framebuffer_(bufferSize_, 0) {
    
    // Initialize to black screen
    Clear(0xFF000000);
}

bool FramebufferDevice::Read(uint64_t address, uint8_t* data, size_t size) const {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    const uint64_t offset = address - baseAddress_;
    if (offset + size > bufferSize_) {
        return false;
    }

    std::memcpy(data, framebuffer_.data() + offset, size);
    return true;
}

bool FramebufferDevice::Write(uint64_t address, const uint8_t* data, size_t size) {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    const uint64_t offset = address - baseAddress_;
    if (offset + size > bufferSize_) {
        return false;
    }

    std::memcpy(framebuffer_.data() + offset, data, size);
    return true;
}

const uint8_t* FramebufferDevice::GetFramebuffer() const {
    return framebuffer_.data();
}

void FramebufferDevice::CopyFramebuffer(uint8_t* dest, size_t destSize) const {
    if (dest == nullptr || destSize < bufferSize_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::memcpy(dest, framebuffer_.data(), bufferSize_);
}

void FramebufferDevice::Clear(uint32_t color) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint32_t* pixels = reinterpret_cast<uint32_t*>(framebuffer_.data());
    const size_t pixelCount = width_ * height_;
    
    std::fill(pixels, pixels + pixelCount, color);
}

namespace {
const char* GlyphRows(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
        case 'C': return "01111" "10000" "10000" "10000" "10000" "10000" "01111";
        case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
        case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
        case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
        case 'G': return "01111" "10000" "10000" "10011" "10001" "10001" "01111";
        case 'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'I': return "11111" "00100" "00100" "00100" "00100" "00100" "11111";
        case 'J': return "00111" "00010" "00010" "00010" "10010" "10010" "01100";
        case 'K': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
        case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
        case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
        case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
        case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
        case 'Q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
        case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
        case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
        case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
        case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'V': return "10001" "10001" "10001" "10001" "01010" "01010" "00100";
        case 'W': return "10001" "10001" "10001" "10101" "10101" "10101" "01010";
        case 'X': return "10001" "01010" "00100" "00100" "00100" "01010" "10001";
        case 'Y': return "10001" "01010" "00100" "00100" "00100" "00100" "00100";
        case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
        case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
        case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
        case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
        case '3': return "11110" "00001" "00001" "01110" "00001" "00001" "11110";
        case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
        case '5': return "11111" "10000" "10000" "11110" "00001" "00001" "11110";
        case '6': return "01111" "10000" "10000" "11110" "10001" "10001" "01110";
        case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
        case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
        case '9': return "01110" "10001" "10001" "01111" "00001" "00001" "11110";
        case ':': return "00000" "00100" "00100" "00000" "00100" "00100" "00000";
        case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
        case '-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
        case '/': return "00001" "00010" "00010" "00100" "01000" "01000" "10000";
        case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
        default:  return "11111" "00001" "00010" "00100" "00100" "00000" "00100";
    }
}
}

void FramebufferDevice::DrawText(size_t x, size_t y, const char* text, uint32_t color, size_t scale) {
    if (text == nullptr || scale == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t* pixels = reinterpret_cast<uint32_t*>(framebuffer_.data());
    size_t cursorX = x;

    for (const char* p = text; *p != '\0'; ++p) {
        const char* glyph = GlyphRows(*p);
        for (size_t row = 0; row < 7; ++row) {
            for (size_t col = 0; col < 5; ++col) {
                if (glyph[row * 5 + col] != '1') {
                    continue;
                }
                for (size_t sy = 0; sy < scale; ++sy) {
                    for (size_t sx = 0; sx < scale; ++sx) {
                        const size_t px = cursorX + col * scale + sx;
                        const size_t py = y + row * scale + sy;
                        if (px < width_ && py < height_) {
                            pixels[py * width_ + px] = color;
                        }
                    }
                }
            }
        }
        cursorX += 6 * scale;
        if (cursorX >= width_) {
            break;
        }
    }
}

void FramebufferDevice::Reset() {
    Clear(0xFF000000);
}

} // namespace ia64
