#include "PPC64Platform.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ia64::ppc64 {
namespace {

constexpr uint32_t kFdtMagic = 0xd00dfeedU;
constexpr uint32_t kFdtBeginNode = 1;
constexpr uint32_t kFdtEndNode = 2;
constexpr uint32_t kFdtProperty = 3;
constexpr uint32_t kFdtEnd = 9;
constexpr uint32_t kFdtVersion = 17;
constexpr uint32_t kFdtLastCompatibleVersion = 16;
constexpr uint32_t kPower9Pvr = 0x004e1200U;

bool rangeEnd(uint64_t base, uint64_t size, uint64_t& end) {
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - base) {
        return false;
    }
    end = base + size;
    return true;
}

bool overlaps(uint64_t aBase, uint64_t aSize, uint64_t bBase, uint64_t bSize) {
    uint64_t aEnd = 0;
    uint64_t bEnd = 0;
    if (!rangeEnd(aBase, aSize, aEnd) || !rangeEnd(bBase, bSize, bEnd)) {
        return true;
    }
    return aBase < bEnd && bBase < aEnd;
}

void putBe32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value >> 24);
    destination[1] = static_cast<uint8_t>(value >> 16);
    destination[2] = static_cast<uint8_t>(value >> 8);
    destination[3] = static_cast<uint8_t>(value);
}

void putBe64(uint8_t* destination, uint64_t value) {
    putBe32(destination, static_cast<uint32_t>(value >> 32));
    putBe32(destination + 4, static_cast<uint32_t>(value));
}

uint32_t readBe32(const uint8_t* source) {
    return (static_cast<uint32_t>(source[0]) << 24) |
           (static_cast<uint32_t>(source[1]) << 16) |
           (static_cast<uint32_t>(source[2]) << 8) |
           static_cast<uint32_t>(source[3]);
}

uint64_t readBe64(const uint8_t* source) {
    return (static_cast<uint64_t>(readBe32(source)) << 32) |
           readBe32(source + 4);
}

bool checkedAlign4(size_t value, size_t& aligned) {
    if (value > std::numeric_limits<size_t>::max() - 3U) return false;
    aligned = (value + 3U) & ~size_t{3U};
    return true;
}

struct StructureBuilder {
    std::vector<uint8_t> structure;
    std::vector<uint8_t> strings;
    std::map<std::string, uint32_t> stringOffsets;

    void word(uint32_t value) {
        const size_t offset = structure.size();
        structure.resize(offset + 4U);
        putBe32(structure.data() + offset, value);
    }

    bool node(const std::string& name) {
        word(kFdtBeginNode);
        const size_t oldSize = structure.size();
        if (name.size() > std::numeric_limits<size_t>::max() - oldSize - 1U) return false;
        structure.insert(structure.end(), name.begin(), name.end());
        structure.push_back(0);
        size_t aligned = 0;
        if (!checkedAlign4(structure.size(), aligned)) return false;
        structure.resize(aligned, 0);
        return true;
    }

    void endNode() { word(kFdtEndNode); }

    bool property(const std::string& name, const std::vector<uint8_t>& value) {
        if (name.empty() || value.size() > std::numeric_limits<uint32_t>::max()) return false;
        auto found = stringOffsets.find(name);
        uint32_t nameOffset = 0;
        if (found == stringOffsets.end()) {
            if (strings.size() > std::numeric_limits<uint32_t>::max() - name.size() - 1U) {
                return false;
            }
            nameOffset = static_cast<uint32_t>(strings.size());
            strings.insert(strings.end(), name.begin(), name.end());
            strings.push_back(0);
            stringOffsets.emplace(name, nameOffset);
        } else {
            nameOffset = found->second;
        }
        word(kFdtProperty);
        word(static_cast<uint32_t>(value.size()));
        word(nameOffset);
        structure.insert(structure.end(), value.begin(), value.end());
        size_t aligned = 0;
        if (!checkedAlign4(structure.size(), aligned)) return false;
        structure.resize(aligned, 0);
        return true;
    }

    bool u32(const std::string& name, uint32_t value) {
        std::vector<uint8_t> bytes(4);
        putBe32(bytes.data(), value);
        return property(name, bytes);
    }

