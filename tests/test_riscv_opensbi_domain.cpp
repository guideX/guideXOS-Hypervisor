#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"
#include "riscv/riscv_boot_contract.h"

#include <set>
#include <filesystem>

namespace {

constexpr uint64_t kDomainResult = boot_contract::kResultAddress;
constexpr uint64_t kDomainSignature = kDomainResult + 0x100;
constexpr uint64_t kDomainState = kDomainResult + 0x400;
constexpr uint64_t kDomainStepBudget = 10000000;
constexpr uint64_t kDomainMilestone = 0x47584F5352495356ULL;
constexpr uint64_t kDomainComplete = 0x434F4D504C455445ULL;
constexpr uint64_t kFirmwareTextEnd = boot_contract::kOpenSbiProtectedRanges[0].start +
                                      boot_contract::kOpenSbiProtectedRanges[0].size;
constexpr uint64_t kFirmwareDataEnd = boot_contract::kOpenSbiProtectedRanges[1].start +
                                      boot_contract::kOpenSbiProtectedRanges[1].size;
constexpr uint64_t kNonidentityVirtual = 0x40006000ULL;
constexpr uint64_t kNonidentityPhysical = 0x83E01000ULL;
constexpr uint64_t kNonidentityValue = 0xA55A1234DEADBEEFULL;
constexpr uint64_t kIdentityAddress = 0x83E02000ULL;
constexpr uint64_t kIdentityValue = 0x1D3D5A7B9CBED0F2ULL;

constexpr std::array<uint64_t, 16> kGuestFaultCauses{{
    5, 7, 1, 7, 5, 5, 5, 5, 7, 5, 5, 5, 5, 1, 13, 7,
}};
constexpr std::array<uint64_t, 16> kGuestFaultAddresses{{
    0x80000000ULL, 0x80000000ULL, 0x80000000ULL, 0x02004000ULL,
    0x0200BFF8ULL, 0x1000B100ULL, 0x84000000ULL, 0x40000000ULL,
    0x1000A100ULL, 0x40000000ULL, 0x40003000ULL, 0x40000000ULL,
    0x40001000ULL, 0x40000000ULL, 0x40005000ULL, 0x40004000ULL,
}};

struct ExpectedPmpRegion {
    const char* name;
    uint8_t config;
    uint64_t lower;
    uint64_t upper;
};

constexpr std::array<ExpectedPmpRegion, 15> kExpectedPmpRegions{{
    {"OpenSBI-data-scratch", 0x18, 0x80080000ULL, 0x800A0000ULL},
    {"OpenSBI-text", 0x18, 0x80000000ULL, 0x80080000ULL},
    {"ACLINT-mtimecmp", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtimecmp-inherited", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtime", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"ACLINT-mtime-inherited", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"UART0", 0x1B, 0x10000000ULL, 0x10000100ULL},
    {"UART1", 0x1B, 0x1000A000ULL, 0x1000A100ULL},
    {"guest-page-tables", 0x19, boot_contract::kPageTablesAddress,
     boot_contract::kPageTablesAddress + boot_contract::kPageTablesSize},
    {"guest-ram-2m", 0x1F, boot_contract::kPmpGuestRamWindows[0].start,
     boot_contract::kPmpGuestRamWindows[0].start + boot_contract::kPmpGuestRamWindows[0].size},
    {"PLIC", 0x1B, 0x0C000000ULL, 0x0C400000ULL},
    {"guest-ram-4m", 0x1F, boot_contract::kPmpGuestRamWindows[1].start,
     boot_contract::kPmpGuestRamWindows[1].start + boot_contract::kPmpGuestRamWindows[1].size},
    {"guest-ram-8m", 0x1F, boot_contract::kPmpGuestRamWindows[2].start,
     boot_contract::kPmpGuestRamWindows[2].start + boot_contract::kPmpGuestRamWindows[2].size},
    {"guest-ram-16m", 0x1F, boot_contract::kPmpGuestRamWindows[3].start,
     boot_contract::kPmpGuestRamWindows[3].start + boot_contract::kPmpGuestRamWindows[3].size},
    {"guest-ram-32m", 0x1F, boot_contract::kPmpGuestRamWindows[4].start,
     boot_contract::kPmpGuestRamWindows[4].start + boot_contract::kPmpGuestRamWindows[4].size},
}};

bool EntryIs(const RV64CPU& cpu, uint8_t config, uint64_t lower, uint64_t upper) {
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (cpu.pmpEntry(index, entry) && entry.active && entry.config == config &&
            entry.lowerBound == lower && entry.upperBound == upper) return true;
    }
    return false;
}

bool HasDenied(const RV64CPU& cpu, AccessType access, uint64_t physical,
               uint64_t virtualAddress = 0, bool matchVirtual = false,
               int matchingEntry = -2, int config = -1) {
    return std::any_of(cpu.pmpDeniedTrace().begin(), cpu.pmpDeniedTrace().end(),
        [&](const PmpTraceEvent& event) {
            return event.privilege == PrivilegeMode::Supervisor && event.access == access &&
                event.physicalAddress == physical && event.allowed == false &&
                (!matchVirtual || (event.hasVirtualAddress &&
                                   event.virtualAddress == virtualAddress)) &&
                (matchingEntry == -2 || event.matchingEntry == matchingEntry) &&
                (config < 0 || event.config == static_cast<uint8_t>(config));
        });
}

bool HasAllowed(const RV64CPU& cpu, AccessType access, uint64_t physical,
                uint64_t virtualAddress) {
    return std::any_of(cpu.pmpTrace().begin(), cpu.pmpTrace().end(),
        [&](const PmpTraceEvent& event) {
            return event.kind == PmpTraceKind::PhysicalAccess && event.allowed &&
                event.privilege == PrivilegeMode::Supervisor && event.access == access &&
                event.physicalAddress == physical && event.hasVirtualAddress &&
                event.virtualAddress == virtualAddress;
        });
}

bool HasMmio(const RV64CPU& cpu, MachineMmioDevice device,
             MachineMmioOperation operation, uint64_t address,
             PrivilegeMode privilege = PrivilegeMode::Supervisor) {
    return std::any_of(cpu.machineMmioTrace().begin(), cpu.machineMmioTrace().end(),
        [&](const MachineMmioEvent& event) {
            return event.device == device && event.operation == operation &&
                   event.address == address && event.privilege == privilege;
        });
}

bool PropertyStringEquals(const FdtTree& tree, const std::string& path,
                          const std::string& name, const std::string& expected) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr) return false;
    std::string actual(property->begin(), property->end());
    if (!actual.empty() && actual.back() == '\0') actual.pop_back();
    return actual == expected;
}

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string JsonQuote(const std::string& value) {
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') result.push_back('\\');
        if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else result.push_back(ch);
    }
    result.push_back('"');
    return result;
}

bool ReadReg(const FdtTree& tree, const std::string& path,
             uint64_t& address, uint64_t& size) {
    const auto* reg = Property(tree, path, "reg");
    if (reg == nullptr || reg->size() < 16 || (reg->size() % 16) != 0) return false;
    address = Be64Pair(*reg, 0);
    size = Be64Pair(*reg, 8);
    return true;
}

bool ReadU32Property(const FdtTree& tree, const std::string& path,
                     const std::string& name, uint32_t& value) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr || property->size() != 4) return false;
    value = Be32(*property, 0);
    return true;
}

const char* PmpModeName(uint8_t config) {
    switch ((config >> 3) & 0x3U) {
        case 0: return "OFF";
        case 1: return "TOR";
        case 2: return "NA4";
        case 3: return "NAPOT";
    }
    return "INVALID";
}

std::string PmpPermissions(uint8_t config) {
    std::string result;
    result += (config & 0x1U) ? 'R' : '-';
    result += (config & 0x2U) ? 'W' : '-';
    result += (config & 0x4U) ? 'X' : '-';
    return result;
}

struct ObservedReservation {
    std::string path;
    uint64_t start = 0;
    uint64_t size = 0;
    bool noMap = false;
    bool valid = false;
};

std::vector<ObservedReservation> CollectReservations(const FdtTree& tree) {
    std::vector<ObservedReservation> result;
    for (const auto& [path, node] : tree.nodes) {
        if (path.rfind("/reserved-memory/", 0) != 0) continue;
        const auto reg = node.properties.find("reg");
        if (reg == node.properties.end()) continue;
        ObservedReservation item;
        item.path = path;
        item.noMap = node.properties.find("no-map") != node.properties.end();
        if (reg->second.size() == 16) {
            item.start = Be64Pair(reg->second, 0);
            item.size = Be64Pair(reg->second, 8);
            uint64_t end = 0;
            item.valid = boot_contract::CheckedEnd(item.start, item.size, end) &&
                boot_contract::IsContained(boot_contract::kRamBase,
                                           boot_contract::kRamSize,
                                           item.start, item.size);
        }
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.start < right.start ||
               (left.start == right.start && left.path < right.path);
    });
    return result;
}

