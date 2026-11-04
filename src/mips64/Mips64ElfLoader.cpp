#include "mips64/Mips64ElfLoader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace mips64 {
namespace {

constexpr size_t kElf64HeaderSize = 64;
constexpr size_t kElf64ProgramHeaderSize = 56;
constexpr size_t kElf64SectionHeaderSize = 64;
constexpr uint16_t kMachineMips = 8;
constexpr uint16_t kTypeExecutable = 2;
constexpr uint32_t kProgramNull = 0;
constexpr uint32_t kProgramLoad = 1;
constexpr uint32_t kProgramDynamic = 2;
constexpr uint32_t kProgramInterp = 3;
constexpr uint32_t kProgramNote = 4;
constexpr uint32_t kProgramPhdr = 6;
constexpr uint32_t kSectionDynamic = 6;
constexpr uint32_t kSectionNoBits = 8;
constexpr uint32_t kSectionRel = 9;
constexpr uint32_t kSectionDynSym = 11;
constexpr uint32_t kSectionRela = 4;
constexpr uint32_t kSectionRelr = 19;
constexpr uint32_t kFlagExecutable = 0x1;
constexpr uint32_t kFlagWritable = 0x2;
constexpr uint32_t kFlagReadable = 0x4;
constexpr uint32_t kMipsArchMask = 0xF0000000;
constexpr uint32_t kMipsArch64R2 = 0x80000000;
constexpr uint32_t kMipsAbiMask = 0x0000F000;
constexpr uint32_t kMipsAbi2 = 0x00000020;
constexpr uint32_t kMipsMachMask = 0x00FF0000;
constexpr uint32_t kMipsAseMask = 0x0F000000;
constexpr uint32_t kMips32BitMode = 0x00000100;
constexpr uint32_t kMipsFp64 = 0x00000200;
constexpr uint32_t kMipsNan2008 = 0x00000400;
constexpr uint32_t kMipsNoReorder = 0x00000001;
constexpr uint32_t kMipsPic = 0x00000002;
constexpr uint32_t kMipsCpic = 0x00000004;
constexpr uint64_t kCompatKsegMask = 0x1FFFFFFFULL;
constexpr uint64_t kPhysicalMask36 = 0x0000000FFFFFFFFFULL;
constexpr uint64_t kXkphysReservedMask = 0x07FFFFF000000000ULL;
constexpr uint64_t kMaximumProgramHeaders = 128;
constexpr uint64_t kMaximumSectionHeaders = 4096;

struct ParsedHeader {
    uint8_t dataEncoding = 0;
    uint8_t osAbi = 0;
    uint8_t abiVersion = 0;
    uint16_t type = 0;
    uint16_t machine = 0;
    uint32_t version = 0;
    uint64_t entry = 0;
    uint64_t programOffset = 0;
    uint64_t sectionOffset = 0;
    uint32_t flags = 0;
    uint16_t headerSize = 0;
    uint16_t programEntrySize = 0;
    uint16_t programCount = 0;
    uint16_t sectionEntrySize = 0;
    uint16_t sectionCount = 0;
};

struct ParsedProgramHeader {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t virtualAddress = 0;
    uint64_t physicalAddress = 0;
    uint64_t fileSize = 0;
    uint64_t memorySize = 0;
    uint64_t alignment = 0;
};

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

uint16_t read16(const uint8_t* bytes, bool little) {
    return little
        ? static_cast<uint16_t>(bytes[0] | (uint16_t(bytes[1]) << 8))
        : static_cast<uint16_t>((uint16_t(bytes[0]) << 8) | bytes[1]);
}

uint32_t read32(const uint8_t* bytes, bool little) {
    uint32_t value = 0;
    if (little) {
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[i]) << (i * 8);
    } else {
        for (unsigned i = 0; i < 4; ++i) value = (value << 8) | bytes[i];
    }
    return value;
}

