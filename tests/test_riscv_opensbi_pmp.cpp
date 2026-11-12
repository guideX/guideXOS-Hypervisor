#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"

#include <cstring>
#include <set>

namespace {

constexpr char kSupervisorPmpSha256[] = "69F76196419A66F00EDCDD473F7E314E9AEFAE1595E79E444CFE8523516804EA";
constexpr char kPmpMessage[] = "Hello from PMP on guideXOS";
constexpr std::array<uint64_t, 7> kExpectedCauses{{5, 7, 1, 5, 5, 1, 13}};
constexpr std::array<uint64_t, 7> kExpectedFaultAddresses{{
    kFirmwareEntry, kFirmwareEntry, kFirmwareEntry, 0x40000000ULL,
    0x40001000ULL, 0x40000000ULL, 0x40002000ULL}};
constexpr uint64_t kPmpAddressMask = (1ULL << 54) - 1ULL;

bool ReadPmpWord(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

bool FindDenyingFirmwareRules(const RV64CPU& cpu, uint64_t firmwareEnd,
                              std::vector<std::pair<size_t, PmpEntryView>>& matched) {
    uint64_t cursor = kFirmwareEntry;
    while (cursor < firmwareEnd) {
        size_t selectedIndex = cpu.pmpImplementedEntries();
        PmpEntryView selected;
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView entry;
            if (!cpu.pmpEntry(index, entry) || !entry.active ||
                entry.lowerBound > cursor || entry.upperBound <= cursor) continue;
            selectedIndex = index;
            selected = entry;
            break;
        }
        if (selectedIndex == cpu.pmpImplementedEntries() ||
            (selected.config & 0x07U) != 0) return false;
        if (matched.empty() || matched.back().first != selectedIndex) {
            matched.emplace_back(selectedIndex, selected);
        }
        cursor = std::min(firmwareEnd, selected.upperBound);
    }
    return true;
}

bool EntryEquals(const RV64CPU& cpu, size_t index, uint8_t config,
                 uint64_t lowerBound, uint64_t upperBound) {
    PmpEntryView entry;
    return cpu.pmpEntry(index, entry) && entry.active && entry.config == config &&
           entry.lowerBound == lowerBound && entry.upperBound == upperBound;
}

void PrintPmpEvidence(const RV64CPU& cpu, const RunEvidence& run,
                      uint64_t firmwareEnd) {
    std::set<uint16_t> addressRegisters;
    std::cout << "OpenSBI PMP CSR trace entries=" << cpu.pmpCsrTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
        const char* kind = event.kind == PmpTraceKind::CsrRead ? "read" : "write";
        std::cout << "  PMP CSR " << kind << " csr=0x" << std::hex << event.csr
                  << " value=0x" << event.csrValue << " pc=0x" << event.pc << std::dec << '\n';
        if (event.csr >= 0x3B0 && event.csr < 0x3F0) {
            addressRegisters.insert(event.csr);
        }
    }
    std::cout << "OpenSBI-accessed PMP address registers=" << addressRegisters.size()
              << " expected-count=" << cpu.pmpImplementedEntries() << '\n';

    std::cout << "OpenSBI effective PMP entries (firmware ELF end=0x" << std::hex
              << firmwareEnd << "):\n";
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        (void)cpu.pmpEntry(index, entry);
        if (!entry.active) continue;
        std::cout << "  entry=" << std::dec << index << " cfg=0x" << std::hex
                  << static_cast<unsigned>(entry.config) << " pmpaddr=0x"
                  << entry.encodedAddress << " range=[0x" << entry.lowerBound
                  << ",0x" << entry.upperBound << ")\n";
    }
    std::cout << "Guest run: steps=" << run.steps << " M-retired=" << run.mRetired
              << " S-retired=" << run.sRetired << " traps=" << run.traps
              << " SBI-ECALLs=" << run.sbiEcalls << " completion=" << run.complete << '\n';
    std::cout << "Unique denied physical checks=" << cpu.pmpDeniedTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
        std::cout << "  denied access=" << static_cast<unsigned>(event.access)
                  << " privilege=" << PrivilegeModeToString(event.privilege)
                  << " pa=0x" << std::hex << event.physicalAddress;
        if (event.hasVirtualAddress) std::cout << " va=0x" << event.virtualAddress;
        std::cout << " size=" << std::dec << event.size << " entry="
                  << static_cast<int>(event.matchingEntry) << " cfg=0x" << std::hex
                  << static_cast<unsigned>(event.config) << " pc=0x" << event.pc << '\n';
    }
}

