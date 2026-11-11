#pragma once

#include "IA64EfiHandoffLayout.h"
#include "IMemory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ia64::acpi {

constexpr size_t kRsdpSizeAcpi2 = 36;
constexpr size_t kAcpiTableHeaderSize = 36;
constexpr size_t kRsdtEntryCount = 2;
constexpr size_t kXsdtEntryCount = 2;
constexpr size_t kRsdtSize = kAcpiTableHeaderSize + 4 * kRsdtEntryCount;
constexpr size_t kXsdtSize = kAcpiTableHeaderSize + 8 * kXsdtEntryCount;
constexpr size_t kMadtHeaderSize = 44;
constexpr size_t kProcessorLocalSapicSize = 16;
constexpr size_t kMadtSize = kMadtHeaderSize + kProcessorLocalSapicSize;
constexpr size_t kFadtV3Size = 244;
constexpr size_t kFacsSize = 64;
constexpr size_t kDsdtAmlSize = 18;
constexpr size_t kDsdtSize = kAcpiTableHeaderSize + kDsdtAmlSize;
constexpr size_t kEfiConfigurationTableEntrySize = 24;

// ACPI MADT subtable type 7 is Processor Local SAPIC, the IA-64 processor
// interrupt-controller record. Type 0 is the x86 Processor Local APIC record.
constexpr uint8_t kMadtTypeProcessorLocalSapic = 7;
constexpr uint32_t kMadtFlagEnabled = 1U;

constexpr std::array<uint8_t, 16> kAcpi20TableGuid = {
    0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
};

struct Rsdp {
    std::array<uint8_t, kRsdpSizeAcpi2> bytes{};
};

struct Rsdt {
    std::array<uint8_t, kRsdtSize> bytes{};
};

struct Xsdt {
    std::array<uint8_t, kXsdtSize> bytes{};
};

struct Madt {
    std::array<uint8_t, kMadtSize> bytes{};
};

struct Fadt {
    std::array<uint8_t, kFadtV3Size> bytes{};
};

// FACS is not an ACPI table-header structure and has no checksum.
struct Facs {
    std::array<uint8_t, kFacsSize> bytes{};
};

struct Dsdt {
    std::array<uint8_t, kDsdtSize> bytes{};
};

struct AcpiFirmwareTables {
    Rsdp rsdp;
    Rsdt rsdt;
    Xsdt xsdt;
    Madt madt;
    Fadt fadt;
    Facs facs;
    Dsdt dsdt;
};

Rsdp buildRsdp(uint64_t rsdtAddress, uint64_t xsdtAddress);
Rsdt buildRsdt(uint64_t madtAddress, uint64_t fadtAddress);
Xsdt buildXsdt(uint64_t madtAddress, uint64_t fadtAddress);
Madt buildMadt(uint8_t acpiProcessorId,
               uint8_t sapicId,
               uint8_t sapicEid,
               uint32_t flags,
               uint32_t uid);
std::optional<Fadt> buildFadt(uint64_t facsAddress, uint64_t dsdtAddress);
Facs buildFacs();
Dsdt buildDsdt();

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t rsdpAddress);

std::optional<AcpiFirmwareTables>
buildAcpiFirmwareTables(const EfiHandoffLayout& layout);

// This is the firmware publication path used by both IA-64 EFI boot routes.
// It clears the two dedicated ACPI pages, writes every generated structure,
// then publishes the ACPI 2.0 EFI configuration-table entry.
bool publishAcpiFirmwareTables(IMemory& memory, const EfiHandoffLayout& layout);

} // namespace ia64::acpi
