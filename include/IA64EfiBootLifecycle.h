#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace ia64 {

constexpr size_t kEfiSystemTableMinimumSize = 120;
constexpr size_t kEfiSystemTableHeaderSizeOffset = 12;
constexpr size_t kEfiSystemTableCrc32Offset = 16;

constexpr uint32_t readEfiLe32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

constexpr void writeEfiLe32(std::span<uint8_t> bytes,
                            size_t offset,
                            uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

constexpr uint32_t calculateEfiCrc32(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

constexpr bool updateEfiSystemTableCrc(std::span<uint8_t> systemTable) {
    if (systemTable.size() < 24) {
        return false;
    }
    const uint32_t headerSize =
        readEfiLe32(systemTable, kEfiSystemTableHeaderSizeOffset);
    if (headerSize < 24 || headerSize > systemTable.size()) {
        return false;
    }
    writeEfiLe32(systemTable, kEfiSystemTableCrc32Offset, 0);
    const uint32_t crc = calculateEfiCrc32(
        std::span<const uint8_t>(systemTable.data(), headerSize));
    writeEfiLe32(systemTable, kEfiSystemTableCrc32Offset, crc);
    return true;
}

// UEFI 2.10 requires these EFI_SYSTEM_TABLE fields to be NULL after a
// successful ExitBootServices call. RuntimeServices and the configuration
// table remain available to the operating system.
constexpr bool prepareEfiSystemTableForExit(std::span<uint8_t> systemTable) {
    if (systemTable.size() < kEfiSystemTableMinimumSize) {
        return false;
    }
    const uint32_t headerSize =
        readEfiLe32(systemTable, kEfiSystemTableHeaderSizeOffset);
    if (headerSize < kEfiSystemTableMinimumSize ||
        headerSize > systemTable.size()) {
        return false;
    }
    for (size_t offset : {size_t{40}, size_t{48}, size_t{56}, size_t{64},
                          size_t{72}, size_t{80}, size_t{96}}) {
        for (size_t index = 0; index < sizeof(uint64_t); ++index) {
            systemTable[offset + index] = 0;
        }
    }
    return updateEfiSystemTableCrc(systemTable);
}

// ExitBootServices is accepted only while the boot-services phase is active,
// after the current memory map has been prepared, and with the image handle
// and map key supplied by the guest matching the live firmware state.
constexpr bool isEfiExitBootServicesRequestValid(
    uint64_t imageHandle,
    uint64_t expectedImageHandle,
    uint64_t mapKey,
    uint64_t currentMapKey,
    bool memoryMapAvailable,
    bool bootServicesAlreadyExited) {
    return memoryMapAvailable && !bootServicesAlreadyExited &&
           imageHandle == expectedImageHandle && mapKey == currentMapKey;
}

constexpr bool areEfiBootServicesAvailable(bool bootServicesAlreadyExited) {
    return !bootServicesAlreadyExited;
}

} // namespace ia64
