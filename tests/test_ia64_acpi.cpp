#include "IA64AcpiFirmware.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kTestMemorySize = 32ULL * 1024ULL * 1024ULL;
constexpr size_t kTestPageSize = 4096;
constexpr size_t kTestAcpiHeaderSize = 36;
constexpr size_t kTestRsdpSize = 36;
constexpr size_t kTestFadtSize = 244;
constexpr size_t kTestFacsSize = 64;

constexpr std::array<uint8_t, 16> kTestAcpi20Guid = {
    0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestGuestMemory final : public ia64::IMemory {
public:
    explicit TestGuestMemory(size_t size) : bytes_(size, 0) {}

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        checkRange(address, size);
        std::memcpy(dest, bytes_.data() + static_cast<size_t>(address), size);
    }

    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        checkRange(address, size);
        std::memcpy(bytes_.data() + static_cast<size_t>(address), src, size);
    }

    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override {
        Write(address, buffer, size);
    }

    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override {
        Write(address, buffer.data(), buffer.size());
    }

    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }

private:
    void checkRange(uint64_t address, size_t size) const {
        if (address > bytes_.size() || size > bytes_.size() - address) {
            throw std::out_of_range("test guest-memory range out of bounds");
        }
    }

    std::vector<uint8_t> bytes_;
};

uint16_t readLe16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t readLe32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

uint64_t readLe64(std::span<const uint8_t> bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    }
    return value;
}

void writeLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void writeLe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

uint8_t independentChecksum(std::span<const uint8_t> bytes) {
    uint8_t sum = 0;
    for (uint8_t byte : bytes) {
        sum = static_cast<uint8_t>(sum + byte);
    }
    return sum;
}

bool rangeInGuest(std::span<const uint8_t> ram, uint64_t address, size_t size) {
    return address <= ram.size() && size <= ram.size() - address;
}

std::span<const uint8_t> guestRange(std::span<const uint8_t> ram,
                                   uint64_t address,
                                   size_t size) {
    if (!rangeInGuest(ram, address, size)) {
        return {};
    }
    return ram.subspan(static_cast<size_t>(address), size);
}

bool tableHeader(std::span<const uint8_t> ram,
                 uint64_t address,
                 const char (&signature)[5],
                 size_t expectedLength,
                 uint8_t expectedRevision,
                 std::span<const uint8_t>* result = nullptr) {
    const auto header = guestRange(ram, address, kTestAcpiHeaderSize);
    if (header.empty() || std::memcmp(header.data(), signature, 4) != 0 ||
        readLe32(header, 4) != expectedLength || header[8] != expectedRevision) {
        return false;
    }
    const auto table = guestRange(ram, address, expectedLength);
    if (table.empty() || independentChecksum(table) != 0) {
        return false;
    }
    if (result) {
        *result = table;
    }
    return true;
}

void resetTableChecksum(std::vector<uint8_t>& ram, uint64_t address, size_t length) {
    ram[static_cast<size_t>(address) + 9] = 0;
    uint8_t sum = 0;
    for (size_t index = 0; index < length; ++index) {
        sum = static_cast<uint8_t>(sum + ram[static_cast<size_t>(address) + index]);
    }
    ram[static_cast<size_t>(address) + 9] = static_cast<uint8_t>(0U - sum);
}

struct ParsedTables {
    uint64_t rsdpAddress = 0;
    uint64_t rsdtAddress = 0;
    uint64_t xsdtAddress = 0;
    uint64_t madtAddress = 0;
    uint64_t fadtAddress = 0;
    uint64_t facsAddress = 0;
    uint64_t dsdtAddress = 0;
};