    bool cells64(const std::string& name, uint64_t first, uint64_t second) {
        std::vector<uint8_t> bytes(16);
        putBe64(bytes.data(), first);
        putBe64(bytes.data() + 8, second);
        return property(name, bytes);
    }

    bool ranges64(const std::string& name,
                  const std::vector<PPC64MemoryRange>& ranges) {
        if (ranges.empty() || ranges.size() >
            std::numeric_limits<size_t>::max() / 16U) return false;
        std::vector<uint8_t> bytes(ranges.size() * 16U);
        for (size_t i = 0; i < ranges.size(); ++i) {
            putBe64(bytes.data() + i * 16U, ranges[i].base);
            putBe64(bytes.data() + i * 16U + 8U, ranges[i].size);
        }
        return property(name, bytes);
    }

    bool string(const std::string& name, const std::string& value) {
        if (value.find('\0') != std::string::npos) return false;
        std::vector<uint8_t> bytes(value.begin(), value.end());
        bytes.push_back(0);
        return property(name, bytes);
    }

    bool empty(const std::string& name) {
        return property(name, {});
    }
};

void setReason(std::string& reason, const char* message) {
    reason = message;
}

bool fdtHeader(const uint8_t* data, size_t size, uint32_t& totalSize,
               uint32_t& structureOffset, uint32_t& stringsOffset,
               uint32_t& reservationOffset, uint32_t& structureSize,
               uint32_t& stringsSize, std::string& reason) {
    if (data == nullptr || size < 40U) {
        setReason(reason, "FDT is truncated before the 40-byte header");
        return false;
    }
    if (readBe32(data) != kFdtMagic) {
        setReason(reason, "FDT magic is invalid");
        return false;
    }
    totalSize = readBe32(data + 4);
    structureOffset = readBe32(data + 8);
    stringsOffset = readBe32(data + 12);
    reservationOffset = readBe32(data + 16);
    const uint32_t version = readBe32(data + 20);
    const uint32_t lastCompatible = readBe32(data + 24);
    stringsSize = readBe32(data + 32);
    structureSize = readBe32(data + 36);
    if (totalSize < 40U || totalSize > size || version < 16U ||
        version > kFdtVersion || lastCompatible > version ||
        lastCompatible > kFdtVersion || structureOffset < 40U ||
        (structureOffset & 3U) != 0 || (reservationOffset & 7U) != 0 ||
        reservationOffset < 40U || stringsOffset < 40U ||
        structureOffset > totalSize || structureSize > totalSize - structureOffset ||
        stringsOffset > totalSize || stringsSize > totalSize - stringsOffset) {
        setReason(reason, "FDT header contains invalid version, offsets, or sizes");
        return false;
    }
    if (structureOffset < stringsOffset + stringsSize &&
        stringsOffset < structureOffset + structureSize) {
        setReason(reason, "FDT structure and strings blocks overlap");
        return false;
    }
    if (reservationOffset >= structureOffset) {
        setReason(reason, "FDT reservation block is not before the structure block");
        return false;
    }
    return true;
}