bool ValidateReservations(const FdtTree& tree, uint64_t fdtActualSize,
                          std::vector<std::string>& failures) {
    const auto reservations = CollectReservations(tree);
    bool valid = !reservations.empty();
    uint64_t priorEnd = 0;
    bool first = true;
    bool fdtReserved = false;
    for (const auto& reservation : reservations) {
        if (!reservation.valid) {
            failures.push_back("invalid or out-of-RAM reservation: " + reservation.path);
            valid = false;
        }
        if (!reservation.noMap) {
            failures.push_back("reservation lacks no-map: " + reservation.path);
            valid = false;
        }
        uint64_t end = 0;
        if (!boot_contract::CheckedEnd(reservation.start, reservation.size, end)) {
            failures.push_back("reservation boundary overflow: " + reservation.path);
            valid = false;
        } else {
            if (!first && reservation.start < priorEnd) {
                failures.push_back("unexpected overlapping reservation: " + reservation.path);
                valid = false;
            }
            priorEnd = end;
            first = false;
        }
        if (reservation.path == "/reserved-memory/firmware-fdt@82200000") {
            fdtReserved = reservation.size == boot_contract::kFdtCapacity &&
                          fdtActualSize <= reservation.size;
        }
    }
    if (!fdtReserved) {
        failures.push_back("actual fixed-up FDT does not fit its 12 KiB reservation");
        valid = false;
    }
    return valid;
}

void WriteReservationJson(const std::filesystem::path& outputDirectory,
                         const FdtTree& tree, uint64_t fdtActualSize,
                         const std::vector<std::string>& failures) {
    std::ofstream out(outputDirectory / "riscv-reservations-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "reservation report is written under the configured artifact directory");
        return;
    }
    const auto reservations = CollectReservations(tree);
    out << "{\n  \"schemaVersion\": 1,\n  \"ram\": {\"base\": \""
        << Hex(boot_contract::kRamBase) << "\", \"size\": \""
        << Hex(boot_contract::kRamSize) << "\"},\n  \"fdtActualBytes\": "
        << fdtActualSize << ",\n  \"fdtCapacityBytes\": "
        << boot_contract::kFdtCapacity << ",\n  \"valid\": "
        << (failures.empty() ? "true" : "false") << ",\n  \"reservations\": [\n";
    for (size_t index = 0; index < reservations.size(); ++index) {
        const auto& reservation = reservations[index];
        uint64_t end = 0;
        (void)boot_contract::CheckedEnd(reservation.start, reservation.size, end);
        out << "    {\"path\": " << JsonQuote(reservation.path)
            << ", \"start\": \"" << Hex(reservation.start)
            << "\", \"endExclusive\": \"" << Hex(end)
            << "\", \"size\": \"" << Hex(reservation.size)
            << "\", \"noMap\": " << (reservation.noMap ? "true" : "false")
            << ", \"withinRam\": " << (reservation.valid ? "true" : "false") << "}"
            << (index + 1 == reservations.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"validationFailures\": [";
    for (size_t index = 0; index < failures.size(); ++index) {
        out << (index == 0 ? "" : ", ") << JsonQuote(failures[index]);
    }
    out << "]\n}\n";
}

bool ValidateVisiblePlatformFdt(const FdtTree& tree,
                                std::vector<std::string>& failures) {
    auto require = [&](bool condition, const char* message) {
        if (!condition) failures.emplace_back(message);
        return condition;
    };
    bool valid = tree.valid;
    valid = require(tree.valid, "OpenSBI handoff FDT is structurally valid") && valid;

    uint64_t base = 0, size = 0;
    bool memoryOk = ReadReg(tree, "/memory@80000000", base, size) &&
        base == boot_contract::kRamBase && size == boot_contract::kRamSize;
    valid = require(memoryOk, "guest-visible RAM range matches configured 64 MiB") && valid;

    uint32_t hartId = UINT32_MAX;
    valid = require(ReadU32Property(tree, "/cpus/cpu@0", "reg", hartId) && hartId == 0,
                    "boot CPU node identifies hart 0") && valid;
    valid = require(PropertyStringEquals(tree, "/cpus/cpu@0", "status", "okay"),
                    "boot CPU status is okay") && valid;
    valid = require(PropertyStringEquals(tree, "/cpus/cpu@0", "mmu-type", "riscv,sv39"),
                    "boot CPU advertises standard Sv39 mmu-type") && valid;
    valid = require(PropertyStringEquals(tree, "/cpus/cpu@0", "riscv,isa", "rv64ima_zicsr"),
                    "boot CPU advertises the configured RV64IMA_Zicsr ISA") && valid;
    valid = require(Property(tree, "/cpus/cpu@0", "opensbi-domain") == nullptr,
                    "OpenSBI removes its domain assignment property") && valid;
    size_t cpuNodeCount = 0;
    size_t enabledCpuCount = 0;
    constexpr std::string_view cpuPrefix = "/cpus/cpu@";
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        if (path.rfind(cpuPrefix, 0) != 0 ||
            path.find('/', cpuPrefix.size()) != std::string::npos)
            continue;
        ++cpuNodeCount;
        const auto* status = Property(tree, path, "status");
        if (status == nullptr || PropertyStringEquals(tree, path, "status", "okay"))
            ++enabledCpuCount;
    }
    valid = require(cpuNodeCount == 1 && enabledCpuCount == 1,
                    "exactly one enabled top-level CPU node remains") && valid;

    uint32_t timebase = 0;
    valid = require(ReadU32Property(tree, "/cpus", "timebase-frequency", timebase) &&
                    timebase == boot_contract::kTimerFrequency,
                    "CPU timebase frequency matches the 10 MHz machine timer") && valid;

    const bool uart0 = ReadReg(tree, "/soc/serial@10000000", base, size) &&
        base == boot_contract::kDevices[0].start && size == boot_contract::kDevices[0].size &&
        PropertyStringEquals(tree, "/soc/serial@10000000", "status", "okay");
    valid = require(uart0, "UART0 address, aperture, and status match the machine") && valid;
    const bool uart1 = ReadReg(tree, "/soc/serial@1000a000", base, size) &&
        base == boot_contract::kDevices[1].start && size == boot_contract::kDevices[1].size &&
        PropertyStringEquals(tree, "/soc/serial@1000a000", "status", "okay");
    valid = require(uart1, "UART1 address, aperture, and status match the machine") && valid;
    uint32_t uart0Irq = UINT32_MAX, uart1Irq = UINT32_MAX;
    valid = require(ReadU32Property(tree, "/soc/serial@10000000", "interrupts", uart0Irq) &&
                    uart0Irq == 10 &&
                    ReadU32Property(tree, "/soc/serial@1000a000", "interrupts", uart1Irq) &&
                    uart1Irq == 12,
                    "UART interrupt source IDs match PLIC sources 10 and 12") && valid;

    const bool plic = ReadReg(tree, "/soc/interrupt-controller@c000000", base, size) &&
        base == boot_contract::kDevices[2].start && size == boot_contract::kDevices[2].size &&
        PropertyStringEquals(tree, "/soc/interrupt-controller@c000000", "status", "okay");
    uint32_t plicSources = 0;
    const auto* contexts = Property(tree, "/soc/interrupt-controller@c000000",
                                    "interrupts-extended");
    const bool contextOk = contexts != nullptr && contexts->size() == 16 &&
        Be32(*contexts, 0) == 1 && Be32(*contexts, 4) == UINT32_MAX &&
        Be32(*contexts, 8) == 1 && Be32(*contexts, 12) == 9;
    valid = require(plic && ReadU32Property(tree, "/soc/interrupt-controller@c000000",
                                            "riscv,ndev", plicSources) && plicSources == 96 &&
                    contextOk,
                    "PLIC aperture, 96 sources, and visible S-context match the initialized controller") && valid;

    const auto* timerReg = Property(tree, "/aclint-mtimer@200bff8", "reg");
    const bool timer = timerReg != nullptr && timerReg->size() == 32 &&
        Be64Pair(*timerReg, 0) == boot_contract::kDevices[3].start &&
        Be64Pair(*timerReg, 8) == boot_contract::kDevices[3].size &&
        Be64Pair(*timerReg, 16) == boot_contract::kDevices[4].start &&
        Be64Pair(*timerReg, 24) == boot_contract::kDevices[4].size &&
        PropertyStringEquals(tree, "/aclint-mtimer@200bff8", "status", "disabled");
    valid = require(timer, "ACLINT ranges match the machine and are hidden from S-mode by domain policy") && valid;
    valid = require(PropertyStringEquals(tree, "/chosen", "stdout-path",
                                         "/soc/serial@10000000"),
                    "chosen console remains UART0") && valid;
    return valid;
}

void WriteFinalFdtJson(const std::filesystem::path& outputDirectory,
                       const FdtTree& tree, uint64_t sourceFdtBytes,
                       uint64_t capacity, const std::vector<std::string>& failures) {
    std::ofstream out(outputDirectory / "riscv-final-fdt-summary-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "final FDT summary is written under the configured artifact directory");
        return;
    }
    uint64_t ramBase = 0, ramSize = 0, uart0Base = 0, uart0Size = 0;
    uint64_t uart1Base = 0, uart1Size = 0, plicBase = 0, plicSize = 0;
    (void)ReadReg(tree, "/memory@80000000", ramBase, ramSize);
    (void)ReadReg(tree, "/soc/serial@10000000", uart0Base, uart0Size);
    (void)ReadReg(tree, "/soc/serial@1000a000", uart1Base, uart1Size);
    (void)ReadReg(tree, "/soc/interrupt-controller@c000000", plicBase, plicSize);
    uint32_t timebase = 0, ndev = 0;
    (void)ReadU32Property(tree, "/cpus", "timebase-frequency", timebase);
    (void)ReadU32Property(tree, "/soc/interrupt-controller@c000000", "riscv,ndev", ndev);
    const auto* contexts = Property(tree, "/soc/interrupt-controller@c000000",
                                    "interrupts-extended");
    out << "{\n  \"schemaVersion\": 1,\n  \"valid\": "
        << (failures.empty() ? "true" : "false")
        << ",\n  \"sourceBytes\": " << sourceFdtBytes
        << ",\n  \"finalBytes\": " << tree.totalSize
        << ",\n  \"allocationCapacityBytes\": " << capacity
        << ",\n  \"bootCpu\": {\"hartId\": 0, \"status\": "
        << JsonQuote(PropertyStringEquals(tree, "/cpus/cpu@0", "status", "okay") ? "okay" : "disabled")
        << ", \"isa\": \"rv64ima_zicsr\", \"mmuType\": "
        << JsonQuote(PropertyStringEquals(tree, "/cpus/cpu@0", "mmu-type", "riscv,sv39")
                         ? "riscv,sv39" : "missing-or-invalid")
        << ", \"opensbiDomainPropertyPresent\": "
        << (Property(tree, "/cpus/cpu@0", "opensbi-domain") ? "true" : "false")
        << ", \"enumeratedHartCount\": 1, \"enabledHartCount\": "
        << (PropertyStringEquals(tree, "/cpus/cpu@0", "status", "okay") ? 1 : 0) << "},\n"
        << "  \"ram\": {\"base\": \"" << Hex(ramBase) << "\", \"size\": \""
        << Hex(ramSize) << "\"},\n  \"timebaseFrequencyHz\": " << timebase
        << ",\n  \"devices\": [\n"
        << "    {\"name\": \"UART0\", \"base\": \"" << Hex(uart0Base)
        << "\", \"size\": \"" << Hex(uart0Size) << "\", \"status\": "
        << JsonQuote(PropertyStringEquals(tree, "/soc/serial@10000000", "status", "okay") ? "okay" : "disabled")
        << ", \"interruptSource\": 10},\n"
        << "    {\"name\": \"UART1\", \"base\": \"" << Hex(uart1Base)
        << "\", \"size\": \"" << Hex(uart1Size) << "\", \"status\": "
        << JsonQuote(PropertyStringEquals(tree, "/soc/serial@1000a000", "status", "okay") ? "okay" : "disabled")
        << ", \"interruptSource\": 12},\n"
        << "    {\"name\": \"PLIC\", \"base\": \"" << Hex(plicBase)
        << "\", \"size\": \"" << Hex(plicSize) << "\", \"sourceCount\": " << ndev
        << ", \"interruptExtendedCells\": [";
    if (contexts != nullptr) {
        for (size_t i = 0; i < contexts->size() / 4; ++i) {
            if (i != 0) out << ", ";
            out << Be32(*contexts, i * 4);
        }
    }
    out << "]},\n"
        << "    {\"name\": \"ACLINT\", \"mtimeBase\": \""
        << Hex(boot_contract::kDevices[3].start) << "\", \"mtimecmpBase\": \""
        << Hex(boot_contract::kDevices[4].start) << "\", \"status\": \"disabled\", "
        << "\"owner\": \"M-mode OpenSBI; SBI TIME exposed to S-mode\"}\n  ],\n"
        << "  \"chosenConsole\": \"/soc/serial@10000000\",\n"
        << "  \"firmwareReservations\": [\"OpenSBI text and data protected windows\", "
        << "\"reserved firmware slot tail\"]\n}\n";
}

bool ComparePmpPolicy(const RV64CPU& cpu, std::vector<std::string>& failures) {
    bool valid = cpu.pmpImplementedEntries() == boot_contract::kPmpImplementedEntries;
    if (!valid) failures.emplace_back("implemented PMP entry count differs from 16-entry contract");
    if (cpu.pmpImplementedEntries() != 16) return false;
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView actual;
        if (!cpu.pmpEntry(index, actual)) {
            failures.emplace_back("PMP entry cannot be inspected at index " + std::to_string(index));
            valid = false;
            continue;
        }
        if (index < kExpectedPmpRegions.size()) {
            const ExpectedPmpRegion& expected = kExpectedPmpRegions[index];
            const uint64_t expectedSize = expected.upper - expected.lower;
            const uint64_t expectedAddress = boot_contract::EncodeNapotAddress(
                expected.lower, expectedSize);
            if (!actual.active || actual.config != expected.config ||
                actual.encodedAddress != expectedAddress ||
                actual.lowerBound != expected.lower || actual.upperBound != expected.upper) {
                std::ostringstream failure;
                failure << "PMP entry " << index << " differs from " << expected.name;
                failures.push_back(failure.str());
                valid = false;
            }
        } else if (actual.active || actual.config != 0) {
            failures.emplace_back("PMP entry 15 is expected OFF");
            valid = false;
        }
    }
    return valid;
}

