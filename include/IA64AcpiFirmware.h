#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace ia64::acpi {

constexpr size_t kRsdpSizeAcpi2 = 36;
constexpr size_t kRsdtHeaderSize = 36;
constexpr size_t kMadtHeaderSize = 44;
constexpr size_t kProcessorLocalSapicSize = 16;
constexpr size_t kEfiConfigurationTableEntrySize = 24;

constexpr uint8_t kMadtTypeProcessorLocalSapic = 0;
constexpr uint32_t kMadtFlagEnabled = 1U;

constexpr std::array<uint8_t, 16> kAcpi20TableGuid = {
    0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
};

struct Rsdp {
    std::array<uint8_t, kRsdpSizeAcpi2> bytes{};
};

struct Rsdt {
    std::array<uint8_t, kRsdtHeaderSize + 4> bytes{};
};

struct Madt {
    std::array<uint8_t, kMadtHeaderSize + kProcessorLocalSapicSize> bytes{};
};

struct AcpiValidation {
    bool valid = false;
    uint32_t rsdpLength = 0;
    uint8_t rsdpChecksum = 0;
    uint32_t rsdtLength = 0;
    uint8_t rsdtChecksum = 0;
    uint32_t madtLength = 0;
    uint8_t madtChecksum = 0;
    uint8_t madtRevision = 0;
    uint8_t processorCount = 0;
    uint8_t sapicId = 0;
    uint8_t sapicEid = 0;
    bool enabled = false;
};

Rsdp buildRsdp(uint64_t rsdtAddress);

Rsdt buildRsdt(uint64_t madtAddress);

Madt buildMadt(uint8_t acpiProcessorId,
               uint8_t sapicId,
               uint8_t sapicEid,
               uint32_t flags,
               uint32_t uid);

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t rsdpAddress);

bool validateAcpiTables(std::span<const uint8_t> rsdpBytes,
                        std::span<const uint8_t> rsdtBytes,
                        std::span<const uint8_t> madtBytes,
                        AcpiValidation* validation = nullptr);

} // namespace ia64::acpi
