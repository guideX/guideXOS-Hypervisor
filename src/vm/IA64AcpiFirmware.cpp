#include "IA64AcpiFirmware.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace ia64::acpi {
namespace {

constexpr uint32_t kFadtRevisionAcpi3 = 3;
constexpr uint8_t kDsdtRevisionAcpi2 = 2;

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
                      uint8_t revision) {
    std::memcpy(bytes.data(), signature, 4);
    put32(bytes, 4, length);
    bytes[8] = revision;
    bytes[9] = 0;
    std::memcpy(bytes.data() + 10, "guideX", 6);
    std::memcpy(bytes.data() + 16, "GUIDE   ", 8);
    put32(bytes, 24, 1);
    std::memcpy(bytes.data() + 28, "INTL", 4);
    put32(bytes, 32, 0x20140604U);
}

void finishChecksum(std::span<uint8_t> bytes) {
    bytes[9] = static_cast<uint8_t>(0U - checksum(bytes, bytes.size()));
}

bool fitsLegacyAddress(uint64_t address) {
    return address != 0 && address <= std::numeric_limits<uint32_t>::max();
}

bool containsRange(uint64_t regionStart,
                   uint64_t regionEnd,
                   uint64_t address,
                   uint64_t size) {
    return regionStart < regionEnd && address >= regionStart &&
           address <= regionEnd && size <= regionEnd - address;
}

bool rangesOverlap(uint64_t firstAddress,
                   uint64_t firstSize,
                   uint64_t secondAddress,
                   uint64_t secondSize) {
    return firstAddress < secondAddress + secondSize &&
           secondAddress < firstAddress + firstSize;
}

bool validLayout(const EfiHandoffLayout& layout) {
    if (layout.base >= layout.end ||
        layout.acpiReclaimBase % kEfiPageSize != 0 ||
        layout.acpiNvsBase % kEfiPageSize != 0 ||
        layout.acpiReclaimEnd <= layout.acpiReclaimBase ||
        layout.acpiNvsEnd <= layout.acpiNvsBase ||
        layout.acpiReclaimEnd - layout.acpiReclaimBase != kEfiAcpiReclaimSize ||
        layout.acpiNvsEnd - layout.acpiNvsBase != kEfiAcpiNvsSize ||
        layout.acpiReclaimBase < layout.base ||
        layout.acpiNvsEnd > layout.end ||
        !(layout.acpiReclaimEnd <= layout.acpiNvsBase ||
          layout.acpiNvsEnd <= layout.acpiReclaimBase)) {
        return false;
    }

    const std::array<std::pair<uint64_t, uint64_t>, 6> reclaimRanges = {{
        {layout.rsdpAddr, kRsdpSizeAcpi2},
        {layout.rsdtAddr, kRsdtSize},
        {layout.xsdtAddr, kXsdtSize},
        {layout.madtAddr, kMadtSize},
        {layout.fadtAddr, kFadtV3Size},
        {layout.dsdtAddr, kDsdtSize}
    }};
    for (size_t i = 0; i < reclaimRanges.size(); ++i) {
        const auto [address, size] = reclaimRanges[i];
        if (!containsRange(layout.acpiReclaimBase, layout.acpiReclaimEnd,
                           address, size)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (rangesOverlap(address, size,
                              reclaimRanges[j].first, reclaimRanges[j].second)) {
                return false;
            }
        }
    }
    return containsRange(layout.acpiNvsBase, layout.acpiNvsEnd,
                         layout.facsAddr, kFacsSize);
}

} // namespace

Rsdp buildRsdp(uint64_t rsdtAddress, uint64_t xsdtAddress) {
    if (!fitsLegacyAddress(rsdtAddress) || xsdtAddress == 0) {
        return {};
    }

    Rsdp rsdp{};
    auto bytes = std::span<uint8_t>(rsdp.bytes);

    constexpr std::array<uint8_t, 8> signature = {
        'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
    };
    std::copy(signature.begin(), signature.end(), bytes.begin());
    constexpr std::array<uint8_t, 6> oemId = {'g', 'u', 'i', 'd', 'e', 'X'};
    std::copy(oemId.begin(), oemId.end(), bytes.begin() + 9);
    bytes[15] = 2;
    put32(bytes, 16, static_cast<uint32_t>(rsdtAddress));
    put32(bytes, 20, static_cast<uint32_t>(kRsdpSizeAcpi2));
    put64(bytes, 24, xsdtAddress);
    bytes[8] = 0;
    bytes[32] = 0;
    bytes[8] = static_cast<uint8_t>(0U - checksum(bytes.first(20), 20));
    bytes[32] = static_cast<uint8_t>(0U - checksum(bytes, bytes.size()));
    return rsdp;
}

