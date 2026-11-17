#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace riscv {

class RV64CPU;

struct RiscvInitramfsPmpRegion {
    uint64_t lowerBound = 0;
    uint64_t upperBound = 0;
    uint8_t config = 0;
    bool active = false;
};

struct RiscvInitramfsRange {
    std::string_view name;
    uint64_t base = 0;
    uint64_t size = 0;
};

struct RiscvNewcEntry {
    std::string name;
    uint32_t inode = 0;
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t links = 0;
    uint32_t mtime = 0;
    uint32_t rdevMajor = 0;
    uint32_t rdevMinor = 0;
    std::vector<uint8_t> data;
};

enum class RiscvInitramfsError {
    None,
    InvalidArchive,
    InvalidContents,
    InvalidElf,
    InvalidPlacement,
    ArithmeticOverflow,
    OutsideRam,
    RangeOverlap,
    PmpCoverageMissing,
    MemoryAccessFailure,
    AllocationFailure,
};

const char* RiscvInitramfsErrorToString(RiscvInitramfsError error);

struct RiscvInitramfsLoadOptions {
    uint64_t ramBase = 0;
    uint64_t ramSize = 0;
    uint64_t archiveAddress = 0;
    uint64_t requiredAlignment = 0x1000;
    std::string_view expectedArchiveSha256;
    std::span<const RiscvInitramfsRange> protectedRanges{};
    std::span<const RiscvInitramfsPmpRegion> pmpRegions{};
};

struct RiscvInitramfsPlan {
    uint64_t address = 0;
    uint64_t length = 0;
    uint64_t endExclusive = 0;
    uint64_t elfEntry = 0;
    std::string archiveSha256;
    std::vector<RiscvNewcEntry> entries;
};

struct RiscvInitramfsResult {
    RiscvInitramfsError error = RiscvInitramfsError::None;
    std::string detail;
    RiscvInitramfsPlan plan;

    bool ok() const { return error == RiscvInitramfsError::None; }
};

// Validate deterministic uncompressed newc metadata, the exact minimal file
// set, static ELF64/RV64 /init, physical placement, and actual PMP coverage.
// Planning never mutates guest memory or CPU/device state.
RiscvInitramfsResult planRiscvInitramfs(
    std::span<const uint8_t> archive,
    const RiscvInitramfsLoadOptions& options);

// Transactionally copy a validated archive into the selected guest RAM range.
// A rejected plan or failed physical write restores the previous range.
RiscvInitramfsResult loadRiscvInitramfs(
    std::span<const uint8_t> archive, RV64CPU& cpu,
    const RiscvInitramfsLoadOptions& options);

} // namespace riscv