bool buildStructure(const PPC64PlatformConfiguration& configuration,
                    const PPC64GuestMemoryLayout& layout,
                    StructureBuilder& builder, std::string& reason) {
    const auto property = [&](const std::string& name, uint32_t value) {
        return builder.u32(name, value);
    };
    if (!builder.node("") || !builder.string("device_type", "chrp") ||
        !builder.string("compatible", "guideXOS,power9-pseries-foundation") ||
        !builder.string("model", "guideXOS POWER9 pSeries foundation") ||
        !property("#address-cells", 2) || !property("#size-cells", 2)) {
        setReason(reason, "failed to encode the FDT root node");
        return false;
    }

    if (!builder.node("cpus") || !property("#address-cells", 1) ||
        !property("#size-cells", 0)) {
        setReason(reason, "failed to encode the FDT CPUs container");
        return false;
    }
    if (!builder.node("PowerPC,POWER9@0") ||
        !builder.string("device_type", "cpu") || !property("reg", 0) ||
        !property("cpu-version", kPower9Pvr) ||
        !property("timebase-frequency", static_cast<uint32_t>(configuration.timebaseFrequency)) ||
        !builder.empty("64-bit") || !builder.string("status", "okay")) {
        setReason(reason, "failed to encode the POWER9 CPU node");
        return false;
    }
    builder.endNode();
    builder.endNode();

    const std::vector<PPC64MemoryRange> defaultMemory{{layout.ramBase,
        layout.ramSize, "guest RAM", true}};
    const auto& guestMemory = layout.guestMemoryRanges.empty()
        ? defaultMemory : layout.guestMemoryRanges;
    for (const PPC64MemoryRange& range : guestMemory) {
        std::ostringstream nodeName;
        nodeName << "memory@" << std::hex << range.base;
        if (!builder.node(nodeName.str()) || !builder.string("device_type", "memory") ||
            !builder.cells64("reg", range.base, range.size)) {
            setReason(reason, "failed to encode a guest memory node");
            return false;
        }
        builder.endNode();
    }

    if (!builder.node("chosen")) {
        setReason(reason, "failed to encode the chosen node");
        return false;
    }
    if (!configuration.bootArguments.empty() &&
        !builder.string("bootargs", configuration.bootArguments)) {
        setReason(reason, "boot arguments contain an invalid string");
        return false;
    }
    builder.endNode();
    builder.endNode();
    builder.word(kFdtEnd);
    return true;
}

} // namespace

PPC64VirtualTimeBase::PPC64VirtualTimeBase(
    const PPC64PlatformConfiguration& configuration) {
    std::string reason;
    if (!configure(configuration, reason)) {
        throw std::invalid_argument(reason);
    }
}

bool PPC64VirtualTimeBase::configure(
    const PPC64PlatformConfiguration& configuration, std::string& reason) {
    if (configuration.timebaseFrequency == 0 ||
        configuration.timebaseFrequency > std::numeric_limits<uint32_t>::max()) {
        setReason(reason, "time-base frequency must fit a nonzero FDT cell");
        return false;
    }
    if (configuration.ticksPerInstruction == 0) {
        setReason(reason, "time-base ticks per instruction must be nonzero");
        return false;
    }
    value_ = configuration.initialTimebase;
    initialValue_ = configuration.initialTimebase;
    frequency_ = configuration.timebaseFrequency;
    ticksPerInstruction_ = configuration.ticksPerInstruction;
    return true;
}

void PPC64VirtualTimeBase::reset() { value_ = initialValue_; }

void PPC64VirtualTimeBase::advanceInstruction() {
    value_ += ticksPerInstruction_; // unsigned arithmetic is the architectural modulo-2^64 wrap
}

bool PPC64VirtualTimeBase::restore(uint64_t value) {
    value_ = value;
    return true;
}

bool validatePPC64PlatformConfiguration(
    const PPC64PlatformConfiguration& configuration, uint64_t ramSize,
    std::string& reason) {
    if (configuration.profile != PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION) {
        setReason(reason, "unsupported PPC64 platform profile");
        return false;
    }
    if (configuration.entryMode != PPC64GuestEntryMode::DIRECT_FDT) {
        setReason(reason, "firmware-assisted entry is not implemented");
        return false;
    }
    if (configuration.cpuCount != 1) {
        setReason(reason, "POWER9 foundation profile supports exactly one vCPU");
        return false;
    }
    if (configuration.timebaseFrequency == 0 ||
        configuration.timebaseFrequency > std::numeric_limits<uint32_t>::max() ||
        configuration.ticksPerInstruction == 0) {
        setReason(reason, "invalid virtual time-base frequency or instruction increment");
        return false;
    }
    if (configuration.maximumFdtSize < 128U ||
        configuration.maximumFdtSize > 16U * 1024U * 1024U ||
        configuration.maximumFdtSize > ramSize) {
        setReason(reason, "FDT size bound is outside the supported RAM range");
        return false;
    }
    if (configuration.bootArguments.find('\0') != std::string::npos) {
        setReason(reason, "boot arguments contain an embedded NUL byte");
        return false;
    }
    for (size_t i = 0; i < configuration.reservedRanges.size(); ++i) {
        const PPC64MemoryRange& range = configuration.reservedRanges[i];
        uint64_t end = 0;
        if (!rangeEnd(range.base, range.size, end) || end > ramSize) {
            setReason(reason, "configured reserved range is empty, overflowing, or outside guest RAM");
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (overlaps(range.base, range.size,
                         configuration.reservedRanges[j].base,
                         configuration.reservedRanges[j].size)) {
                setReason(reason, "configured reserved ranges overlap");
                return false;
            }
        }
    }
    if (configuration.requestedFdtAddress != 0 &&
        (configuration.requestedFdtAddress & 7ULL) != 0) {
        setReason(reason, "requested FDT placement must be 8-byte aligned");
        return false;
    }
    return true;
}

