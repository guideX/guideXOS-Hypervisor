#pragma once
#pragma once

#include "IODevice.h"
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ia64 {

class IInterruptController;

class VirtualTimer : public IMemoryMappedDevice, public ITickableDevice {
public:
    static constexpr uint64_t kDefaultBaseAddress = 0xFFFF0100ULL;
    static constexpr size_t kRegisterSize = 24;
    static constexpr uint64_t kControlOffset = 0;
    static constexpr uint64_t kIntervalOffset = 8;
    static constexpr uint64_t kVectorOffset = 16;
    static constexpr uint64_t kStatusOffset = 17;

    explicit VirtualTimer(uint64_t baseAddress = kDefaultBaseAddress);

    uint64_t GetBaseAddress() const override { return baseAddress_; }
    size_t GetSize() const override { return kRegisterSize; }
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;
    void Tick(uint64_t cycles) override;

    void AttachInterruptController(IInterruptController* controller);
    void SetCallback(std::function<void()> callback);
    void Configure(uint64_t intervalCycles, uint8_t vector, bool periodic, bool enabled);
    void Acknowledge();

    bool IsEnabled() const { return enabled_; }
    bool IsPeriodic() const { return periodic_; }
    bool HasPendingInterrupt() const { return interruptPending_; }
    uint64_t GetIntervalCycles() const { return intervalCycles_; }
    uint8_t GetInterruptVector() const { return interruptVector_; }

private:
    void Raise();

    uint64_t baseAddress_;
    IInterruptController* interruptController_;
    std::function<void()> callback_;
    uint64_t intervalCycles_;
    uint64_t elapsedCycles_;
    uint8_t interruptVector_;
    bool enabled_;
    bool periodic_;
    bool interruptPending_;
};

} // namespace ia64
