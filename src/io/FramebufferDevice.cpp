#include "FramebufferDevice.h"
#include <cstring>
#include <algorithm>

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

void FramebufferDevice::Reset() {
    Clear(0xFF000000);
}

} // namespace ia64