bool samePPC64PlatformConfiguration(
    const PPC64PlatformConfiguration& left,
    const PPC64PlatformConfiguration& right) {
    if (left.profile != right.profile || left.entryMode != right.entryMode ||
        left.cpuCount != right.cpuCount ||
        left.timebaseFrequency != right.timebaseFrequency ||
        left.ticksPerInstruction != right.ticksPerInstruction ||
        left.initialTimebase != right.initialTimebase ||
        left.requestedFdtAddress != right.requestedFdtAddress ||
        left.maximumFdtSize != right.maximumFdtSize ||
        left.bootArguments != right.bootArguments ||
        left.reservedRanges.size() != right.reservedRanges.size()) {
        return false;
    }
    for (size_t i = 0; i < left.reservedRanges.size(); ++i) {
        const auto& a = left.reservedRanges[i];
        const auto& b = right.reservedRanges[i];
        if (a.base != b.base || a.size != b.size || a.purpose != b.purpose ||
            a.guestAccessible != b.guestAccessible) return false;
    }
    return true;
}

bool validatePPC64GuestMemoryLayout(const PPC64GuestMemoryLayout& layout,
                                    std::string& reason) {
    uint64_t ramEnd = 0;
    uint64_t imageEnd = 0;
    uint64_t fdtEnd = 0;
    if (!rangeEnd(layout.ramBase, layout.ramSize, ramEnd) ||
        !rangeEnd(layout.imageBase, layout.imageSize, imageEnd) ||
        !rangeEnd(layout.fdtBase, layout.fdtSize, fdtEnd) ||
        (layout.imageBase & 3ULL) != 0 || (layout.fdtBase & 7ULL) != 0 ||
        layout.imageBase < layout.ramBase || imageEnd > ramEnd ||
        layout.fdtBase < layout.ramBase || fdtEnd > ramEnd ||
        (layout.entryAddress & 3ULL) != 0 ||
        layout.entryAddress < layout.imageBase || layout.entryAddress >= imageEnd ||
        overlaps(layout.imageBase, layout.imageSize, layout.fdtBase, layout.fdtSize)) {
        setReason(reason, "guest RAM, image, FDT, or entry geometry is invalid or overlapping");
        return false;
    }
    if ((layout.initrdSize == 0) != (layout.initrdBase == 0)) {
        setReason(reason, "initrd base and size must either both be zero or both be present");
        return false;
    }
    uint64_t initrdEnd = 0;
    if (layout.initrdSize != 0 &&
        (!rangeEnd(layout.initrdBase, layout.initrdSize, initrdEnd) ||
         layout.initrdBase < layout.ramBase || initrdEnd > ramEnd ||
         overlaps(layout.initrdBase, layout.initrdSize, layout.imageBase, layout.imageSize) ||
         overlaps(layout.initrdBase, layout.initrdSize, layout.fdtBase, layout.fdtSize))) {
        setReason(reason, "initrd placement is outside guest RAM or overlaps another payload");
        return false;
    }
    for (size_t i = 0; i < layout.reservations.size(); ++i) {
        const PPC64MemoryRange& reservation = layout.reservations[i];
        uint64_t end = 0;
        if (!rangeEnd(reservation.base, reservation.size, end) ||
            reservation.base < layout.ramBase || end > ramEnd) {
            setReason(reason, "memory reservation is empty, overflowing, or outside guest RAM");
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (overlaps(reservation.base, reservation.size,
                         layout.reservations[j].base, layout.reservations[j].size)) {
                setReason(reason, "guest memory reservations overlap");
                return false;
            }
        }
    }
    for (size_t i = 0; i < layout.guestMemoryRanges.size(); ++i) {
        const PPC64MemoryRange& range = layout.guestMemoryRanges[i];
        uint64_t end = 0;
        if (!range.guestAccessible || !rangeEnd(range.base, range.size, end) ||
            range.base < layout.ramBase || end > ramEnd) {
            setReason(reason, "advertised guest RAM range is invalid");
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (overlaps(range.base, range.size,
                         layout.guestMemoryRanges[j].base,
                         layout.guestMemoryRanges[j].size)) {
                setReason(reason, "advertised guest RAM ranges overlap");
                return false;
            }
        }
        for (const PPC64MemoryRange& reservation : layout.reservations) {
            if (!reservation.guestAccessible &&
                overlaps(range.base, range.size, reservation.base, reservation.size)) {
                setReason(reason, "advertised guest RAM overlaps host-protected backing");
                return false;
            }
        }
    }
    return true;
}