void WriteEffectivePmpJson(const std::filesystem::path& outputDirectory,
                           const RV64CPU& cpu) {
    std::ofstream out(outputDirectory / "riscv-effective-pmp-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "effective PMP report is written under the configured artifact directory");
        return;
    }
    out << "{\n  \"schemaVersion\": 1,\n  \"implementedEntries\": "
        << cpu.pmpImplementedEntries() << ",\n  \"entries\": [\n";
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        (void)cpu.pmpEntry(index, entry);
        const uint8_t mode = static_cast<uint8_t>((entry.config >> 3) & 0x3U);
        const char* semanticName = index < kExpectedPmpRegions.size()
            ? kExpectedPmpRegions[index].name : "OFF";
        out << "    {\"index\": " << index << ", \"semanticName\": "
            << JsonQuote(semanticName) << ", \"encodedAddress\": \""
            << Hex(entry.encodedAddress) << "\", \"configurationByte\": \""
            << Hex(entry.config) << "\", \"addressMatchingMode\": "
            << JsonQuote(PmpModeName(entry.config)) << ", \"lowerBound\": \""
            << Hex(entry.lowerBound) << "\", \"upperBoundExclusive\": \""
            << Hex(entry.upperBound) << "\", \"permissions\": "
            << JsonQuote(PmpPermissions(entry.config)) << ", \"locked\": "
            << ((entry.config & 0x80U) ? "true" : "false")
            << ", \"active\": " << (entry.active ? "true" : "false") << "}"
            << (index + 1 == cpu.pmpImplementedEntries() ? "\n" : ",\n");
        (void)mode;
    }
    out << "  ]\n}\n";
}

void WritePmpComparisonJson(const std::filesystem::path& outputDirectory,
                            const RV64CPU& cpu, bool matches,
                            const std::vector<std::string>& failures) {
    std::ofstream out(outputDirectory / "riscv-pmp-comparison-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "PMP comparison report is written under the configured artifact directory");
        return;
    }
    out << "{\n  \"schemaVersion\": 1,\n  \"matches\": "
        << (matches ? "true" : "false") << ",\n  \"expectedActiveEntries\": 15,\n"
        << "  \"expectedOffEntries\": 1,\n  \"entries\": [\n";
    const size_t count = std::min(cpu.pmpImplementedEntries(), size_t{16});
    for (size_t index = 0; index < count; ++index) {
        PmpEntryView actual;
        (void)cpu.pmpEntry(index, actual);
        const bool expectedActive = index < kExpectedPmpRegions.size();
        const ExpectedPmpRegion* expected = expectedActive ? &kExpectedPmpRegions[index] : nullptr;
        const uint64_t expectedAddress = expected
            ? boot_contract::EncodeNapotAddress(expected->lower, expected->upper - expected->lower)
            : 0;
        const bool entryMatches = expected
            ? actual.active && actual.config == expected->config &&
              actual.encodedAddress == expectedAddress && actual.lowerBound == expected->lower &&
              actual.upperBound == expected->upper
            : !actual.active && actual.config == 0;
        out << "    {\"index\": " << index << ", \"expected\": {\"name\": "
            << JsonQuote(expected ? expected->name : "OFF") << ", \"active\": "
            << (expectedActive ? "true" : "false") << ", \"configurationByte\": \""
            << Hex(expected ? expected->config : 0) << "\", \"encodedAddress\": \""
            << Hex(expectedAddress) << "\", \"lowerBound\": \""
            << Hex(expected ? expected->lower : 0) << "\", \"upperBoundExclusive\": \""
            << Hex(expected ? expected->upper : 0) << "\"}, \"actual\": {\"active\": "
            << (actual.active ? "true" : "false") << ", \"configurationByte\": \""
            << Hex(actual.config) << "\", \"encodedAddress\": \""
            << Hex(actual.encodedAddress) << "\", \"lowerBound\": \""
            << Hex(actual.lowerBound) << "\", \"upperBoundExclusive\": \""
            << Hex(actual.upperBound) << "\"}, \"matches\": "
            << (entryMatches ? "true" : "false") << "}"
            << (index + 1 == count ? "\n" : ",\n");
    }
    out << "  ],\n  \"differences\": [";
    for (size_t index = 0; index < failures.size(); ++index) {
        out << (index == 0 ? "" : ", ") << JsonQuote(failures[index]);
    }
    out << "],\n  \"inheritedAclintDuplicates\": ["
        << "{\"entry\": 3, \"duplicatesEntry\": 2, \"address\": \"0x2004000\"}, "
        << "{\"entry\": 5, \"duplicatesEntry\": 4, \"address\": \"0x200bff8\"}]\n}\n";
}

