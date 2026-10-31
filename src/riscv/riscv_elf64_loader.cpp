#include "riscv/riscv_elf64_loader.h"

#include "riscv_cpu.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace riscv {
namespace {

constexpr size_t kElfHeaderSize = 64;
constexpr size_t kProgramHeaderSize = 56;
constexpr size_t kSectionHeaderSize = 64;
constexpr uint16_t kElfTypeExecutable = 2;
constexpr uint16_t kElfMachineRiscv = 243;
constexpr uint16_t kExtendedProgramHeaderCount = 0xFFFF;
constexpr uint32_t kPtNull = 0;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtInterp = 3;
constexpr uint32_t kPtNote = 4;
constexpr uint32_t kPtShlib = 5;
constexpr uint32_t kPtPhdr = 6;
constexpr uint32_t kPtTls = 7;
constexpr uint32_t kPtGnuEhFrame = 0x6474E550U;
constexpr uint32_t kPtGnuStack = 0x6474E551U;
constexpr uint32_t kPtGnuRelro = 0x6474E552U;
constexpr uint32_t kPtRiscvAttributes = 0x70000003U;
constexpr uint32_t kPfExecute = 1U;
constexpr uint32_t kPfWrite = 2U;
constexpr uint32_t kPfRead = 4U;
constexpr uint32_t kPfMask = kPfExecute | kPfWrite | kPfRead;
constexpr uint64_t kDtNull = 0;
constexpr uint64_t kDtNeeded = 1;
constexpr uint64_t kDtPltRelSize = 2;
constexpr uint64_t kDtRel = 17;
constexpr uint64_t kDtRelSize = 18;
constexpr uint64_t kDtRelEntry = 19;
constexpr uint64_t kDtTextRel = 22;
constexpr uint64_t kDtJmpRel = 23;
constexpr uint64_t kDtRela = 7;
constexpr uint64_t kDtRelaSize = 8;
constexpr uint64_t kDtRelaEntry = 9;

bool contains(uint64_t start, uint64_t length, uint64_t address, uint64_t size) {
    if (address < start) return false;
    const uint64_t offset = address - start;
    return offset <= length && size <= length - offset;
}

bool fileRangeContains(size_t fileSize, uint64_t offset, uint64_t size) {
    return offset <= static_cast<uint64_t>(fileSize) &&
           size <= static_cast<uint64_t>(fileSize) - offset;
}

bool rangesOverlap(uint64_t left, uint64_t leftSize,
                   uint64_t right, uint64_t rightSize) {
    if (leftSize == 0 || rightSize == 0) return false;
    return left <= right ? right - left < leftSize : left - right < rightSize;
}

bool readLe16(std::span<const uint8_t> bytes, size_t offset, uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = static_cast<uint16_t>(bytes[offset]) |
            static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
    return true;
}

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
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return true;
}

Elf64PlanResult fail(Elf64LoadError error, std::string detail) {
    Elf64PlanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

bool hasRelocationTag(std::span<const uint8_t> file, uint64_t offset,
                      uint64_t size, bool& malformed) {
    malformed = false;
    if (size % 16 != 0 || !fileRangeContains(file.size(), offset, size)) {
        malformed = true;
        return false;
    }

    bool foundNull = false;
    const size_t start = static_cast<size_t>(offset);
    const size_t end = start + static_cast<size_t>(size);
    for (size_t cursor = start; cursor < end; cursor += 16) {
        uint64_t tag = 0;
        uint64_t value = 0;
        if (!readLe64(file, cursor, tag) || !readLe64(file, cursor + 8, value)) {
            malformed = true;
            return false;
        }
        (void)value;
        if (tag == kDtNull) {
            foundNull = true;
            break;
        }
        if (tag == kDtRel || tag == kDtRelSize || tag == kDtRelEntry ||
            tag == kDtRela || tag == kDtRelaSize || tag == kDtRelaEntry ||
            tag == kDtJmpRel || tag == kDtPltRelSize || tag == kDtTextRel) {
            return true;
        }
    }
    malformed = !foundNull;
    return false;
}

bool hasDynamicRequirement(std::span<const uint8_t> file, uint64_t offset,
                           uint64_t size, bool& malformed) {
    malformed = false;
    if (size % 16 != 0 || !fileRangeContains(file.size(), offset, size)) {
        malformed = true;
        return false;
    }

    bool foundNull = false;
    const size_t start = static_cast<size_t>(offset);
    const size_t end = start + static_cast<size_t>(size);
    for (size_t cursor = start; cursor < end; cursor += 16) {
        uint64_t tag = 0;
        uint64_t value = 0;
        if (!readLe64(file, cursor, tag) || !readLe64(file, cursor + 8, value)) {
            malformed = true;
            return false;
        }
        (void)value;
        if (tag == kDtNull) {
            foundNull = true;
            break;
        }
        if (tag == kDtNeeded) return true;
    }
    malformed = !foundNull;
    return false;
}

} // namespace