bool parsePublishedTables(std::span<const uint8_t> ram,
                          uint64_t configurationTableAddress,
                          ParsedTables* output = nullptr) {
    if (!rangeInGuest(ram, configurationTableAddress, 48)) {
        return false;
    }

    const size_t acpiEntryOffset =
        static_cast<size_t>(configurationTableAddress) + 24;
    if (!std::equal(kTestAcpi20Guid.begin(), kTestAcpi20Guid.end(),
                    ram.begin() + static_cast<std::ptrdiff_t>(acpiEntryOffset))) {
        return false;
    }
    ParsedTables parsed{};
    parsed.rsdpAddress = readLe64(ram, acpiEntryOffset + 16);
    const auto rsdp = guestRange(ram, parsed.rsdpAddress, kTestRsdpSize);
    if (rsdp.empty() || std::memcmp(rsdp.data(), "RSD PTR ", 8) != 0 ||
        rsdp[15] != 2 || readLe32(rsdp, 20) != kTestRsdpSize ||
        independentChecksum(rsdp.first(20)) != 0 ||
        independentChecksum(rsdp) != 0) {
        return false;
    }

    parsed.rsdtAddress = readLe32(rsdp, 16);
    parsed.xsdtAddress = readLe64(rsdp, 24);
    std::span<const uint8_t> rsdt;
    std::span<const uint8_t> xsdt;
    constexpr size_t rsdtLength = kTestAcpiHeaderSize + 2 * sizeof(uint32_t);
    constexpr size_t xsdtLength = kTestAcpiHeaderSize + 2 * sizeof(uint64_t);
    if (!tableHeader(ram, parsed.rsdtAddress, "RSDT", rsdtLength, 1, &rsdt) ||
        !tableHeader(ram, parsed.xsdtAddress, "XSDT", xsdtLength, 1, &xsdt) ||
        (rsdtLength - kTestAcpiHeaderSize) % sizeof(uint32_t) != 0 ||
        (xsdtLength - kTestAcpiHeaderSize) % sizeof(uint64_t) != 0) {
        return false;
    }

    std::array<uint64_t, 2> rsdtEntries{};
    std::array<uint64_t, 2> xsdtEntries{};
    for (size_t index = 0; index < 2; ++index) {
        rsdtEntries[index] = readLe32(rsdt, kTestAcpiHeaderSize + index * 4);
        xsdtEntries[index] = readLe64(xsdt, kTestAcpiHeaderSize + index * 8);
        if (rsdtEntries[index] == 0 ||
            rsdtEntries[index] != xsdtEntries[index] ||
            (index != 0 && rsdtEntries[index] == rsdtEntries[0])) {
            return false;
        }
    }

    bool sawMadt = false;
    bool sawFadt = false;
    for (uint64_t address : xsdtEntries) {
        const auto header = guestRange(ram, address, kTestAcpiHeaderSize);
        if (header.empty()) {
            return false;
        }
        if (std::memcmp(header.data(), "APIC", 4) == 0 && !sawMadt) {
            parsed.madtAddress = address;
            sawMadt = true;
        } else if (std::memcmp(header.data(), "FACP", 4) == 0 && !sawFadt) {
            parsed.fadtAddress = address;
            sawFadt = true;
        } else {
            return false;
        }
    }
    if (!sawMadt || !sawFadt) {
        return false;
    }

    std::span<const uint8_t> madt;
    constexpr size_t madtLength = kTestAcpiHeaderSize + 8 + 16;
    if (!tableHeader(ram, parsed.madtAddress, "APIC", madtLength, 1, &madt) ||
        readLe32(madt, 36) != 0xFEE00000U || readLe32(madt, 40) != 1U ||
        madt[44] != 7 || madt[45] != 16 || madt[46] != 0 ||
        madt[47] != 0 || madt[48] != 0 || readLe32(madt, 52) != 1 ||
        readLe32(madt, 56) != 0) {
        return false;
    }

    std::span<const uint8_t> fadt;
    if (!tableHeader(ram, parsed.fadtAddress, "FACP", kTestFadtSize, 3, &fadt)) {
        return false;
    }
    const uint32_t legacyFacs = readLe32(fadt, 36);
    const uint32_t legacyDsdt = readLe32(fadt, 40);
    parsed.facsAddress = readLe64(fadt, 132);
    parsed.dsdtAddress = readLe64(fadt, 140);
    if (legacyFacs == 0 || legacyDsdt == 0 ||
        parsed.facsAddress != legacyFacs || parsed.dsdtAddress != legacyDsdt ||
        parsed.facsAddress >= ram.size() || parsed.dsdtAddress >= ram.size() ||
        readLe16(fadt, 46) != 0 || readLe32(fadt, 48) != 0 ||
        readLe16(fadt, 109) != 0 || readLe32(fadt, 112) != 0) {
        return false;
    }
    for (size_t index = 44; index < 132; ++index) {
        if (fadt[index] != 0) {
            return false;
        }
    }
    for (size_t index = 148; index < fadt.size(); ++index) {
        if (fadt[index] != 0) {
            return false;
        }
    }

    const auto facs = guestRange(ram, parsed.facsAddress, kTestFacsSize);
    if (facs.empty() || std::memcmp(facs.data(), "FACS", 4) != 0 ||
        readLe32(facs, 4) != kTestFacsSize || readLe32(facs, 8) != 0 ||
        readLe32(facs, 12) != 0 || readLe32(facs, 16) != 0 ||
        readLe32(facs, 20) != 0 || readLe64(facs, 24) != 0 || facs[32] != 1) {
        return false;
    }
    for (size_t index = 33; index < facs.size(); ++index) {
        if (facs[index] != 0) {
            return false;
        }
    }

    std::span<const uint8_t> dsdt;
    constexpr size_t dsdtLength = kTestAcpiHeaderSize + 18;
    if (!tableHeader(ram, parsed.dsdtAddress, "DSDT", dsdtLength, 2, &dsdt) ||
        dsdt[36] != 0x08 || std::memcmp(dsdt.data() + 37, "OEM0", 4) != 0 ||
        dsdt[41] != 0x0D ||
        std::memcmp(dsdt.data() + 42, "guideX IA64", 11) != 0 ||
        dsdt[53] != 0) {
        return false;
    }
    if (std::memcmp(fadt.data() + 10, dsdt.data() + 10, 14) != 0 ||
        std::memcmp(fadt.data() + 10, madt.data() + 10, 14) != 0) {
        return false;
    }

    std::array<std::pair<uint64_t, uint64_t>, 7> allocations = {{
        {parsed.rsdpAddress, kTestRsdpSize},
        {parsed.rsdtAddress, rsdtLength},
        {parsed.xsdtAddress, xsdtLength},
        {parsed.madtAddress, madtLength},
        {parsed.fadtAddress, kTestFadtSize},
        {parsed.facsAddress, kTestFacsSize},
        {parsed.dsdtAddress, dsdtLength}
    }};
    std::sort(allocations.begin(), allocations.end());
    for (size_t index = 1; index < allocations.size(); ++index) {
        const auto [previousAddress, previousLength] = allocations[index - 1];
        const auto [address, length] = allocations[index];
        if (previousAddress > std::numeric_limits<uint64_t>::max() - previousLength ||
            address < previousAddress + previousLength ||
            !rangeInGuest(ram, address, static_cast<size_t>(length))) {
            return false;
        }
    }

    if (output) {
        *output = parsed;
    }
    return true;
}

