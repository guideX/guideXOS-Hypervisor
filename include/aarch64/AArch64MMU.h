#pragma once

#include <cstdint>
#include <string>

namespace ia64 { class IMemory; }

namespace aarch64 {

enum class MMUAccess : uint8_t { Instruction, Read, Write };
enum class MMURegion : uint8_t { Lower, Upper };
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
    struct Attributes {
        uint64_t virtualBase = 0;
        uint64_t physicalBase = 0;
        uint64_t mappingSize = 0;
        uint8_t level = 0;
        uint8_t accessPermissions = 0;
        uint8_t attributeIndex = 0;
        uint8_t memoryAttribute = 0;
        bool accessFlag = false;
        bool el0Allowed = false;
        bool readOnly = false;
        bool userExecuteNever = false;
        bool privilegedExecuteNever = false;
        bool global = false;
    } attributes;
    std::string detail;
};

struct MMUContext {
    bool valid = false;
    MMURegion region = MMURegion::Lower;
    uint8_t vaBits = 0;
    uint8_t asid = 0;
    uint64_t configurationKey = 0;
    MMUFaultKind fault = MMUFaultKind::None;
    uint8_t level = 0;
    uint8_t faultStatus = 0;
    bool duringTableWalk = false;
    std::string detail;
};

// Bounded ARMv8-A EL0/EL1 stage-1 walker: 4 KiB granules, 39/48-bit VAs,
// up to 48-bit output addresses, and a deliberately small Normal plus
// Device-nGnRnE attribute profile. It reads descriptors directly from
// physical IMemory.
class AArch64MMU {
public:
    static constexpr uint64_t kPageSize = 4096;

    static bool validateTcr(uint64_t tcr, std::string* reason = nullptr);
    static MMUContext selectContext(uint64_t virtualAddress,
                                   uint8_t exceptionLevel,
                                   uint64_t tcr,
                                   uint64_t ttbr0,
                                   uint64_t ttbr1);
    static MMUResult checkAccess(const MMUResult::Attributes& attributes,
                                 uint64_t virtualAddress,
                                 uint8_t exceptionLevel,
                                 MMUAccess access,
                                 bool writeExecuteNever);
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