PPC64PlatformCapabilities getPPC64FoundationCapabilities() {
    PPC64PlatformCapabilities result;
    result.power9FoundationProfile = true;
    result.directFdtEntry = true;
    result.validFdt = true;
    result.deterministicTimebase = true;
    result.paprHypercallSubset = true;
    return result;
}

bool PPC64FDTBuilder::build(const PPC64PlatformConfiguration& configuration,
                            const PPC64GuestMemoryLayout& layout,
                            std::vector<uint8_t>& output, std::string& reason) {
    if (!validatePPC64PlatformConfiguration(configuration, layout.ramSize, reason) ||
        configuration.timebaseFrequency > std::numeric_limits<uint32_t>::max() ||
        configuration.timebaseFrequency == 0 ||
        layout.ramSize == 0 || layout.fdtSize == 0 ||
        !validatePPC64GuestMemoryLayout(layout, reason)) {
        if (reason.empty()) setReason(reason, "invalid FDT profile or layout input");
        return false;
    }
    StructureBuilder structure;
    if (!buildStructure(configuration, layout, structure, reason)) return false;
    uint64_t reservationBytes = 16U;
    if (layout.reservations.size() >
        (std::numeric_limits<uint32_t>::max() / 16U) - 1U) {
        setReason(reason, "too many FDT memory reservations");
        return false;
    }
    reservationBytes += static_cast<uint64_t>(layout.reservations.size()) * 16U;
    const uint64_t reservationOffset = 40U;
    const uint64_t structureOffset = reservationOffset + reservationBytes;
    const uint64_t stringsOffset = structureOffset + structure.structure.size();
    const uint64_t totalSize = stringsOffset + structure.strings.size();
    if (totalSize > configuration.maximumFdtSize ||
        totalSize > std::numeric_limits<uint32_t>::max()) {
        setReason(reason, "generated FDT exceeds its configured size bound");
        return false;
    }
    output.assign(static_cast<size_t>(totalSize), 0);
    putBe32(output.data(), kFdtMagic);
    putBe32(output.data() + 4, static_cast<uint32_t>(totalSize));
    putBe32(output.data() + 8, static_cast<uint32_t>(structureOffset));
    putBe32(output.data() + 12, static_cast<uint32_t>(stringsOffset));
    putBe32(output.data() + 16, static_cast<uint32_t>(reservationOffset));
    putBe32(output.data() + 20, kFdtVersion);
    putBe32(output.data() + 24, kFdtLastCompatibleVersion);
    putBe32(output.data() + 28, 0); // boot CPU physical ID
    putBe32(output.data() + 32, static_cast<uint32_t>(structure.strings.size()));
    putBe32(output.data() + 36, static_cast<uint32_t>(structure.structure.size()));
    size_t offset = static_cast<size_t>(reservationOffset);
    for (const PPC64MemoryRange& reservation : layout.reservations) {
        putBe64(output.data() + offset, reservation.base);
        putBe64(output.data() + offset + 8, reservation.size);
        offset += 16U;
    }
    std::copy(structure.structure.begin(), structure.structure.end(),
              output.begin() + static_cast<std::ptrdiff_t>(structureOffset));
    std::copy(structure.strings.begin(), structure.strings.end(),
              output.begin() + static_cast<std::ptrdiff_t>(stringsOffset));
    return validate(output.data(), output.size(), reason);
}