std::vector<uint8_t> copyRam(const TestGuestMemory& memory) {
    return std::vector<uint8_t>(memory.GetRawData(),
                                memory.GetRawData() + memory.GetTotalSize());
}

void testProductionFirmwarePathAndIndependentReadback() {
    ia64::EfiHandoffLayout layout{};
    require(ia64::tryComputeEfiHandoffLayout(kTestMemorySize, layout),
            "32 MiB EFI layout must compute");
    require(layout.acpiReclaimBase % kTestPageSize == 0 &&
                layout.acpiNvsBase % kTestPageSize == 0 &&
                layout.acpiReclaimEnd - layout.acpiReclaimBase == kTestPageSize &&
                layout.acpiNvsEnd - layout.acpiNvsBase == kTestPageSize,
            "ACPI structures must use dedicated page-aligned reservations");

    const auto first = ia64::acpi::buildAcpiFirmwareTables(layout);
    const auto second = ia64::acpi::buildAcpiFirmwareTables(layout);
    require(first.has_value() && second.has_value(),
            "the production ACPI builder must create a complete table set");
    require(first->rsdp.bytes == second->rsdp.bytes &&
                first->rsdt.bytes == second->rsdt.bytes &&
                first->xsdt.bytes == second->xsdt.bytes &&
                first->madt.bytes == second->madt.bytes &&
                first->fadt.bytes == second->fadt.bytes &&
                first->facs.bytes == second->facs.bytes &&
                first->dsdt.bytes == second->dsdt.bytes,
            "production ACPI table bytes must be deterministic");

    TestGuestMemory memory(static_cast<size_t>(kTestMemorySize));
    std::vector<uint8_t> dirtyPage(kTestPageSize, 0xA5);
    memory.Write(layout.acpiReclaimBase, dirtyPage.data(), dirtyPage.size());
    memory.Write(layout.acpiNvsBase, dirtyPage.data(), dirtyPage.size());
    require(ia64::acpi::publishAcpiFirmwareTables(memory, layout),
            "the production firmware path must publish through guest memory");

    const std::span<const uint8_t> ram(memory.GetRawData(), memory.GetTotalSize());
    ParsedTables parsed{};
    require(parsePublishedTables(ram, layout.configurationTableAddr, &parsed),
            "independent discovery, structure, AML, pointer and checksum checks must pass");
    require(parsed.rsdpAddress == layout.rsdpAddr &&
                parsed.rsdtAddress == layout.rsdtAddr &&
                parsed.xsdtAddress == layout.xsdtAddr &&
                parsed.madtAddress == layout.madtAddr &&
                parsed.fadtAddress == layout.fadtAddr &&
                parsed.facsAddress == layout.facsAddr &&
                parsed.dsdtAddress == layout.dsdtAddr,
            "EFI discovery and every physical table pointer must resolve to the layout");

    require(ram[static_cast<size_t>(layout.acpiReclaimBase) + kTestPageSize - 1] == 0 &&
                ram[static_cast<size_t>(layout.acpiNvsBase) + kTestPageSize - 1] == 0,
            "the production path must clear stale page tails on reset/bootstrap");
    require(independentChecksum(guestRange(ram, layout.facsAddr, kTestFacsSize)) != 0,
            "the FACS fixture must not accidentally rely on a table checksum");

    const uint32_t guestGlobalLock = 1;
    memory.Write(layout.facsAddr + 16,
                 reinterpret_cast<const uint8_t*>(&guestGlobalLock),
                 sizeof(guestGlobalLock));
    std::array<uint8_t, sizeof(guestGlobalLock)> lockBytes{};
    memory.Read(layout.facsAddr + 16, lockBytes.data(), lockBytes.size());
    require(readLe32(lockBytes, 0) == guestGlobalLock,
            "guest-writable FACS global-lock storage must have RAM backing");
    const uint32_t unlocked = 0;
    memory.Write(layout.facsAddr + 16,
                 reinterpret_cast<const uint8_t*>(&unlocked), sizeof(unlocked));

    memory.Write(layout.rsdpAddr, reinterpret_cast<const uint8_t*>("INVALID!"), 8);
    TestGuestMemory secondVm(static_cast<size_t>(kTestMemorySize));
    require(ia64::acpi::publishAcpiFirmwareTables(secondVm, layout) &&
                ia64::acpi::publishAcpiFirmwareTables(memory, layout),
            "repeated bootstrap and a second VM must publish without shared mutable state");
    const std::span<const uint8_t> firstVmRam(memory.GetRawData(), memory.GetTotalSize());
    const std::span<const uint8_t> secondVmRam(secondVm.GetRawData(), secondVm.GetTotalSize());
    require(parsePublishedTables(firstVmRam, layout.configurationTableAddr) &&
                parsePublishedTables(secondVmRam, layout.configurationTableAddr),
            "reset and cross-VM publication must each expose a complete ACPI graph");
    require(std::equal(guestRange(firstVmRam, layout.acpiReclaimBase, kTestPageSize).begin(),
                       guestRange(firstVmRam, layout.acpiReclaimBase, kTestPageSize).end(),
                       guestRange(secondVmRam, layout.acpiReclaimBase, kTestPageSize).begin()) &&
                std::equal(guestRange(firstVmRam, layout.acpiNvsBase, kTestPageSize).begin(),
                           guestRange(firstVmRam, layout.acpiNvsBase, kTestPageSize).end(),
                           guestRange(secondVmRam, layout.acpiNvsBase, kTestPageSize).begin()),
            "reset and cross-VM table pages must be deterministic and independent");
    std::cout << "testProductionFirmwarePathAndIndependentReadback PASSED\n";
}

