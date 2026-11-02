#pragma once

#include <cstdint>
#include <string>

namespace ia64 { class IMemory; }

namespace aarch64 {

enum class MMUAccess : uint8_t { Instruction, Read, Write };
enum class MMUFaultKind : uint8_t {
    None, AddressSize, Translation, AccessFlag, Permission, ExternalAbort,
    UnsupportedConfiguration
};

struct MMUResult {
    bool success = false;
    uint64_t physicalAddress = 0;
    uint8_t level = 0;
    uint8_t faultStatus = 0;
    MMUFaultKind fault = MMUFaultKind::None;
    bool duringTableWalk = false;
    std::string detail;
};

// Bounded ARMv8-A EL0/EL1 stage-1 walker: 4 KiB granules, 39/48-bit VAs,
// up to 48-bit output addresses, and a deliberately small Normal-memory
// attribute profile. It reads descriptors directly from physical IMemory.
class AArch64MMU {
public:
    static constexpr uint64_t kPageSize = 4096;

    static bool validateTcr(uint64_t tcr, std::string* reason = nullptr);
    static MMUResult translate(const ia64::IMemory& physicalMemory,
                               uint64_t virtualAddress,
                               uint8_t exceptionLevel,
                               MMUAccess access,
                               uint64_t tcr,
                               uint64_t ttbr0,
                               uint64_t ttbr1,
                               uint64_t mair,
                               bool writeExecuteNever);
};

} // namespace aarch64
