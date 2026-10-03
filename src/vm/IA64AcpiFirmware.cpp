#include "IA64AcpiFirmware.h"

#include <algorithm>
#include <cstring>

namespace ia64::acpi {
namespace {

void put16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void put32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

void put64(std::span<uint8_t> bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

uint32_t get32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

uint8_t checksum(std::span<const uint8_t> bytes, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return sum;
}

void writeTableHeader(std::span<uint8_t> bytes,
                      const char (&signature)[5],
                      uint32_t length,
                      uint8_t revision,
                      const char* oemId,
                      const char* oemTableId) {
    std::memcpy(bytes.data(), signature, 4);
    put32(bytes, 4, length);
    bytes[8] = revision;
    bytes[9] = 0;
    std::memcpy(bytes.data() + 10, oemId, 6);
    std::memcpy(bytes.data() + 16, oemTableId, 8);
    put32(bytes, 24, 1);
    std::memcpy(bytes.data() + 28, "INTL", 4);
    put32(bytes, 32, 0x20140604U);
}

} // namespace

Rsdp buildRsdp(uint64_t rsdtAddress) {
    Rsdp rsdp{};
    auto bytes = std::span<uint8_t>(rsdp.bytes);

    constexpr std::array<uint8_t, 8> signature = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    std::copy(signature.begin(), signature.end(), bytes.begin());
    bytes[8] = 0;
    constexpr std::array<uint8_t, 6> oemId = {'g', 'u', 'i', 'd', 'e', 'X'};
    std::copy(oemId.begin(), oemId.end(), bytes.begin() + 9);
    bytes[15] = 2;
    put32(bytes, 16, static_cast<uint32_t>(rsdtAddress));
    put32(bytes, 20, static_cast<uint32_t>(kRsdpSizeAcpi2));
    put64(bytes, 24, rsdtAddress);
    bytes[32] = 0;
    bytes[33] = 0;
    bytes[34] = 0;
    bytes[35] = 0;

    const uint8_t sum = checksum(bytes, 20);
    bytes[8] = static_cast<uint8_t>(0U - sum);
    const uint8_t extSum = checksum(bytes, kRsdpSizeAcpi2);
    bytes[32] = static_cast<uint8_t>(0U - extSum);
    return rsdp;
}

Rsdt buildRsdt(uint64_t madtAddress) {
    Rsdt rsdt{};
    auto bytes = std::span<uint8_t>(rsdt.bytes);

    writeTableHeader(bytes, "RSDT", static_cast<uint32_t>(rsdt.bytes.size()), 1,
                     "guideX", "GUIDE   ");
    put32(bytes, kRsdtHeaderSize, static_cast<uint32_t>(madtAddress));

    const uint8_t sum = checksum(bytes, rsdt.bytes.size());
    bytes[9] = static_cast<uint8_t>(0U - sum);
    return rsdt;
}

Madt buildMadt(uint8_t acpiProcessorId,
               uint8_t sapicId,
               uint8_t sapicEid,
               uint32_t flags,
               uint32_t uid) {
    Madt madt{};
    auto bytes = std::span<uint8_t>(madt.bytes);

    writeTableHeader(bytes, "APIC", static_cast<uint32_t>(madt.bytes.size()), 1,
                     "guideX", "GUIDE   ");
    put32(bytes, 36, 0xFEE00000U);
    put32(bytes, 40, 1U);

    const size_t entry = kMadtHeaderSize;
    bytes[entry] = kMadtTypeProcessorLocalSapic;
    bytes[entry + 1] = kProcessorLocalSapicSize;
    bytes[entry + 2] = acpiProcessorId;
    bytes[entry + 3] = sapicId;
    bytes[entry + 4] = sapicEid;
    bytes[entry + 5] = 0;
    bytes[entry + 6] = 0;
    bytes[entry + 7] = 0;
    put32(bytes, entry + 8, flags);
    put32(bytes, entry + 12, uid);

    const uint8_t sum = checksum(bytes, madt.bytes.size());
    bytes[9] = static_cast<uint8_t>(0U - sum);
    return madt;
}

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t rsdpAddress) {
    std::array<uint8_t, kEfiConfigurationTableEntrySize> entry{};
    std::copy(kAcpi20TableGuid.begin(), kAcpi20TableGuid.end(), entry.begin());
    put64(std::span<uint8_t>(entry), 16, rsdpAddress);
    return entry;
}

bool validateAcpiTables(std::span<const uint8_t> rsdpBytes,
                        std::span<const uint8_t> rsdtBytes,
                        std::span<const uint8_t> madtBytes,
                        AcpiValidation* validation) {
    AcpiValidation parsed{};
    if (rsdpBytes.size() < kRsdpSizeAcpi2 ||
        std::memcmp(rsdpBytes.data(), "RSD PTR ", 8) != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.rsdpLength = get32(rsdpBytes, 20);
    parsed.rsdpChecksum = rsdpBytes[8];
    if (parsed.rsdpLength != kRsdpSizeAcpi2) {
        if (validation) *validation = parsed;
        return false;
    }

    const uint8_t rsdpSum = checksum(rsdpBytes, 20);
    if (rsdpSum != 0) {
        if (validation) *validation = parsed;
        return false;
    }
    const uint8_t rsdpExtSum = checksum(rsdpBytes, parsed.rsdpLength);
    if (rsdpExtSum != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    if (rsdtBytes.size() < kRsdtHeaderSize ||
        std::memcmp(rsdtBytes.data(), "RSDT", 4) != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.rsdtLength = get32(rsdtBytes, 4);
    parsed.rsdtChecksum = rsdtBytes[9];
    if (parsed.rsdtLength < kRsdtHeaderSize + 4 ||
        parsed.rsdtLength > rsdtBytes.size()) {
        if (validation) *validation = parsed;
        return false;
    }

    const uint8_t rsdtSum = checksum(rsdtBytes, parsed.rsdtLength);
    if (rsdtSum != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    if (madtBytes.size() < kMadtHeaderSize + kProcessorLocalSapicSize ||
        std::memcmp(madtBytes.data(), "APIC", 4) != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.madtLength = get32(madtBytes, 4);
    parsed.madtChecksum = madtBytes[9];
    parsed.madtRevision = madtBytes[8];
    if (parsed.madtLength < kMadtHeaderSize + kProcessorLocalSapicSize ||
        parsed.madtLength > madtBytes.size()) {
        if (validation) *validation = parsed;
        return false;
    }

    const uint8_t madtSum = checksum(madtBytes, parsed.madtLength);
    if (madtSum != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    const size_t entry = kMadtHeaderSize;
    if (madtBytes[entry] != kMadtTypeProcessorLocalSapic ||
        madtBytes[entry + 1] != kProcessorLocalSapicSize) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.processorCount = 1;
    parsed.sapicId = madtBytes[entry + 3];
    parsed.sapicEid = madtBytes[entry + 4];
    parsed.enabled = (get32(madtBytes, entry + 8) & kMadtFlagEnabled) != 0;
    parsed.valid = parsed.enabled;
    if (validation) *validation = parsed;
    return parsed.valid;
}

} // namespace ia64::acpi
