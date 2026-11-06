#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace riscv::boot_contract {

inline constexpr uint32_t kSchemaVersion = 1;
inline constexpr uint64_t kRamBase = 0x80000000ULL;
inline constexpr uint64_t kRamSize = 0x04000000ULL;
inline constexpr uint64_t kFirmwareEntry = 0x80000000ULL;
inline constexpr uint64_t kFirmwareSlotEnd = 0x80200000ULL;
inline constexpr uint64_t kSupervisorEntry = 0x80200000ULL;
inline constexpr uint64_t kSupervisorReservationEnd = 0x80220000ULL;
inline constexpr uint64_t kFdtAddress = 0x82200000ULL;
inline constexpr uint64_t kFdtCapacity = 0x3000ULL;
inline constexpr uint64_t kPageTablesAddress = 0x83C00000ULL;
inline constexpr uint64_t kPageTablesSize = 0x4000ULL;
inline constexpr uint64_t kResultAddress = 0x83E00000ULL;
inline constexpr uint64_t kResultSize = 0x1000ULL;
inline constexpr uint64_t kSupervisorStackAddress = 0x83FF0000ULL;
inline constexpr uint64_t kSupervisorStackSize = 0x10000ULL;
inline constexpr uint64_t kPhysicalAddressBits = 56;
inline constexpr uint64_t kTimerFrequency = 10000000ULL;
inline constexpr size_t kPmpImplementedEntries = 16;

struct PhysicalRange {
    std::string_view name;
    uint64_t start;
    uint64_t size;
    uint64_t alignment;
    std::string_view owner;
    std::string_view kind;
    std::string_view privilegeAccess;
    std::string_view initializationSource;
    std::string_view lifetime;
    bool reservedMemory;
    bool pmpCoverage;
};

inline constexpr std::array<PhysicalRange, 8> kFixtureReservations{{
    {"opensbi-text", 0x80000000ULL, 0x00080000ULL, 0x1000ULL,
     "resident OpenSBI firmware", "RAM", "M-mode only", "OpenSBI reserved-memory fixup",
     "firmware lifetime", true, true},
    {"opensbi-data-scratch", 0x80080000ULL, 0x00020000ULL, 0x1000ULL,
     "resident OpenSBI firmware", "RAM", "M-mode only", "OpenSBI reserved-memory fixup",
     "firmware lifetime", true, true},
    {"opensbi-slot-gap", 0x800A0000ULL, 0x00160000ULL, 0x1000ULL,
     "reserved firmware load slot", "RAM", "unmatched S/U access denies",
     "fixture FDT", "platform lifetime", true, true},
    {"supervisor-test-payload", 0x80200000ULL, 0x00020000ULL, 0x1000ULL,
     "RISCV-16 test guest", "RAM", "S-mode", "FW_JUMP ELF loader",
     "test fixture only", true, true},
    {"firmware-fdt", 0x82200000ULL, 0x00003000ULL, 8ULL,
     "OpenSBI handoff FDT", "RAM", "S-mode read-only by PMP window",
     "FW_JUMP input and OpenSBI fixup", "handoff and test lifetime", true, true},
    {"guest-page-tables", 0x83C00000ULL, 0x00004000ULL, 0x1000ULL,
     "RISCV-16 test guest", "RAM", "S-mode read-only by PMP priority",
     "fixture ELF preload", "test fixture only", true, true},
    {"guest-result", 0x83E00000ULL, 0x00001000ULL, 0x1000ULL,
     "RISCV-16 test guest", "RAM", "S-mode read/write", "fixture ELF preload",
     "test fixture only", true, true},
    {"guest-stack", 0x83FF0000ULL, 0x00010000ULL, 0x1000ULL,
     "RISCV-16 test guest", "RAM", "S-mode read/write", "fixture ELF loader",
     "test fixture only", true, true},
}};

// Linux Image placement derives from the pinned upstream RV64 S-mode format.
// The FDT and firmware ranges alias the authoritative RISCV-17 inventory above;
// Linux drops all test payload, page-table, result, and stack reservations.
inline constexpr uint64_t kLinuxImageTextOffset = 0x00200000ULL;
inline constexpr uint64_t kLinuxImageAlignment = 0x00200000ULL;
inline constexpr uint64_t kLinuxImageEntry = kRamBase + kLinuxImageTextOffset;
inline constexpr uint64_t kLinuxFdtAddress = kFdtAddress;
inline constexpr uint64_t kLinuxFdtCapacity = kFdtCapacity;
inline constexpr uint64_t kLinuxBootHartId = 0;
inline constexpr uint32_t kLinuxImageHeaderVersion = 0x00000002U;
inline constexpr std::array<PhysicalRange, 4> kLinuxReservations{{
    kFixtureReservations[0], // OpenSBI text
    kFixtureReservations[1], // OpenSBI data/scratch
    kFixtureReservations[2], // protected firmware-slot gap
    kFixtureReservations[4], // FDT and post-fixup capacity
}};