bool PPC64FDTBuilder::validate(const uint8_t* data, size_t size,
                               std::string& reason) {
    uint32_t totalSize = 0;
    uint32_t structureOffset = 0;
    uint32_t stringsOffset = 0;
    uint32_t reservationOffset = 0;
    uint32_t structureSize = 0;
    uint32_t stringsSize = 0;
    if (!fdtHeader(data, size, totalSize, structureOffset, stringsOffset,
                   reservationOffset, structureSize, stringsSize, reason)) return false;
    size_t cursor = reservationOffset;
    bool reservationTerminated = false;
    while (cursor <= structureOffset && structureOffset - cursor >= 16U) {
        const uint64_t address = readBe64(data + cursor);
        const uint64_t length = readBe64(data + cursor + 8U);
        cursor += 16U;
        if (address == 0 && length == 0) {
            reservationTerminated = true;
            break;
        }
        uint64_t end = 0;
        if (!rangeEnd(address, length, end)) {
            setReason(reason, "FDT memory reservation contains an invalid range");
            return false;
        }
    }
    if (!reservationTerminated || cursor > structureOffset) {
        setReason(reason, "FDT memory reservation block is unterminated or overlaps structure");
        return false;
    }

    const size_t structureEnd = static_cast<size_t>(structureOffset) + structureSize;
    cursor = structureOffset;
    std::vector<bool> nodeOpen;
    bool rootSeen = false;
    bool endSeen = false;
    while (cursor < structureEnd) {
        if (structureEnd - cursor < 4U) {
            setReason(reason, "FDT structure ends inside a token");
            return false;
        }
        const uint32_t token = readBe32(data + cursor);
        cursor += 4U;
        if (token == kFdtBeginNode) {
            const void* terminator = std::memchr(data + cursor, 0, structureEnd - cursor);
            if (terminator == nullptr) {
                setReason(reason, "FDT node name is unterminated");
                return false;
            }
            const size_t nameLength = static_cast<const uint8_t*>(terminator) - (data + cursor);
            size_t next = 0;
            if (!checkedAlign4(cursor + nameLength + 1U, next) || next > structureEnd ||
                (!rootSeen && !nodeOpen.empty())) {
                setReason(reason, "FDT node name alignment or nesting is invalid");
                return false;
            }
            if (!rootSeen) rootSeen = true;
            nodeOpen.push_back(true);
            cursor = next;
        } else if (token == kFdtEndNode) {
            if (nodeOpen.empty()) {
                setReason(reason, "FDT contains an unmatched end-node token");
                return false;
            }
            nodeOpen.pop_back();
        } else if (token == kFdtProperty) {
            if (nodeOpen.empty() || structureEnd - cursor < 8U) {
                setReason(reason, "FDT property is outside a node or truncated");
                return false;
            }
            const uint32_t length = readBe32(data + cursor);
            const uint32_t nameOffset = readBe32(data + cursor + 4U);
            cursor += 8U;
            if (length > structureEnd - cursor || nameOffset >= stringsSize) {
                setReason(reason, "FDT property length or name offset is invalid");
                return false;
            }
            const size_t stringCursor = static_cast<size_t>(stringsOffset) + nameOffset;
            if (std::memchr(data + stringCursor, 0,
                            static_cast<size_t>(stringsOffset) + stringsSize - stringCursor) == nullptr) {
                setReason(reason, "FDT property name is unterminated");
                return false;
            }
            size_t next = 0;
            if (!checkedAlign4(cursor + length, next) || next > structureEnd) {
                setReason(reason, "FDT property value padding is invalid");
                return false;
            }
            cursor = next;
        } else if (token == kFdtEnd) {
            if (!rootSeen || !nodeOpen.empty() || cursor != structureEnd) {
                setReason(reason, "FDT end token appears before the root is complete");
                return false;
            }
            endSeen = true;
            break;
        } else {
            setReason(reason, "FDT contains an unknown structure token");
            return false;
        }
    }
    if (!endSeen) {
        setReason(reason, "FDT structure block has no final end token");
        return false;
    }
    return true;
}

} // namespace ia64::ppc64