const char* Elf64LoadErrorToString(Elf64LoadError error) {
    switch (error) {
        case Elf64LoadError::None: return "no error";
        case Elf64LoadError::InvalidElfFormat: return "invalid ELF format";
        case Elf64LoadError::UnsupportedArchitecture: return "unsupported architecture";
        case Elf64LoadError::UnsupportedAbi: return "unsupported ABI";
        case Elf64LoadError::UnsupportedExecutableType: return "unsupported executable type";
        case Elf64LoadError::MalformedProgramHeaders: return "malformed program headers";
        case Elf64LoadError::InvalidSegmentBounds: return "invalid segment bounds";
        case Elf64LoadError::OverlappingSegments: return "overlapping load segments";
        case Elf64LoadError::InsufficientGuestMemory: return "insufficient guest memory";
        case Elf64LoadError::ReservedRegionCollision: return "reserved-region collision";
        case Elf64LoadError::UnsupportedRuntimeLinking:
            return "unsupported runtime-linking requirements";
        case Elf64LoadError::UnsupportedRelocations: return "unsupported relocations";
        case Elf64LoadError::InvalidEntrypoint: return "invalid entrypoint";
        case Elf64LoadError::ResourceLimitExceeded: return "ELF resource limit exceeded";
        case Elf64LoadError::GuestMemoryAccessFailure: return "guest-memory access failure";
    }
    return "unknown ELF loader error";
}