uint64_t read64(const uint8_t* bytes, bool little) {
    uint64_t value = 0;
    if (little) {
        for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    } else {
        for (unsigned i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    }
    return value;
}

bool rangeFits(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

bool isPowerOfTwo(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool parseHeader(const uint8_t* file, size_t fileSize, ParsedHeader& header,
                 bool& little, std::string* error) {
    if (!file) return fail(error, "ELF buffer is null");
    if (fileSize < kElf64HeaderSize) return fail(error, "truncated ELF64 header");
    if (file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') {
        return fail(error, "invalid ELF magic");
    }
    if (file[4] != 2) return fail(error, "unsupported ELF class; ELF64 is required");
    if (file[5] != 1 && file[5] != 2) {
        return fail(error, "unsupported ELF data encoding");
    }
    little = file[5] == 1;
    if (file[6] != 1) return fail(error, "ELF identification version is not current");

    header.dataEncoding = file[5];
    header.osAbi = file[7];
    header.abiVersion = file[8];
    header.type = read16(file + 16, little);
    header.machine = read16(file + 18, little);
    header.version = read32(file + 20, little);
    header.entry = read64(file + 24, little);
    header.programOffset = read64(file + 32, little);
    header.sectionOffset = read64(file + 40, little);
    header.flags = read32(file + 48, little);
    header.headerSize = read16(file + 52, little);
    header.programEntrySize = read16(file + 54, little);
    header.programCount = read16(file + 56, little);
    header.sectionEntrySize = read16(file + 58, little);
    header.sectionCount = read16(file + 60, little);

    if (header.headerSize != kElf64HeaderSize) {
        return fail(error, "ELF64 header size is not 64 bytes");
    }
    if (header.type != kTypeExecutable) {
        return fail(error, "only static ET_EXEC images are supported");
    }
    if (header.machine != kMachineMips) return fail(error, "ELF machine is not EM_MIPS");
    if (header.version != 1) return fail(error, "ELF header version is not current");
    if (header.osAbi != 0 && header.osAbi != 3) {
        return fail(error, "unsupported ELF OS ABI");
    }
    if (header.abiVersion != 0) return fail(error, "unsupported ELF ABI version");
    if (header.programCount == 0 || header.programCount == 0xFFFF ||
        header.programCount > kMaximumProgramHeaders) {
        return fail(error, "invalid or unsupported program-header count");
    }
    if (header.programEntrySize != kElf64ProgramHeaderSize) {
        return fail(error, "ELF64 program-header entries must be 56 bytes");
    }
    if (!rangeFits(header.programOffset,
                   uint64_t(header.programCount) * kElf64ProgramHeaderSize,
                   fileSize)) {
        return fail(error, "program-header table is outside the ELF file");
    }
    return true;
}

bool validateMachineFlags(uint32_t flags, std::string* error) {
    if ((flags & kMipsArchMask) != kMipsArch64R2) {
        return fail(error, "only the MIPS64 Release 2 ELF architecture flag is supported");
    }
    if ((flags & kMipsAbiMask) != 0 || (flags & kMipsAbi2) != 0) {
        return fail(error, "only the ELF64 N64 ABI encoding is supported");
    }
    if ((flags & kMipsMachMask) != 0 || (flags & kMipsAseMask) != 0) {
        return fail(error, "MIPS machine variants and ASE encodings are unsupported");
    }
    if ((flags & (kMips32BitMode | kMipsFp64 | kMipsNan2008)) != 0) {
        return fail(error, "ELF requires an unsupported MIPS execution or floating-point mode");
    }
    constexpr uint32_t allowed = kMipsArchMask | kMipsAbiMask | kMipsAbi2 |
        kMipsMachMask | kMipsAseMask | kMips32BitMode | kMipsFp64 | kMipsNan2008 |
        kMipsNoReorder | kMipsPic | kMipsCpic;
    if ((flags & ~allowed) != 0) return fail(error, "ELF contains unknown MIPS machine flags");
    return true;
}

bool parseProgramHeaders(const uint8_t* file, size_t fileSize,
                         const ParsedHeader& header, bool little,
                         std::vector<ParsedProgramHeader>& result,
                         std::string* error) {
    result.clear();
    result.reserve(header.programCount);
    for (uint16_t index = 0; index < header.programCount; ++index) {
        const uint64_t offset = header.programOffset +
            uint64_t(index) * kElf64ProgramHeaderSize;
        if (!rangeFits(offset, kElf64ProgramHeaderSize, fileSize)) {
            return fail(error, "program-header entry is truncated");
        }
        const uint8_t* bytes = file + static_cast<size_t>(offset);
        ParsedProgramHeader phdr;
        phdr.type = read32(bytes, little);
        phdr.flags = read32(bytes + 4, little);
        phdr.offset = read64(bytes + 8, little);
        phdr.virtualAddress = read64(bytes + 16, little);
        phdr.physicalAddress = read64(bytes + 24, little);
        phdr.fileSize = read64(bytes + 32, little);
        phdr.memorySize = read64(bytes + 40, little);
        phdr.alignment = read64(bytes + 48, little);
        result.push_back(phdr);
    }
    return true;
}

bool validateSections(const uint8_t* file, size_t fileSize,
                      const ParsedHeader& header, bool little,
                      std::string* error) {
    if (header.sectionCount == 0) {
        if (header.sectionOffset != 0) {
            return fail(error, "extended ELF section counts are unsupported");
        }
        if (header.sectionEntrySize != 0 &&
            header.sectionEntrySize != kElf64SectionHeaderSize) {
            return fail(error, "invalid ELF64 section-header entry size");
        }
        return true;
    }
    if (header.sectionCount > kMaximumSectionHeaders ||
        header.sectionEntrySize != kElf64SectionHeaderSize ||
        !rangeFits(header.sectionOffset,
                   uint64_t(header.sectionCount) * kElf64SectionHeaderSize,
                   fileSize)) {
        return fail(error, "section-header table is invalid or outside the ELF file");
    }
    for (uint16_t index = 0; index < header.sectionCount; ++index) {
        const uint64_t offset = header.sectionOffset +
            uint64_t(index) * kElf64SectionHeaderSize;
        const uint8_t* bytes = file + static_cast<size_t>(offset);
        const uint32_t type = read32(bytes + 4, little);
        const uint64_t sectionOffset = read64(bytes + 24, little);
        const uint64_t sectionSize = read64(bytes + 32, little);
        if (type == kSectionDynamic || type == kSectionRel ||
            type == kSectionRela || type == kSectionDynSym || type == kSectionRelr) {
            return fail(error, "ELF section requires dynamic linking or relocation processing");
        }
        if (type != kSectionNoBits &&
            !rangeFits(sectionOffset, sectionSize, fileSize)) {
            return fail(error, "ELF section data is outside the file");
        }
    }
    return true;
}

bool translateVirtual(uint64_t address, uint64_t& physical,
                      bool& requiresKx, std::string* error) {
    requiresKx = false;
    if (address >= 0xFFFFFFFF80000000ULL &&
        address <= 0xFFFFFFFF9FFFFFFFULL) {
        physical = address & kCompatKsegMask;
        return true;
    }
    if (address >= 0xFFFFFFFFA0000000ULL &&
        address <= 0xFFFFFFFFBFFFFFFFULL) {
        physical = address & kCompatKsegMask;
        return true;
    }
    if (address >= 0x8000000000000000ULL &&
        address <= 0xBFFFFFFFFFFFFFFFULL) {
        const uint8_t cca = static_cast<uint8_t>((address >> 59) & 0x7);
        if ((address & kXkphysReservedMask) != 0) {
            return fail(error, "XKPHYS address sets reserved bits 58:36");
        }
        if (cca != 2 && cca != 3) {
            return fail(error, "XKPHYS address uses an unsupported cache attribute");
        }
        physical = address & kPhysicalMask36;
        requiresKx = true;
        return true;
    }
    return fail(error,
                "ELF virtual address requires unsupported TLB or compatibility mapping");
}

bool overlaps(uint64_t firstBase, uint64_t firstSize,
              uint64_t secondBase, uint64_t secondSize) {
    return firstBase < secondBase + secondSize && secondBase < firstBase + firstSize;
}

}  // namespace

bool Mips64ElfLoader::BuildPlan(const uint8_t* file, size_t fileSize,
                                const Mips64ElfLoadOptions& options,
                                Mips64ElfLoadPlan& plan, std::string* error) {
    if (error) error->clear();
    if (!file || fileSize > options.maximumFileBytes) {
        return fail(error, "ELF file is null or exceeds the configured file-size bound");
    }

    ParsedHeader header;
    bool little = false;
    if (!parseHeader(file, fileSize, header, little, error)) return false;
    const uint8_t expectedEncoding = options.guestEndianness == Mips64Endianness::Little ? 1 : 2;
    if (header.dataEncoding != expectedEncoding) {
        return fail(error, "ELF byte order does not match the guest CPU");
    }
    if (options.guestEndianness != Mips64Endianness::Little &&
        options.guestEndianness != Mips64Endianness::Big) {
        return fail(error, "unsupported configured MIPS64 guest endianness");
    }
    if (!validateMachineFlags(header.flags, error)) return false;
    if (options.guestRamSize == 0) return fail(error, "guest RAM size is zero");

    std::vector<ParsedProgramHeader> programHeaders;
    if (!parseProgramHeaders(file, fileSize, header, little, programHeaders, error)) return false;
    if (!validateSections(file, fileSize, header, little, error)) return false;

    std::vector<Mips64ElfLoadSegment> segments;
    std::vector<std::pair<uint64_t, uint64_t>> physicalRanges;
    std::vector<std::pair<uint64_t, uint64_t>> virtualRanges;
    uint64_t totalStagedBytes = 0;
    bool entryExecutable = false;
    uint64_t entryPhysical = 0;
    bool requiresKx = false;

    for (const ParsedProgramHeader& phdr : programHeaders) {
        if (phdr.type == kProgramNull) {
            continue;
        }
        if (phdr.type == kProgramNote || phdr.type == kProgramPhdr) {
            if (!rangeFits(phdr.offset, phdr.fileSize, fileSize)) {
                return fail(error, "non-load program-header file range is outside the ELF file");
            }
            continue;
        }
        if (phdr.type == kProgramDynamic || phdr.type == kProgramInterp) {
            return fail(error, "dynamic linking and interpreters are unsupported");
        }
        if (phdr.type != kProgramLoad) {
            std::ostringstream message;
            message << "unsupported ELF program-header type 0x" << std::hex << phdr.type;
            return fail(error, message.str());
        }
        if ((phdr.flags & ~(kFlagReadable | kFlagWritable | kFlagExecutable)) != 0) {
            return fail(error, "PT_LOAD segment contains unknown permission flags");
        }
        if ((phdr.flags & kFlagReadable) == 0) {
            return fail(error, "PT_LOAD segments must be readable");
        }
        if (phdr.fileSize > phdr.memorySize) {
            return fail(error, "PT_LOAD p_filesz exceeds p_memsz");
        }
        if (!rangeFits(phdr.offset, phdr.fileSize, fileSize)) {
            return fail(error, "PT_LOAD file range is outside the ELF file");
        }
        if (phdr.memorySize > std::numeric_limits<uint64_t>::max() - phdr.virtualAddress) {
            return fail(error, "PT_LOAD virtual address range overflows 64 bits");
        }
        if (phdr.memorySize == 0) {
            if (phdr.fileSize != 0) return fail(error, "empty PT_LOAD has file data");
            continue;
        }
        if (phdr.memorySize > std::numeric_limits<size_t>::max()) {
            return fail(error, "PT_LOAD memory size exceeds host staging capacity");
        }
        if (phdr.alignment > 1 && (!isPowerOfTwo(phdr.alignment) ||
            (phdr.offset & (phdr.alignment - 1)) !=
                (phdr.virtualAddress & (phdr.alignment - 1)))) {
            return fail(error, "PT_LOAD alignment is not a power of two or is incongruent");
        }
        if (phdr.memorySize > options.maximumStagedBytes - totalStagedBytes) {
            return fail(error, "PT_LOAD memory exceeds the configured staging bound");
        }

        uint64_t physical = 0;
        bool segmentRequiresKx = false;
        if (!translateVirtual(phdr.virtualAddress, physical, segmentRequiresKx, error)) return false;
        uint64_t lastPhysical = 0;
        bool lastRequiresKx = false;
        const uint64_t lastVirtual = phdr.virtualAddress + phdr.memorySize - 1;
        if (!translateVirtual(lastVirtual, lastPhysical, lastRequiresKx, error) ||
            lastPhysical != physical + phdr.memorySize - 1 ||
            lastRequiresKx != segmentRequiresKx) {
            return fail(error, "PT_LOAD crosses a MIPS virtual mapping boundary");
        }
        if (phdr.physicalAddress != 0 && phdr.physicalAddress != physical) {
            uint64_t declaredPhysical = 0;
            bool declaredRequiresKx = false;
            if (!translateVirtual(phdr.physicalAddress, declaredPhysical,
                                  declaredRequiresKx, error) ||
                declaredPhysical != physical) {
                return fail(error,
                            "PT_LOAD p_paddr is neither the resolved physical address nor a matching supported MIPS direct-map alias");
            }
            // p_paddr describes placement here; the entry VA determines KX state.
            static_cast<void>(declaredRequiresKx);
        }
        if (physical > options.guestRamSize ||
            phdr.memorySize > options.guestRamSize - physical) {
            return fail(error, "PT_LOAD target is outside configured guest RAM");
        }
        for (const Mips64ElfRange& reserved : options.reservedPhysicalRanges) {
            if (reserved.size > std::numeric_limits<uint64_t>::max() - reserved.base) {
                return fail(error, "configured reserved RAM range overflows 64 bits");
            }
            if (overlaps(physical, phdr.memorySize, reserved.base, reserved.size)) {
                return fail(error, "PT_LOAD target overlaps reserved Malta boot memory");
            }
        }
        for (const auto& range : physicalRanges) {
            if (overlaps(physical, phdr.memorySize, range.first, range.second - range.first)) {
                return fail(error, "PT_LOAD physical memory regions overlap");
            }
        }
        const uint64_t virtualEnd = phdr.virtualAddress + phdr.memorySize;
        for (const auto& range : virtualRanges) {
            if (phdr.virtualAddress < range.second && range.first < virtualEnd) {
                return fail(error, "PT_LOAD virtual memory regions overlap");
            }
        }

        if (header.entry >= phdr.virtualAddress && header.entry < virtualEnd &&
            (phdr.flags & kFlagExecutable) != 0) {
            uint64_t resolvedEntry = 0;
            bool entryRequiresKx = false;
            if (!translateVirtual(header.entry, resolvedEntry, entryRequiresKx, error)) return false;
            entryExecutable = true;
            entryPhysical = resolvedEntry;
            requiresKx = requiresKx || entryRequiresKx;
        }
        requiresKx = requiresKx || segmentRequiresKx;
        totalStagedBytes += phdr.memorySize;
        physicalRanges.emplace_back(physical, physical + phdr.memorySize);
        virtualRanges.emplace_back(phdr.virtualAddress, virtualEnd);

        Mips64ElfLoadSegment segment;
        segment.fileOffset = phdr.offset;
        segment.virtualAddress = phdr.virtualAddress;
        segment.declaredPhysicalAddress = phdr.physicalAddress;
        segment.physicalAddress = physical;
        segment.fileSize = phdr.fileSize;
        segment.memorySize = phdr.memorySize;
        segment.alignment = phdr.alignment;
        segment.flags = phdr.flags;
        segments.push_back(std::move(segment));
    }

    if (segments.empty()) return fail(error, "ELF has no non-empty PT_LOAD segments");
    if ((header.entry & 3) != 0 || !entryExecutable) {
        return fail(error, "ELF entry point is not aligned inside an executable PT_LOAD segment");
    }
    if (entryPhysical >= options.guestRamSize) {
        return fail(error, "ELF entry point resolves outside guest RAM");
    }

    for (Mips64ElfLoadSegment& segment : segments) {
        segment.image.assign(static_cast<size_t>(segment.memorySize), 0);
        if (segment.fileSize != 0) {
            std::memcpy(segment.image.data(),
                        file + static_cast<size_t>(segment.fileOffset),
                        static_cast<size_t>(segment.fileSize));
        }
    }

    Mips64ElfLoadPlan stagedPlan;
    stagedPlan.endianness = options.guestEndianness;
    stagedPlan.entryPoint = header.entry;
    stagedPlan.entryPhysicalAddress = entryPhysical;
    stagedPlan.machineFlags = header.flags;
    stagedPlan.requiresKernelKx = requiresKx;
    stagedPlan.segments = std::move(segments);
    plan = std::move(stagedPlan);
    return true;
}

bool Mips64ElfLoader::CommitPlan(
    const Mips64ElfLoadPlan& plan, ia64::PhysicalAddressMap& memory,
    const std::vector<ia64::PhysicalRamWrite>& additionalWrites, std::string* error) {
    ia64::PreparedPhysicalRamWriteBatch prepared;
    if (!PreparePlan(plan, memory, additionalWrites, prepared, error)) return false;
    memory.CommitRamWriteBatch(std::move(prepared));
    return true;
}

bool Mips64ElfLoader::PreparePlan(
    const Mips64ElfLoadPlan& plan, ia64::PhysicalAddressMap& memory,
    const std::vector<ia64::PhysicalRamWrite>& additionalWrites,
    ia64::PreparedPhysicalRamWriteBatch& prepared, std::string* error) {
    std::vector<ia64::PhysicalRamWrite> writes;
    writes.reserve(plan.segments.size() + additionalWrites.size());
    for (const Mips64ElfLoadSegment& segment : plan.segments) {
        if (segment.memorySize != segment.image.size() || segment.image.empty()) {
            return fail(error, "load plan contains an invalid staged segment image");
        }
        writes.push_back({segment.physicalAddress, segment.image.data(), segment.image.size()});
    }
    writes.insert(writes.end(), additionalWrites.begin(), additionalWrites.end());
    return memory.PrepareRamWriteBatch(writes, prepared, error);
}

bool Mips64ElfLoader::LoadBuffer(
    const uint8_t* file, size_t fileSize, const Mips64ElfLoadOptions& options,
    ia64::PhysicalAddressMap& memory, Mips64ElfLoadPlan* loadedPlan,
    const std::vector<ia64::PhysicalRamWrite>& additionalWrites, std::string* error) {
    Mips64ElfLoadPlan plan;
    if (!BuildPlan(file, fileSize, options, plan, error)) return false;
    if (!CommitPlan(plan, memory, additionalWrites, error)) return false;
    if (loadedPlan) *loadedPlan = std::move(plan);
    return true;
}

}  // namespace mips64
