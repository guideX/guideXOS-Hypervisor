#include "riscv/riscv_linux_image_loader.h"

#include "riscv_cpu.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace riscv {
namespace {

constexpr size_t kHeaderSize = 64;
constexpr uint32_t kHeaderVersion02 = 0x00000002U;
constexpr uint64_t kDeprecatedMagic = 0x0000005643534952ULL;
constexpr uint32_t kMagic2 = 0x05435352U;
constexpr uint8_t kPmpRead = 1U;
constexpr uint8_t kPmpWrite = 2U;
constexpr uint8_t kPmpExecute = 4U;
constexpr uint8_t kPmpRequiredRwx = kPmpRead | kPmpWrite | kPmpExecute;

bool readLe32(std::span<const uint8_t> bytes, size_t offset, uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    return true;
}

bool readLe64(std::span<const uint8_t> bytes, size_t offset, uint64_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8) return false;
    value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return true;
}

bool checkedEnd(uint64_t base, uint64_t size, uint64_t& end) {
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - base) return false;
    end = base + size;
    return true;
}

bool isAligned(uint64_t value, uint64_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1ULL)) == 0 &&
           (value & (alignment - 1ULL)) == 0;
}

bool isContained(uint64_t outerBase, uint64_t outerSize,
                 uint64_t innerBase, uint64_t innerSize) {
    uint64_t outerEnd = 0;
    uint64_t innerEnd = 0;
    return checkedEnd(outerBase, outerSize, outerEnd) &&
           checkedEnd(innerBase, innerSize, innerEnd) &&
           innerBase >= outerBase && innerEnd <= outerEnd;
}

bool overlaps(uint64_t leftBase, uint64_t leftSize,
              uint64_t rightBase, uint64_t rightSize) {
    uint64_t leftEnd = 0;
    uint64_t rightEnd = 0;
    if (!checkedEnd(leftBase, leftSize, leftEnd) ||
        !checkedEnd(rightBase, rightSize, rightEnd)) return true;
    return leftBase < rightEnd && rightBase < leftEnd;
}