void testIndependentParserRejectsMalformedFirmware() {
    ia64::EfiHandoffLayout layout{};
    require(ia64::tryComputeEfiHandoffLayout(kTestMemorySize, layout),
            "32 MiB EFI layout must compute");
    TestGuestMemory memory(static_cast<size_t>(kTestMemorySize));
    require(ia64::acpi::publishAcpiFirmwareTables(memory, layout),
            "the baseline production tables must publish");
    const auto baseline = copyRam(memory);
    const auto validSpan = std::span<const uint8_t>(baseline);
    require(parsePublishedTables(validSpan, layout.configurationTableAddr),
            "baseline firmware must parse before negative cases");

    auto missingFadt = baseline;
    writeLe32(missingFadt, static_cast<size_t>(layout.rsdtAddr) + 40,
              static_cast<uint32_t>(layout.madtAddr));
    writeLe64(missingFadt, static_cast<size_t>(layout.xsdtAddr) + 44,
              layout.madtAddr);
    resetTableChecksum(missingFadt, layout.rsdtAddr, 44);
    resetTableChecksum(missingFadt, layout.xsdtAddr, 52);
    require(!parsePublishedTables(missingFadt, layout.configurationTableAddr),
            "the parser must reject a root chain without a unique FADT");

    auto zeroFacs = baseline;
    writeLe32(zeroFacs, static_cast<size_t>(layout.fadtAddr) + 36, 0);
    writeLe64(zeroFacs, static_cast<size_t>(layout.fadtAddr) + 132, 0);
    resetTableChecksum(zeroFacs, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(zeroFacs, layout.configurationTableAddr),
            "the parser must reject a zero FACS address");

    auto inconsistentPointer = baseline;
    writeLe64(inconsistentPointer, static_cast<size_t>(layout.fadtAddr) + 132,
              layout.facsAddr + 8);
    resetTableChecksum(inconsistentPointer, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(inconsistentPointer,
                                  layout.configurationTableAddr),
            "the parser must reject inconsistent legacy and extended pointers");

    auto unmappedFacs = baseline;
    const uint32_t outOfRam = static_cast<uint32_t>(kTestMemorySize + kTestPageSize);
    writeLe32(unmappedFacs, static_cast<size_t>(layout.fadtAddr) + 36, outOfRam);
    writeLe64(unmappedFacs, static_cast<size_t>(layout.fadtAddr) + 132, outOfRam);
    resetTableChecksum(unmappedFacs, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(unmappedFacs, layout.configurationTableAddr),
            "the parser must reject an unmapped FACS physical address");

    auto zeroDsdt = baseline;
    writeLe32(zeroDsdt, static_cast<size_t>(layout.fadtAddr) + 40, 0);
    writeLe64(zeroDsdt, static_cast<size_t>(layout.fadtAddr) + 140, 0);
    resetTableChecksum(zeroDsdt, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(zeroDsdt, layout.configurationTableAddr),
            "the parser must reject a zero DSDT address");

    auto badFadtLength = baseline;
    writeLe32(badFadtLength, static_cast<size_t>(layout.fadtAddr) + 4,
              static_cast<uint32_t>(kTestFadtSize - 1));
    resetTableChecksum(badFadtLength, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(badFadtLength, layout.configurationTableAddr),
            "the parser must reject an unsupported or truncated FADT length");

    auto badDsdtChecksum = baseline;
    badDsdtChecksum[static_cast<size_t>(layout.dsdtAddr) + 9] ^= 0x01;
    require(!parsePublishedTables(badDsdtChecksum, layout.configurationTableAddr),
            "the parser must reject a bad DSDT checksum");

    auto unsupportedRevision = baseline;
    unsupportedRevision[static_cast<size_t>(layout.fadtAddr) + 8] = 4;
    resetTableChecksum(unsupportedRevision, layout.fadtAddr, kTestFadtSize);
    require(!parsePublishedTables(unsupportedRevision,
                                  layout.configurationTableAddr),
            "the parser must reject a FADT revision outside the selected ABI");

    require(!parsePublishedTables(
                std::span<const uint8_t>(baseline.data(),
                    static_cast<size_t>(layout.dsdtAddr) + 1),
                layout.configurationTableAddr),
            "the parser must reject truncated guest-memory serialization");
    std::cout << "testIndependentParserRejectsMalformedFirmware PASSED\n";
}