void WriteMemoryInventoryJson(const std::filesystem::path& outputDirectory,
                              const std::vector<uint8_t>& firmware,
                              const std::vector<uint8_t>& firmwareElf,
                              const ElfLayout& firmwareLayout,
                              const std::vector<uint8_t>& supervisor,
                              const ElfLayout& supervisorLayout,
                              const std::vector<uint8_t>& fdt, uint64_t fdtActualSize,
                              const std::vector<std::string>& reservationFailures) {
    std::ofstream out(outputDirectory / "riscv-memory-inventory-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "memory inventory is written under the configured artifact directory");
        return;
    }
    auto emitRange = [&](std::string_view name, uint64_t start, uint64_t size,
                         uint64_t alignment, std::string_view owner,
                         std::string_view kind, std::string_view privileges,
                         std::string_view source, std::string_view lifetime,
                         bool reserved, bool pmp, const char* relation,
                         bool comma) {
        uint64_t end = 0;
        (void)boot_contract::CheckedEnd(start, size, end);
        out << "    {\"name\": " << JsonQuote(std::string(name))
            << ", \"start\": \"" << Hex(start) << "\", \"endExclusive\": \""
            << Hex(end) << "\", \"size\": \"" << Hex(size)
            << "\", \"requiredAlignment\": \"" << Hex(alignment)
            << "\", \"owner\": " << JsonQuote(std::string(owner))
            << ", \"classification\": " << JsonQuote(std::string(kind))
            << ", \"privilegeAccess\": " << JsonQuote(std::string(privileges))
            << ", \"initializationSource\": " << JsonQuote(std::string(source))
            << ", \"lifetime\": " << JsonQuote(std::string(lifetime))
            << ", \"reservedMemory\": " << (reserved ? "true" : "false")
            << ", \"requiresPmpCoverage\": " << (pmp ? "true" : "false")
            << ", \"permittedRelationship\": " << JsonQuote(relation) << "}"
            << (comma ? ",\n" : "\n");
    };
    size_t total = 1 + boot_contract::kOpenSbiProtectedRanges.size() +
        boot_contract::kFixtureReservations.size() + boot_contract::kPmpGuestRamWindows.size() +
        boot_contract::kDevices.size() + 1 + supervisorLayout.loads.size();
    size_t emitted = 0;
    out << "{\n  \"schemaVersion\": 1,\n  \"physicalAddressBits\": 56,\n"
        << "  \"ranges\": [\n";
    emitRange("guest-ram", boot_contract::kRamBase, boot_contract::kRamSize, 0x1000,
              "physical guest RAM", "RAM container", "M-mode and domain-filtered S-mode",
              "FDT /memory", "platform lifetime", false, true,
              "contains reserved and PMP subranges", ++emitted < total);
    for (const auto& range : boot_contract::kOpenSbiProtectedRanges) {
        emitRange(range.name, range.start, range.size, range.alignment, range.owner,
                  range.kind, range.privilegeAccess, range.initializationSource,
                  range.lifetime, range.reservedMemory, range.pmpCoverage,
                  "protected firmware subrange inside guest RAM", ++emitted < total);
    }
    for (const auto& range : boot_contract::kFixtureReservations) {
        emitRange(range.name, range.start, range.size, range.alignment, range.owner,
                  range.kind, range.privilegeAccess, range.initializationSource,
                  range.lifetime, range.reservedMemory, range.pmpCoverage,
                  "reservation nested in guest RAM", ++emitted < total);
    }
    for (const auto& range : boot_contract::kPmpGuestRamWindows) {
        emitRange(range.name, range.start, range.size, range.alignment, range.owner,
                  range.kind, range.privilegeAccess, range.initializationSource,
                  range.lifetime, range.reservedMemory, range.pmpCoverage,
                  "PMP permission window; windows partition domain RAM", ++emitted < total);
    }
    for (const auto& range : boot_contract::kDevices) {
        emitRange(range.name, range.start, range.size, range.alignment, range.owner,
                  range.kind, range.privilegeAccess, range.initializationSource,
                  range.lifetime, range.reservedMemory, range.pmpCoverage,
                  "separate MMIO aperture outside guest RAM", ++emitted < total);
    }
    emitRange("OpenSBI firmware loaded binary", boot_contract::kFirmwareEntry,
              static_cast<uint64_t>(firmware.size()), 4, "resident OpenSBI v1.8.1",
              "RAM image", "M-mode only after handoff", "FW_JUMP binary load",
              "resident firmware lifetime", true, true,
              "actual binary extent inside firmware protected memory", ++emitted < total);
    for (size_t index = 0; index < supervisorLayout.loads.size(); ++index) {
        const auto& load = supervisorLayout.loads[index];
        emitRange("supervisor ELF PT_LOAD segment " + std::to_string(index),
                  load.base, load.size, 0x1000, "RISCV-16 focused guest",
                  "RAM image segment", "S-mode", "ELF64 loader",
                  "test fixture only", true, true,
                  "individual PT_LOAD segment contained by its fixture reservation",
                  ++emitted < total);
    }
    (void)firmwareLayout;
    out << "  ],\n  \"images\": {\n"
        << "    \"firmwareBinary\": {\"bytes\": " << firmware.size()
        << ", \"sha256\": " << JsonQuote(Sha256(firmware)) << "},\n"
        << "    \"firmwareElf\": {\"sha256\": " << JsonQuote(Sha256(firmwareElf))
        << ", \"lowestLoadAddress\": \"" << Hex(firmwareLayout.lowest)
        << "\", \"highestLoadAddressExclusive\": \""
        << Hex(firmwareLayout.highest) << "\"},\n"
        << "    \"supervisorElf\": {\"bytes\": " << supervisor.size()
        << ", \"sha256\": " << JsonQuote(Sha256(supervisor))
        << ", \"loadAddressEnvelope\": {\"start\": \"" << Hex(supervisorLayout.lowest)
        << "\", \"endExclusive\": \"" << Hex(supervisorLayout.highest)
        << "\", \"isContiguousImage\": false}, \"ptLoadSegments\": [";
    for (size_t index = 0; index < supervisorLayout.loads.size(); ++index) {
        const auto& load = supervisorLayout.loads[index];
        out << (index == 0 ? "" : ", ") << "{\"start\": \"" << Hex(load.base)
            << "\", \"endExclusive\": \"" << Hex(load.base + load.size)
            << "\", \"fileBytes\": " << load.fileSize << ", \"memoryBytes\": "
            << load.size << ", \"flags\": " << load.flags << "}";
    }
    out << "]},\n"
        << "    \"sourceFdt\": {\"bytes\": " << fdt.size()
        << ", \"sha256\": " << JsonQuote(Sha256(fdt))
        << "},\n    \"fixedUpFdtActualBytes\": " << fdtActualSize << "\n  },\n"
        << "  \"unmappedAddressExamples\": [\"0x40000000\", \"0x84000000\"],\n"
        << "  \"reservationValidation\": {\"valid\": "
        << (reservationFailures.empty() ? "true" : "false") << "}\n}\n";
}

void WriteHandoffJson(const std::filesystem::path& outputDirectory,
                      const RV64CPU& cpu, const RunEvidence& handoff) {
    const uint64_t laterSatp = CsrOrZero(cpu, 0x180);
    const uint64_t laterMode = laterSatp >> 60;
    std::ofstream out(outputDirectory / "riscv-handoff-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "handoff report is written under the configured artifact directory");
        return;
    }
    out << "{\n  \"schemaVersion\": 1,\n  \"actualOpenSbiHandoff\": {\n"
        << "    \"a0HartId\": " << handoff.handoffHartId
        << ",\n    \"a1FdtAddress\": \"" << Hex(handoff.handoffFdt)
        << "\",\n    \"satp\": \"" << Hex(handoff.handoffSatp)
        << "\",\n    \"privilege\": "
        << JsonQuote(PrivilegeModeToString(handoff.handoffPrivilege))
        << ",\n    \"pc\": \"" << Hex(handoff.handoffPc) << "\"\n  },\n"
        << "  \"laterGuestState\": {\"privilege\": "
        << JsonQuote(PrivilegeModeToString(cpu.privilege()))
        << ", \"pc\": \"" << Hex(cpu.getPC()) << "\", \"satp\": \""
        << Hex(laterSatp) << "\", \"satpMode\": " << laterMode
        << ", \"modeName\": " << JsonQuote(laterMode == 8 ? "Sv39" : "Bare/other")
        << "},\n  \"requirementsForFutureLinuxStyleHandoff\": {\n"
        << "    \"a0BootHartId\": 0,\n    \"a1ValidFdt\": true,\n"
        << "    \"satpBareAtEntry\": true,\n    \"privilegeAtEntry\": \"S\",\n"
        << "    \"kernelPlacementAlignmentBytes\": 2097152,\n"
        << "    \"residentFirmwareReserved\": true,\n"
        << "    \"firmwareKernelFdtRuntimeNonOverlap\": true,\n"
        << "    \"linuxExecutionPerformed\": false\n  }\n}\n";
}

