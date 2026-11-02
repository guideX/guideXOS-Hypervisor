#include "PPC64ElfLoader.h"

#include "VirtualMachine.h"

#include <algorithm>
#include <limits>

namespace ia64::ppc64 {
namespace {

constexpr size_t kElf64HeaderSize = 64;
constexpr size_t kElf64ProgramHeaderSize = 56;
constexpr uint16_t kEtExec = 2;
constexpr uint16_t kEmPpc64 = 21;
constexpr uint32_t kPtNull = 0;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtNote = 4;
constexpr uint32_t kPfExecute = 1;
constexpr uint32_t kPfWrite = 2;
constexpr uint32_t kPfRead = 4;
constexpr uint32_t kPfMask = kPfExecute | kPfWrite | kPfRead;
constexpr uint32_t kPpc64AbiMask = 3;
constexpr uint32_t kPpc64AbiV2 = 2;
constexpr size_t kMaxProgramHeaders = 4096;

uint16_t readBe16(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                 static_cast<uint16_t>(bytes[1]));
}

uint32_t readBe32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t readBe64(const uint8_t* bytes) {
    return (static_cast<uint64_t>(readBe32(bytes)) << 32) |
           static_cast<uint64_t>(readBe32(bytes + 4));
}

bool validFileRange(uint64_t offset, uint64_t size, size_t imageSize) {
    return offset <= static_cast<uint64_t>(imageSize) &&
           size <= static_cast<uint64_t>(imageSize) - offset;
}

bool validMemoryRange(uint64_t base, uint64_t size) {
    return size != 0 && size <= std::numeric_limits<uint64_t>::max() - base;
}

bool rangesOverlap(uint64_t leftBase, uint64_t leftSize,
                   uint64_t rightBase, uint64_t rightSize) {
    return leftBase < rightBase + rightSize &&
           rightBase < leftBase + leftSize;
}

bool reject(std::string& reason, const char* message) {
    reason = message;
    return false;
}

} // namespace

