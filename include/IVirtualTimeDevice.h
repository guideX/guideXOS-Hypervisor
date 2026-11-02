#pragma once

#include <cstdint>

namespace ia64 {

// Device time follows the VM's deterministic virtual-cycle counter. A
// clocked device is advanced only at scheduler boundaries; implementations
// must not consult host clocks or schedule host callbacks.
class IVirtualTimeDevice {
public:
    virtual ~IVirtualTimeDevice() = default;
    virtual void AdvanceVirtualTime(uint64_t virtualCycle) noexcept = 0;
};

}  // namespace ia64