void WriteBootContractJson(const std::filesystem::path& outputDirectory,
                           const RV64CPU& cpu, const RunEvidence& handoff,
                           const std::vector<uint8_t>& firmware,
                           const std::vector<uint8_t>& firmwareElf,
                           const std::vector<uint8_t>& supervisor,
                           const std::vector<uint8_t>& sourceFdt,
                           const std::vector<uint8_t>& fixedFdt,
                           const ElfLayout& firmwareLayout,
                           const ElfLayout& supervisorLayout,
                           const FdtTree& guestFdt,
                           bool fdtValid, bool reservationsValid,
                           bool pmpValid, bool handoffValid) {
    std::ofstream out(outputDirectory / "riscv-boot-contract-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "versioned boot contract is written under the configured artifact directory");
        return;
    }
    const auto reservations = CollectReservations(guestFdt);
    uint64_t laterSatp = CsrOrZero(cpu, 0x180);
    out << "{\n  \"schema\": {\"name\": \"riscv-boot-contract\", \"version\": 1},\n"
        << "  \"contractProfile\": \"guideXOS-RV64IMA_Zicsr-single-hart-restricted-domain\",\n"
        << "  \"hartCount\": 1,\n  \"bootHartId\": 0,\n"
        << "  \"isaRequirements\": [\"RV64I\", \"M\", \"A\", \"Zicsr\"],\n"
        << "  \"firmware\": {\"name\": \"OpenSBI v1.8.1 FW_JUMP\", "
        << "\"entryPrivilege\": \"M\", \"entry\": \""
        << Hex(boot_contract::kFirmwareEntry) << "\", \"binaryBytes\": "
        << firmware.size() << ", \"binarySha256\": " << JsonQuote(Sha256(firmware))
        << ", \"elfSha256\": " << JsonQuote(Sha256(firmwareElf))
        << ", \"elfLoadExtent\": {\"start\": \"" << Hex(firmwareLayout.lowest)
        << "\", \"endExclusive\": \"" << Hex(firmwareLayout.highest) << "\"}},\n"
        << "  \"nextStage\": {\"entry\": \"" << Hex(boot_contract::kSupervisorEntry)
        << "\", \"imageBytes\": " << supervisor.size()
        << ", \"imageSha256\": " << JsonQuote(Sha256(supervisor))
        << ", \"loadAddressEnvelope\": {\"start\": \""
        << Hex(supervisorLayout.lowest) << "\", \"endExclusive\": \""
        << Hex(supervisorLayout.highest) << "\", \"isContiguousImage\": false}, "
        << "\"placementAlignmentBytes\": 2097152, "
        << "\"isLinuxImage\": false},\n"
        << "  \"memory\": {\"base\": \"" << Hex(boot_contract::kRamBase)
        << "\", \"size\": \"" << Hex(boot_contract::kRamSize)
        << "\", \"physicalAddressBits\": " << boot_contract::kPhysicalAddressBits
        << "},\n  \"fdt\": {\"loadAddress\": \""
        << Hex(boot_contract::kFdtAddress) << "\", \"sourceBytes\": "
        << sourceFdt.size() << ", \"actualPostOpenSbiBytes\": " << guestFdt.totalSize
        << ", \"allocationCapacityBytes\": " << boot_contract::kFdtCapacity
        << ", \"fixedUpSha256\": " << JsonQuote(Sha256(fixedFdt)) << "},\n"
        << "  \"handoff\": {\"a0HartId\": " << handoff.handoffHartId
        << ", \"a1FdtAddress\": \"" << Hex(handoff.handoffFdt)
        << "\", \"initialSatp\": \"" << Hex(handoff.handoffSatp)
        << "\", \"privilege\": "
        << JsonQuote(PrivilegeModeToString(handoff.handoffPrivilege))
        << ", \"entryPc\": \"" << Hex(handoff.handoffPc)
        << "\", \"laterSatp\": \"" << Hex(laterSatp)
        << "\", \"laterSatpMode\": " << (laterSatp >> 60)
        << ", \"actualHandoffValid\": " << (handoffValid ? "true" : "false") << "},\n"
        << "  \"devices\": [\n";
    for (size_t i = 0; i < boot_contract::kDevices.size(); ++i) {
        const auto& device = boot_contract::kDevices[i];
        out << "    {\"name\": " << JsonQuote(std::string(device.name))
            << ", \"base\": \"" << Hex(device.start) << "\", \"size\": \""
            << Hex(device.size) << "\", \"classification\": "
            << JsonQuote(std::string(device.kind)) << ", \"owner\": "
            << JsonQuote(std::string(device.owner)) << ", \"privilegeAccess\": "
            << JsonQuote(std::string(device.privilegeAccess)) << "}"
            << (i + 1 == boot_contract::kDevices.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"reservedMemory\": [\n";
    for (size_t i = 0; i < reservations.size(); ++i) {
        const auto& reservation = reservations[i];
        out << "    {\"path\": " << JsonQuote(reservation.path)
            << ", \"start\": \"" << Hex(reservation.start) << "\", \"size\": \""
            << Hex(reservation.size) << "\", \"noMap\": "
            << (reservation.noMap ? "true" : "false") << "}"
            << (i + 1 == reservations.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"pmp\": {\"implementedEntryCount\": "
        << cpu.pmpImplementedEntries() << ", \"expectedActiveEntries\": 15, "
        << "\"expectedOffEntries\": 1, \"comparisonMatches\": "
        << (pmpValid ? "true" : "false")
        << ", \"effectiveEntriesReport\": \"riscv-effective-pmp-v1.json\", "
        << "\"differenceReport\": \"riscv-pmp-comparison-v1.json\"},\n"
        << "  \"validation\": {\"fdtConsistent\": " << (fdtValid ? "true" : "false")
        << ", \"reservationsValid\": " << (reservationsValid ? "true" : "false")
        << ", \"pmpMatches\": " << (pmpValid ? "true" : "false")
        << ", \"handoffValid\": " << (handoffValid ? "true" : "false") << "},\n"
        << "  \"translation\": {\"handoffSatpBare\": true, \"laterGuestSatpMode\": "
        << (laterSatp >> 60) << ", \"mmuTranslationCacheImplemented\": false},\n"
        << "  \"linuxExecution\": false\n}\n";
}

void WriteValidationFailuresJson(const std::filesystem::path& outputDirectory,
                                 const std::vector<std::string>& failures) {
    std::ofstream out(outputDirectory / "riscv-contract-validation-failures-v1.json",
                      std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "contract validation report is written under the configured artifact directory");
        return;
    }
    out << "{\n  \"schemaVersion\": 1,\n  \"failureCount\": " << failures.size()
        << ",\n  \"failures\": [";
    for (size_t index = 0; index < failures.size(); ++index) {
        out << (index == 0 ? "" : ", ") << JsonQuote(failures[index]);
    }
    out << "]\n}\n";
}

void DumpIntegrationTrace(const std::string& path, const RV64CPU& cpu,
                          const RunEvidence& milestoneRun, const RunEvidence& finalRun,
                          const FdtTree& guestFdt) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "bounded RISCV-16 integration trace is written to its build artifact path");
        return;
    }
    out << "RISCV-16 authentic OpenSBI restricted-domain integration\n"
        << "Milestone steps=" << milestoneRun.steps << " completion steps=" << finalRun.steps
        << " final complete=" << finalRun.complete << " mtimecmp=0x" << std::hex
        << cpu.mtimecmp() << "\n";
    uint64_t marker = 0, deadline = 0;
    (void)ReadWord(cpu, kDomainState, marker);
    (void)ReadWord(cpu, kDomainState + 8, deadline);
    out << "Guest timer milestone=0x" << std::hex << marker
        << " deadline=0x" << deadline << "\n"
        << "Milestone handoff=" << milestoneRun.handedOff
        << " M-retired=" << std::dec << milestoneRun.mRetired
        << " S-retired=" << milestoneRun.sRetired
        << " SBI calls=" << milestoneRun.sbiEcalls << "\n";
    out << "Guest signature words:";
    for (size_t index = 0; index < 15; ++index) {
        uint64_t value = 0;
        (void)ReadWord(cpu, kDomainSignature + index * 8ULL, value);
        out << " [" << std::dec << index << "]=0x" << std::hex << value;
    }
    out << '\n';
    out << "Final effective PMP regions:\n";
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (!cpu.pmpEntry(index, entry) || !entry.active) continue;
        out << "entry=" << std::dec << index << " cfg=0x" << std::hex
            << static_cast<unsigned>(entry.config) << " pmpaddr=0x" << entry.encodedAddress
            << " range=[0x" << entry.lowerBound << ",0x" << entry.upperBound << ")\n";
    }
    out << "OpenSBI PMP CSR events=" << std::dec << cpu.pmpCsrTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
        out << (event.kind == PmpTraceKind::CsrRead ? "read" : "write")
            << " csr=0x" << std::hex << event.csr << " value=0x" << event.csrValue
            << " pc=0x" << event.pc << '\n';
    }
    out << "Denied S-mode physical accesses=" << std::dec << cpu.pmpDeniedTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
        out << "access=" << static_cast<unsigned>(event.access) << " pa=0x" << std::hex
            << event.physicalAddress << " va=";
        if (event.hasVirtualAddress) out << "0x" << event.virtualAddress;
        else out << "<implicit-physical>";
        out << " entry=" << std::dec << static_cast<int>(event.matchingEntry)
            << " cfg=0x" << std::hex << static_cast<unsigned>(event.config)
            << " pc=0x" << event.pc << '\n';
    }
    out << "Last guest MMIO events (bounded to 256):\n";
    const auto& mmio = cpu.machineMmioTrace();
    const size_t begin = mmio.size() > 256 ? mmio.size() - 256 : 0;
    for (size_t index = begin; index < mmio.size(); ++index) {
        const MachineMmioEvent& event = mmio[index];
        out << "seq=" << event.sequence << " device=" << static_cast<unsigned>(event.device)
            << " op=" << static_cast<unsigned>(event.operation) << " pa=0x" << std::hex
            << event.address << " value=0x" << event.value << " size=" << std::dec
            << static_cast<unsigned>(event.size) << " privilege="
            << PrivilegeModeToString(event.privilege) << " pc=0x" << std::hex
            << event.pc << '\n';
    }
    out << "Guest-visible FDT valid=" << guestFdt.valid << " total-size="
        << std::dec << guestFdt.totalSize << "\n"
        << "UART output:\n" << cpu.machineUartOutput() << '\n';
    out << "Guest-visible FDT node paths and status properties:\n";
    for (const auto& [nodePath, node] : guestFdt.nodes) {
        const auto status = node.properties.find("status");
        out << nodePath << " status=";
        if (status == node.properties.end()) out << "<absent>";
        else out << std::string(status->second.begin(), status->second.end());
        out << '\n';
    }
    out << "Milestone bounded trace entries=" << milestoneRun.trace.size()
        << " traps=" << milestoneRun.trapTrace.size() << '\n';
    for (const TraceEntry& entry : milestoneRun.trapTrace) {
        out << "trap pc=0x" << std::hex << entry.pc << " instruction=" << entry.disassembly
            << " before=" << PrivilegeModeToString(entry.before)
            << " after=" << PrivilegeModeToString(entry.after) << " cause=0x"
            << static_cast<uint64_t>(entry.cause) << " mtval=0x" << entry.mtval << '\n';
    }
    out << "Final bounded trace entries=" << finalRun.trace.size()
        << " traps=" << finalRun.trapTrace.size() << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: test_riscv_opensbi_domain fw_jump.bin fw_jump.elf supervisor.elf platform.dtb trace.log\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> supervisor = ReadFile(argv[3]);
    const std::vector<uint8_t> fdt = ReadFile(argv[4]);
    constexpr char kSupervisorHash[] = "2F90EBB20D7080321A7FF2DEDB088CCFA8251FDD68F64F0D47B943B6350B709D";
    constexpr char kFdtHash[] = "AA83E5A3B42046DCC00D9FC90E04334A2B7ED713843A5164EFFBBF137D5D72B7";
    Expect(Sha256(firmware) == kFirmwareBinSha256,
           "domain boot uses the unchanged pinned OpenSBI v1.8.1 raw FW_JUMP image");
    Expect(Sha256(firmwareElf) == kFirmwareElfSha256,
           "domain boot uses the unchanged pinned OpenSBI v1.8.1 firmware ELF");
    Expect(Sha256(supervisor) == kSupervisorHash,
           "independent Sv39 supervisor payload matches its recorded SHA-256");
    Expect(Sha256(fdt) == kFdtHash,
           "compiled RISCV-16 domain FDT matches its recorded SHA-256");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    const ElfLayout guestLayout = InspectElf(supervisor);
    Expect(firmwareLayout.valid && firmwareLayout.machine == 243 &&
           firmwareLayout.entry == kFirmwareEntry && firmwareLayout.highest <= kFirmwareSlotEnd,
           "pinned OpenSBI image has the expected RISC-V FW_JUMP layout");
    Expect(guestLayout.valid && guestLayout.machine == 243 && guestLayout.entry == kPayloadEntry,
           "independent S-mode ELF has the configured domain entrypoint");
    Expect(Sha256(firmware) == kFirmwareBinSha256 &&
           Sha256(firmwareElf) == kFirmwareElfSha256,
           "accepted OpenSBI artifacts remain byte-for-byte unchanged");

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    const bool prepared = PrepareGuest(cpu, firmware, supervisor, fdt,
                                       boot_contract::kFdtCapacity);
    Expect(prepared, "authentic OpenSBI, domain FDT, and supervisor ELF load into 64 MiB RAM");
    MachinePlicMapping plic;
    plic.baseAddress = boot_contract::kDevices[2].start;
    plic.sourceCount = 96;
    plic.enabled = true;
    MachineUartMapping uart1;
    uart1.baseAddress = boot_contract::kDevices[1].start;
    uart1.enabled = true;
    MachineTimerMapping timer;
    timer.mtimeAddress = boot_contract::kDevices[3].start;
    timer.mtimecmpAddress = boot_contract::kDevices[4].start;
    Expect(cpu.configureMachinePlic(plic) && cpu.configureMachineUart1(uart1) &&
           cpu.configureMachineTimer(timer),
           "emulator maps only the FDT-described PLIC, independent UART1, and ACLINT timer");
    bool milestoneSeen = false;
    RunEvidence milestoneRun;
    RunEvidence finalRun;
    FdtTree guestFdt;
    if (prepared) {
        Expect(cpu.sbiProvider() == nullptr,
               "authentic OpenSBI test has no host-side SBI interception provider");
        Expect(cpu.pmpImplementedEntries() == 16 &&
               cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16,
               "OpenSBI runs on the unchanged StandardPmp16 hardware profile");
        uint8_t firmwareBefore[8]{};
        const bool savedFirmware = cpu.readPhysical(kFirmwareEntry, firmwareBefore,
                                                    sizeof(firmwareBefore));
        milestoneRun = RunFirmware(cpu, kDomainStepBudget * 2,
            [](const RV64CPU& running) {
                uint64_t marker = 0;
                return ReadWord(running, kDomainState, marker) && marker == kDomainMilestone;
            });
        uint64_t milestone = 0;
        uint64_t deadline = 0;
        milestoneSeen = ReadWord(cpu, kDomainState, milestone) &&
                        milestone == kDomainMilestone &&
                        ReadWord(cpu, kDomainState + 8, deadline);
        Expect(milestoneSeen && !milestoneRun.complete,
               "guest reaches its post-SBI timer milestone before interrupt continuation");
        Expect(milestoneRun.handedOff && milestoneRun.mRetired > 0 && milestoneRun.sRetired > 0,
               "OpenSBI's selected domain reaches the configured S-mode entry after real M-mode execution");
        Expect(milestoneRun.handoffObserved && milestoneRun.handoffHartId == 0 &&
               milestoneRun.handoffFdt == boot_contract::kFdtAddress &&
               milestoneRun.handoffSatp == 0 &&
               milestoneRun.handoffPrivilege == PrivilegeMode::Supervisor &&
               milestoneRun.handoffPc == boot_contract::kSupervisorEntry,
               "actual OpenSBI handoff observes a0=0, valid FDT in a1, Bare satp, and S-mode entry");
        Expect(milestoneRun.sawSbiTrapInM && milestoneRun.sawSbiReturnToS &&
               milestoneRun.sbiEcalls >= 2,
               "S-mode uses authentic OpenSBI Base and TIME ECALLs through M-mode");
        Expect(milestoneSeen && deadline != 0 && cpu.mtimecmp() == deadline,
               "SBI TIME deadline remains armed through the guest's nonidentity Sv39 access");
        const bool identityStoreAllowed =
            HasAllowed(cpu, AccessType::Store, kIdentityAddress, kIdentityAddress);
        const bool identityLoadAllowed =
            HasAllowed(cpu, AccessType::Load, kIdentityAddress, kIdentityAddress);
        const bool nonidentityStoreAllowed =
            HasAllowed(cpu, AccessType::Store, kNonidentityPhysical, kNonidentityVirtual);
        const bool nonidentityLoadAllowed =
            HasAllowed(cpu, AccessType::Load, kNonidentityPhysical, kNonidentityVirtual);
        if (!identityStoreAllowed || !identityLoadAllowed ||
            !nonidentityStoreAllowed || !nonidentityLoadAllowed) {
            std::cerr << "allowed trace flags identity-store=" << identityStoreAllowed
                      << " identity-load=" << identityLoadAllowed
                      << " store=" << nonidentityStoreAllowed
                      << " load=" << nonidentityLoadAllowed << '\n';
            for (const PmpTraceEvent& event : cpu.pmpTrace()) {
                if (event.physicalAddress == kNonidentityPhysical ||
                    event.virtualAddress == kNonidentityVirtual ||
                    event.physicalAddress == kIdentityAddress ||
                    event.virtualAddress == kIdentityAddress) {
                    std::cerr << "candidate allowed-event access="
                              << static_cast<unsigned>(event.access) << " allowed=" << event.allowed
                              << " privilege=" << PrivilegeModeToString(event.privilege)
                              << " pa=0x" << std::hex << event.physicalAddress
                              << " va=0x" << event.virtualAddress << " hasva=" << event.hasVirtualAddress
                              << " pc=0x" << event.pc << std::dec << '\n';
                }
            }
        }
        Expect(identityStoreAllowed && identityLoadAllowed &&
               nonidentityStoreAllowed && nonidentityLoadAllowed,
               "PMP trace records permitted identity and nonidentity Sv39 RAM accesses");
        Expect(HasDenied(cpu, AccessType::Store, 0x02004000ULL, 0, false, -2, 0x18),
               "direct M-mode timer comparator store is denied by the exact no-permission PMP match");
        Expect(!HasMmio(cpu, MachineMmioDevice::Timer, MachineMmioOperation::Write,
                        0x02004000ULL),
               "denied supervisor comparator store creates no timer MMIO write side effect");

        Expect(cpu.injectMachineUartReceiveByte(0x41) &&
               cpu.injectMachineUart1ReceiveByte(0x5A),
               "UART receive bytes are queued after OpenSBI has handed off to the guest");

        finalRun = RunFirmware(cpu, kDomainStepBudget);
        Expect(finalRun.complete && finalRun.sawSbiTrapInM && finalRun.sawSbiReturnToS,
               "guest services the timer interrupt and writes its completion marker");
        Expect(cpu.privilege() == PrivilegeMode::Supervisor &&
               cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
               "OpenSBI hands off to S-mode after emitting its authentic v1.8 console banner");
        Expect(cpu.machineUartOutput().find("Domain1 Name") != std::string::npos &&
               cpu.machineUartOutput().find("supervisor-domain") != std::string::npos &&
               cpu.machineUartOutput().find("Boot HART Domain") != std::string::npos,
               "OpenSBI's console log identifies the selected domain and boot-hart assignment");
        Expect(cpu.machineUartOutput().find("RISCV-16 restricted domain: UART and PLIC authorized")
                   != std::string::npos,
               "S-mode guest transmits its complete authorization message through UART0");

        Expect(savedFirmware && HasDenied(cpu, AccessType::Store, kFirmwareEntry,
                                         kFirmwareEntry, true),
               "guest firmware store has an exact PMP-denial record before RAM side effects");

        std::array<uint64_t, 15> signature{};
        bool signatureReadable = true;
        for (size_t index = 0; index < signature.size(); ++index) {
            signatureReadable = signatureReadable &&
                ReadWord(cpu, kDomainSignature + index * 8ULL, signature[index]);
        }
        Expect(signatureReadable && signature[0] == 0x47584F5352495356ULL &&
               signature[1] == 3 && signature[2] == 0 && signature[3] == 16 &&
               signature[4] == deadline && signature[5] == 0x41 &&
               signature[6] == 0x5A && signature[7] == 1 && signature[8] >= 1 &&
               signature[9] == 15 && (signature[10] >> 60) == 8 &&
               signature[12] == kNonidentityVirtual &&
               signature[13] == kNonidentityValue &&
               signature[14] == kDomainComplete,
               "guest signature proves both identity and nonidentity Sv39 access plus policy completion");
        if (signatureReadable && signature[12] == kNonidentityVirtual &&
            signature[13] == kNonidentityValue) {
            uint64_t translatedValue = 0;
            Expect(ReadWord(cpu, kNonidentityPhysical, translatedValue) &&
                   translatedValue == kNonidentityValue,
                   "nonidentity Sv39 store reaches the intended permitted physical RAM page");
            uint64_t identityValue = 0;
            Expect(ReadWord(cpu, kIdentityAddress, identityValue) &&
                   identityValue == kIdentityValue,
                   "identity Sv39 store reaches the intended permitted physical RAM page");
        } else {
            Expect(false, "nonidentity Sv39 virtual address and value are recorded in the signature");
        }
        if (!signatureReadable || signature[0] != 0x47584F5352495356ULL ||
            signature[1] != 3 || signature[2] != 0 || signature[3] != 16 ||
            signature[4] != deadline || signature[5] != 0x41 || signature[6] != 0x5A ||
            signature[7] != 1 || signature[8] != 1 || signature[9] != 15 ||
            (signature[10] >> 60) != 8 || signature[12] != kNonidentityVirtual ||
            signature[13] != kNonidentityValue || signature[14] != kDomainComplete) {
            std::cerr << "guest signature:";
            for (size_t index = 0; index < signature.size(); ++index)
                std::cerr << " [" << index << "]=0x" << std::hex << signature[index];
            std::cerr << std::dec << '\n';
        }
        Expect(cpu.mtimecmp() == UINT64_MAX,
               "authentic SBI TIME disarms the machine comparator after S-mode interrupt service");
        Expect(cpu.machineUartReceiveOverrun() == false &&
               cpu.machineUart1ReceiveOverrun() == false &&
               !cpu.machineUartReceiveDataReady() && !cpu.machineUart1ReceiveDataReady(),
               "both permitted guest receive reads consume exactly their own queued UART byte");

        std::vector<PmpEntryView> activeEntries;
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView entry;
            if (cpu.pmpEntry(index, entry) && entry.active) activeEntries.push_back(entry);
        }
        Expect(activeEntries.size() == 15,
               "selected domain programs eleven explicit regions plus four inherited machine-only windows");
        bool pmpOrderValid = activeEntries.size() == kExpectedPmpRegions.size();
        for (size_t index = 0; pmpOrderValid && index < kExpectedPmpRegions.size(); ++index) {
            const PmpEntryView& actual = activeEntries[index];
            const ExpectedPmpRegion& expected = kExpectedPmpRegions[index];
            pmpOrderValid = actual.config == expected.config &&
                actual.lowerBound == expected.lower && actual.upperBound == expected.upper;
        }
        Expect(pmpOrderValid,
               "OpenSBI sorts the fifteen effective PMP windows by region size and matching priority");
        Expect(EntryIs(cpu, 0x18, 0x02004000ULL, 0x02004008ULL) &&
               EntryIs(cpu, 0x18, 0x0200BFF8ULL, 0x0200C000ULL) &&
               EntryIs(cpu, 0x1B, 0x10000000ULL, 0x10000100ULL) &&
               EntryIs(cpu, 0x1B, 0x1000A000ULL, 0x1000A100ULL) &&
               EntryIs(cpu, 0x19, 0x83C00000ULL, 0x83C04000ULL) &&
               EntryIs(cpu, 0x1B, 0x0C000000ULL, 0x0C400000ULL),
               "PMP state encodes ACLINT deny, separate UARTs, read-only page tables, and full PLIC aperture");
        Expect(EntryIs(cpu, 0x1F, 0x80200000ULL, 0x80400000ULL) &&
               EntryIs(cpu, 0x1F, 0x80400000ULL, 0x80800000ULL) &&
               EntryIs(cpu, 0x1F, 0x80800000ULL, 0x81000000ULL) &&
               EntryIs(cpu, 0x1F, 0x81000000ULL, 0x82000000ULL) &&
               EntryIs(cpu, 0x1F, 0x82000000ULL, 0x84000000ULL),
               "PMP state covers only the five aligned guest RAM windows after the firmware reservation");
        Expect(EntryIs(cpu, 0x18, kFirmwareEntry, kFirmwareTextEnd) &&
               EntryIs(cpu, 0x18, kFirmwareTextEnd, kFirmwareDataEnd),
               "OpenSBI inherits both M-mode firmware windows with S-mode access denied");
        bool catchAll = false;
        for (const PmpEntryView& entry : activeEntries) {
            catchAll = catchAll || (entry.lowerBound == 0 && entry.upperBound >= (1ULL << 56));
        }
        Expect(!catchAll, "effective supervisor policy contains no physical catch-all grant");

        std::set<uint16_t> addressRegisters;
        bool sawPmpConfigWrite = false;
        for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
            if (event.csr >= 0x3B0 && event.csr < 0x3C0) addressRegisters.insert(event.csr);
            if (event.kind == PmpTraceKind::CsrWrite && event.csr == 0x3A0)
                sawPmpConfigWrite = true;
        }
        Expect(addressRegisters.size() == 16 && sawPmpConfigWrite,
               "OpenSBI executes real PMP address probes and configuration CSR writes");

        bool allGuestTraps = true;
        std::array<uint64_t, 16> guestFaultPcs{};
        for (size_t index = 0; index < kGuestFaultCauses.size(); ++index) {
            uint64_t cause = UINT64_MAX, faultAddress = UINT64_MAX, pc = 0;
            const uint64_t record = kDomainResult + 0x208 + index * 24ULL;
            const bool readable = ReadWord(cpu, record, cause) &&
                ReadWord(cpu, record + 8, faultAddress) && ReadWord(cpu, record + 16, pc);
            const bool valid = readable && cause == kGuestFaultCauses[index] &&
                               faultAddress == kGuestFaultAddresses[index] && pc != 0;
            guestFaultPcs[index] = pc;
            allGuestTraps = allGuestTraps && valid;
            if (!valid) {
                std::cerr << "fault log mismatch index=" << index << " readable=" << readable
                          << " cause=0x" << std::hex << cause << " address=0x" << faultAddress
                          << " pc=0x" << pc << std::dec << '\n';
            }
        }
        Expect(allGuestTraps,
               "S-mode trap handler recovers only from all sixteen expected faults with exact causes and stval");
        Expect(HasDenied(cpu, AccessType::Load, 0x80000000ULL, 0, false, -2, 0x18) &&
               HasDenied(cpu, AccessType::Store, 0x80000000ULL, 0, false, -2, 0x18) &&
               HasDenied(cpu, AccessType::Fetch, 0x80000000ULL, 0, false, -2, 0x18),
               "bare S-mode firmware load, store, and fetch are denied by PMP");
        Expect(HasDenied(cpu, AccessType::Load, 0x80000000ULL, 0x40001000ULL, true) &&
               HasDenied(cpu, AccessType::Fetch, 0x80000000ULL, 0x40000000ULL, true),
               "Sv39 firmware aliases and instruction fetch retain physical PMP denial");
        const bool pageTableReadDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [&](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Load && event.physicalAddress == 0x80000000ULL &&
                       event.pc == guestFaultPcs[9] && event.matchingEntry >= 0 &&
                       event.config == 0x18 && !event.hasVirtualAddress;
            });
        Expect(pageTableReadDenied,
               "protected firmware page-table read is denied by PMP during the guest's recorded fault");
        const bool adWriteDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [&](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Store && event.physicalAddress == 0x83C02018ULL &&
                       event.pc == guestFaultPcs[10] && event.config == 0x19 && !event.allowed;
            });
        Expect(adWriteDenied,
               "implicit Sv39 A-bit PTE update is denied by the S-read-only PMP entry");
        Expect(HasDenied(cpu, AccessType::Store, 0x02004000ULL, 0x40004000ULL, true) &&
               HasDenied(cpu, AccessType::Load, 0x0200BFF8ULL),
               "bare and Sv39 ACLINT comparator/counter accesses are denied by PMP");
        Expect(HasDenied(cpu, AccessType::Load, 0x1000B100ULL) &&
               HasDenied(cpu, AccessType::Load, 0x84000000ULL) &&
               HasDenied(cpu, AccessType::Load, 0x40000000ULL) &&
               HasDenied(cpu, AccessType::Store, 0x1000A100ULL),
               "unassigned, out-of-RAM, and out-of-aperture MMIO accesses reach PMP denial");
        Expect(!HasDenied(cpu, AccessType::Load, 0, 0x40005000ULL, true),
               "execute-only page produces an architectural page fault before final physical access");

        Expect(HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Read, 0x10000000ULL) &&
               HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Write, 0x10000000ULL) &&
               HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Read, 0x1000A000ULL),
               "guest instructions perform permitted UART0 RX/TX and UART1 RX accesses");
        Expect(HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C000030ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C002080ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Read, 0x0C201004ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C201004ULL),
               "guest configures S-context PLIC priority/enables and performs claim/completion");
        Expect(!HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Write, 0x1000A100ULL),
               "out-of-window UART store is denied before the MMIO device can record a side effect");
        const MachinePlicDiagnostics& plicEvidence = cpu.machinePlicDiagnostics();
        Expect(plicEvidence.priorityWrites > 0 && plicEvidence.enableWrites > 0 &&
               plicEvidence.thresholdWrites > 0 && plicEvidence.claimReads == 1 &&
               plicEvidence.completionWrites == 1,
               "PLIC S-context diagnostics confirm exactly one external-source claim and completion");
        Expect(signature[7] == 1 && signature[8] == 1,
               "guest handled one real UART1 external interrupt and one SBI TIME interrupt");

        std::array<uint8_t, 0x3000> visibleFdtBytes{};
        const bool fdtReadable = cpu.readPhysical(kFdtAddress, visibleFdtBytes.data(),
                                                  visibleFdtBytes.size());
        guestFdt = fdtReadable ? ParseFdt(std::vector<uint8_t>(visibleFdtBytes.begin(),
                                                               visibleFdtBytes.end())) : FdtTree{};
        Expect(guestFdt.valid, "OpenSBI leaves a valid supervisor-visible fixed-up FDT");
        Expect(guestFdt.valid && guestFdt.totalSize <= boot_contract::kFdtCapacity,
               "OpenSBI FDT fixup growth remains within the allocated 12 KiB FDT reservation");
        Expect(guestFdt.valid && guestFdt.nodes.find("/chosen/opensbi-domains") == guestFdt.nodes.end(),
               "OpenSBI removes the privileged domain configuration node from S-mode FDT");
        const auto* cpuDomainAssignment = Property(guestFdt, "/cpus/cpu@0", "opensbi-domain");
        Expect(PropertyStringEquals(guestFdt, "/cpus/cpu@0", "status", "okay") &&
               cpuDomainAssignment == nullptr &&
               Property(guestFdt, "/cpus/cpu@0", "mmu-type") != nullptr,
               "fixed-up FDT exposes the assigned boot CPU as enabled and removes OpenSBI-only assignment");
        Expect(PropertyStringEquals(guestFdt, "/aclint-mtimer@200bff8", "status", "disabled"),
               "fixed-up FDT marks direct ACLINT access disabled");
        Expect(PropertyStringEquals(guestFdt, "/soc/serial@10000000", "status", "okay") &&
               PropertyStringEquals(guestFdt, "/soc/serial@1000a000", "status", "okay") &&
               PropertyStringEquals(guestFdt, "/soc/interrupt-controller@c000000", "status", "okay"),
               "fixed-up FDT retains enabled UART0, UART1, and PLIC nodes");
        if (guestFdt.valid) {
            std::ofstream fixedFdt(std::string(argv[5]) + ".fixedup.dtb",
                                   std::ios::binary | std::ios::out | std::ios::trunc);
            if (fixedFdt) fixedFdt.write(
                reinterpret_cast<const char*>(visibleFdtBytes.data()), guestFdt.totalSize);
        }

        if (guestFdt.valid) {
            std::vector<uint8_t> fixedFdt(visibleFdtBytes.begin(),
                visibleFdtBytes.begin() + static_cast<std::ptrdiff_t>(guestFdt.totalSize));
            std::vector<std::string> fdtFailures;
            std::vector<std::string> reservationFailures;
            std::vector<std::string> pmpFailures;
            const bool fdtValid = ValidateVisiblePlatformFdt(guestFdt, fdtFailures);
            const bool reservationsValid = ValidateReservations(
                guestFdt, guestFdt.totalSize, reservationFailures);
            const bool pmpValid = ComparePmpPolicy(cpu, pmpFailures);
            const bool handoffValid = milestoneRun.handoffObserved &&
                milestoneRun.handoffHartId == 0 &&
                milestoneRun.handoffFdt == boot_contract::kFdtAddress &&
                milestoneRun.handoffSatp == 0 &&
                milestoneRun.handoffPrivilege == PrivilegeMode::Supervisor &&
                milestoneRun.handoffPc == boot_contract::kSupervisorEntry;
            Expect(fdtValid, "machine-readable FDT/device comparison matches initialized hardware");
            Expect(reservationsValid,
                   "final reservations are bounded, nonoverlapping, no-map, and contain the fixed-up FDT");
            Expect(pmpValid, "all sixteen effective PMP entries match their exact encoded contract");
            Expect(handoffValid, "structured handoff evidence captures the actual Bare-to-S-mode boundary");
            Expect(HasDenied(cpu, AccessType::Load, 0x40000000ULL) &&
                   HasDenied(cpu, AccessType::Load, 0x84000000ULL),
                   "unmatched S-mode physical addresses remain denied without a PMP catch-all");

            std::vector<std::string> allFailures = fdtFailures;
            allFailures.insert(allFailures.end(), reservationFailures.begin(), reservationFailures.end());
            allFailures.insert(allFailures.end(), pmpFailures.begin(), pmpFailures.end());
            if (!handoffValid) allFailures.emplace_back("actual OpenSBI handoff state mismatch");
            const std::filesystem::path artifactDirectory =
                std::filesystem::path(argv[5]).parent_path();
            WriteReservationJson(artifactDirectory, guestFdt, guestFdt.totalSize,
                                reservationFailures);
            WriteFinalFdtJson(artifactDirectory, guestFdt, fdt.size(),
                              boot_contract::kFdtCapacity, fdtFailures);
            WriteEffectivePmpJson(artifactDirectory, cpu);
            WritePmpComparisonJson(artifactDirectory, cpu, pmpValid, pmpFailures);
            WriteMemoryInventoryJson(artifactDirectory, firmware, firmwareElf,
                                     firmwareLayout, supervisor, guestLayout, fdt,
                                     guestFdt.totalSize, reservationFailures);
            WriteHandoffJson(artifactDirectory, cpu, milestoneRun);
            WriteBootContractJson(artifactDirectory, cpu, milestoneRun, firmware,
                                  firmwareElf, supervisor, fdt, fixedFdt,
                                  firmwareLayout, guestLayout, guestFdt, fdtValid,
                                  reservationsValid, pmpValid, handoffValid);
            WriteValidationFailuresJson(artifactDirectory, allFailures);
        }

        DumpIntegrationTrace(argv[5], cpu, milestoneRun, finalRun, guestFdt);
        std::cout << "Milestone steps=" << milestoneRun.steps << " final steps="
                  << finalRun.steps << " timer deadline=0x" << std::hex << deadline
                  << " final mtimecmp=0x" << cpu.mtimecmp() << std::dec << '\n';
        std::cout << "Final effective PMP regions=" << activeEntries.size()
                  << " unique denied physical accesses=" << cpu.pmpDeniedTrace().size()
                  << " PLIC claims=" << plicEvidence.claimReads
                  << " PLIC completions=" << plicEvidence.completionWrites << '\n';
    }
    std::cout << "RISCV-16 authentic OpenSBI domain checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
