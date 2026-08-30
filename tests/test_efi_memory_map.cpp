#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "IA64EfiHandoffLayout.h"

namespace {

constexpr uint32_t kEfiConventionalMemory = 7;
constexpr uint32_t kEfiReservedMemory = 0;
constexpr uint64_t kEfiMemoryDescriptorVersion = 1;
constexpr uint64_t kEfiPageSize = 4096;

// GNU-EFI 3.0e's IA-64 definition: Type + Pad + four 64-bit fields.
struct HistoricalEfiMemoryDescriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physicalStart;
    uint64_t virtualStart;
    uint64_t numberOfPages;
    uint64_t attribute;
};

static_assert(sizeof(HistoricalEfiMemoryDescriptor) == 40);
static_assert(alignof(HistoricalEfiMemoryDescriptor) == 8);
static_assert(offsetof(HistoricalEfiMemoryDescriptor, type) == 0);
static_assert(offsetof(HistoricalEfiMemoryDescriptor, physicalStart) == 8);
static_assert(offsetof(HistoricalEfiMemoryDescriptor, virtualStart) == 16);
static_assert(offsetof(HistoricalEfiMemoryDescriptor, numberOfPages) == 24);
static_assert(offsetof(HistoricalEfiMemoryDescriptor, attribute) == 32);

struct MapEntry {
    uint32_t type;
    uint64_t physicalStart;
    uint64_t numberOfPages;
    uint64_t attribute;
};

uint64_t checkedEnd(const MapEntry& entry) {
    if (entry.numberOfPages > (std::numeric_limits<uint64_t>::max() -
                               entry.physicalStart) / kEfiPageSize) {
        throw std::runtime_error("descriptor extent overflows");
    }
    return entry.physicalStart + entry.numberOfPages * kEfiPageSize;
}

uint64_t roundUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        value > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        throw std::runtime_error("invalid alignment");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

// The relevant historical ELILO search, kept generic for regression use.
uint64_t findKernelMemory(const std::vector<MapEntry>& map,
                          uint64_t lowAddress,
                          uint64_t maxAddress,
                          uint64_t alignment) {
    if (maxAddress < lowAddress) {
        throw std::runtime_error("invalid requested range");
    }
    const uint64_t size = maxAddress - lowAddress;
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (const MapEntry& entry : map) {
        if (entry.type != kEfiConventionalMemory) {
            continue;
        }
        const uint64_t end = checkedEnd(entry);
        const uint64_t address = roundUp(entry.physicalStart, alignment);
        if (address < best && address < end &&
            size <= end - address) {
            best = address;
        }
    }
    if (best == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("no suitable conventional descriptor");
    }
    return best;
}

void putLe(uint8_t* bytes, size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testHistoricalDescriptorAbiAndStride() {
    require(kEfiMemoryDescriptorVersion == 1,
            "historical descriptor version must be one");

    std::array<uint8_t, sizeof(HistoricalEfiMemoryDescriptor) * 2> bytes{};
    putLe(bytes.data(), 0, kEfiConventionalMemory, 4);
    putLe(bytes.data(), 8, 0x00100000, 8);
    putLe(bytes.data(), 24, 0x100, 8);
    putLe(bytes.data(), 32, 0xF, 8);

    std::array<uint8_t, sizeof(HistoricalEfiMemoryDescriptor)> second{};
    putLe(second.data(), 0, kEfiReservedMemory, 4);
    putLe(second.data(), 8, 0x00200000, 8);
    std::memcpy(bytes.data() + sizeof(HistoricalEfiMemoryDescriptor),
                second.data(), second.size());

    HistoricalEfiMemoryDescriptor first{};
    HistoricalEfiMemoryDescriptor next{};
    std::memcpy(&first, bytes.data(), sizeof(first));
    std::memcpy(&next, bytes.data() + sizeof(first), sizeof(next));
    require(first.type == kEfiConventionalMemory,
            "first descriptor type must be conventional");
    require(first.physicalStart == 0x00100000 &&
                first.virtualStart == 0 &&
                first.numberOfPages == 0x100 && first.attribute == 0xF,
            "historical descriptor fields must use the documented offsets");
    require(next.type == kEfiReservedMemory &&
                next.physicalStart == 0x00200000,
            "descriptor iteration must use the returned descriptor stride");
}

void testGenericEliloSearchAndMapInvariants() {
    const std::vector<MapEntry> map = {
        {kEfiReservedMemory, 0x00000000, 0x100, 0xF},
        {kEfiConventionalMemory, 0x00100000, 0x1000, 0xF},
        {kEfiReservedMemory, 0x01100000, 0x100, 0xF},
    };

    uint64_t previousEnd = 0;
    for (size_t index = 0; index < map.size(); ++index) {
        const uint64_t end = checkedEnd(map[index]);
        require(index == 0 || map[index].physicalStart >= previousEnd,
                "memory descriptors must be ordered and non-overlapping");
        require(end > map[index].physicalStart,
                "memory descriptor must have a positive extent");
        previousEnd = end;
    }

    const uint64_t requestedLow = 0x00123456;
    const uint64_t requestedHigh = 0x002d4567;
    const uint64_t alignment = 0x00200000;
    const uint64_t selected = findKernelMemory(
        map, requestedLow, requestedHigh, alignment);
    require(selected == 0x00200000,
            "ELILO-style search must select the aligned conventional range");
    require((selected & (alignment - 1)) == 0,
            "selected range must satisfy the requested alignment");
    require(selected + (requestedHigh - requestedLow) <= checkedEnd(map[1]),
            "selected range must remain inside its source descriptor");

    auto wrongType = map;
    wrongType[1].type = kEfiReservedMemory;
    bool rejectedWrongType = false;
    try {
        (void)findKernelMemory(wrongType, requestedLow, requestedHigh, alignment);
    } catch (const std::runtime_error&) {
        rejectedWrongType = true;
    }
    require(rejectedWrongType,
            "ELILO-style search must reject non-conventional memory");

    auto tooSmall = map;
    tooSmall[1].numberOfPages = 0x200;
    bool rejectedExtent = false;
    try {
        (void)findKernelMemory(tooSmall, requestedLow, 0x00900000, alignment);
    } catch (const std::runtime_error&) {
        rejectedExtent = true;
    }
    require(rejectedExtent,
            "ELILO-style search must reject a range beyond descriptor end");
}

void testTextOutputQueryModeLayout() {
    ia64::EfiHandoffLayout layout{};
    require(ia64::tryComputeEfiHandoffLayout(512ULL * 1024ULL * 1024ULL, layout),
            "512 MiB EFI handoff layout must be computable");
    require(layout.textOutputQueryModeStubCodeAddr != 0 &&
                layout.textOutputQueryModeStubDescAddr != 0,
            "QueryMode stub addresses must be populated");
    require(layout.textOutputQueryModeStubCodeAddr != layout.textOutputQueryModeStubDescAddr,
            "QueryMode code and descriptor must be distinct");
    require(layout.textOutputQueryModeStubCodeAddr >= layout.base &&
                layout.textOutputQueryModeStubDescAddr < layout.end,
            "QueryMode stub must remain inside the EFI handoff region");
}

} // namespace

int main() {
    try {
        testHistoricalDescriptorAbiAndStride();
        testGenericEliloSearchAndMapInvariants();
        testTextOutputQueryModeLayout();
        std::cout << "EFI memory-map tests passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "EFI memory-map test failed: " << error.what() << std::endl;
        return 1;
    }
}