RiscvLinuxImagePlanResult fail(RiscvLinuxImageError error, std::string detail) {
    RiscvLinuxImagePlanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

bool pmpAllows(std::span<const RiscvLinuxPmpRegion> regions,
               uint64_t base, uint64_t size, uint8_t permissions) {
    uint64_t end = 0;
    if (!checkedEnd(base, size, end) || regions.empty()) return false;
    for (const auto& region : regions) {
        if (region.active && region.upperBound <= region.lowerBound) return false;
    }

    uint64_t cursor = base;
    while (cursor < end) {
        const RiscvLinuxPmpRegion* firstMatch = nullptr;
        for (const auto& region : regions) {
            if (!region.active || cursor < region.lowerBound ||
                cursor >= region.upperBound) continue;
            if (firstMatch == nullptr || region.index < firstMatch->index) {
                firstMatch = &region;
            }
        }
        if (firstMatch == nullptr ||
            (firstMatch->config & permissions) != permissions) return false;

        uint64_t next = end;
        for (const auto& region : regions) {
            if (!region.active) continue;
            if (region.lowerBound > cursor && region.lowerBound < next) {
                next = region.lowerBound;
            }
            if (region.upperBound > cursor && region.upperBound < next) {
                next = region.upperBound;
            }
        }
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool parseHeader(std::span<const uint8_t> file, RiscvLinuxImageHeader& header) {
    return readLe32(file, 0, header.code0) &&
           readLe32(file, 4, header.code1) &&
           readLe64(file, 8, header.textOffset) &&
           readLe64(file, 16, header.imageSize) &&
           readLe64(file, 24, header.flags) &&
           readLe32(file, 32, header.version) &&
           readLe32(file, 36, header.reserved1) &&
           readLe64(file, 40, header.reserved2) &&
           readLe64(file, 48, header.magic) &&
           readLe32(file, 56, header.magic2) &&
           readLe32(file, 60, header.reserved3);
}

int64_t jalImmediate(uint32_t instruction) {
    uint32_t immediate = ((instruction >> 31) & 0x1U) << 20;
    immediate |= ((instruction >> 21) & 0x3FFU) << 1;
    immediate |= ((instruction >> 20) & 0x1U) << 11;
    immediate |= ((instruction >> 12) & 0xFFU) << 12;
    if ((immediate & (1U << 20)) != 0) immediate |= 0xFFE00000U;
    return static_cast<int32_t>(immediate);
}

} // namespace

const char* RiscvLinuxImageErrorToString(RiscvLinuxImageError error) {
    switch (error) {
        case RiscvLinuxImageError::None: return "no error";
        case RiscvLinuxImageError::TruncatedHeader: return "truncated Linux Image header";
        case RiscvLinuxImageError::FileTooLarge: return "Linux Image file exceeds configured limit";
        case RiscvLinuxImageError::BadMagic: return "invalid Linux Image magic";
        case RiscvLinuxImageError::UnsupportedVersion: return "unsupported Linux Image header version";
        case RiscvLinuxImageError::UnsupportedEndianness: return "unsupported big-endian Linux Image";
        case RiscvLinuxImageError::UnsupportedFlags: return "unsupported Linux Image header flags";
        case RiscvLinuxImageError::NonzeroReservedField: return "nonzero reserved Linux Image header field";
        case RiscvLinuxImageError::InvalidExecutableHeader: return "invalid uncompressed Linux executable header words";
        case RiscvLinuxImageError::EntryOutsideImage: return "Linux Image initial branch target is outside its effective footprint";
        case RiscvLinuxImageError::InvalidImageSize: return "zero or inconsistent Linux Image size";
        case RiscvLinuxImageError::FileExceedsImageFootprint: return "file-backed bytes exceed the effective Image footprint";
        case RiscvLinuxImageError::InvalidRamRange: return "invalid guest RAM range";
        case RiscvLinuxImageError::ArithmeticOverflow: return "Linux Image placement arithmetic overflow";
        case RiscvLinuxImageError::UnsupportedTextOffset: return "Linux Image text_offset does not match the RV64 S-mode profile";
        case RiscvLinuxImageError::MisalignedLoadAddress: return "Linux Image load address is not PMD aligned";
        case RiscvLinuxImageError::ImageOutsideRam: return "effective Linux Image footprint does not fit guest RAM";
        case RiscvLinuxImageError::FirmwareJumpMismatch: return "OpenSBI FW_JUMP target does not match the Linux entrypoint";
        case RiscvLinuxImageError::FirmwareOverlap: return "Linux Image overlaps resident firmware memory";
        case RiscvLinuxImageError::InvalidFdt: return "Linux boot FDT is invalid";
        case RiscvLinuxImageError::InvalidBootHart: return "Linux boot FDT does not enable the expected boot hart";
        case RiscvLinuxImageError::FdtCapacityInsufficient: return "reserved FDT capacity is insufficient";
        case RiscvLinuxImageError::FdtOutsideRam: return "FDT reservation is outside guest RAM";
        case RiscvLinuxImageError::FdtOverlap: return "Linux Image overlaps the active FDT reservation";
        case RiscvLinuxImageError::InvalidReservation: return "invalid Linux boot memory reservation";
        case RiscvLinuxImageError::ReservationOverlap: return "Linux boot memory reservations overlap";
        case RiscvLinuxImageError::ImageReservationConflict: return "Linux Image overlaps an active memory reservation";
        case RiscvLinuxImageError::PmpCoverageMissing: return "effective PMP entries do not grant required S-mode access";
        case RiscvLinuxImageError::CpuMemoryMismatch: return "configured RAM does not match the production CPU memory";
        case RiscvLinuxImageError::AllocationFailure: return "allocation failed while staging the Linux Image transaction";
        case RiscvLinuxImageError::GuestMemoryAccessFailure: return "guest memory access failed during Linux Image transaction";
    }
    return "unknown Linux Image error";
}

RiscvLinuxImagePlanResult planRiscvLinuxImage(
    std::span<const uint8_t> file, const RiscvLinuxImageLoadOptions& options) {
    if (file.size() > options.maxFileBytes) {
        return fail(RiscvLinuxImageError::FileTooLarge,
                    "file length exceeds maxFileBytes");
    }
    if (file.size() < kHeaderSize) {
        return fail(RiscvLinuxImageError::TruncatedHeader,
                    "raw Image does not contain the complete 64-byte header");
    }

    RiscvLinuxImagePlanResult result;
    if (!parseHeader(file, result.plan.header)) {
        return fail(RiscvLinuxImageError::TruncatedHeader,
                    "one or more Linux Image header fields are truncated");
    }
    const auto& header = result.plan.header;

    if (header.magic2 != kMagic2 ||
        (header.magic != 0 && header.magic != kDeprecatedMagic)) {
        return fail(RiscvLinuxImageError::BadMagic,
                    "magic2 must be RSC\\x05; deprecated magic may be zero or RISCV");
    }
    if (header.version != kHeaderVersion02) {
        return fail(RiscvLinuxImageError::UnsupportedVersion,
                    "only Linux RISC-V image header version 0.2 is supported");
    }
    if ((header.flags & 1ULL) != 0) {
        return fail(RiscvLinuxImageError::UnsupportedEndianness,
                    "header flags select big-endian execution");
    }
    if (header.flags != 0) {
        return fail(RiscvLinuxImageError::UnsupportedFlags,
                    "unsupported flag bits are set");
    }
    if (header.reserved1 != 0 || header.reserved2 != 0 || header.reserved3 != 0) {
        return fail(RiscvLinuxImageError::NonzeroReservedField,
                    "non-EFI v0.2 reserved fields must be zero");
    }
    if ((header.code0 & 0x7FU) != 0x6FU ||
        ((header.code0 >> 7) & 0x1FU) != 0 || header.code1 != 0) {
        return fail(RiscvLinuxImageError::InvalidExecutableHeader,
                    "code0 must be an uncompressed JAL x0 entry branch and code1 must be zero");
    }
    if (header.imageSize == 0 || header.imageSize < kHeaderSize) {
        return fail(RiscvLinuxImageError::InvalidImageSize,
                    "image_size must include the complete header and executable entry word");
    }
    if (file.size() > header.imageSize) {
        return fail(RiscvLinuxImageError::FileExceedsImageFootprint,
                    "file-backed Image bytes extend past image_size");
    }

    uint64_t ramEnd = 0;
    if (!checkedEnd(options.ramBase, options.ramSize, ramEnd)) {
        return fail(RiscvLinuxImageError::InvalidRamRange,
                    "RAM is empty or its half-open end overflows");
    }
    if (header.textOffset != options.requiredTextOffset) {
        return fail(RiscvLinuxImageError::UnsupportedTextOffset,
                    "header text_offset differs from the selected RV64 S-mode image profile");
    }
    if (header.textOffset >
        std::numeric_limits<uint64_t>::max() - options.ramBase) {
        return fail(RiscvLinuxImageError::ArithmeticOverflow,
                    "Linux Image text offset overflows the physical load address");
    }
    if (!isAligned(options.ramBase, 0x00200000ULL) ||
        !isAligned(header.textOffset, 0x00200000ULL)) {
        return fail(RiscvLinuxImageError::MisalignedLoadAddress,
                    "RAM base and text_offset must produce a 2 MiB aligned address");
    }

    result.plan.fileSize = static_cast<uint64_t>(file.size());
    result.plan.effectiveSize = header.imageSize;
    result.plan.loadAddress = options.ramBase + header.textOffset;
    result.plan.entryPoint = result.plan.loadAddress;
    const int64_t initialBranchOffset = jalImmediate(header.code0);
    if (initialBranchOffset < static_cast<int64_t>(kHeaderSize) ||
        static_cast<uint64_t>(initialBranchOffset) >= header.imageSize) {
        return fail(RiscvLinuxImageError::EntryOutsideImage,
                    "the Linux header JAL does not enter the effective loaded image body");
    }
    if (static_cast<uint64_t>(initialBranchOffset) >
        std::numeric_limits<uint64_t>::max() - result.plan.loadAddress) {
        return fail(RiscvLinuxImageError::ArithmeticOverflow,
                    "Linux header branch target arithmetic overflows");
    }
    result.plan.initialBranchTarget = result.plan.loadAddress +
        static_cast<uint64_t>(initialBranchOffset);
    uint64_t imageEnd = 0;
    if (!checkedEnd(result.plan.loadAddress, result.plan.effectiveSize, imageEnd)) {
        return fail(RiscvLinuxImageError::ArithmeticOverflow,
                    "effective Image end address overflows");
    }
    if (!isAligned(result.plan.loadAddress, 0x00200000ULL)) {
        return fail(RiscvLinuxImageError::MisalignedLoadAddress,
                    "Linux RV64 Image must begin at a PMD boundary");
    }
    if (result.plan.loadAddress < options.ramBase || imageEnd > ramEnd) {
        return fail(RiscvLinuxImageError::ImageOutsideRam,
                    "effective image_size extent is not contained in guest RAM");
    }

    uint64_t firmwareEnd = 0;
    if (!checkedEnd(options.residentFirmwareBase, options.residentFirmwareSize, firmwareEnd) ||
        options.residentFirmwareBase < options.ramBase || firmwareEnd > ramEnd) {
        return fail(RiscvLinuxImageError::InvalidRamRange,
                    "resident firmware range is empty or outside guest RAM");
    }
    if (overlaps(result.plan.loadAddress, result.plan.effectiveSize,
                 options.residentFirmwareBase, options.residentFirmwareSize)) {
        return fail(RiscvLinuxImageError::FirmwareOverlap,
                    "effective Image extent intersects resident OpenSBI memory");
    }
    if (options.firmwareJumpAddress != result.plan.entryPoint) {
        return fail(RiscvLinuxImageError::FirmwareJumpMismatch,
                    "validated OpenSBI FW_JUMP target differs from the Linux entry");
    }

    if (!options.fdtValid) {
        return fail(RiscvLinuxImageError::InvalidFdt,
                    "FDT header or selected Linux platform properties failed validation");
    }
    if (!options.bootHartEnabled || options.bootHartId != options.expectedBootHartId) {
        return fail(RiscvLinuxImageError::InvalidBootHart,
                    "the selected FDT boot hart is disabled or has the wrong hart ID");
    }
    if (options.fdtCapacity < options.minimumFdtCapacity ||
        options.fdtActualBytes == 0 ||
        options.fdtActualBytes > options.fdtCapacity) {
        return fail(RiscvLinuxImageError::FdtCapacityInsufficient,
                    "reserved FDT capacity does not cover the required growth and source bytes");
    }
    uint64_t fdtEnd = 0;
    if (!checkedEnd(options.fdtAddress, options.fdtCapacity, fdtEnd) ||
        !isAligned(options.fdtAddress, 8) ||
        options.fdtAddress < options.ramBase || fdtEnd > ramEnd) {
        return fail(RiscvLinuxImageError::FdtOutsideRam,
                    "FDT reservation is unaligned or outside guest RAM");
    }
    if (overlaps(result.plan.loadAddress, result.plan.effectiveSize,
                 options.fdtAddress, options.fdtCapacity)) {
        return fail(RiscvLinuxImageError::FdtOverlap,
                    "effective Image extent intersects the complete FDT reservation");
    }

    bool fdtCoveredByReservation = false;
    for (size_t left = 0; left < options.reservedRanges.size(); ++left) {
        const auto& range = options.reservedRanges[left];
        if (range.size == 0 || range.base < options.ramBase ||
            !isAligned(range.base, range.alignment) ||
            !isContained(options.ramBase, options.ramSize, range.base, range.size)) {
            return fail(RiscvLinuxImageError::InvalidReservation,
                        "a reservation is empty, misaligned, overflowing, or outside guest RAM");
        }
        fdtCoveredByReservation = fdtCoveredByReservation ||
            isContained(range.base, range.size, options.fdtAddress, options.fdtCapacity);
        if (overlaps(result.plan.loadAddress, result.plan.effectiveSize,
                     range.base, range.size)) {
            return fail(RiscvLinuxImageError::ImageReservationConflict,
                        std::string("effective Image extent intersects reservation ") +
                            std::string(range.name));
        }
        for (size_t right = left + 1; right < options.reservedRanges.size(); ++right) {
            const auto& other = options.reservedRanges[right];
            if (overlaps(range.base, range.size, other.base, other.size)) {
                return fail(RiscvLinuxImageError::ReservationOverlap,
                            "active Linux profile reservations overlap each other");
            }
        }
    }
    if (!fdtCoveredByReservation) {
        return fail(RiscvLinuxImageError::InvalidReservation,
                    "the full post-fixup FDT capacity is not in a reserved range");
    }

    if (!pmpAllows(options.pmpRegions, result.plan.loadAddress,
                   result.plan.effectiveSize, kPmpRequiredRwx) ||
        !pmpAllows(options.pmpRegions, options.fdtAddress,
                   options.fdtCapacity, kPmpRead)) {
        return fail(RiscvLinuxImageError::PmpCoverageMissing,
                    "first-match PMP policy must allow S-mode RWX for the Image and read for the FDT");
    }
    return result;
}

RiscvLinuxImageLoadResult loadRiscvLinuxImage(
    std::span<const uint8_t> file, RV64CPU& cpu,
    const RiscvLinuxImageLoadOptions& options) {
    RiscvLinuxImageLoadResult result;
    const auto planned = planRiscvLinuxImage(file, options);
    if (!planned.ok()) {
        result.error = planned.error;
        result.detail = planned.detail;
        return result;
    }
    if (cpu.memoryBase() != options.ramBase ||
        cpu.memorySize() < options.ramSize ||
        planned.plan.effectiveSize > std::numeric_limits<size_t>::max()) {
        result.error = RiscvLinuxImageError::CpuMemoryMismatch;
        result.detail = "CPU physical memory does not contain the complete configured RAM range";
        return result;
    }

    std::vector<uint8_t> staged;
    std::vector<uint8_t> previous;
    try {
        const size_t effectiveSize = static_cast<size_t>(planned.plan.effectiveSize);
        staged.assign(effectiveSize, 0);
        std::copy(file.begin(), file.end(), staged.begin());
        previous.resize(effectiveSize);
    } catch (const std::bad_alloc&) {
        result.error = RiscvLinuxImageError::AllocationFailure;
        result.detail = "could not allocate the full zero-filled Image and rollback buffer";
        return result;
    }

    if (!cpu.readPhysical(planned.plan.loadAddress, previous.data(), previous.size())) {
        result.error = RiscvLinuxImageError::GuestMemoryAccessFailure;
        result.detail = "could not stage the original effective Image memory range";
        return result;
    }
    if (!cpu.writePhysical(planned.plan.loadAddress, staged.data(), staged.size())) {
        const bool rollbackSucceeded = cpu.writePhysical(
            planned.plan.loadAddress, previous.data(), previous.size());
        result.error = RiscvLinuxImageError::GuestMemoryAccessFailure;
        result.detail = rollbackSucceeded
            ? "guest memory rejected the Image write and the original range was restored"
            : "guest memory rejected the Image write and rollback also failed";
        return result;
    }

    result.plan = planned.plan;
    return result;
}

} // namespace riscv