inline constexpr std::array<PhysicalRange, 5> kPmpGuestRamWindows{{
    {"guest-ram-2m", 0x80200000ULL, 0x00200000ULL, 0x00200000ULL,
     "supervisor domain", "RAM", "S-mode RWX", "OpenSBI domain FDT",
     "domain lifetime", false, true},
    {"guest-ram-4m", 0x80400000ULL, 0x00400000ULL, 0x00400000ULL,
     "supervisor domain", "RAM", "S-mode RWX", "OpenSBI domain FDT",
     "domain lifetime", false, true},
    {"guest-ram-8m", 0x80800000ULL, 0x00800000ULL, 0x00800000ULL,
     "supervisor domain", "RAM", "S-mode RWX", "OpenSBI domain FDT",
     "domain lifetime", false, true},
    {"guest-ram-16m", 0x81000000ULL, 0x01000000ULL, 0x01000000ULL,
     "supervisor domain", "RAM", "S-mode RWX", "OpenSBI domain FDT",
     "domain lifetime", false, true},
    {"guest-ram-32m", 0x82000000ULL, 0x02000000ULL, 0x02000000ULL,
     "supervisor domain", "RAM", "S-mode RWX", "OpenSBI domain FDT",
     "domain lifetime", false, true},
}};

inline constexpr std::array<PhysicalRange, 5> kDevices{{
    {"UART0", 0x10000000ULL, 0x100ULL, 0x100ULL,
     "machine UART model and supervisor console", "MMIO", "S-mode RW, no execute",
     "fixture FDT and machine mapping", "platform lifetime", false, true},
    {"UART1", 0x1000A000ULL, 0x100ULL, 0x100ULL,
     "machine UART model", "MMIO", "S-mode RW, no execute",
     "fixture FDT and machine mapping", "platform lifetime", false, true},
    {"PLIC", 0x0C000000ULL, 0x00400000ULL, 0x00400000ULL,
     "machine PLIC model", "MMIO", "S-mode RW, no execute",
     "fixture FDT and machine mapping", "platform lifetime", false, true},
    {"ACLINT-mtime", 0x0200BFF8ULL, 8ULL, 8ULL,
     "M-mode OpenSBI timer", "MMIO", "M-mode only; hidden from S-mode",
     "fixture FDT and machine mapping", "platform lifetime", false, true},
    {"ACLINT-mtimecmp", 0x02004000ULL, 8ULL, 8ULL,
     "M-mode OpenSBI timer", "MMIO", "M-mode only; hidden from S-mode",
     "fixture FDT and machine mapping", "platform lifetime", false, true},
}};

inline constexpr std::array<PhysicalRange, 2> kOpenSbiProtectedRanges{{
    {"OpenSBI-text", 0x80000000ULL, 0x00080000ULL, 4ULL,
     "resident OpenSBI", "RAM", "M-mode only", "OpenSBI v1.8.1 PMP setup",
     "firmware lifetime", true, true},
    {"OpenSBI-data-scratch", 0x80080000ULL, 0x00020000ULL, 4ULL,
     "resident OpenSBI", "RAM", "M-mode only", "OpenSBI v1.8.1 PMP setup",
     "firmware lifetime", true, true},
}};

constexpr bool CheckedEnd(uint64_t start, uint64_t size, uint64_t& endExclusive) {
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - start) return false;
    endExclusive = start + size;
    return true;
}

constexpr bool IsAligned(uint64_t value, uint64_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1ULL)) == 0 &&
           (value & (alignment - 1ULL)) == 0;
}

constexpr bool IsContained(uint64_t outerStart, uint64_t outerSize,
                           uint64_t innerStart, uint64_t innerSize) {
    uint64_t outerEnd = 0;
    uint64_t innerEnd = 0;
    return CheckedEnd(outerStart, outerSize, outerEnd) &&
           CheckedEnd(innerStart, innerSize, innerEnd) &&
           innerStart >= outerStart && innerEnd <= outerEnd;
}

constexpr bool Overlaps(uint64_t leftStart, uint64_t leftSize,
                        uint64_t rightStart, uint64_t rightSize) {
    uint64_t leftEnd = 0;
    uint64_t rightEnd = 0;
    if (!CheckedEnd(leftStart, leftSize, leftEnd) ||
        !CheckedEnd(rightStart, rightSize, rightEnd)) return true;
    return leftStart < rightEnd && rightStart < leftEnd;
}

constexpr uint64_t Log2PowerOfTwo(uint64_t value) {
    uint64_t order = 0;
    while (value > 1) {
        value >>= 1;
        ++order;
    }
    return order;
}

constexpr uint64_t EncodeNapotAddress(uint64_t base, uint64_t size) {
    if (size < 8 || (size & (size - 1ULL)) != 0 ||
        !IsAligned(base, size)) return 0;
    const uint64_t trailingOnes = Log2PowerOfTwo(size) - 3ULL;
    const uint64_t mask = trailingOnes == 0 ? 0ULL : (1ULL << trailingOnes) - 1ULL;
    return (base >> 2) | mask;
}

} // namespace riscv::boot_contract
