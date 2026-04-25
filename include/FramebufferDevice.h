#pragma once

#include "IODevice.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>

namespace ia64 {

/**
 * @brief Simple framebuffer device for graphical output
 * 
 * Provides a memory-mapped framebuffer for graphical display.
 * Default resolution: 640x480, 32-bit BGRA format
 * Base address: 0xB8000000 (traditional VGA-like region)
 */
class FramebufferDevice : public IMemoryMappedDevice {
public:
    static constexpr uint64_t kDefaultBaseAddress = 0xB8000000ULL;
    static constexpr size_t kDefaultWidth = 640;
    static constexpr size_t kDefaultHeight = 480;
    static constexpr size_t kBytesPerPixel = 4; // BGRA32

    explicit FramebufferDevice(
        uint64_t baseAddress = kDefaultBaseAddress,
        size_t width = kDefaultWidth,
        size_t height = kDefaultHeight);

    ~FramebufferDevice() override = default;

    // IMemoryMappedDevice interface
    uint64_t GetBaseAddress() const override { return baseAddress_; }
    size_t GetSize() const override { return bufferSize_; }
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;

    // Framebuffer access
    const uint8_t* GetFramebuffer() const;
    size_t GetWidth() const { return width_; }
    size_t GetHeight() const { return height_; }
    size_t GetBytesPerPixel() const { return kBytesPerPixel; }
    size_t GetPitch() const { return pitch_; }

    // Thread-safe framebuffer copy
    void CopyFramebuffer(uint8_t* dest, size_t destSize) const;

    // Clear framebuffer
    void Clear(uint32_t color = 0xFF000000); // Black by default

    // Draw diagnostic text directly into the framebuffer.
    void DrawText(size_t x, size_t y, const char* text, uint32_t color = 0xFFFFFFFF, size_t scale = 2);

    // Reset device
    void Reset();

private:
    uint64_t baseAddress_;
    size_t width_;
    size_t height_;
    size_t pitch_;        // Bytes per scanline
    size_t bufferSize_;   // Total buffer size in bytes
    std::vector<uint8_t> framebuffer_;
    mutable std::mutex mutex_;
};

} // namespace ia64
