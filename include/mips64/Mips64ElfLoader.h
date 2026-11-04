#pragma once

#include "PhysicalAddressMap.h"
#include "mips64/Mips64Cpu.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mips64 {

struct Mips64ElfRange {
    uint64_t base = 0;
    uint64_t size = 0;
};

struct Mips64ElfLoadOptions {
    Mips64Endianness guestEndianness = Mips64Endianness::Little;
    uint64_t guestRamSize = 0;
    uint64_t maximumFileBytes = 256ULL * 1024ULL * 1024ULL;
    uint64_t maximumStagedBytes = 256ULL * 1024ULL * 1024ULL;
    std::vector<Mips64ElfRange> reservedPhysicalRanges;
};

struct Mips64ElfLoadSegment {
    uint64_t fileOffset = 0;
    uint64_t virtualAddress = 0;
    uint64_t declaredPhysicalAddress = 0;
    uint64_t physicalAddress = 0;
    uint64_t fileSize = 0;
    uint64_t memorySize = 0;
    uint64_t alignment = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> image;
};

struct Mips64ElfLoadPlan {
    Mips64Endianness endianness = Mips64Endianness::Little;
    uint64_t entryPoint = 0;
    uint64_t entryPhysicalAddress = 0;
    uint32_t machineFlags = 0;
    bool requiresKernelKx = false;
    std::vector<Mips64ElfLoadSegment> segments;
};

// MIPS64 parser and staged loader. This deliberately does not reuse the
// IA-64 ELFLoader structures or CPU initialization path.
class Mips64ElfLoader final {
public:
    static bool BuildPlan(const uint8_t* file, size_t fileSize,
                          const Mips64ElfLoadOptions& options,
                          Mips64ElfLoadPlan& plan,
                          std::string* error = nullptr);

    // Segment writes and caller-supplied boot data (for example PROM tables)
    // become visible through a dense RAM backing swap only after all targets
    // have passed validation.
    static bool CommitPlan(const Mips64ElfLoadPlan& plan,
                           ia64::PhysicalAddressMap& memory,
                           const std::vector<ia64::PhysicalRamWrite>& additionalWrites = {},
                           std::string* error = nullptr);

    static bool PreparePlan(const Mips64ElfLoadPlan& plan,
                            ia64::PhysicalAddressMap& memory,
                            const std::vector<ia64::PhysicalRamWrite>& additionalWrites,
                            ia64::PreparedPhysicalRamWriteBatch& prepared,
                            std::string* error = nullptr);

    static bool LoadBuffer(const uint8_t* file, size_t fileSize,
                           const Mips64ElfLoadOptions& options,
                           ia64::PhysicalAddressMap& memory,
                           Mips64ElfLoadPlan* loadedPlan = nullptr,
                           const std::vector<ia64::PhysicalRamWrite>& additionalWrites = {},
                           std::string* error = nullptr);
};

}  // namespace mips64