Elf64PlanResult planRiscvElf64(std::span<const uint8_t> file,
                               const Elf64LoadOptions& options) {
    try {
        if (file.size() < kElfHeaderSize) {
            return fail(Elf64LoadError::InvalidElfFormat, "truncated ELF64 header");
        }
        if (file.size() > options.maxFileBytes) {
            return fail(Elf64LoadError::ResourceLimitExceeded,
                        "ELF file exceeds the configured file-size limit");
        }
        if (file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') {
            return fail(Elf64LoadError::InvalidElfFormat, "ELF magic is invalid");
        }
        if (file[4] != 2 || file[5] != 1 || file[6] != 1) {
            return fail(Elf64LoadError::InvalidElfFormat,
                        "ELF class, data encoding, or identification version is unsupported");
        }
        if (file[7] != 0 && file[7] != 3) {
            return fail(Elf64LoadError::UnsupportedAbi,
                        "only System V and GNU ELF OS/ABI values are accepted");
        }
        if (file[8] != 0) {
            return fail(Elf64LoadError::UnsupportedAbi,
                        "nonzero ELF ABI version is unsupported");
        }
        for (size_t i = 9; i < 16; ++i) {
            if (file[i] != 0) {
                return fail(Elf64LoadError::UnsupportedAbi,
                            "nonzero reserved ELF identification byte");
            }
        }

        uint16_t type = 0;
        uint16_t machine = 0;
        uint32_t version = 0;
        uint64_t entry = 0;
        uint64_t phoff = 0;
        uint64_t shoff = 0;
        uint32_t flags = 0;
        uint16_t ehsize = 0;
        uint16_t phentsize = 0;
        uint16_t phnum = 0;
        uint16_t shentsize = 0;
        uint16_t shnum = 0;
        if (!readLe16(file, 16, type) || !readLe16(file, 18, machine) ||
            !readLe32(file, 20, version) || !readLe64(file, 24, entry) ||
            !readLe64(file, 32, phoff) || !readLe64(file, 40, shoff) ||
            !readLe32(file, 48, flags) || !readLe16(file, 52, ehsize) ||
            !readLe16(file, 54, phentsize) || !readLe16(file, 56, phnum) ||
            !readLe16(file, 58, shentsize) || !readLe16(file, 60, shnum)) {
            return fail(Elf64LoadError::InvalidElfFormat, "truncated ELF header fields");
        }
        if (version != 1 || ehsize != kElfHeaderSize) {
            return fail(Elf64LoadError::InvalidElfFormat,
                        "ELF header version or size does not match ELF64");
        }
        if (type != kElfTypeExecutable) {
            return fail(Elf64LoadError::UnsupportedExecutableType,
                        "only fixed-address ET_EXEC files are supported");
        }
        if (machine != kElfMachineRiscv) {
            return fail(Elf64LoadError::UnsupportedArchitecture,
                        "ELF e_machine is not EM_RISCV");
        }
        if (flags != 0) {
            return fail(Elf64LoadError::UnsupportedAbi,
                        "only soft-float RV64IMA flags are supported; RVC, floating-point ABI, RVE, TSO, and unknown flags are rejected");
        }

        if (options.ramSize == 0 ||
            options.ramSize > std::numeric_limits<uint64_t>::max() - options.ramBase) {
            return fail(Elf64LoadError::InvalidSegmentBounds,
                        "configured guest RAM range is empty or overflows");
        }
        const uint64_t ramEnd = options.ramBase + options.ramSize;
        for (const auto& region : options.reservedRegions) {
            if (region.size == 0 ||
                region.size > std::numeric_limits<uint64_t>::max() - region.base ||
                region.base < options.ramBase || region.base + region.size > ramEnd) {
                return fail(Elf64LoadError::InvalidSegmentBounds,
                            "configured reserved region lies outside guest RAM");
            }
        }

        if (phnum == kExtendedProgramHeaderCount || phnum == 0) {
            return fail(Elf64LoadError::MalformedProgramHeaders,
                        "extended or empty program-header numbering is unsupported");
        }
        if (phnum > options.maxProgramHeaders || phentsize != kProgramHeaderSize ||
            phoff < kElfHeaderSize) {
            return fail(Elf64LoadError::MalformedProgramHeaders,
                        "program-header count, offset, or entry size is invalid");
        }
        const uint64_t phTableSize = static_cast<uint64_t>(phnum) * kProgramHeaderSize;
        if (!fileRangeContains(file.size(), phoff, phTableSize)) {
            return fail(Elf64LoadError::MalformedProgramHeaders,
                        "program-header table extends beyond the file");
        }

        // Section headers are not used to determine load placement. If present,
        // their table extent is still checked so no referenced table is trusted
        // outside the input file.
        if (shoff == 0 && shnum != 0) {
            return fail(Elf64LoadError::InvalidElfFormat,
                        "section count is nonzero but the section table is absent");
        }
        if (shoff != 0) {
            if (shentsize != kSectionHeaderSize ||
                !fileRangeContains(file.size(), shoff,
                                  shnum == 0 ? kSectionHeaderSize
                                             : static_cast<uint64_t>(shnum) * kSectionHeaderSize)) {
                return fail(Elf64LoadError::InvalidElfFormat,
                            "referenced section-header table is malformed");
            }
        } else if (shentsize != 0 && shentsize != kSectionHeaderSize) {
            return fail(Elf64LoadError::InvalidElfFormat,
                        "section-header entry size is invalid");
        }

        Elf64PlanResult result;
        result.plan.entryPoint = entry;
        result.plan.flags = flags;
        result.plan.programHeaderCount = phnum;
        result.plan.segments.reserve(std::min<uint16_t>(phnum, options.maxLoadSegments));

        bool entryInFileBackedExecutable = false;
        for (uint16_t i = 0; i < phnum; ++i) {
            const size_t at = static_cast<size_t>(phoff) +
                              static_cast<size_t>(i) * kProgramHeaderSize;
            uint32_t pType = 0;
            uint32_t pFlags = 0;
            uint64_t pOffset = 0;
            uint64_t pVaddr = 0;
            uint64_t pPaddr = 0;
            uint64_t pFilesz = 0;
            uint64_t pMemsz = 0;
            uint64_t pAlign = 0;
            if (!readLe32(file, at, pType) || !readLe32(file, at + 4, pFlags) ||
                !readLe64(file, at + 8, pOffset) || !readLe64(file, at + 16, pVaddr) ||
                !readLe64(file, at + 24, pPaddr) || !readLe64(file, at + 32, pFilesz) ||
                !readLe64(file, at + 40, pMemsz) || !readLe64(file, at + 48, pAlign)) {
                return fail(Elf64LoadError::MalformedProgramHeaders,
                            "program-header entry is truncated");
            }

            if (!fileRangeContains(file.size(), pOffset, pFilesz)) {
                return fail(Elf64LoadError::InvalidSegmentBounds,
                            "program-header file range extends beyond the ELF file");
            }

            switch (pType) {
                case kPtNull:
                case kPtNote:
                case kPtPhdr:
                case kPtGnuEhFrame:
                case kPtGnuRelro:
                case kPtRiscvAttributes:
                    continue;
                case kPtInterp:
                    return fail(Elf64LoadError::UnsupportedRuntimeLinking,
                                "PT_INTERP requires an unsupported runtime interpreter");
                case kPtTls:
                case kPtShlib:
                    return fail(Elf64LoadError::UnsupportedRuntimeLinking,
                                "PT_TLS and PT_SHLIB are outside the static guest profile");
                case kPtDynamic: {
                    bool malformed = false;
                    if (hasRelocationTag(file, pOffset, pFilesz, malformed)) {
                        return fail(Elf64LoadError::UnsupportedRelocations,
                                    "PT_DYNAMIC contains runtime relocation requirements");
                    }
                    if (malformed) {
                        return fail(Elf64LoadError::MalformedProgramHeaders,
                                    "PT_DYNAMIC table is malformed or has no DT_NULL terminator");
                    }
                    bool hasDependency = false;
                    hasDependency = hasDynamicRequirement(file, pOffset, pFilesz, malformed);
                    if (malformed) {
                        return fail(Elf64LoadError::MalformedProgramHeaders,
                                    "PT_DYNAMIC table is malformed or has no DT_NULL terminator");
                    }
                    return fail(Elf64LoadError::UnsupportedRuntimeLinking,
                                hasDependency
                                    ? "PT_DYNAMIC contains a DT_NEEDED dependency"
                                    : "PT_DYNAMIC requires an unsupported runtime linker");
                }
                case kPtGnuStack:
                    if ((pFlags & kPfExecute) != 0) {
                        return fail(Elf64LoadError::UnsupportedAbi,
                                    "executable guest stacks are not supported");
                    }
                    continue;
                case kPtLoad:
                    break;
                default:
                    return fail(Elf64LoadError::UnsupportedRuntimeLinking,
                                "unknown program-header type may require runtime processing");
            }

            if ((pFlags & ~kPfMask) != 0) {
                return fail(Elf64LoadError::UnsupportedAbi,
                            "PT_LOAD contains unknown permission flags");
            }
            if (pFilesz > pMemsz || pMemsz == 0) {
                return fail(Elf64LoadError::InvalidSegmentBounds,
                            "PT_LOAD has p_filesz greater than p_memsz or an empty memory range");
            }
            if (pMemsz > std::numeric_limits<uint64_t>::max() - pVaddr) {
                return fail(Elf64LoadError::InvalidSegmentBounds,
                            "PT_LOAD virtual address range overflows");
            }
            if (pPaddr != pVaddr) {
                return fail(Elf64LoadError::InvalidSegmentBounds,
                            "identity loading requires p_paddr to equal p_vaddr");
            }
            if (pAlign > 1) {
                if ((pAlign & (pAlign - 1)) != 0 ||
                    (pVaddr % pAlign) != (pOffset % pAlign)) {
                    return fail(Elf64LoadError::InvalidSegmentBounds,
                                "PT_LOAD alignment is not a power of two or violates offset/address congruence");
                }
            }
            if (pVaddr < options.ramBase || pVaddr > ramEnd ||
                pMemsz > ramEnd - pVaddr) {
                return fail(Elf64LoadError::InsufficientGuestMemory,
                            "PT_LOAD destination lies outside authorized guest RAM");
            }
            for (const auto& region : options.reservedRegions) {
                if (rangesOverlap(pVaddr, pMemsz, region.base, region.size)) {
                    std::string name = region.name != nullptr ? region.name : "reserved";
                    return fail(Elf64LoadError::ReservedRegionCollision,
                                "PT_LOAD collides with reserved region " + name);
                }
            }
            if (result.plan.segments.size() >= options.maxLoadSegments) {
                return fail(Elf64LoadError::ResourceLimitExceeded,
                            "PT_LOAD segment count exceeds the configured limit");
            }
            if (pMemsz > options.maxLoadBytes -
                             std::min(result.plan.totalMemoryBytes, options.maxLoadBytes)) {
                return fail(Elf64LoadError::ResourceLimitExceeded,
                            "total PT_LOAD memory exceeds the configured limit");
            }
            for (const auto& previous : result.plan.segments) {
                if (rangesOverlap(pVaddr, pMemsz,
                                  previous.virtualAddress, previous.memorySize)) {
                    return fail(Elf64LoadError::OverlappingSegments,
                                "PT_LOAD memory ranges overlap");
                }
            }

            Elf64LoadSegment segment;
            segment.fileOffset = pOffset;
            segment.virtualAddress = pVaddr;
            segment.physicalAddress = pPaddr;
            segment.fileSize = pFilesz;
            segment.memorySize = pMemsz;
            segment.alignment = pAlign;
            segment.flags = pFlags;
            result.plan.totalMemoryBytes += pMemsz;
            if ((pFlags & kPfExecute) != 0 && entry >= pVaddr &&
                entry - pVaddr < pFilesz) {
                entryInFileBackedExecutable = true;
            }
            result.plan.segments.push_back(segment);
        }

        if (result.plan.segments.empty()) {
            return fail(Elf64LoadError::MalformedProgramHeaders,
                        "ELF contains no usable PT_LOAD segment");
        }
        if (!entryInFileBackedExecutable || (entry & 3ULL) != 0) {
            return fail(Elf64LoadError::InvalidEntrypoint,
                        "ELF entry is not 4-byte aligned inside file-backed executable PT_LOAD data");
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(Elf64LoadError::ResourceLimitExceeded,
                    "allocation failed while constructing the bounded load plan");
    }
}

Elf64LoadResult loadRiscvElf64(std::span<const uint8_t> file,
                               RV64CPU& cpu,
                               const Elf64LoadOptions& options) {
    Elf64LoadResult result;
    Elf64PlanResult planned = planRiscvElf64(file, options);
    if (!planned.ok()) {
        result.error = planned.error;
        result.detail = std::move(planned.detail);
        return result;
    }
    if (cpu.memoryBase() != options.ramBase ||
        cpu.memorySize() < options.ramSize) {
        result.error = Elf64LoadError::InsufficientGuestMemory;
        result.detail = "configured guest RAM does not match the CPU memory region";
        return result;
    }
    const MachineTimerMapping& timer = cpu.machineTimerMapping();
    for (const auto& segment : planned.plan.segments) {
        if (rangesOverlap(segment.physicalAddress, segment.memorySize,
                          timer.mtimeAddress, 8) ||
            rangesOverlap(segment.physicalAddress, segment.memorySize,
                          timer.mtimecmpAddress, 8)) {
            result.error = Elf64LoadError::ReservedRegionCollision;
            result.detail = "PT_LOAD destination overlaps CPU timer MMIO";
            return result;
        }
    }

    struct StagedSegment {
        uint64_t address = 0;
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> backup;
    };
    std::vector<StagedSegment> staged;
    try {
        staged.reserve(planned.plan.segments.size());
        for (const auto& segment : planned.plan.segments) {
            const size_t memorySize = static_cast<size_t>(segment.memorySize);
            const size_t fileSize = static_cast<size_t>(segment.fileSize);
            const size_t fileOffset = static_cast<size_t>(segment.fileOffset);
            StagedSegment write;
            write.address = segment.physicalAddress;
            write.bytes.assign(memorySize, 0);
            if (fileSize != 0) {
                std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(fileOffset),
                            fileSize, write.bytes.begin());
            }
            write.backup.resize(memorySize);
            if (!cpu.readPhysical(write.address, write.backup.data(), memorySize)) {
                result.error = Elf64LoadError::GuestMemoryAccessFailure;
                result.detail = "could not stage an original PT_LOAD memory range";
                return result;
            }
            staged.push_back(std::move(write));
        }
    } catch (const std::bad_alloc&) {
        result.error = Elf64LoadError::ResourceLimitExceeded;
        result.detail = "allocation failed while staging PT_LOAD data and rollback images";
        return result;
    }

    size_t committed = 0;
    for (const auto& write : staged) {
        if (!cpu.writePhysical(write.address, write.bytes.data(), write.bytes.size())) {
            bool rollbackSucceeded = cpu.writePhysical(write.address, write.backup.data(),
                                                       write.backup.size());
            while (committed != 0) {
                --committed;
                const auto& old = staged[committed];
                rollbackSucceeded &= cpu.writePhysical(old.address, old.backup.data(),
                                                       old.backup.size());
            }
            result.error = Elf64LoadError::GuestMemoryAccessFailure;
            result.detail = rollbackSucceeded
                ? "guest memory rejected a staged write; prior segment writes were rolled back"
                : "guest memory rejected a write and a rollback write";
            return result;
        }
        ++committed;
    }

    result.plan = std::move(planned.plan);
    cpu.reset();
    cpu.setPC(result.plan.entryPoint);
    return result;
}

} // namespace riscv
