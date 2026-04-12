#pragma once

#include <cstddef>
#include <cstdint>

namespace ia64 {

class IMemoryMappedDevice {
public:
    virtual ~IMemoryMappedDevice() = default;

    virtual uint64_t GetBaseAddress() const = 0;
    virtual size_t GetSize() const = 0;
    virtual bool Read(uint64_t address, uint8_t* data, size_t size) const = 0;
    virtual bool Write(uint64_t address, const uint8_t* data, size_t size) = 0;

    bool HandlesRange(uint64_t address, size_t size) const {
        if (size == 0) {
            return false;
        }

        const uint64_t base = GetBaseAddress();
        const uint64_t end = base + static_cast<uint64_t>(GetSize());
        const uint64_t requestEnd = address + static_cast<uint64_t>(size);

        return address >= base && requestEnd >= address && requestEnd <= end;
    }
};

class ITickableDevice {
public:
    virtual ~ITickableDevice() = default;
    virtual void Tick(uint64_t cycles) = 0;
};

} // namespace ia64