Rsdt buildRsdt(uint64_t madtAddress, uint64_t fadtAddress) {
    if (!fitsLegacyAddress(madtAddress) || !fitsLegacyAddress(fadtAddress)) {
        return {};
    }

    Rsdt rsdt{};
    auto bytes = std::span<uint8_t>(rsdt.bytes);
    writeTableHeader(bytes, "RSDT", static_cast<uint32_t>(bytes.size()), 1);
    put32(bytes, kAcpiTableHeaderSize, static_cast<uint32_t>(madtAddress));
    put32(bytes, kAcpiTableHeaderSize + 4, static_cast<uint32_t>(fadtAddress));
    finishChecksum(bytes);
    return rsdt;
}

Xsdt buildXsdt(uint64_t madtAddress, uint64_t fadtAddress) {
    if (madtAddress == 0 || fadtAddress == 0) {
        return {};
    }

    Xsdt xsdt{};
    auto bytes = std::span<uint8_t>(xsdt.bytes);
    writeTableHeader(bytes, "XSDT", static_cast<uint32_t>(bytes.size()), 1);
    put64(bytes, kAcpiTableHeaderSize, madtAddress);
    put64(bytes, kAcpiTableHeaderSize + 8, fadtAddress);
    finishChecksum(bytes);
    return xsdt;
}

Madt buildMadt(uint8_t acpiProcessorId,
               uint8_t sapicId,
               uint8_t sapicEid,
               uint32_t flags,
               uint32_t uid) {
    Madt madt{};
    auto bytes = std::span<uint8_t>(madt.bytes);

    writeTableHeader(bytes, "APIC", static_cast<uint32_t>(bytes.size()), 1);
    put32(bytes, 36, 0xFEE00000U);
    put32(bytes, 40, 1U);

    const size_t entry = kMadtHeaderSize;
    bytes[entry] = kMadtTypeProcessorLocalSapic;
    bytes[entry + 1] = kProcessorLocalSapicSize;
    bytes[entry + 2] = acpiProcessorId;
    bytes[entry + 3] = sapicId;
    bytes[entry + 4] = sapicEid;
    put32(bytes, entry + 8, flags);
    put32(bytes, entry + 12, uid);

    finishChecksum(bytes);
    return madt;
}

std::optional<Fadt> buildFadt(uint64_t facsAddress, uint64_t dsdtAddress) {
    if (!fitsLegacyAddress(facsAddress) || !fitsLegacyAddress(dsdtAddress)) {
        return std::nullopt;
    }

    Fadt fadt{};
    auto bytes = std::span<uint8_t>(fadt.bytes);
    writeTableHeader(bytes, "FACP", static_cast<uint32_t>(bytes.size()),
                     static_cast<uint8_t>(kFadtRevisionAcpi3));

    // The 32-bit and extended pointers intentionally agree. The virtual
    // platform keeps its firmware pages below 4 GiB and Linux v3.2 ACPICA
    // consumes these ACPI 3.0 FADT addresses directly.
    put32(bytes, 36, static_cast<uint32_t>(facsAddress));
    put32(bytes, 40, static_cast<uint32_t>(dsdtAddress));
    put64(bytes, 132, facsAddress);
    put64(bytes, 140, dsdtAddress);

    // The current IA-64 model has no ACPI fixed PM blocks, SCI, SMI command
    // port, reset register, or sleep-control hardware. The zero-initialized
    // fields leave each of those capabilities unadvertised.
    finishChecksum(bytes);
    return fadt;
}

Facs buildFacs() {
    Facs facs{};
    auto bytes = std::span<uint8_t>(facs.bytes);
    std::memcpy(bytes.data(), "FACS", 4);
    put32(bytes, 4, static_cast<uint32_t>(bytes.size()));
    // Hardware signature, waking vectors, global lock, flags, OSPM flags,
    // and reserved bytes start zero. ACPI 2.0 FACS version is 1. There is no
    // ACPI table header and no checksum in this structure.
    bytes[32] = 1;
    return facs;
}