void CheckGuestResult(const RV64CPU& cpu) {
    const uint64_t signature = kResultAddress + 0x100;
    std::array<uint64_t, 15> words{};
    bool readable = true;
    for (size_t index = 0; index < words.size(); ++index) {
        readable = readable && ReadPmpWord(cpu, signature + 8ULL * index, words[index]);
    }
    Expect(readable, "PMP guest result signature is readable through permitted S-mode RAM");
    Expect(words[0] == kSignatureMagic && words[1] == 2 && words[2] == 0 && words[3] == 7,
           "guest-authored version 2 PMP signature reports success and seven recovered traps");
    Expect(words[4] == kSbiSpecVersion && words[5] == 42 && words[6] == 42 &&
           words[7] == 0x6A09E667F3BCC909ULL && words[8] == 0x1122334455667788ULL,
           "guest signature validates SBI Base, arithmetic, stack, and data accesses");
    Expect(words[9] == kFirmwareEntry && words[10] == 0x40000000ULL &&
           words[11] == 0x40001000ULL && words[12] == 0x40002000ULL && words[13] == 1,
           "guest signature identifies physical and Sv39 test addresses");
    Expect(words[14] == kComplete,
           "guest wrote the completion marker after the result signature");

    std::vector<uint8_t> message(std::strlen(kPmpMessage) + 1, 0xFF);
    Expect(cpu.readPhysical(kResultAddress, message.data(), message.size()) &&
           std::string(reinterpret_cast<const char*>(message.data())) == kPmpMessage,
           "guest writes the required PMP message into permitted guest RAM");

    uint64_t count = 0;
    Expect(ReadPmpWord(cpu, kResultAddress + 0x200, count) && count == kExpectedCauses.size(),
           "guest trap handler recorded all expected access and page faults");
    for (size_t index = 0; index < kExpectedCauses.size(); ++index) {
        uint64_t cause = UINT64_MAX;
        uint64_t address = UINT64_MAX;
        uint64_t pc = 0;
        const uint64_t record = kResultAddress + 0x208 + 24ULL * index;
        const bool recordReadable = ReadPmpWord(cpu, record, cause) &&
            ReadPmpWord(cpu, record + 8, address) && ReadPmpWord(cpu, record + 16, pc);
        Expect(recordReadable && cause == kExpectedCauses[index] &&
               address == kExpectedFaultAddresses[index],
               "guest trap recovery records the expected cause and fault address at index " +
                   std::to_string(index));
        Expect(pc != 0, "guest trap record retains a nonzero fault PC");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_opensbi_pmp fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> supervisor = ReadFile(argv[3]);
    const std::vector<uint8_t> fdt = ReadFile(argv[4]);
    Expect(Sha256(firmware) == kFirmwareBinSha256,
           "integration uses the unchanged pinned OpenSBI FW_JUMP binary");
    Expect(Sha256(firmwareElf) == kFirmwareElfSha256,
           "integration uses the unchanged pinned OpenSBI ELF");
    Expect(Sha256(supervisor) == kSupervisorPmpSha256,
           "independent PMP supervisor payload matches its fixture manifest");
    Expect(Sha256(fdt) == kDtbSha256,
           "integration reuses the accepted platform FDT without modification");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    Expect(firmwareLayout.valid && firmwareLayout.machine == 243 &&
           firmwareLayout.entry == kFirmwareEntry,
           "pinned OpenSBI ELF has the expected RISC-V FW_JUMP entry");
    const ElfLayout supervisorLayout = InspectElf(supervisor);
    Expect(supervisorLayout.valid && supervisorLayout.machine == 243 &&
           supervisorLayout.entry == kPayloadEntry,
           "independent PMP guest is a loadable RV64 supervisor ELF");

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    Expect(cpu.pmpImplementedEntries() == 16,
           "authentic firmware executes on the explicit standard PMP16 machine profile");
    const bool prepared = PrepareGuest(cpu, firmware, supervisor, fdt);
    Expect(prepared, "authentic OpenSBI, PMP guest, and FDT load into bounded guest RAM");
    if (prepared) {
        Expect(cpu.sbiProvider() == nullptr,
               "authentic OpenSBI test has no host-side SBI service provider");
        Expect(cpu.pmpImplementedEntries() == 16,
               "PMP capacity remains exactly sixteen before guest firmware runs");
        const RunEvidence run = RunFirmware(cpu, kStepBudget);
        PrintEvidence(run, cpu, firmware, fdt);
        PrintPmpEvidence(cpu, run, firmwareLayout.highest);

        Expect(run.mRetired > 0 && run.sRetired > 0,
               "OpenSBI and the independent supervisor payload both retire instructions");
        Expect(run.handedOff && cpu.privilege() == PrivilegeMode::Supervisor,
               "OpenSBI performs a real M-to-S handoff under active PMP configuration");
        Expect(run.sawSbiTrapInM && run.sawSbiReturnToS,
               "guest executes a genuine SBI Base call through OpenSBI after protection checks");
        Expect(run.complete, "PMP guest writes its completion marker within the execution budget");
        Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
               "authentic OpenSBI boot output is emitted through modeled UART MMIO");
        Expect(cpu.machineUartOutput().find(kPmpMessage) != std::string::npos,
               "S-mode guest's authorized UART output contains the PMP completion message");
        if (run.complete) CheckGuestResult(cpu);

        std::set<uint16_t> probedAddressCsrs;
        bool sawAddressMask = false;
        for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
            if (event.csr >= 0x3B0 && event.csr < 0x3C0) {
                probedAddressCsrs.insert(event.csr);
            }
            if (event.kind == PmpTraceKind::CsrRead && event.csr == 0x3B0 &&
                event.csrValue == kPmpAddressMask) {
                sawAddressMask = true;
            }
        }
        Expect(probedAddressCsrs.size() == 16,
               "OpenSBI guest CSR probes discover exactly pmpaddr0 through pmpaddr15");
        Expect(sawAddressMask,
               "OpenSBI guest readback detects the implemented 54-bit pmpaddr mask");

        std::vector<std::pair<size_t, PmpEntryView>> firmwareRules;
        const bool protectedRange = FindDenyingFirmwareRules(
            cpu, firmwareLayout.highest, firmwareRules);
        Expect(protectedRange,
               "OpenSBI-programmed PMP rules deny S-mode across the actual firmware ELF range");
        if (protectedRange) {
            std::cout << "OpenSBI firmware protection covers ELF range [0x" << std::hex
                      << kFirmwareEntry << ",0x" << firmwareLayout.highest << ") via"
                      << std::dec << firmwareRules.size() << " entries:\n";
            for (const auto& [index, entry] : firmwareRules) {
                std::cout << "  entry=" << index << " range=[0x" << std::hex
                          << entry.lowerBound << ",0x" << entry.upperBound << ") cfg=0x"
                          << static_cast<unsigned>(entry.config) << std::dec << '\n';
            }
        }

        const std::string& uart = cpu.machineUartOutput();
        Expect(uart.find("Boot HART PMP Count         : 16") != std::string::npos &&
               uart.find("Boot HART PMP Granularity   : 2 bits") != std::string::npos &&
               uart.find("Boot HART PMP Address Bits  : 54") != std::string::npos,
               "OpenSBI reports guest-probed count, four-byte granularity, and 54-bit PMP addresses");
        Expect(uart.find("Domain0 Region00            : 0x0000000080080000-0x000000008009ffff") != std::string::npos &&
               uart.find("Domain0 Region01            : 0x0000000080000000-0x000000008007ffff") != std::string::npos &&
               uart.find("Domain0 Region02            : 0x0000000010000000-0x0000000010000fff") != std::string::npos &&
               uart.find("Domain0 Region03            : 0x0000000000000000-0xffffffffffffffff") != std::string::npos,
               "OpenSBI's own report exposes firmware text/data, UART, and catch-all domain ranges");
        Expect(EntryEquals(cpu, 0, 0x18, 0x80080000ULL, 0x800A0000ULL) &&
               EntryEquals(cpu, 1, 0x18, 0x80000000ULL, 0x80080000ULL) &&
               EntryEquals(cpu, 2, 0x1B, 0x10000000ULL, 0x10001000ULL) &&
               EntryEquals(cpu, 3, 0x1F, 0, 1ULL << 57),
               "effective PMP entries match the firmware, UART, and catch-all regions reported by OpenSBI");

        const bool bareLoadDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Load &&
                       event.physicalAddress == kFirmwareEntry;
            });
        const bool bareStoreDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Store &&
                       event.physicalAddress == kFirmwareEntry;
            });
        const bool fetchDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Fetch &&
                       event.physicalAddress == kFirmwareEntry;
            });
        const bool sv39Denied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.hasVirtualAddress && event.virtualAddress == 0x40000000ULL &&
                       event.physicalAddress == kFirmwareEntry;
            });
        Expect(bareLoadDenied && bareStoreDenied && fetchDenied,
               "bounded physical-access trace proves S-mode firmware load/store/fetch denial");
        Expect(sv39Denied,
               "bounded physical-access trace proves PMP denial after Sv39 translation");
    }

    std::cout << "RISCV-15 authentic OpenSBI PMP checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
