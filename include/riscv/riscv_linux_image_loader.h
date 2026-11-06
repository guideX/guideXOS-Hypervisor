#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace riscv {

class RV64CPU;

// Linux v6.12 arch/riscv/include/asm/image.h uses this exact 64-byte layout.
struct RiscvLinuxImageHeader {
    uint32_t code0 = 0;
    uint32_t code1 = 0;
    uint64_t textOffset = 0;
    uint64_t imageSize = 0;
    uint64_t flags = 0;
    uint32_t version = 0;
    uint32_t reserved1 = 0;
    uint64_t reserved2 = 0;
    uint64_t magic = 0;
    uint32_t magic2 = 0;
    uint32_t reserved3 = 0;
};
static_assert(sizeof(RiscvLinuxImageHeader) == 64);

struct RiscvLinuxReservedRange {
    std::string_view name;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t alignment = 1;
};

// A PMP entry as observed from the production CPU or derived from the checked
// Linux boot profile. lowerBound/upperBound are half-open physical addresses.
struct RiscvLinuxPmpRegion {
    size_t index = 0;
    uint64_t lowerBound = 0;
    uint64_t upperBound = 0;
    uint8_t config = 0;
    bool active = false;
};

enum class RiscvLinuxImageError {
    None,
    TruncatedHeader,
    FileTooLarge,
    BadMagic,
    UnsupportedVersion,
    UnsupportedEndianness,
    UnsupportedFlags,
    NonzeroReservedField,
    InvalidExecutableHeader,
    EntryOutsideImage,
    InvalidImageSize,
    FileExceedsImageFootprint,
    InvalidRamRange,
    ArithmeticOverflow,
    UnsupportedTextOffset,
    MisalignedLoadAddress,
    ImageOutsideRam,
    FirmwareJumpMismatch,
    FirmwareOverlap,
    InvalidFdt,
    InvalidBootHart,
    FdtCapacityInsufficient,
    FdtOutsideRam,
    FdtOverlap,
    InvalidReservation,
    ReservationOverlap,
    ImageReservationConflict,
    PmpCoverageMissing,
    CpuMemoryMismatch,
    AllocationFailure,
    GuestMemoryAccessFailure,
};

const char* RiscvLinuxImageErrorToString(RiscvLinuxImageError error);

struct RiscvLinuxImageLoadOptions {
    uint64_t ramBase = 0;
    uint64_t ramSize = 0;
    uint64_t requiredTextOffset = 0x00200000ULL;

    uint64_t residentFirmwareBase = 0;
    uint64_t residentFirmwareSize = 0;
    uint64_t firmwareJumpAddress = 0;

    uint64_t fdtAddress = 0;
    uint64_t fdtCapacity = 0;
    uint64_t minimumFdtCapacity = 0;
    uint64_t fdtActualBytes = 0;
    bool fdtValid = false;
    bool bootHartEnabled = false;
    uint64_t bootHartId = 0;
    uint64_t expectedBootHartId = 0;

    std::span<const RiscvLinuxReservedRange> reservedRanges{};
    std::span<const RiscvLinuxPmpRegion> pmpRegions{};
    size_t maxFileBytes = 64U * 1024U * 1024U;
};

struct RiscvLinuxImageLoadPlan {
    RiscvLinuxImageHeader header;
    uint64_t fileSize = 0;
    uint64_t effectiveSize = 0;
    uint64_t loadAddress = 0;
    uint64_t entryPoint = 0;
    uint64_t initialBranchTarget = 0;
};

struct RiscvLinuxImagePlanResult {
    RiscvLinuxImageError error = RiscvLinuxImageError::None;
    std::string detail;
    RiscvLinuxImageLoadPlan plan;

    bool ok() const { return error == RiscvLinuxImageError::None; }
};

struct RiscvLinuxImageLoadResult {
    RiscvLinuxImageError error = RiscvLinuxImageError::None;
    std::string detail;
    RiscvLinuxImageLoadPlan plan;

    bool ok() const { return error == RiscvLinuxImageError::None; }
};

// Strictly parse and plan the pinned little-endian RV64 Linux Image format.
// This function never modifies guest memory or CPU state.
RiscvLinuxImagePlanResult planRiscvLinuxImage(
    std::span<const uint8_t> file, const RiscvLinuxImageLoadOptions& options);

// Stage file-backed bytes, zero-fill the remaining effective image footprint,
// and save the previous guest bytes before the one contiguous commit. Rejected
// plans or failed memory access leave guest memory and CPU state unchanged.
RiscvLinuxImageLoadResult loadRiscvLinuxImage(
    std::span<const uint8_t> file, RV64CPU& cpu,
    const RiscvLinuxImageLoadOptions& options);

} // namespace riscv