bool parsePPC64Elf64BE(const std::vector<uint8_t>& image,
                       PPC64ElfImage& result, std::string& reason) {
    if (image.size() < kElf64HeaderSize) {
        return reject(reason, "truncated ELF64 header");
    }
    const uint8_t* const bytes = image.data();
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
        return reject(reason, "input is not an ELF image");
    }
    if (bytes[4] != 2) return reject(reason, "ELF class must be ELF64");
    if (bytes[5] != 2) return reject(reason, "ELF endianness must be big-endian");
    if (bytes[6] != 1) return reject(reason, "ELF identification version is invalid");

    const uint16_t type = readBe16(bytes + 16);
    const uint16_t machine = readBe16(bytes + 18);
    const uint32_t version = readBe32(bytes + 20);
    const uint64_t entry = readBe64(bytes + 24);
    const uint64_t phoff = readBe64(bytes + 32);
    const uint32_t elfFlags = readBe32(bytes + 48);
    const uint16_t ehsize = readBe16(bytes + 52);
    const uint16_t phentsize = readBe16(bytes + 54);
    const uint16_t phnum = readBe16(bytes + 56);

    if (version != 1 || ehsize != kElf64HeaderSize) {
        return reject(reason, "ELF header version or size is unsupported");
    }
    if (type != kEtExec) return reject(reason, "ELF type must be ET_EXEC");
    if (machine != kEmPpc64) return reject(reason, "ELF machine must be EM_PPC64");
    if ((elfFlags & kPpc64AbiMask) != kPpc64AbiV2) {
        return reject(reason, "ELF PPC64 ABI must be ABI v2 for this profile");
    }
    if (phnum == 0 || phnum == 0xffff || phnum > kMaxProgramHeaders ||
        phentsize != kElf64ProgramHeaderSize) {
        return reject(reason, "ELF program-header table shape is unsupported");
    }
    const uint64_t phbytes = static_cast<uint64_t>(phnum) * phentsize;
    if (!validFileRange(phoff, phbytes, image.size())) {
        return reject(reason, "ELF program-header table is truncated");
    }

    PPC64ElfImage parsed;
    parsed.virtualEntry = entry;
    parsed.flags = elfFlags;
    parsed.physicalBase = std::numeric_limits<uint64_t>::max();
    parsed.physicalEnd = 0;
    for (uint16_t index = 0; index < phnum; ++index) {
        const size_t offset = static_cast<size_t>(phoff) +
                              static_cast<size_t>(index) * phentsize;
        const uint8_t* const header = bytes + offset;
        const uint32_t programType = readBe32(header);
        const uint32_t flags = readBe32(header + 4);
        const uint64_t fileOffset = readBe64(header + 8);
        const uint64_t virtualAddress = readBe64(header + 16);
        const uint64_t physicalAddress = readBe64(header + 24);
        const uint64_t fileSize = readBe64(header + 32);
        const uint64_t memorySize = readBe64(header + 40);
        const uint64_t alignment = readBe64(header + 48);

        if (programType == kPtNull) continue;
        if (programType == kPtNote) {
            if (!validFileRange(fileOffset, fileSize, image.size()) ||
                memorySize < fileSize) {
                return reject(reason, "PT_NOTE file or memory range is invalid");
            }
            continue;
        }
        if (programType != kPtLoad) {
            return reject(reason, "ELF contains an unsupported non-PT_LOAD program header");
        }
        if ((flags & ~kPfMask) != 0 || (flags & kPfRead) == 0) {
            return reject(reason, "PT_LOAD flags are unsupported");
        }
        if (memorySize < fileSize) {
            return reject(reason, "PT_LOAD p_memsz is smaller than p_filesz");
        }
        if (!validFileRange(fileOffset, fileSize, image.size())) {
            return reject(reason, "PT_LOAD file range is outside the ELF image");
        }
        if (!validMemoryRange(virtualAddress, memorySize) ||
            !validMemoryRange(physicalAddress, memorySize)) {
            return reject(reason, "PT_LOAD memory range overflows ELF64");
        }
        if (alignment > 1) {
            if ((alignment & (alignment - 1U)) != 0) {
                return reject(reason, "PT_LOAD alignment is not a power of two");
            }
            if (fileOffset % alignment != virtualAddress % alignment ||
                fileOffset % alignment != physicalAddress % alignment) {
                return reject(reason, "PT_LOAD file and address alignments are incongruent");
            }
        }

        PPC64ElfLoadSegment segment;
        segment.fileOffset = fileOffset;
        segment.virtualAddress = virtualAddress;
        segment.physicalAddress = physicalAddress;
        segment.fileSize = fileSize;
        segment.memorySize = memorySize;
        segment.alignment = alignment;
        segment.flags = flags;
        const auto begin = image.begin() + static_cast<std::ptrdiff_t>(fileOffset);
        const auto end = begin + static_cast<std::ptrdiff_t>(fileSize);
        segment.fileBytes.assign(begin, end);
        parsed.physicalBase = std::min(parsed.physicalBase, physicalAddress);
        parsed.physicalEnd = std::max(parsed.physicalEnd,
                                      physicalAddress + memorySize);
        parsed.loadSegments.push_back(std::move(segment));
    }
    if (parsed.loadSegments.empty()) {
        return reject(reason, "ELF has no PT_LOAD segments");
    }

    std::vector<const PPC64ElfLoadSegment*> ordered;
    ordered.reserve(parsed.loadSegments.size());
    for (const auto& segment : parsed.loadSegments) ordered.push_back(&segment);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->physicalAddress < right->physicalAddress;
    });
    for (size_t index = 1; index < ordered.size(); ++index) {
        if (rangesOverlap(ordered[index - 1U]->physicalAddress,
                          ordered[index - 1U]->memorySize,
                          ordered[index]->physicalAddress,
                          ordered[index]->memorySize)) {
            return reject(reason, "PT_LOAD physical memory ranges overlap");
        }
    }

    if ((entry & 3ULL) != 0) {
        return reject(reason, "ELF entry is not instruction aligned");
    }
    bool entryMapped = false;
    for (const auto& segment : parsed.loadSegments) {
        if ((segment.flags & kPfExecute) == 0 || segment.fileSize < sizeof(uint32_t) ||
            entry < segment.virtualAddress ||
            entry > segment.virtualAddress + segment.fileSize - sizeof(uint32_t)) {
            continue;
        }
        const uint64_t delta = entry - segment.virtualAddress;
        if (delta > std::numeric_limits<uint64_t>::max() - segment.physicalAddress) {
            return reject(reason, "ELF entry physical translation overflows");
        }
        parsed.physicalEntry = segment.physicalAddress + delta;
        entryMapped = true;
        break;
    }
    if (!entryMapped) {
        return reject(reason, "ELF entry is outside file-backed executable PT_LOAD data");
    }
    result = std::move(parsed);
    reason.clear();
    return true;
}

bool loadPPC64Elf(VirtualMachine& vm, const std::vector<uint8_t>& image,
                  PPC64ElfImage& result, std::string& reason) {
    PPC64ElfImage parsed;
    if (!parsePPC64Elf64BE(image, parsed, reason)) return false;

    std::vector<PPC64PlatformLoadSegment> staged;
    staged.reserve(parsed.loadSegments.size());
    for (const auto& segment : parsed.loadSegments) {
        PPC64PlatformLoadSegment item;
        item.physicalAddress = segment.physicalAddress;
        item.memorySize = segment.memorySize;
        item.executable = (segment.flags & kPfExecute) != 0;
        item.fileBytes = segment.fileBytes;
        staged.push_back(std::move(item));
    }
    if (!vm.preparePPC64PlatformSegments(staged, parsed.physicalEntry, &reason)) {
        return false;
    }
    result = std::move(parsed);
    reason.clear();
    return true;
}

} // namespace ia64::ppc64
