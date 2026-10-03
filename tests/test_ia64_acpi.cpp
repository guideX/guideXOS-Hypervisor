#include "IA64AcpiFirmware.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <span>

void testRsdp() {
    const uint64_t rsdtAddr = 0x1000;
    const auto rsdp = ia64::acpi::buildRsdp(rsdtAddr);
    assert(rsdp.bytes.size() == ia64::acpi::kRsdpSizeAcpi2);
    assert(std::memcmp(rsdp.bytes.data(), "RSD PTR ", 8) == 0);

    uint32_t rsdtEncoded = 0;
    for (size_t i = 0; i < 4; ++i) {
        rsdtEncoded |= static_cast<uint32_t>(rsdp.bytes[16 + i]) << (i * 8);
    }
    assert(rsdtEncoded == static_cast<uint32_t>(rsdtAddr));

    uint8_t sum = 0;
    for (size_t i = 0; i < 20; ++i) {
        sum = static_cast<uint8_t>(sum + rsdp.bytes[i]);
    }
    assert(sum == 0);

    uint8_t extSum = 0;
    for (size_t i = 0; i < ia64::acpi::kRsdpSizeAcpi2; ++i) {
        extSum = static_cast<uint8_t>(extSum + rsdp.bytes[i]);
    }
    assert(extSum == 0);
    std::cout << "testRsdp PASSED\n";
}

void testRsdt() {
    const uint64_t madtAddr = 0x2000;
    const auto rsdt = ia64::acpi::buildRsdt(madtAddr);
    assert(rsdt.bytes.size() == ia64::acpi::kRsdtHeaderSize + 4);
    assert(std::memcmp(rsdt.bytes.data(), "RSDT", 4) == 0);

    uint32_t madtEncoded = 0;
    for (size_t i = 0; i < 4; ++i) {
        madtEncoded |= static_cast<uint32_t>(rsdt.bytes[ia64::acpi::kRsdtHeaderSize + i]) << (i * 8);
    }
    assert(madtEncoded == static_cast<uint32_t>(madtAddr));

    uint8_t sum = 0;
    for (size_t i = 0; i < rsdt.bytes.size(); ++i) {
        sum = static_cast<uint8_t>(sum + rsdt.bytes[i]);
    }
    assert(sum == 0);
    std::cout << "testRsdt PASSED\n";
}

void testMadt() {
    const auto madt = ia64::acpi::buildMadt(0, 0, 0x40,
                                            ia64::acpi::kMadtFlagEnabled, 0);
    assert(madt.bytes.size() == ia64::acpi::kMadtHeaderSize + ia64::acpi::kProcessorLocalSapicSize);
    assert(std::memcmp(madt.bytes.data(), "APIC", 4) == 0);

    const size_t entry = ia64::acpi::kMadtHeaderSize;
    assert(madt.bytes[entry] == ia64::acpi::kMadtTypeProcessorLocalSapic);
    assert(madt.bytes[entry + 1] == ia64::acpi::kProcessorLocalSapicSize);
    assert(madt.bytes[entry + 2] == 0);
    assert(madt.bytes[entry + 3] == 0);
    assert(madt.bytes[entry + 4] == 0x40);

    uint32_t flags = 0;
    for (size_t i = 0; i < 4; ++i) {
        flags |= static_cast<uint32_t>(madt.bytes[entry + 8 + i]) << (i * 8);
    }
    assert((flags & ia64::acpi::kMadtFlagEnabled) != 0);

    uint8_t sum = 0;
    for (size_t i = 0; i < madt.bytes.size(); ++i) {
        sum = static_cast<uint8_t>(sum + madt.bytes[i]);
    }
    assert(sum == 0);
    std::cout << "testMadt PASSED\n";
}

void testEfiConfigEntry() {
    const uint64_t rsdpAddr = 0x3000;
    const auto entry = ia64::acpi::buildEfiConfigurationTableEntry(rsdpAddr);
    assert(entry.size() == ia64::acpi::kEfiConfigurationTableEntrySize);

    uint64_t addr = 0;
    for (size_t i = 0; i < 8; ++i) {
        addr |= static_cast<uint64_t>(entry[16 + i]) << (i * 8);
    }
    assert(addr == rsdpAddr);
    std::cout << "testEfiConfigEntry PASSED\n";
}

void testValidate() {
    const auto rsdp = ia64::acpi::buildRsdp(0x1000);
    const auto rsdt = ia64::acpi::buildRsdt(0x2000);
    const auto madt = ia64::acpi::buildMadt(0, 0, 0x40,
                                            ia64::acpi::kMadtFlagEnabled, 0);

    ia64::acpi::AcpiValidation validation;
    const bool valid = ia64::acpi::validateAcpiTables(rsdp.bytes, rsdt.bytes,
                                                      madt.bytes, &validation);
    assert(valid);
    assert(validation.processorCount == 1);
    assert(validation.sapicEid == 0x40);
    assert(validation.enabled);
    std::cout << "testValidate PASSED\n";
}

void testValidateRejectsBadChecksum() {
    auto rsdp = ia64::acpi::buildRsdp(0x1000);
    rsdp.bytes[20] = 0xFF;
    const auto rsdt = ia64::acpi::buildRsdt(0x2000);
    const auto madt = ia64::acpi::buildMadt(0, 0, 0x40,
                                            ia64::acpi::kMadtFlagEnabled, 0);

    ia64::acpi::AcpiValidation validation;
    const bool valid = ia64::acpi::validateAcpiTables(rsdp.bytes, rsdt.bytes,
                                                      madt.bytes, &validation);
    assert(!valid);
    std::cout << "testValidateRejectsBadChecksum PASSED\n";
}

int main() {
    testRsdp();
    testRsdt();
    testMadt();
    testEfiConfigEntry();
    testValidate();
    testValidateRejectsBadChecksum();
    std::cout << "All ACPI firmware tests PASSED\n";
    return 0;
}