void testBuilderRejectsUnsafeLayoutsAndAddresses() {
    ia64::EfiHandoffLayout layout{};
    require(ia64::tryComputeEfiHandoffLayout(kTestMemorySize, layout),
            "32 MiB EFI layout must compute");

    auto overlap = layout;
    overlap.dsdtAddr = overlap.fadtAddr + 1;
    require(!ia64::acpi::buildAcpiFirmwareTables(overlap),
            "the builder must reject overlapping table allocations");
    require(!ia64::acpi::buildFadt(0x100000000ULL, layout.dsdtAddr),
            "the FADT builder must not truncate addresses beyond 32 bits");
    const auto invalidRsdp = ia64::acpi::buildRsdp(0x100000000ULL,
                                                    layout.xsdtAddr);
    require(std::memcmp(invalidRsdp.bytes.data(), "RSD PTR ", 8) != 0,
            "the RSDP builder must reject a nonrepresentable RSDT address");
    std::cout << "testBuilderRejectsUnsafeLayoutsAndAddresses PASSED\n";
}

} // namespace

int main() {
    try {
        testProductionFirmwarePathAndIndependentReadback();
        testIndependentParserRejectsMalformedFirmware();
        testBuilderRejectsUnsafeLayoutsAndAddresses();
        std::cout << "All ACPI firmware tests PASSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IA-64 ACPI firmware test failure: " << error.what() << '\n';
        return 1;
    }
}
