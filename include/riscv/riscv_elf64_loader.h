#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace riscv {

class RV64CPU;

enum class Elf64LoadError {
    None,
    InvalidElfFormat,
    UnsupportedArchitecture,
    UnsupportedAbi,
    UnsupportedExecutableType,
    MalformedProgramHeaders,
    InvalidSegmentBounds,
    OverlappingSegments,
    InsufficientGuestMemory,
    ReservedRegionCollision,
    UnsupportedRuntimeLinking,
    UnsupportedRelocations,
    InvalidEntrypoint,
    ResourceLimitExceeded,
    GuestMemoryAccessFailure,
};

const char* Elf64LoadErrorToString(Elf64LoadError error);

struct Elf64ReservedRegion {
    uint64_t base = 0;
    uint64_t size = 0;
    const char* name = nullptr;
};

struct Elf64LoadOptions {
    uint64_t ramBase = 0;
    uint64_t ramSize = 0;
    std::span<const Elf64ReservedRegion> reservedRegions{};
    size_t maxFileBytes = 16U * 1024U * 1024U;
    uint64_t maxLoadBytes = 8U * 1024U * 1024U;
    uint16_t maxProgramHeaders = 64;
    uint16_t maxLoadSegments = 16;
};

struct Elf64LoadSegment {
    uint64_t fileOffset = 0;
    uint64_t virtualAddress = 0;
    uint64_t physicalAddress = 0;
    uint64_t fileSize = 0;
    uint64_t memorySize = 0;
    uint64_t alignment = 0;
    uint32_t flags = 0;
};

struct Elf64LoadPlan {
    uint64_t entryPoint = 0;
    uint32_t flags = 0;
    uint16_t programHeaderCount = 0;
    std::vector<Elf64LoadSegment> segments;
    uint64_t totalMemoryBytes = 0;
};

struct Elf64PlanResult {
    Elf64LoadError error = Elf64LoadError::None;
    std::string detail;
    Elf64LoadPlan plan;

    bool ok() const { return error == Elf64LoadError::None; }
};

struct Elf64LoadResult {
    Elf64LoadError error = Elf64LoadError::None;
    std::string detail;
    Elf64LoadPlan plan;

    bool ok() const { return error == Elf64LoadError::None; }
};

// Build a fully validated identity-mapped load plan without modifying memory
// or CPU state. Section headers are optional and are never used for placement.
Elf64PlanResult planRiscvElf64(std::span<const uint8_t> file,
                               const Elf64LoadOptions& options);

// Stage every segment and its rollback image before writing. On success this
// writes only PT_LOAD memory ranges, zeroes their BSS tails, resets the CPU to
// M-mode, and sets PC to the validated ELF entry point. Rejected files leave
// both guest RAM and CPU state unchanged.
Elf64LoadResult loadRiscvElf64(std::span<const uint8_t> file,
                               RV64CPU& cpu,
                               const Elf64LoadOptions& options);

} // namespace riscv