Dsdt buildDsdt() {
    constexpr char kNamespaceString[] = "guideX IA64";

    Dsdt dsdt{};
    auto bytes = std::span<uint8_t>(dsdt.bytes);
    writeTableHeader(bytes, "DSDT", static_cast<uint32_t>(bytes.size()),
                     kDsdtRevisionAcpi2);

    // The exact AML corresponds to Name (OEM0, "guideX IA64") in
    // src/vm/IA64MinimalDsdt.asl. It declares one informational namespace
    // object and no devices, resources, methods, or hardware operations.
    bytes[kAcpiTableHeaderSize] = 0x08; // NameOp
    std::memcpy(bytes.data() + kAcpiTableHeaderSize + 1, "OEM0", 4);
    bytes[kAcpiTableHeaderSize + 5] = 0x0D; // StringPrefix
    std::memcpy(bytes.data() + kAcpiTableHeaderSize + 6,
                kNamespaceString, sizeof(kNamespaceString));

    finishChecksum(bytes);
    return dsdt;
}

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t rsdpAddress) {
    std::array<uint8_t, kEfiConfigurationTableEntrySize> entry{};
    std::copy(kAcpi20TableGuid.begin(), kAcpi20TableGuid.end(), entry.begin());
    put64(std::span<uint8_t>(entry), 16, rsdpAddress);
    return entry;
}

std::optional<AcpiFirmwareTables>
buildAcpiFirmwareTables(const EfiHandoffLayout& layout) {
    if (!validLayout(layout) || !fitsLegacyAddress(layout.rsdtAddr) ||
        !fitsLegacyAddress(layout.madtAddr) ||
        !fitsLegacyAddress(layout.fadtAddr) ||
        !fitsLegacyAddress(layout.facsAddr) ||
        !fitsLegacyAddress(layout.dsdtAddr)) {
        return std::nullopt;
    }

    const auto fadt = buildFadt(layout.facsAddr, layout.dsdtAddr);
    if (!fadt) {
        return std::nullopt;
    }

    AcpiFirmwareTables tables{};
    tables.rsdp = buildRsdp(layout.rsdtAddr, layout.xsdtAddr);
    tables.rsdt = buildRsdt(layout.madtAddr, layout.fadtAddr);
    tables.xsdt = buildXsdt(layout.madtAddr, layout.fadtAddr);
    tables.madt = buildMadt(0, 0, 0, kMadtFlagEnabled, 0);
    tables.fadt = *fadt;
    tables.facs = buildFacs();
    tables.dsdt = buildDsdt();
    return tables;
}

bool publishAcpiFirmwareTables(IMemory& memory, const EfiHandoffLayout& layout) {
    if (layout.end > memory.GetTotalSize() ||
        layout.configurationTableAddr > memory.GetTotalSize() ||
        kEfiConfigurationTableEntrySize * 2 >
            memory.GetTotalSize() - layout.configurationTableAddr) {
        return false;
    }

    const auto tables = buildAcpiFirmwareTables(layout);
    if (!tables) {
        return false;
    }

    std::array<uint8_t, kEfiPageSize> zeroPage{};
    memory.Write(layout.acpiReclaimBase, zeroPage.data(), zeroPage.size());
    memory.Write(layout.acpiNvsBase, zeroPage.data(), zeroPage.size());
    memory.Write(layout.rsdpAddr, tables->rsdp.bytes.data(), tables->rsdp.bytes.size());
    memory.Write(layout.rsdtAddr, tables->rsdt.bytes.data(), tables->rsdt.bytes.size());
    memory.Write(layout.xsdtAddr, tables->xsdt.bytes.data(), tables->xsdt.bytes.size());
    memory.Write(layout.madtAddr, tables->madt.bytes.data(), tables->madt.bytes.size());
    memory.Write(layout.fadtAddr, tables->fadt.bytes.data(), tables->fadt.bytes.size());
    memory.Write(layout.facsAddr, tables->facs.bytes.data(), tables->facs.bytes.size());
    memory.Write(layout.dsdtAddr, tables->dsdt.bytes.data(), tables->dsdt.bytes.size());

    const auto entry = buildEfiConfigurationTableEntry(layout.rsdpAddr);
    memory.Write(layout.configurationTableAddr + kEfiConfigurationTableEntrySize,
                 entry.data(), entry.size());
    return true;
}

} // namespace ia64::acpi
