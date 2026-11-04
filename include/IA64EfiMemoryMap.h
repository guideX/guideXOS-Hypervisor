#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ia64 {

constexpr uint32_t kEfiPalCodeMemoryType = 13;
constexpr uint64_t kEfiPalMemoryPageSize = 0x1000ULL;
constexpr uint64_t kEfiMemoryWriteBack = 0x8ULL;

struct EfiMemoryDescriptor {
    uint32_t type = 0;
    uint64_t physicalStart = 0;
    uint64_t numberOfPages = 0;
    uint64_t attributes = 0;
};

enum class EfiPalDescriptorScan {
    Missing,
    Found,
    Duplicate,
    Malformed
};

inline bool makeEfiPalCodeDescriptor(uint64_t palEntry,
                                     uint64_t handoffStart,
                                     uint64_t handoffEnd,
                                     uint64_t attributes,
                                     EfiMemoryDescriptor& descriptor) {
    const uint64_t pageSize = kEfiPalMemoryPageSize;
    if (handoffStart >= handoffEnd || handoffEnd - handoffStart < pageSize ||
        (handoffStart & (pageSize - 1ULL)) != 0 ||
        (handoffEnd & (pageSize - 1ULL)) != 0 ||
        palEntry < handoffStart || palEntry > handoffEnd - 16ULL ||
        palEntry > std::numeric_limits<uint64_t>::max() - 16ULL ||
        (palEntry & 0xFULL) != 0 ||
        (attributes & kEfiMemoryWriteBack) == 0) {
        return false;
    }

    const uint64_t pageStart = palEntry & ~(pageSize - 1ULL);
    if (pageStart < handoffStart || pageStart > handoffEnd - pageSize ||
        palEntry + 16ULL > pageStart + pageSize) {
        return false;
    }

    descriptor = {kEfiPalCodeMemoryType, pageStart, 1, attributes};
    return true;
}

inline bool overlayEfiMemoryMapRange(std::vector<EfiMemoryDescriptor>& map,
                                     uint64_t physicalLimit,
                                     uint64_t requestedStart,
                                     uint64_t requestedEnd,
                                     uint32_t type,
                                     uint64_t attributes) {
    constexpr uint64_t pageSize = kEfiPalMemoryPageSize;
    if (physicalLimit == 0 || (physicalLimit & (pageSize - 1ULL)) != 0 ||
        requestedStart >= requestedEnd || requestedEnd > physicalLimit ||
        requestedEnd > std::numeric_limits<uint64_t>::max() - (pageSize - 1ULL)) {
        return false;
    }

    const uint64_t start = requestedStart & ~(pageSize - 1ULL);
    const uint64_t end = (requestedEnd + pageSize - 1ULL) & ~(pageSize - 1ULL);
    if (end > physicalLimit || map.empty()) {
        return false;
    }

    std::vector<EfiMemoryDescriptor> replacement;
    replacement.reserve(map.size() + 2);
    uint64_t previousEnd = 0;
    bool first = true;
    uint64_t coveredThrough = start;

    for (const EfiMemoryDescriptor& descriptor : map) {
        if (descriptor.numberOfPages == 0 ||
            (descriptor.physicalStart & (pageSize - 1ULL)) != 0 ||
            descriptor.numberOfPages >
                (std::numeric_limits<uint64_t>::max() - descriptor.physicalStart) / pageSize) {
            return false;
        }
        const uint64_t descriptorEnd = descriptor.physicalStart +
                                       descriptor.numberOfPages * pageSize;
        if (descriptorEnd > physicalLimit ||
            (!first && descriptor.physicalStart < previousEnd)) {
            return false;
        }
        first = false;
        previousEnd = descriptorEnd;

        if (descriptorEnd <= start || descriptor.physicalStart >= end) {
            replacement.push_back(descriptor);
            continue;
        }

        const uint64_t markedStart = std::max(descriptor.physicalStart, start);
        const uint64_t markedEnd = std::min(descriptorEnd, end);
        if (markedStart != coveredThrough) {
            return false;
        }

        if (descriptor.physicalStart < markedStart) {
            replacement.push_back({
                descriptor.type,
                descriptor.physicalStart,
                (markedStart - descriptor.physicalStart) / pageSize,
                descriptor.attributes
            });
        }
        replacement.push_back({type,
                               markedStart,
                               (markedEnd - markedStart) / pageSize,
                               attributes});
        if (markedEnd < descriptorEnd) {
            replacement.push_back({
                descriptor.type,
                markedEnd,
                (descriptorEnd - markedEnd) / pageSize,
                descriptor.attributes
            });
        }
        coveredThrough = markedEnd;
    }

    if (coveredThrough != end) {
        return false;
    }

    std::vector<EfiMemoryDescriptor> merged;
    merged.reserve(replacement.size());
    for (const EfiMemoryDescriptor& descriptor : replacement) {
        if (!merged.empty()) {
            EfiMemoryDescriptor& previous = merged.back();
            const uint64_t previousEnd = previous.physicalStart +
                                         previous.numberOfPages * pageSize;
            if (previous.type == descriptor.type &&
                previous.attributes == descriptor.attributes &&
                previousEnd == descriptor.physicalStart) {
                previous.numberOfPages += descriptor.numberOfPages;
                continue;
            }
        }
        merged.push_back(descriptor);
    }

    map.swap(merged);
    return true;
}

inline EfiPalDescriptorScan findUniqueEfiPalCodeDescriptor(
    std::span<const EfiMemoryDescriptor> map,
    EfiMemoryDescriptor* result = nullptr) {
    const EfiMemoryDescriptor* match = nullptr;
    for (const EfiMemoryDescriptor& descriptor : map) {
        if (descriptor.type != kEfiPalCodeMemoryType) {
            continue;
        }
        if (match != nullptr) {
            return EfiPalDescriptorScan::Duplicate;
        }
        match = &descriptor;
    }

    if (match == nullptr) {
        return EfiPalDescriptorScan::Missing;
    }

    if (match->numberOfPages == 0 ||
        (match->physicalStart & (kEfiPalMemoryPageSize - 1ULL)) != 0 ||
        (match->attributes & kEfiMemoryWriteBack) == 0 ||
        match->numberOfPages >
            (std::numeric_limits<uint64_t>::max() - match->physicalStart) /
                kEfiPalMemoryPageSize) {
        return EfiPalDescriptorScan::Malformed;
    }

    if (result != nullptr) {
        *result = *match;
    }
    return EfiPalDescriptorScan::Found;
}

inline bool isLinuxIa64AvailableEfiMemory(const EfiMemoryDescriptor& descriptor) {
    if ((descriptor.attributes & kEfiMemoryWriteBack) == 0) {
        return false;
    }
    return descriptor.type == 1 || descriptor.type == 2 ||
           descriptor.type == 3 || descriptor.type == 4 ||
           descriptor.type == 7;
}

} // namespace ia64
