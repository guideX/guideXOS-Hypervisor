#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"
#include "riscv_linux_test_helpers.h"
#include "riscv_trap_accounting.h"

#include <cctype>
#include <filesystem>
#include <limits>
#include <unordered_map>

namespace {

constexpr uint64_t kOpenSbiStepBudget = 20000000ULL;
constexpr uint64_t kLinuxRepeatedPcLimit = 100000ULL;
constexpr size_t kFirmwareTraceLimit = 64;
constexpr size_t kLinuxStepBudget = 100000;
constexpr size_t kLinuxTraceHeadLimit = 64;
constexpr size_t kLinuxTraceTailLimit = 64;

struct FirmwareTraceLine {
    StepResult step;
    PrivilegeMode before = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
};

struct FirmwareRun {
    uint64_t steps = 0;
    uint64_t machineRetired = 0;
    uint64_t supervisorRetired = 0;
    uint64_t traps = 0;
    uint64_t interrupts = 0;
    uint64_t sbiEcalls = 0;
    uint64_t mretCount = 0;
    bool handoff = false;
    std::string stopReason;
    std::deque<FirmwareTraceLine> trace;
};

struct ExpectedRegion {
    const char* name;
    uint8_t config;
    uint64_t lower;
    uint64_t upper;
};

const std::array<ExpectedRegion, 14> kLinuxEffectivePmp{{
    {"OpenSBI-data-scratch", 0x18,
     boot_contract::kOpenSbiProtectedRanges[1].start,
     boot_contract::kOpenSbiProtectedRanges[1].start +
         boot_contract::kOpenSbiProtectedRanges[1].size},
    {"OpenSBI-text", 0x18,
     boot_contract::kOpenSbiProtectedRanges[0].start,
     boot_contract::kOpenSbiProtectedRanges[0].start +
         boot_contract::kOpenSbiProtectedRanges[0].size},
    {"ACLINT-mtimecmp-domain-deny", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtimecmp-machine-deny", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtime-domain-deny", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"ACLINT-mtime-machine-deny", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"UART0", 0x1B, boot_contract::kDevices[0].start,
     boot_contract::kDevices[0].start + boot_contract::kDevices[0].size},
    {"UART1", 0x1B, boot_contract::kDevices[1].start,
     boot_contract::kDevices[1].start + boot_contract::kDevices[1].size},
    {"guest-ram-2m", 0x1F, boot_contract::kPmpGuestRamWindows[0].start,
     boot_contract::kPmpGuestRamWindows[0].start +
         boot_contract::kPmpGuestRamWindows[0].size},
    {"PLIC", 0x1B, boot_contract::kDevices[2].start,
     boot_contract::kDevices[2].start + boot_contract::kDevices[2].size},
    {"guest-ram-4m", 0x1F, boot_contract::kPmpGuestRamWindows[1].start,
     boot_contract::kPmpGuestRamWindows[1].start +
         boot_contract::kPmpGuestRamWindows[1].size},
    {"guest-ram-8m", 0x1F, boot_contract::kPmpGuestRamWindows[2].start,
     boot_contract::kPmpGuestRamWindows[2].start +
         boot_contract::kPmpGuestRamWindows[2].size},
    {"guest-ram-16m", 0x1F, boot_contract::kPmpGuestRamWindows[3].start,
     boot_contract::kPmpGuestRamWindows[3].start +
         boot_contract::kPmpGuestRamWindows[3].size},
    {"guest-ram-32m", 0x1F, boot_contract::kPmpGuestRamWindows[4].start,
     boot_contract::kPmpGuestRamWindows[4].start +
         boot_contract::kPmpGuestRamWindows[4].size},
}};

std::string Upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
    return value;
}

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string JsonQuote(const std::string& value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string JsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyAt = json.find(needle);
    if (keyAt == std::string::npos) return {};
    const size_t colon = json.find(':', keyAt + needle.size());
    if (colon == std::string::npos) return {};
    const size_t begin = json.find('"', colon + 1);
    if (begin == std::string::npos) return {};
    const size_t end = json.find('"', begin + 1);
    if (end == std::string::npos) return {};
    return json.substr(begin + 1, end - begin - 1);
}

std::string AsString(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

FirmwareRun RunOpenSbiToLinux(RV64CPU& cpu) {
    FirmwareRun run;
    uint64_t priorPc = UINT64_MAX;
    uint64_t repeatedPc = 0;
    for (; run.steps < kOpenSbiStepBudget; ++run.steps) {
        const uint64_t pc = cpu.getPC();
        if (pc == priorPc) ++repeatedPc;
        else {
            priorPc = pc;
            repeatedPc = 1;
        }
        if (repeatedPc > kLinuxRepeatedPcLimit) {
            run.stopReason = "repeated PC limit at " + Hex(pc);
            break;
        }
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        if (run.trace.size() == kFirmwareTraceLimit) run.trace.pop_front();
        run.trace.push_back({step, before, cpu.privilege()});
        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++run.machineRetired;
            else if (before == PrivilegeMode::Supervisor) ++run.supervisorRetired;
            if (step.insn.mnemonic == Mnemonic::MRET) ++run.mretCount;
            if (cpu.privilege() == PrivilegeMode::Supervisor &&
                cpu.getPC() == boot_contract::kLinuxImageEntry) {
                run.handoff = true;
                run.stopReason = "OpenSBI transferred control to Linux Image entry";
                ++run.steps;
                break;
            }
        } else if (step.outcome == StepOutcome::Trap) {
            ++run.traps;
            if (step.interruptTaken) ++run.interrupts;
            if (before == PrivilegeMode::Supervisor &&
                step.trapCause == TrapCause::EcallFromS &&
                cpu.privilege() == PrivilegeMode::Machine && !step.sbiHandled) {
                ++run.sbiEcalls;
            }
        } else {
            run.stopReason = std::string("OpenSBI stopped on ") +
                StepOutcomeToString(step.outcome);
            break;
        }
    }
    if (run.stopReason.empty()) run.stopReason = "OpenSBI instruction budget exhausted";
    return run;
}

std::vector<uint8_t> ReadHandoffFdt(const RV64CPU& cpu) {
    std::vector<uint8_t> header(8, 0);
    if (!cpu.readPhysical(boot_contract::kLinuxFdtAddress, header.data(), header.size()))
        return {};
    const uint32_t totalSize = Be32(header, 4);
    if (totalSize < 40 || totalSize > boot_contract::kLinuxFdtCapacity) return {};
    std::vector<uint8_t> bytes(totalSize, 0);
    if (!cpu.readPhysical(boot_contract::kLinuxFdtAddress, bytes.data(), bytes.size()))
        return {};
    return bytes;
}

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::vector<RiscvLinuxPmpRegion> CapturePmp(const RV64CPU& cpu) {
    std::vector<RiscvLinuxPmpRegion> regions;
    regions.reserve(cpu.pmpImplementedEntries());
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (!cpu.pmpEntry(index, entry)) continue;
        regions.push_back({index, entry.lowerBound, entry.upperBound,
                           entry.config, entry.active});
    }
    return regions;
}

bool ComparePmp(const RV64CPU& cpu, std::vector<std::string>& failures) {
    bool valid = true;
    if (cpu.pmpImplementedEntries() != 16) {
        failures.emplace_back("CPU does not implement exactly 16 PMP entries");
        return false;
    }
    for (size_t index = 0; index < kLinuxEffectivePmp.size(); ++index) {
        PmpEntryView actual;
        const auto& expected = kLinuxEffectivePmp[index];
        const bool entryMatches = cpu.pmpEntry(index, actual) && actual.active &&
            actual.config == expected.config && actual.lowerBound == expected.lower &&
            actual.upperBound == expected.upper;
        if (!entryMatches) {
            failures.emplace_back("PMP entry " + std::to_string(index) +
                                  " differs from Linux policy region " + expected.name);
            valid = false;
        }
    }
    for (size_t index = kLinuxEffectivePmp.size(); index < 16; ++index) {
        PmpEntryView actual;
        if (!cpu.pmpEntry(index, actual) || actual.active) {
            failures.emplace_back("PMP entry " + std::to_string(index) +
                                  " should be disabled for the Linux profile");
            valid = false;
        }
    }
    return valid;
}

bool CompareReservations(const FdtTree& tree, std::vector<std::string>& failures) {
    struct ExpectedReservation { const char* path; uint64_t base; uint64_t size; };
    const std::array<ExpectedReservation, 4> expected{{
        {"/reserved-memory/mmode_resv1@80000000", 0x80000000ULL, 0x80000ULL},
        {"/reserved-memory/mmode_resv0@80080000", 0x80080000ULL, 0x20000ULL},
        {"/reserved-memory/opensbi-slot-gap@800a0000", 0x800A0000ULL, 0x160000ULL},
        {"/reserved-memory/firmware-fdt@82200000",
         boot_contract::kLinuxFdtAddress, boot_contract::kLinuxFdtCapacity},
    }};
    size_t observedCount = 0;
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        if (path.rfind("/reserved-memory/", 0) == 0 && path.find('/', 17) == std::string::npos)
            ++observedCount;
    }
    bool valid = observedCount == expected.size();
    if (!valid) failures.emplace_back("fixed-up FDT has unexpected reserved-memory node count");
    for (const auto& item : expected) {
        const bool rangeOk = LinuxFdtReg(tree, item.path, item.base, item.size);
        const auto* noMap = Property(tree, item.path, "no-map");
        const bool noMapOk = noMap != nullptr && noMap->empty();
        if (!rangeOk || !noMapOk) {
            failures.emplace_back(std::string("missing, malformed, or mappable firmware/FDT reservation ") +
                                  item.path);
            valid = false;
        }
    }
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        if (path.rfind("/reserved-memory/", 0) != 0) continue;
        if (path.find("supervisor-payload") != std::string::npos ||
            path.find("page-tables") != std::string::npos ||
            path.find("guest-result") != std::string::npos ||
            path.find("guest-stack") != std::string::npos ||
            path.find("linux-image") != std::string::npos) {
            failures.emplace_back("test-only or kernel no-map reservation leaked into Linux FDT: " + path);
            valid = false;
        }
    }
    return valid;
}

struct LinuxStepLine {
    StepResult step;
    PrivilegeMode before = PrivilegeMode::Supervisor;
    PrivilegeMode after = PrivilegeMode::Supervisor;
};

struct LinuxRun {
    uint64_t retired = 0;
    uint64_t traps = 0;
    uint64_t interrupts = 0;
    uint64_t sbiEcalls = 0;
    uint64_t lastRetiredPc = 0;
    uint64_t nextPc = 0;
    bool hasRetired = false;
    bool reachedFrontier = false;
    PrivilegeMode frontierPrivilegeBefore = PrivilegeMode::Supervisor;
    uint64_t frontierPhysicalAddress = 0;
    std::string stopReason;
    StepResult frontier;
    std::vector<LinuxStepLine> trace;
    std::deque<LinuxStepLine> tailTrace;
};

struct LinuxSymbols {
    bool valid = false;
    uint64_t imageStart = 0;
    uint64_t bssStart = 0;
    uint64_t bssStop = 0;
    uint64_t bootCpuHartId = 0;
    uint64_t initTask = 0;
    uint64_t initThreadUnion = 0;
    uint64_t earlyPgDir = 0;
    uint64_t trampolinePgDir = 0;
    uint64_t swapperPgDir = 0;
    uint64_t relocateEnableMmu = 0;
    uint64_t setupVm = 0;
    uint64_t handleException = 0;
    uint64_t socEarlyInit = 0;
    uint64_t startKernel = 0;
    uint64_t headTextAddress = 0;
    uint64_t headTextOffset = 0;
    uint64_t headTextSize = 0;
    std::map<std::string, uint64_t> values;

    uint64_t physical(uint64_t virtualAddress) const {
        return virtualAddress >= imageStart
            ? boot_contract::kLinuxImageEntry + (virtualAddress - imageStart) : 0;
    }
};

struct BssProgressSample {
    uint64_t stores = 0;
    uint64_t address = 0;
    bool zeroAfterGuestStore = false;
};

struct BssEvidence {
    uint64_t loopStorePc = 0;
    uint64_t loopBranchPc = 0;
    uint64_t loopExitPc = 0;
    uint64_t expectedStart = 0;
    uint64_t expectedStop = 0;
    uint64_t initialPointer = 0;
    uint64_t initialStop = 0;
    uint64_t pointerAt100k = 0;
    uint64_t stopAt100k = 0;
    uint64_t storeCountAt100k = 0;
    uint64_t storesRetired = 0;
    uint64_t firstStoreAddress = 0;
    uint64_t lastStoreAddress = 0;
    uint64_t exitPointer = 0;
    uint64_t exitStop = 0;
    bool observedLoopEntry = false;
    bool boundsMatchSymbols = false;
    bool loopExitObserved = false;
    bool allMemoryZero = false;
    std::string zeroRangeSha256;
    std::vector<BssProgressSample> progress;
};

struct LinuxSnapshot {
    std::string name;
    std::string detail;
    uint64_t pc = 0;
    uint64_t retired = 0;
    PrivilegeMode privilege = PrivilegeMode::Supervisor;
    uint64_t satp = 0;
    std::array<uint64_t, NUM_GENERAL_REGISTERS> registers{};
};

struct LinuxCsrChange {
    uint64_t pc = 0;
    PrivilegeMode privilege = PrivilegeMode::Supervisor;
    uint16_t csr = 0;
    uint64_t before = 0;
    uint64_t after = 0;
};

struct LinuxTrapEvent {
    size_t accountingEventIndex = SIZE_MAX;
    uint64_t pc = 0;
    uint32_t raw = 0;
    uint8_t length = 0;
    std::string disassembly;
    PrivilegeMode origin = PrivilegeMode::Supervisor;
    PrivilegeMode target = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
    TrapCause cause = TrapCause::IllegalInstruction;
    uint64_t interruptCause = 0;
    uint64_t vector = 0;
    uint64_t nextPc = 0;
    uint64_t mcause = 0;
    uint64_t mepc = 0;
    uint64_t mtval = 0;
    uint64_t scause = 0;
    uint64_t sepc = 0;
    uint64_t stval = 0;
    uint64_t mstatus = 0;
    uint64_t sstatus = 0;
    uint64_t satp = 0;
    bool interrupt = false;
    bool sbiHandled = false;
    bool progressAfterward = false;
    uint64_t resumedPc = 0;
    bool returned = false;
    uint64_t returnPc = 0;
    std::string transitionName;
    bool architecturalTransition = false;
    std::array<uint64_t, NUM_GENERAL_REGISTERS> registers{};
};

struct LinuxStage {
    uint64_t targetRetired = 0;
    uint64_t actualRetired = 0;
    uint64_t additionalRetired = 0;
    uint64_t stepCalls = 0;
    uint64_t pc = 0;
    uint64_t lastRetiredPc = 0;
    uint64_t bssStores = 0;
    uint64_t sModeRetired = 0;
    uint64_t mModeRetired = 0;
    uint64_t sModeHandlerInstructions = 0;
    uint64_t mModeHandlerInstructions = 0;
    uint64_t activeHandlers = 0;
    uint64_t maxNestingDepth = 0;
};

struct Sv39WalkLevel {
    unsigned level = 0;
    uint64_t tableAddress = 0;
    uint64_t pteAddress = 0;
    uint64_t pte = 0;
    bool readable = false;
};

struct ContinuationEvidence {
    BssEvidence bss;
    riscv_test::TrapAccounting accounting;
    uint64_t linuxStepCalls = 100000;
    uint64_t supervisorRetired = 100000;
    uint64_t machineRetiredAfterHandoff = 0;
    uint64_t trapEvents = 0;
    uint64_t interruptEvents = 0;
    uint64_t ecallEvents = 0;
    uint64_t sbiEcalls = 0;
    uint64_t sfenceVmaCount = 0;
    uint64_t firstSatpWritePc = 0;
    uint64_t firstSatpValue = 0;
    uint64_t finalSatp = 0;
    uint64_t repeatedArchitecturalStates = 0;
    std::string stopClassification = "instruction_budget";
    std::string stopReason = "bounded Linux instruction budget exhausted";
    bool stoppedAtArchitectureFrontier = false;
    bool setupVmReached = false;
    bool satpWritten = false;
    bool kernelVirtualExecutionReached = false;
    bool pmpUnchanged = false;
    bool fdtUnchanged = false;
    bool uartPrefixPreserved = false;
    uint64_t handoffRetiredCounter = 0;
    uint64_t handoffMtime = 0;
    uint64_t finalMtime = 0;
    uint64_t handoffMtimecmp = 0;
    uint64_t finalMtimecmp = 0;
    uint64_t handoffMmioSequence = 0;
    uint64_t finalMmioSequence = 0;
    uint64_t handoffMmioReads = 0;
    uint64_t finalMmioReads = 0;
    uint64_t handoffMmioWrites = 0;
    uint64_t finalMmioWrites = 0;
    std::vector<LinuxSnapshot> milestones;
    std::vector<LinuxCsrChange> csrChanges;
    std::vector<LinuxTrapEvent> traps;
    std::vector<LinuxStage> stages;
    std::vector<Sv39WalkLevel> firstPageWalk;
};

bool InRange(size_t offset, size_t bytes, size_t total) {
    return offset <= total && bytes <= total - offset;
}

std::string ElfString(const std::vector<uint8_t>& elf, uint64_t offset,
                      uint64_t size, uint32_t nameOffset) {
    if (nameOffset >= size || offset > elf.size() || size > elf.size() - offset) return {};
    const size_t begin = static_cast<size_t>(offset + nameOffset);
    size_t end = begin;
    const size_t limit = static_cast<size_t>(offset + size);
    while (end < limit && elf[end] != 0) ++end;
    if (end == limit) return {};
    return std::string(elf.begin() + static_cast<std::ptrdiff_t>(begin),
                       elf.begin() + static_cast<std::ptrdiff_t>(end));
}

std::map<std::string, uint64_t> ParseElfSymbolTable(const std::vector<uint8_t>& elf) {
    std::map<std::string, uint64_t> symbols;
    if (elf.size() < 64 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' ||
        elf[3] != 'F' || elf[4] != 2 || elf[5] != 1) return symbols;
    const uint64_t sectionOffset = Le64(elf, 40);
    const uint16_t sectionEntrySize = Le16(elf, 58);
    const uint16_t sectionCount = Le16(elf, 60);
    if (sectionEntrySize < 64 || sectionCount == 0 || sectionOffset > elf.size() ||
        static_cast<uint64_t>(sectionCount) * sectionEntrySize > elf.size() - sectionOffset) {
        return symbols;
    }
    struct Section { uint32_t type; uint64_t offset; uint64_t size; uint32_t link; uint64_t entrySize; };
    std::vector<Section> sections;
    sections.reserve(sectionCount);
    for (uint16_t index = 0; index < sectionCount; ++index) {
        const size_t at = static_cast<size_t>(sectionOffset +
            static_cast<uint64_t>(index) * sectionEntrySize);
        sections.push_back({Le32(elf, at + 4), Le64(elf, at + 24), Le64(elf, at + 32),
                            Le32(elf, at + 40), Le64(elf, at + 56)});
    }
    for (const Section& section : sections) {
        if (section.type != 2 || section.link >= sections.size() || section.entrySize < 24 ||
            section.offset > elf.size() || section.size > elf.size() - section.offset) continue;
        const Section& strings = sections[section.link];
        if (strings.offset > elf.size() || strings.size > elf.size() - strings.offset) continue;
        const uint64_t count = section.size / section.entrySize;
        for (uint64_t index = 0; index < count; ++index) {
            const size_t at = static_cast<size_t>(section.offset + index * section.entrySize);
            const std::string name = ElfString(elf, strings.offset, strings.size, Le32(elf, at));
            if (!name.empty()) symbols[name] = Le64(elf, at + 8);
        }
    }
    return symbols;
}

struct ElfFunctionSymbol {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
};

std::vector<ElfFunctionSymbol> ParseElfFunctionSymbols(const std::vector<uint8_t>& elf) {
    std::vector<ElfFunctionSymbol> functions;
    if (elf.size() < 64 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' ||
        elf[3] != 'F' || elf[4] != 2 || elf[5] != 1) return functions;
    const uint64_t sectionOffset = Le64(elf, 40);
    const uint16_t sectionEntrySize = Le16(elf, 58);
    const uint16_t sectionCount = Le16(elf, 60);
    if (sectionEntrySize < 64 || sectionCount == 0 || sectionOffset > elf.size() ||
        static_cast<uint64_t>(sectionCount) * sectionEntrySize > elf.size() - sectionOffset)
        return functions;
    struct Section { uint32_t type; uint64_t offset; uint64_t size; uint32_t link; uint64_t entrySize; };
    std::vector<Section> sections;
    sections.reserve(sectionCount);
    for (uint16_t index = 0; index < sectionCount; ++index) {
        const size_t at = static_cast<size_t>(sectionOffset +
            static_cast<uint64_t>(index) * sectionEntrySize);
        sections.push_back({Le32(elf, at + 4), Le64(elf, at + 24), Le64(elf, at + 32),
                            Le32(elf, at + 40), Le64(elf, at + 56)});
    }
    for (const Section& section : sections) {
        if (section.type != 2 || section.link >= sections.size() || section.entrySize < 24 ||
            section.offset > elf.size() || section.size > elf.size() - section.offset) continue;
        const Section& strings = sections[section.link];
        if (strings.offset > elf.size() || strings.size > elf.size() - strings.offset) continue;
        const uint64_t count = section.size / section.entrySize;
        for (uint64_t index = 0; index < count; ++index) {
            const size_t at = static_cast<size_t>(section.offset + index * section.entrySize);
            const uint8_t symbolType = elf[at + 4] & 0x0FU;
            if (symbolType != 2) continue; // STT_FUNC
            const uint64_t address = Le64(elf, at + 8);
            const uint64_t size = Le64(elf, at + 16);
            const std::string name = ElfString(elf, strings.offset, strings.size, Le32(elf, at));
            if (!name.empty() && size != 0 && address <= UINT64_MAX - size)
                functions.push_back({name, address, size});
        }
    }
    return functions;
}

std::string FunctionContaining(const std::vector<ElfFunctionSymbol>& functions,
                               uint64_t address) {
    const ElfFunctionSymbol* match = nullptr;
    for (const ElfFunctionSymbol& function : functions) {
        if (function.address <= address && address < function.address + function.size &&
            (match == nullptr || function.size < match->size)) match = &function;
    }
    return match != nullptr ? match->name : std::string{};
}

LinuxSymbols ParseLinuxVmlinux(const std::vector<uint8_t>& elf) {
    LinuxSymbols symbols;
    if (elf.size() < 64 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' ||
        elf[3] != 'F' || elf[4] != 2 || elf[5] != 1) return symbols;
    const uint64_t sectionOffset = Le64(elf, 40);
    const uint16_t sectionEntrySize = Le16(elf, 58);
    const uint16_t sectionCount = Le16(elf, 60);
    const uint16_t sectionNameIndex = Le16(elf, 62);
    if (sectionEntrySize < 64 || sectionCount == 0 || sectionNameIndex >= sectionCount ||
        sectionOffset > elf.size() ||
        static_cast<uint64_t>(sectionCount) * sectionEntrySize > elf.size() - sectionOffset) {
        return symbols;
    }
    struct Section { uint32_t type; uint64_t offset; uint64_t size; uint32_t link; uint64_t address; uint64_t entrySize; };
    std::vector<Section> sections;
    sections.reserve(sectionCount);
    for (uint16_t index = 0; index < sectionCount; ++index) {
        const size_t at = static_cast<size_t>(sectionOffset +
            static_cast<uint64_t>(index) * sectionEntrySize);
        sections.push_back({Le32(elf, at + 4), Le64(elf, at + 24), Le64(elf, at + 32),
                            Le32(elf, at + 40), Le64(elf, at + 16), Le64(elf, at + 56)});
    }
    const Section& sectionNames = sections[sectionNameIndex];
    const uint64_t sectionNameTableOffset = sectionNames.offset;
    const uint64_t sectionNameTableSize = sectionNames.size;
    if (sectionNameTableOffset > elf.size() ||
        sectionNameTableSize > elf.size() - sectionNameTableOffset) return symbols;

    for (uint16_t index = 0; index < sectionCount; ++index) {
        const size_t at = static_cast<size_t>(sectionOffset +
            static_cast<uint64_t>(index) * sectionEntrySize);
        const std::string name = ElfString(elf, sectionNameTableOffset, sectionNameTableSize,
                                           Le32(elf, at));
        if (name == ".head.text") {
            symbols.headTextAddress = sections[index].address;
            symbols.headTextOffset = sections[index].offset;
            symbols.headTextSize = sections[index].size;
        }
    }

    for (const Section& section : sections) {
        if (section.type != 2 || section.link >= sections.size() || section.entrySize < 24 ||
            section.offset > elf.size() || section.size > elf.size() - section.offset) continue;
        const Section& strings = sections[section.link];
        if (strings.offset > elf.size() || strings.size > elf.size() - strings.offset) continue;
        const uint64_t count = section.size / section.entrySize;
        for (uint64_t index = 0; index < count; ++index) {
            const size_t at = static_cast<size_t>(section.offset + index * section.entrySize);
            const std::string name = ElfString(elf, strings.offset, strings.size, Le32(elf, at));
            if (!name.empty()) symbols.values[name] = Le64(elf, at + 8);
        }
    }
    const auto get = [&symbols](const char* name) -> uint64_t {
        const auto found = symbols.values.find(name);
        return found == symbols.values.end() ? 0 : found->second;
    };
    symbols.imageStart = get("_start");
    symbols.bssStart = get("__bss_start");
    symbols.bssStop = get("__bss_stop");
    symbols.bootCpuHartId = get("boot_cpu_hartid");
    symbols.initTask = get("init_task");
    symbols.initThreadUnion = get("init_thread_union");
    symbols.earlyPgDir = get("early_pg_dir");
    symbols.trampolinePgDir = get("trampoline_pg_dir");
    symbols.swapperPgDir = get("swapper_pg_dir");
    symbols.relocateEnableMmu = get("relocate_enable_mmu");
    symbols.setupVm = get("setup_vm");
    symbols.handleException = get("handle_exception");
    symbols.socEarlyInit = get("soc_early_init");
    symbols.startKernel = get("start_kernel");
    symbols.valid = symbols.imageStart == 0xFFFFFFFF80000000ULL &&
        symbols.bssStart == 0xFFFFFFFF810E5000ULL &&
        symbols.bssStop == 0xFFFFFFFF8113B3A8ULL &&
        symbols.bootCpuHartId != 0 && symbols.initTask != 0 &&
        symbols.initThreadUnion != 0 && symbols.earlyPgDir != 0 &&
        symbols.trampolinePgDir != 0 && symbols.swapperPgDir != 0 &&
        symbols.relocateEnableMmu != 0 && symbols.setupVm != 0 &&
        symbols.handleException != 0 && symbols.socEarlyInit != 0 &&
        symbols.startKernel != 0 && symbols.headTextSize > 0;
    return symbols;
}

uint32_t ElfWordAt(const std::vector<uint8_t>& elf, uint64_t offset) {
    if (!InRange(static_cast<size_t>(offset), 4, elf.size())) return 0;
    return Le32(elf, static_cast<size_t>(offset));
}

bool VerifyHeadDisassembly(const std::vector<uint8_t>& vmlinux,
                           const std::vector<uint8_t>& image,
                           const LinuxSymbols& symbols) {
    if (!symbols.valid || symbols.headTextAddress != symbols.imageStart ||
        symbols.headTextOffset > vmlinux.size() ||
        symbols.headTextSize > vmlinux.size() - symbols.headTextOffset || image.size() < 0x114) {
        return false;
    }
    static constexpr std::array<size_t, 7> offsets{{0x0, 0xd4, 0x100, 0x104, 0x108, 0x10c, 0x110}};
    for (const size_t offset : offsets) {
        if (offset + 4 > symbols.headTextSize || offset + 4 > image.size()) return false;
        const uint32_t linked = ElfWordAt(vmlinux, symbols.headTextOffset + offset);
        const uint32_t rawImage = Le32(image, offset);
        if (linked != rawImage) return false;
    }
    return ElfWordAt(vmlinux, symbols.headTextOffset + 0x104) == 0x0006B023U &&
        ElfWordAt(vmlinux, symbols.headTextOffset + 0x108) == 0x00868693U &&
        ElfWordAt(vmlinux, symbols.headTextOffset + 0x10c) == 0xFEE6CCE3U;
}

LinuxRun RunLinuxEntry(RV64CPU& cpu, BssEvidence* bss = nullptr) {
    LinuxRun run;
    run.trace.reserve(kLinuxTraceHeadLimit);
    for (size_t index = 0; index < kLinuxStepBudget; ++index) {
        const uint64_t pcBefore = cpu.getPC();
        const uint64_t a3Before = cpu.readGR(13);
        const uint64_t a4Before = cpu.readGR(14);
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        if (bss != nullptr && pcBefore == bss->loopStorePc && !bss->observedLoopEntry) {
            bss->observedLoopEntry = true;
            bss->initialPointer = a3Before;
            bss->initialStop = a4Before;
            bss->boundsMatchSymbols = a3Before == bss->expectedStart &&
                a4Before == bss->expectedStop;
        }
        if (bss != nullptr && pcBefore == bss->loopStorePc &&
            step.outcome == StepOutcome::Ok && step.insn.rawWord == 0x0006B023U &&
            step.insn.mnemonic == Mnemonic::SD && step.insn.rs2 == 0 &&
            a3Before >= bss->expectedStart && a3Before < bss->expectedStop) {
            ++bss->storesRetired;
            if (bss->firstStoreAddress == 0) bss->firstStoreAddress = a3Before;
            bss->lastStoreAddress = a3Before;
            const bool sample = bss->storesRetired == 1 ||
                (bss->storesRetired % 4096ULL) == 0;
            if (sample || bss->progress.size() < 2) {
                uint64_t stored = UINT64_MAX;
                const bool readable = cpu.readU64(a3Before, stored);
                bss->progress.push_back({bss->storesRetired, a3Before,
                                         readable && stored == 0});
            }
        }
        if (bss != nullptr && pcBefore == bss->loopBranchPc &&
            step.outcome == StepOutcome::Ok && step.insn.rawWord == 0xFEE6CCE3U &&
            step.nextPc == bss->loopExitPc) {
            bss->loopExitObserved = true;
            bss->exitPointer = a3Before;
            bss->exitStop = a4Before;
            if (bss->progress.empty() || bss->progress.back().stores != bss->storesRetired) {
                uint64_t stored = UINT64_MAX;
                const uint64_t last = bss->storesRetired == 0 ? 0 :
                    bss->expectedStart + (bss->storesRetired - 1) * 8ULL;
                const bool readable = bss->storesRetired == 0 || cpu.readU64(last, stored);
                bss->progress.push_back({bss->storesRetired, last,
                                         bss->storesRetired == 0 || (readable && stored == 0)});
            }
        }
        if (index < kLinuxTraceHeadLimit) {
            run.trace.push_back({step, before, cpu.privilege()});
        } else {
            if (run.tailTrace.size() == kLinuxTraceTailLimit)
                run.tailTrace.pop_front();
            run.tailTrace.push_back({step, before, cpu.privilege()});
        }
        run.nextPc = cpu.getPC();
        if (step.outcome == StepOutcome::Ok) {
            ++run.retired;
            run.lastRetiredPc = step.pc;
            run.hasRetired = true;
            continue;
        }
        if (step.outcome == StepOutcome::Trap) {
            ++run.traps;
            if (step.interruptTaken) ++run.interrupts;
            if (before == PrivilegeMode::Supervisor &&
                step.trapCause == TrapCause::EcallFromS &&
                cpu.privilege() == PrivilegeMode::Machine && !step.sbiHandled) {
                ++run.sbiEcalls;
            }
        }
        run.frontierPrivilegeBefore = before;
        run.frontier = step;
        run.reachedFrontier = true;
        run.stopReason = std::string("first Linux ") + StepOutcomeToString(step.outcome);
        break;
    }
    if (run.stopReason.empty())
        run.stopReason = "bounded Linux instruction budget exhausted; next instruction is a progress frontier";
    if (!run.reachedFrontier) {
        run.nextPc = cpu.getPC();
        run.frontierPrivilegeBefore = cpu.privilege();
        run.frontier.pc = cpu.getPC();
        run.frontier.nextPc = cpu.getPC();
        run.frontier.outcome = StepOutcome::Ok; // Peek only; the next instruction was not executed.
        std::array<uint8_t, 4> bytes{};
        const Sv39Translation translation = cpu.translateFetch(run.frontier.pc, bytes.size());
        if (translation.ok) run.frontierPhysicalAddress = translation.paddr;
        if (translation.ok && cpu.readPhysical(run.frontierPhysicalAddress,
                                               bytes.data(), bytes.size())) {
            run.frontier.insn = Decoder().Decode(
                static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24));
        }
    } else {
        const auto fetch = std::find_if(cpu.pmpTrace().rbegin(), cpu.pmpTrace().rend(),
            [&](const PmpTraceEvent& event) {
                return event.kind == PmpTraceKind::PhysicalAccess &&
                    event.access == AccessType::Fetch &&
                    event.hasVirtualAddress && event.virtualAddress == run.frontier.pc;
            });
        if (fetch != cpu.pmpTrace().rend())
            run.frontierPhysicalAddress = fetch->physicalAddress;
    }
    return run;
}

LinuxSnapshot CaptureSnapshot(const RV64CPU& cpu, const LinuxRun& run,
                              const std::string& name, const std::string& detail) {
    LinuxSnapshot snapshot;
    snapshot.name = name;
    snapshot.detail = detail;
    snapshot.pc = cpu.getPC();
    snapshot.retired = run.retired;
    snapshot.privilege = cpu.privilege();
    snapshot.satp = CsrOrZero(cpu, 0x180);
    for (size_t index = 0; index < snapshot.registers.size(); ++index)
        snapshot.registers[index] = cpu.readGR(index);
    return snapshot;
}

void RecordMilestone(ContinuationEvidence& evidence, const RV64CPU& cpu,
                     const LinuxRun& run, const std::string& name,
                     const std::string& detail) {
    if (std::any_of(evidence.milestones.begin(), evidence.milestones.end(),
        [&](const LinuxSnapshot& item) { return item.name == name; })) return;
    if (evidence.milestones.size() < 128)
        evidence.milestones.push_back(CaptureSnapshot(cpu, run, name, detail));
}

uint64_t ArchitecturalStateFingerprint(const RV64CPU& cpu) {
    uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8U));
            hash *= 1099511628211ULL;
        }
    };
    add(cpu.getPC());
    add(static_cast<uint64_t>(cpu.privilege()));
    for (size_t index = 1; index < NUM_GENERAL_REGISTERS; ++index) add(cpu.readGR(index));
    for (const uint16_t csr : {uint16_t{0x100}, uint16_t{0x105}, uint16_t{0x141},
                               uint16_t{0x142}, uint16_t{0x143}, uint16_t{0x180},
                               uint16_t{0x300}, uint16_t{0x305}, uint16_t{0x341},
                               uint16_t{0x342}, uint16_t{0x343}}) {
        add(CsrOrZero(cpu, csr));
    }
    return hash;
}

bool IsCsrInstruction(Mnemonic mnemonic) {
    return mnemonic == Mnemonic::CSRRW || mnemonic == Mnemonic::CSRRS ||
        mnemonic == Mnemonic::CSRRC || mnemonic == Mnemonic::CSRRWI ||
        mnemonic == Mnemonic::CSRRSI || mnemonic == Mnemonic::CSRRCI;
}

uint64_t ReadGuestU64(const RV64CPU& cpu, uint64_t address, bool& readable) {
    uint8_t bytes[8]{};
    readable = cpu.readPhysical(address, bytes, sizeof(bytes));
    if (!readable) return 0;
    uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index)
        result |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
    return result;
}

void ObserveBssStep(RV64CPU& cpu, LinuxRun& run, ContinuationEvidence& evidence,
                    const StepResult& step, uint64_t pcBefore,
                    uint64_t a3Before, uint64_t a4Before) {
    BssEvidence& bss = evidence.bss;
    if (pcBefore == bss.loopStorePc && !bss.observedLoopEntry) {
        bss.observedLoopEntry = true;
        bss.initialPointer = a3Before;
        bss.initialStop = a4Before;
        bss.boundsMatchSymbols = a3Before == bss.expectedStart &&
            a4Before == bss.expectedStop;
    }
    if (pcBefore == bss.loopStorePc && step.outcome == StepOutcome::Ok &&
        step.insn.rawWord == 0x0006B023U && step.insn.mnemonic == Mnemonic::SD &&
        step.insn.rs2 == 0 && a3Before >= bss.expectedStart &&
        a3Before < bss.expectedStop) {
        ++bss.storesRetired;
        if (bss.firstStoreAddress == 0) bss.firstStoreAddress = a3Before;
        bss.lastStoreAddress = a3Before;
        if (bss.storesRetired == 1 || (bss.storesRetired % 4096ULL) == 0) {
            bool readable = false;
            const uint64_t stored = ReadGuestU64(cpu, a3Before, readable);
            bss.progress.push_back({bss.storesRetired, a3Before,
                                    readable && stored == 0});
        }
    }
    if (pcBefore == bss.loopBranchPc && step.outcome == StepOutcome::Ok &&
        step.insn.rawWord == 0xFEE6CCE3U && step.nextPc == bss.loopExitPc) {
        bss.loopExitObserved = true;
        bss.exitPointer = a3Before;
        bss.exitStop = a4Before;
        if (bss.progress.empty() || bss.progress.back().stores != bss.storesRetired) {
            bool readable = false;
            const uint64_t last = bss.storesRetired == 0 ? 0 :
                bss.expectedStart + (bss.storesRetired - 1) * 8ULL;
            const uint64_t stored = bss.storesRetired == 0 ? 0 :
                ReadGuestU64(cpu, last, readable);
            bss.progress.push_back({bss.storesRetired, last,
                bss.storesRetired == 0 || (readable && stored == 0)});
        }
        const uint64_t length = bss.expectedStop - bss.expectedStart;
        if (length <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            std::vector<uint8_t> bytes(static_cast<size_t>(length), 0xFF);
            const bool rangeReadable = cpu.readPhysical(
                bss.expectedStart, bytes.data(), bytes.size());
            bss.allMemoryZero = rangeReadable && std::all_of(
                bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0; });
            if (rangeReadable) bss.zeroRangeSha256 = Sha256(bytes);
        }
        if (bss.allMemoryZero && bss.storesRetired == length / 8ULL) {
            RecordMilestone(evidence, cpu, run, "bss_clear_completed",
                            "all linked BSS words were stored by guest SD instructions and read back zero");
        }
    }
}

std::vector<Sv39WalkLevel> CapturePageWalk(const RV64CPU& cpu, uint64_t virtualAddress) {
    std::vector<Sv39WalkLevel> result;
    const uint64_t satp = CsrOrZero(cpu, 0x180);
    if (((satp >> 60) & 0xFULL) != kSatpModeSv39) return result;
    uint64_t table = (satp & kSatpPpnMask) << 12;
    for (int level = 2; level >= 0; --level) {
        const uint64_t vpn = (virtualAddress >> (12U + 9U * static_cast<unsigned>(level))) & 0x1FFULL;
        const uint64_t pteAddress = table + vpn * 8ULL;
        bool readable = false;
        const uint64_t pte = ReadGuestU64(cpu, pteAddress, readable);
        result.push_back({static_cast<unsigned>(level), table, pteAddress, pte, readable});
        if (!readable || (pte & 1ULL) == 0) break;
        if ((pte & 0xEULL) != 0) break;
        table = ((pte >> 10) & kSatpPpnMask) << 12;
    }
    return result;
}

void RecordTrap(ContinuationEvidence& evidence, const RV64CPU& cpu,
                const StepResult& step, PrivilegeMode before,
                const Decoder& decoder) {
    if (evidence.traps.size() >= riscv_test::kTrapAccountingEventLimit) return;
    LinuxTrapEvent event;
    event.pc = step.pc;
    event.raw = step.insn.rawWord;
    event.length = step.length;
    event.disassembly = decoder.Disassemble(step.insn);
    event.origin = before;
    event.target = step.trapTarget;
    event.after = cpu.privilege();
    event.cause = step.trapCause;
    event.interruptCause = step.interruptCause;
    event.vector = step.trapVector;
    event.nextPc = cpu.getPC();
    event.mcause = CsrOrZero(cpu, 0x342);
    event.mepc = CsrOrZero(cpu, 0x341);
    event.mtval = CsrOrZero(cpu, 0x343);
    event.scause = CsrOrZero(cpu, 0x142);
    event.sepc = CsrOrZero(cpu, 0x141);
    event.stval = CsrOrZero(cpu, 0x143);
    event.mstatus = CsrOrZero(cpu, 0x300);
    event.sstatus = CsrOrZero(cpu, 0x100);
    event.satp = CsrOrZero(cpu, 0x180);
    event.interrupt = step.interruptTaken;
    event.sbiHandled = step.sbiHandled;
    if (!evidence.accounting.events().empty())
        event.accountingEventIndex = evidence.accounting.events().size() - 1;
    for (size_t index = 0; index < event.registers.size(); ++index)
        event.registers[index] = cpu.readGR(index);
    evidence.traps.push_back(std::move(event));
    const LinuxTrapEvent& captured = evidence.traps.back();
    if (evidence.firstPageWalk.empty() &&
        (captured.cause == TrapCause::InstructionPageFault ||
         captured.cause == TrapCause::LoadPageFault ||
         captured.cause == TrapCause::StorePageFault)) {
        const uint64_t faultAddress = captured.origin == PrivilegeMode::Supervisor
            ? captured.stval : captured.mtval;
        evidence.firstPageWalk = CapturePageWalk(cpu, faultAddress);
    }
}

void CaptureProgressFrontier(RV64CPU& cpu, LinuxRun& run) {
    run.nextPc = cpu.getPC();
    run.frontierPrivilegeBefore = cpu.privilege();
    run.frontier = StepResult{};
    run.frontier.pc = cpu.getPC();
    run.frontier.nextPc = cpu.getPC();
    run.frontier.outcome = StepOutcome::Ok; // The current instruction is inspected, never executed here.
    const Sv39Translation translation = cpu.translateFetch(run.frontier.pc, 4);
    if (!translation.ok) return;
    run.frontierPhysicalAddress = translation.paddr;
    std::array<uint8_t, 4> bytes{};
    if (cpu.readPhysical(run.frontierPhysicalAddress, bytes.data(), bytes.size())) {
        run.frontier.insn = Decoder().Decode(
            static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) |
            (static_cast<uint32_t>(bytes[3]) << 24));
    }
}

void ContinueLinuxInStages(RV64CPU& cpu, LinuxRun& run,
                           ContinuationEvidence& evidence,
                           const LinuxSymbols& symbols) {
    static constexpr std::array<uint64_t, 4> kTotalBudgets{{250000, 1000000, 5000000, 20000000}};
    // The initial corrected replay reached 1,000,001 M-mode handler
    // instructions across 1,942 completed/active invocations by 15.7M total
    // retirements. Keep independent bounded limits, with enough headroom for
    // the remaining 20M continuation rather than treating cumulative short
    // firmware handlers as one stalled invocation.
    static constexpr uint64_t kMachineInstructionBudget = 4000000;
    static constexpr uint64_t kMachineHandlerInstructionBudget = 3000000;
    static constexpr uint64_t kSupervisorHandlerInstructionBudget = 1000000;
    static constexpr uint64_t kIndividualHandlerInstructionBudget = 200000;
    static constexpr uint64_t kTrapEventBudget = riscv_test::kTrapAccountingEventLimit;
    static constexpr size_t kMaximumTrapNesting = riscv_test::kTrapAccountingNestingLimit;
    static constexpr uint64_t kPersistentStateRepeatLimit = 8192;
    Decoder decoder;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> repeatedStates;
    const auto synchronizeAccounting = [&]() {
        const auto& counts = evidence.accounting.counters();
        evidence.linuxStepCalls = counts.stepCalls;
        evidence.supervisorRetired = counts.supervisorRetired;
        evidence.machineRetiredAfterHandoff = counts.machineRetired;
        evidence.trapEvents = counts.nonRetiringTrapTransitions;
        evidence.interruptEvents = counts.interruptTransitions;
        evidence.ecallEvents = counts.supervisorEcalls + counts.machineEcalls;
        evidence.sbiEcalls = counts.sbiCalls;
    };
    uint64_t lastStageTarget = run.retired;
    run.reachedFrontier = false;
    run.stopReason.clear();
    for (const uint64_t target : kTotalBudgets) {
        if (target <= lastStageTarget || run.reachedFrontier) break;
        LinuxStage stage;
        stage.targetRetired = target;
        const uint64_t retiredAtStageStart = run.retired;
        const uint64_t callsAtStageStart = evidence.linuxStepCalls;
        while (run.retired < target && !run.reachedFrontier) {
            const uint64_t pcBefore = cpu.getPC();
            const uint64_t a3Before = cpu.readGR(13);
            const uint64_t a4Before = cpu.readGR(14);
            const PrivilegeMode privilegeBefore = cpu.privilege();
            uint16_t csrAddressBefore = 0;
            uint64_t csrValueBefore = 0;
            bool hasCsrBefore = false;
            const uint64_t headPhysical = boot_contract::kLinuxImageEntry;
            const bool pcInPhysicalHead = pcBefore >= headPhysical &&
                pcBefore < headPhysical + 0x1000ULL;
            const bool pcInVirtualHead = pcBefore >= symbols.imageStart &&
                pcBefore < symbols.imageStart + 0x1000ULL;
            if (pcInPhysicalHead || pcInVirtualHead) {
                const uint64_t fetchAddress = pcInPhysicalHead
                    ? pcBefore : symbols.physical(pcBefore);
                std::array<uint8_t, 4> bytes{};
                if (cpu.readPhysical(fetchAddress, bytes.data(), bytes.size())) {
                    const uint32_t raw = static_cast<uint32_t>(bytes[0]) |
                        (static_cast<uint32_t>(bytes[1]) << 8) |
                        (static_cast<uint32_t>(bytes[2]) << 16) |
                        (static_cast<uint32_t>(bytes[3]) << 24);
                    const DecodedInstruction decoded = decoder.Decode(raw);
                    if (IsCsrInstruction(decoded.mnemonic)) {
                        csrAddressBefore = static_cast<uint16_t>((raw >> 20) & 0xFFFU);
                        hasCsrBefore = cpu.readCsr(csrAddressBefore, csrValueBefore);
                    }
                }
            }
            const StepResult step = cpu.step();
            const uint64_t satpAfterTrap = step.outcome == StepOutcome::Trap
                ? CsrOrZero(cpu, 0x180) : 0;
            const bool sv39RelocationTransition = step.outcome == StepOutcome::Trap &&
                step.trapCause == TrapCause::InstructionPageFault &&
                privilegeBefore == PrivilegeMode::Supervisor &&
                step.trapTarget == PrivilegeMode::Supervisor &&
                step.pc == boot_contract::kLinuxImageEntry + 0x98ULL &&
                step.trapVector == symbols.imageStart + 0x98ULL &&
                ((satpAfterTrap >> 60) & 0xFULL) == kSatpModeSv39;
            const riscv_test::TrapAccountingDelta accountingDelta =
                evidence.accounting.observe(privilegeBefore, step, cpu.privilege(), cpu,
                                            !sv39RelocationTransition);
            synchronizeAccounting();
            if (accountingDelta.trapReturned &&
                accountingDelta.returnedEventIndex < evidence.traps.size()) {
                LinuxTrapEvent& returnedTrap = evidence.traps[accountingDelta.returnedEventIndex];
                returnedTrap.returned = true;
                returnedTrap.returnPc = step.nextPc;
            }
            ++stage.stepCalls;
            if (run.trace.size() < kLinuxTraceHeadLimit) {
                run.trace.push_back({step, privilegeBefore, cpu.privilege()});
            } else {
                if (run.tailTrace.size() == kLinuxTraceTailLimit)
                    run.tailTrace.pop_front();
                run.tailTrace.push_back({step, privilegeBefore, cpu.privilege()});
            }
            run.nextPc = cpu.getPC();

            if (pcBefore == symbols.physical(symbols.relocateEnableMmu) ||
                pcBefore == symbols.relocateEnableMmu) {
                RecordMilestone(evidence, cpu, run, "relocate_enable_mmu_entered",
                                "guest PC matched pinned vmlinux symbol");
            }
            if (pcBefore == symbols.physical(symbols.setupVm) || pcBefore == symbols.setupVm) {
                evidence.setupVmReached = true;
                RecordMilestone(evidence, cpu, run, "setup_vm_entered",
                                "guest PC matched pinned vmlinux setup_vm symbol");
            }
            if (pcBefore == symbols.physical(symbols.socEarlyInit) || pcBefore == symbols.socEarlyInit)
                RecordMilestone(evidence, cpu, run, "soc_early_init_entered",
                                "guest PC matched pinned vmlinux symbol");
            if (pcBefore == symbols.physical(symbols.startKernel) || pcBefore == symbols.startKernel)
                RecordMilestone(evidence, cpu, run, "start_kernel_entered",
                                "guest PC matched pinned vmlinux symbol");
            if (pcBefore >= 0xFFFFFFFF80000000ULL && !evidence.kernelVirtualExecutionReached) {
                evidence.kernelVirtualExecutionReached = true;
                RecordMilestone(evidence, cpu, run, "kernel_virtual_address_execution",
                                "guest executed at a canonical kernel virtual address");
            }

            if (step.outcome == StepOutcome::Ok) {
                ++run.retired;
                run.hasRetired = true;
                run.lastRetiredPc = step.pc;
                ObserveBssStep(cpu, run, evidence, step, pcBefore, a3Before, a4Before);

                const bool isStore = step.insn.mnemonic == Mnemonic::SB ||
                    step.insn.mnemonic == Mnemonic::SH || step.insn.mnemonic == Mnemonic::SW ||
                    step.insn.mnemonic == Mnemonic::SD;
                if (isStore && privilegeBefore == PrivilegeMode::Supervisor) {
                    const uint64_t storeAddress = cpu.readGR(step.insn.rs1) +
                        static_cast<uint64_t>(step.insn.immediate);
                    if (storeAddress == symbols.physical(symbols.bootCpuHartId)) {
                        bool readable = false;
                        const uint64_t value = ReadGuestU64(cpu, storeAddress, readable);
                        RecordMilestone(evidence, cpu, run, "boot_cpu_hartid_initialized",
                            readable && value == boot_contract::kLinuxBootHartId
                                ? "guest store wrote hart 0 to boot_cpu_hartid"
                                : "guest store reached boot_cpu_hartid but readback differed");
                    }
                }
                if (cpu.readGR(4) == symbols.physical(symbols.initTask) ||
                    cpu.readGR(4) == symbols.initTask) {
                    RecordMilestone(evidence, cpu, run, "initial_task_pointer_set",
                                    "tp matched pinned vmlinux init_task symbol");
                }
                const uint64_t stackBase = symbols.physical(symbols.initThreadUnion);
                if (cpu.readGR(2) >= stackBase && cpu.readGR(2) <= stackBase + 0x4000ULL) {
                    RecordMilestone(evidence, cpu, run, "initial_stack_setup",
                                    "sp entered the pinned initial thread union stack");
                }
                if (run.retired == kLinuxStepBudget) {
                    evidence.bss.pointerAt100k = cpu.readGR(13);
                    evidence.bss.stopAt100k = cpu.readGR(14);
                    evidence.bss.storeCountAt100k =
                        (evidence.bss.pointerAt100k - evidence.bss.expectedStart) / 8ULL;
                    RecordMilestone(evidence, cpu, run, "riscv18_100000_instruction_frontier",
                                    "same CPU continued from the accepted bounded BSS-loop state");
                }

                if (hasCsrBefore) {
                    const uint16_t csr = csrAddressBefore;
                    const uint64_t afterValue = CsrOrZero(cpu, csr);
                    if (csrValueBefore != afterValue && evidence.csrChanges.size() < 256) {
                        evidence.csrChanges.push_back({step.pc, privilegeBefore, csr,
                                                       csrValueBefore, afterValue});
                        if (csr == 0x105 && afterValue != 0) {
                            RecordMilestone(evidence, cpu, run,
                                "early_exception_vector_installed",
                                "guest changed stvec during early startup");
                            if (afterValue == symbols.handleException ||
                                afterValue == symbols.physical(symbols.handleException)) {
                                RecordMilestone(evidence, cpu, run,
                                    "kernel_exception_handler_restored",
                                    "stvec matched the pinned handle_exception symbol");
                            }
                        }
                        if (csr == 0x180 && !evidence.satpWritten) {
                            evidence.satpWritten = true;
                            evidence.firstSatpWritePc = step.pc;
                            evidence.firstSatpValue = afterValue;
                            RecordMilestone(evidence, cpu, run, "first_satp_write",
                                "guest wrote satp using the production CSR semantics");
                        }
                    }
                }
                if (step.insn.mnemonic == Mnemonic::SFENCE_VMA) ++evidence.sfenceVmaCount;

                if (evidence.bss.loopExitObserved && evidence.bss.zeroRangeSha256.empty()) {
                    evidence.stopClassification = "bss_range_read_failed";
                    evidence.stopReason = "BSS loop exited but the bounded BSS verification read failed";
                    run.reachedFrontier = true;
                    run.frontier = step;
                    run.frontierPrivilegeBefore = privilegeBefore;
                    break;
                }
            } else if (step.outcome == StepOutcome::Trap) {
                ++run.traps;
                if (step.interruptTaken) ++run.interrupts;
                const size_t trapsBefore = evidence.traps.size();
                RecordTrap(evidence, cpu, step, privilegeBefore, decoder);
                if (accountingDelta.trapEntered && evidence.traps.size() > trapsBefore)
                    evidence.traps.back().accountingEventIndex = accountingDelta.enteredEventIndex;
                if (evidence.traps.size() > trapsBefore) {
                    if (sv39RelocationTransition) {
                        evidence.traps.back().transitionName =
                            "sv39_kernel_virtual_alias_relocation";
                        evidence.traps.back().architecturalTransition = true;
                        RecordMilestone(evidence, cpu, run,
                            "sv39_kernel_virtual_alias_transition",
                            "expected instruction page fault vectored from the unmapped physical alias to the mapped kernel virtual alias");
                    }
                }
                if (evidence.trapEvents > kTrapEventBudget) {
                    evidence.stopClassification = "trap_event_budget";
                    evidence.stopReason = "bounded Linux trap-event budget exhausted";
                    run.reachedFrontier = true;
                    run.frontier = step;
                    run.frontierPrivilegeBefore = privilegeBefore;
                    break;
                }
            } else {
                run.frontier = step;
                run.frontierPrivilegeBefore = privilegeBefore;
                run.reachedFrontier = true;
                if (step.outcome == StepOutcome::Unsupported) {
                    evidence.stopClassification = "unsupported_instruction";
                    evidence.stopReason = "first unsupported Linux instruction";
                } else if (step.outcome == StepOutcome::Waiting) {
                    evidence.stopClassification = "persistent_wait";
                    evidence.stopReason = "Linux hart entered WFI with no locally enabled pending interrupt";
                } else if (step.outcome == StepOutcome::Halt) {
                    evidence.stopClassification = "architectural_halt";
                    evidence.stopReason = "Linux execution returned an architectural halt outcome";
                } else {
                    evidence.stopClassification = "architectural_fault";
                    evidence.stopReason = std::string("Linux step returned ") +
                        StepOutcomeToString(step.outcome);
                }
                break;
            }

            if (!run.reachedFrontier) {
                const auto& counts = evidence.accounting.counters();
                std::string classification;
                std::string reason;
                if (counts.machineRetired > kMachineInstructionBudget) {
                    classification = "m_mode_instruction_budget";
                    reason = "bounded post-handoff M-mode firmware instruction allowance exhausted";
                } else if (counts.machineHandlerInstructions > kMachineHandlerInstructionBudget) {
                    classification = "m_mode_handler_instruction_budget";
                    reason = "bounded M-mode trap-handler instruction allowance exhausted";
                } else if (counts.supervisorHandlerInstructions > kSupervisorHandlerInstructionBudget) {
                    classification = "s_mode_handler_instruction_budget";
                    reason = "bounded S-mode trap-handler instruction allowance exhausted";
                } else if (counts.largestHandlerInvocation > kIndividualHandlerInstructionBudget) {
                    classification = "individual_handler_instruction_budget";
                    reason = "one matched trap-handler invocation exceeded its instruction allowance";
                } else if (counts.maxNestingDepth > kMaximumTrapNesting) {
                    classification = "trap_nesting_budget";
                    reason = "bounded trap nesting depth exceeded";
                } else if (evidence.accounting.lifetimesUncertain()) {
                    classification = "trap_accounting_uncertain";
                    reason = "trap-handler lifetime transitions could not be paired conclusively";
                }
                if (!classification.empty()) {
                    evidence.stopClassification = std::move(classification);
                    evidence.stopReason = std::move(reason);
                    run.reachedFrontier = true;
                    if (step.outcome == StepOutcome::Trap) {
                        run.frontier = step;
                        run.frontierPrivilegeBefore = privilegeBefore;
                    } else {
                        run.frontierPrivilegeBefore = cpu.privilege();
                        CaptureProgressFrontier(cpu, run);
                    }
                    break;
                }
            }

            const uint64_t currentPc = cpu.getPC();
            const uint64_t state = ArchitecturalStateFingerprint(cpu);
            auto& repeat = repeatedStates[currentPc];
            if (repeat.first == state) {
                ++repeat.second;
                if (repeat.second >= kPersistentStateRepeatLimit) {
                    evidence.repeatedArchitecturalStates = repeat.second;
                    evidence.stopClassification = "architectural_state_nonprogress";
                    evidence.stopReason = "repeated PC and identical architectural state persisted";
                    run.reachedFrontier = true;
                    run.frontierPrivilegeBefore = cpu.privilege();
                    CaptureProgressFrontier(cpu, run);
                    break;
                }
            } else {
                repeat.first = state;
                repeat.second = 0;
            }
        }
        stage.actualRetired = run.retired;
        stage.additionalRetired = run.retired - retiredAtStageStart;
        stage.stepCalls = evidence.linuxStepCalls - callsAtStageStart;
        stage.pc = cpu.getPC();
        stage.lastRetiredPc = run.lastRetiredPc;
        stage.bssStores = evidence.bss.storesRetired;
        stage.sModeRetired = evidence.accounting.counters().supervisorRetired;
        stage.mModeRetired = evidence.accounting.counters().machineRetired;
        stage.sModeHandlerInstructions = evidence.accounting.counters().supervisorHandlerInstructions;
        stage.mModeHandlerInstructions = evidence.accounting.counters().machineHandlerInstructions;
        stage.activeHandlers = evidence.accounting.activeHandlerInvocations();
        stage.maxNestingDepth = evidence.accounting.counters().maxNestingDepth;
        evidence.stages.push_back(stage);
        lastStageTarget = target;
        if (run.reachedFrontier) break;
    }

    if (!run.reachedFrontier) {
        evidence.stopClassification = "instruction_budget";
        evidence.stopReason = "20-million post-handoff guest-retirement cap reached with guest forward progress";
        CaptureProgressFrontier(cpu, run);
    }
    run.stopReason = evidence.stopReason;
    evidence.finalSatp = CsrOrZero(cpu, 0x180);
    evidence.finalMtime = cpu.mtime();
    evidence.finalMtimecmp = cpu.mtimecmp();
    evidence.finalMmioSequence = cpu.machineMmioEventSequence();
    evidence.finalMmioReads = cpu.mmioReadCount();
    evidence.finalMmioWrites = cpu.mmioWriteCount();
    if (run.reachedFrontier && run.frontier.outcome != StepOutcome::Waiting &&
        run.frontierPhysicalAddress == 0) {
        const auto fetch = std::find_if(cpu.pmpTrace().rbegin(), cpu.pmpTrace().rend(),
            [&](const PmpTraceEvent& event) {
                return event.kind == PmpTraceKind::PhysicalAccess &&
                    event.access == AccessType::Fetch && event.hasVirtualAddress &&
                    event.virtualAddress == run.frontier.pc;
            });
        if (fetch != cpu.pmpTrace().rend()) run.frontierPhysicalAddress = fetch->physicalAddress;
    }
}

void WriteContinuationArtifacts(const std::filesystem::path& dir,
                                const std::vector<uint8_t>& firmware,
                                const std::vector<uint8_t>& firmwareElf,
                                const std::vector<uint8_t>& image,
                                const std::vector<uint8_t>& vmlinux,
                                const std::vector<uint8_t>& config,
                                const std::vector<uint8_t>& handoffFdt,
                                const LinuxSymbols& symbols,
                                const FirmwareRun& firmwareRun,
                                const LinuxRun& run,
                                const ContinuationEvidence& evidence,
                                const RV64CPU& cpu) {
    Decoder decoder;
    const auto& accounting = evidence.accounting;
    const auto& trapCounts = accounting.counters();
    const std::vector<ElfFunctionSymbol> linuxFunctions = ParseElfFunctionSymbols(vmlinux);
    const std::vector<ElfFunctionSymbol> firmwareFunctions = ParseElfFunctionSymbols(firmwareElf);
    const auto writeRegisters = [](std::ostream& out,
                                  const std::array<uint64_t, NUM_GENERAL_REGISTERS>& registers) {
        out << '[';
        for (size_t index = 0; index < registers.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(Hex(registers[index]));
        out << ']';
    };
    {
        std::ofstream out(dir / "riscv20-linux-symbols.json", std::ios::out | std::ios::trunc);
        out << "{\n  \"schemaVersion\": 2,\n  \"vmlinuxSha256\": \""
            << Sha256(vmlinux) << "\",\n  \"imageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"imageStartVirtual\": \"" << Hex(symbols.imageStart) << "\",\n"
            << "  \"bssStartVirtual\": \"" << Hex(symbols.bssStart) << "\",\n"
            << "  \"bssStopVirtual\": \"" << Hex(symbols.bssStop) << "\",\n"
            << "  \"bssStartPhysical\": \"" << Hex(symbols.physical(symbols.bssStart)) << "\",\n"
            << "  \"bssStopPhysical\": \"" << Hex(symbols.physical(symbols.bssStop)) << "\",\n"
            << "  \"boot_cpu_hartidVirtual\": \"" << Hex(symbols.bootCpuHartId) << "\",\n"
            << "  \"boot_cpu_hartidPhysical\": \"" << Hex(symbols.physical(symbols.bootCpuHartId)) << "\",\n"
            << "  \"init_taskVirtual\": \"" << Hex(symbols.initTask) << "\",\n"
            << "  \"init_thread_unionVirtual\": \"" << Hex(symbols.initThreadUnion) << "\",\n"
            << "  \"early_pg_dirVirtual\": \"" << Hex(symbols.earlyPgDir) << "\",\n"
            << "  \"trampoline_pg_dirVirtual\": \"" << Hex(symbols.trampolinePgDir) << "\",\n"
            << "  \"swapper_pg_dirVirtual\": \"" << Hex(symbols.swapperPgDir) << "\",\n"
            << "  \"relocate_enable_mmuVirtual\": \"" << Hex(symbols.relocateEnableMmu) << "\",\n"
            << "  \"setup_vmVirtual\": \"" << Hex(symbols.setupVm) << "\",\n"
            << "  \"handle_exceptionVirtual\": \"" << Hex(symbols.handleException) << "\",\n"
            << "  \"soc_early_initVirtual\": \"" << Hex(symbols.socEarlyInit) << "\",\n"
            << "  \"start_kernelVirtual\": \"" << Hex(symbols.startKernel) << "\",\n"
            << "  \"imageLoopStoreWord\": \"" << Hex(Le32(image, 0x104)) << "\",\n"
            << "  \"imageLoopIncrementWord\": \"" << Hex(Le32(image, 0x108)) << "\",\n"
            << "  \"imageLoopBranchWord\": \"" << Hex(Le32(image, 0x10C)) << "\",\n"
            << "  \"loopStorePc\": \"" << Hex(boot_contract::kLinuxImageEntry + 0x104ULL) << "\",\n"
            << "  \"loopBranchPc\": \"" << Hex(boot_contract::kLinuxImageEntry + 0x10CULL) << "\",\n"
            << "  \"loopExitPc\": \"" << Hex(boot_contract::kLinuxImageEntry + 0x110ULL) << "\"\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-run-summary.json", std::ios::out | std::ios::trunc);
        out << "{\n  \"schemaVersion\": 2,\n  \"phase\": \"RISCV-20\",\n"
            << "  \"linuxSourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
            << "  \"linuxImageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"vmlinuxSha256\": \"" << Sha256(vmlinux) << "\",\n"
            << "  \"configSha256\": \"" << Sha256(config) << "\",\n"
            << "  \"openSbiSha256\": \"" << Sha256(firmware) << "\",\n"
            << "  \"firmwareSteps\": " << firmwareRun.steps << ",\n"
            << "  \"firmwareMModeRetired\": " << firmwareRun.machineRetired << ",\n"
            << "  \"stageRetiredBudgetDomain\": \"post-handoff S-mode Linux plus M-mode OpenSBI guest instructions\",\n"
            << "  \"totalPostHandoffStepCalls\": " << trapCounts.stepCalls << ",\n"
            << "  \"postHandoffRetiredInstructions\": " << run.retired << ",\n"
            << "  \"linuxRetiredInstructions\": " << trapCounts.supervisorRetired << ",\n"
            << "  \"normalSModeLinuxInstructions\": " << trapCounts.supervisorLinuxInstructions << ",\n"
            << "  \"sModeRetiredInstructions\": " << trapCounts.supervisorRetired << ",\n"
            << "  \"sModeHandlerInstructions\": " << trapCounts.supervisorHandlerInstructions << ",\n"
            << "  \"mModeOpenSbiInstructions\": " << trapCounts.machineRetired << ",\n"
            << "  \"mModeHandlerInstructions\": " << trapCounts.machineHandlerInstructions << ",\n"
            << "  \"unattributedHandlerInstructions\": " << trapCounts.unattributedHandlerInstructions << ",\n"
            << "  \"handlerAccounting\": {\n"
            << "    \"schemaVersion\": 2,\n"
            << "    \"instructionAttribution\": \"current privilege plus active trap target, closed by matching MRET/SRET\",\n"
            << "    \"nonRetiringTrapTransitions\": " << trapCounts.nonRetiringTrapTransitions << ",\n"
            << "    \"supervisorEcalls\": " << trapCounts.supervisorEcalls << ",\n"
            << "    \"machineEcalls\": " << trapCounts.machineEcalls << ",\n"
            << "    \"machineTimerInterrupts\": " << trapCounts.machineTimerInterrupts << ",\n"
            << "    \"supervisorTimerInterrupts\": " << trapCounts.supervisorTimerInterrupts << ",\n"
            << "    \"externalInterrupts\": " << trapCounts.externalInterrupts << ",\n"
            << "    \"otherInterrupts\": " << trapCounts.otherInterrupts << ",\n"
            << "    \"supervisorSynchronousExceptions\": " << trapCounts.supervisorSynchronousExceptions << ",\n"
            << "    \"machineSynchronousExceptions\": " << trapCounts.machineSynchronousExceptions << ",\n"
            << "    \"mretInstructions\": " << trapCounts.mretInstructions << ",\n"
            << "    \"sretInstructions\": " << trapCounts.sretInstructions << ",\n"
            << "    \"handlerEntries\": " << trapCounts.handlerEntries << ",\n"
            << "    \"handlerReturns\": " << trapCounts.handlerReturns << ",\n"
            << "    \"activeHandlerInvocationsAtStop\": " << accounting.activeHandlerInvocations() << ",\n"
            << "    \"maximumNestingDepth\": " << trapCounts.maxNestingDepth << ",\n"
            << "    \"largestHandlerInvocation\": " << trapCounts.largestHandlerInvocation << ",\n"
            << "    \"lifetimeAttributionUncertain\": " << (accounting.lifetimesUncertain() ? "true" : "false") << ",\n"
            << "    \"droppedEventDetails\": " << trapCounts.droppedEvents << "\n"
            << "  },\n"
            << "  \"budgets\": {\"totalRetired\":20000000,\"mModeRetired\":4000000,"
            << "\"mModeHandlerInstructions\":3000000,\"sModeHandlerInstructions\":1000000,"
            << "\"individualHandlerInstructions\":200000,\"trapEvents\":8192,\"maximumNestingDepth\":64},\n"
            << "  \"trapEvents\": " << trapCounts.nonRetiringTrapTransitions << ",\n"
            << "  \"interruptEvents\": " << trapCounts.interruptTransitions << ",\n"
            << "  \"ecallEvents\": " << (trapCounts.supervisorEcalls + trapCounts.machineEcalls) << ",\n"
            << "  \"sbiEcalls\": " << trapCounts.sbiCalls << ",\n"
            << "  \"firstLinuxPc\": \"" << Hex(boot_contract::kLinuxImageEntry) << "\",\n"
            << "  \"lastRetiredPc\": \"" << Hex(run.lastRetiredPc) << "\",\n"
            << "  \"finalPc\": \"" << Hex(cpu.getPC()) << "\",\n"
            << "  \"finalPrivilege\": \"" << PrivilegeModeToString(cpu.privilege()) << "\",\n"
            << "  \"finalSatp\": \"" << Hex(evidence.finalSatp) << "\",\n"
            << "  \"virtualTimeTicksAtHandoff\": " << evidence.handoffMtime << ",\n"
            << "  \"virtualTimeTicksFinal\": " << evidence.finalMtime << ",\n"
            << "  \"virtualTimeTicksRetiredDelta\": "
            << evidence.finalMtime - evidence.handoffMtime << ",\n"
            << "  \"firmwareMtimecmpAtHandoff\": \"" << Hex(evidence.handoffMtimecmp) << "\",\n"
            << "  \"firmwareMtimecmpFinal\": \"" << Hex(evidence.finalMtimecmp) << "\",\n"
            << "  \"bssStartVirtual\": \"" << Hex(symbols.bssStart) << "\",\n"
            << "  \"bssStopVirtual\": \"" << Hex(symbols.bssStop) << "\",\n"
            << "  \"bssStartPhysical\": \"" << Hex(evidence.bss.expectedStart) << "\",\n"
            << "  \"bssStopPhysical\": \"" << Hex(evidence.bss.expectedStop) << "\",\n"
            << "  \"bssGuestStoresRetired\": " << evidence.bss.storesRetired << ",\n"
            << "  \"bssExpectedStores\": " << (evidence.bss.expectedStop -
                evidence.bss.expectedStart) / 8ULL << ",\n"
            << "  \"bssLoopExitObserved\": " << (evidence.bss.loopExitObserved ? "true" : "false") << ",\n"
            << "  \"bssAllZeroAfterExit\": " << (evidence.bss.allMemoryZero ? "true" : "false") << ",\n"
            << "  \"bssZeroRangeSha256\": \"" << evidence.bss.zeroRangeSha256 << "\",\n"
            << "  \"setupVmReached\": " << (evidence.setupVmReached ? "true" : "false") << ",\n"
            << "  \"firstSatpWriteObserved\": " << (evidence.satpWritten ? "true" : "false") << ",\n"
            << "  \"firstSatpWritePc\": \"" << Hex(evidence.firstSatpWritePc) << "\",\n"
            << "  \"firstSatpValue\": \"" << Hex(evidence.firstSatpValue) << "\",\n"
            << "  \"kernelVirtualExecutionReached\": "
            << (evidence.kernelVirtualExecutionReached ? "true" : "false") << ",\n"
            << "  \"hostSbiProviderPresent\": false,\n"
            << "  \"handoffRetiredCounter\": " << evidence.handoffRetiredCounter << ",\n"
            << "  \"finalRetiredCounter\": " << cpu.instructionsRetired() << ",\n"
            << "  \"handoffMmioSequence\": " << evidence.handoffMmioSequence << ",\n"
            << "  \"finalMmioSequence\": " << evidence.finalMmioSequence << ",\n"
            << "  \"handoffMmioReads\": " << evidence.handoffMmioReads << ",\n"
            << "  \"finalMmioReads\": " << evidence.finalMmioReads << ",\n"
            << "  \"handoffMmioWrites\": " << evidence.handoffMmioWrites << ",\n"
            << "  \"finalMmioWrites\": " << evidence.finalMmioWrites << ",\n"
            << "  \"fdtSha256AtHandoff\": \"" << Sha256(handoffFdt) << "\",\n"
            << "  \"stopClassification\": " << JsonQuote(evidence.stopClassification) << ",\n"
            << "  \"stopReason\": " << JsonQuote(evidence.stopReason) << "\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-milestones.json", std::ios::out | std::ios::trunc);
        out << "{\n  \"schemaVersion\": 2,\n  \"stages\": [\n";
        for (size_t index = 0; index < evidence.stages.size(); ++index) {
            const LinuxStage& stage = evidence.stages[index];
            out << "    {\"targetRetired\":" << stage.targetRetired
                << ",\"actualRetired\":" << stage.actualRetired
                << ",\"additionalRetired\":" << stage.additionalRetired
                << ",\"stepCalls\":" << stage.stepCalls
                << ",\"pc\":\"" << Hex(stage.pc)
                << "\",\"lastRetiredPc\":\"" << Hex(stage.lastRetiredPc)
                << "\",\"bssStores\":" << stage.bssStores
                << ",\"sModeRetired\":" << stage.sModeRetired
                << ",\"mModeRetired\":" << stage.mModeRetired
                << ",\"sModeHandlerInstructions\":" << stage.sModeHandlerInstructions
                << ",\"mModeHandlerInstructions\":" << stage.mModeHandlerInstructions
                << ",\"activeHandlers\":" << stage.activeHandlers
                << ",\"maxNestingDepth\":" << stage.maxNestingDepth << "}"
                << (index + 1 == evidence.stages.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"milestones\": [\n";
        for (size_t index = 0; index < evidence.milestones.size(); ++index) {
            const LinuxSnapshot& milestone = evidence.milestones[index];
            out << "    {\"name\":" << JsonQuote(milestone.name)
                << ",\"detail\":" << JsonQuote(milestone.detail)
                << ",\"pc\":\"" << Hex(milestone.pc)
                << "\",\"retired\":" << milestone.retired
                << ",\"privilege\":\"" << PrivilegeModeToString(milestone.privilege)
                << "\",\"satp\":\"" << Hex(milestone.satp) << "\",\"registers\":";
            writeRegisters(out, milestone.registers);
            out << "}" << (index + 1 == evidence.milestones.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"bss\": {\n"
            << "    \"observedLoopEntry\": " << (evidence.bss.observedLoopEntry ? "true" : "false") << ",\n"
            << "    \"boundsMatchSymbols\": " << (evidence.bss.boundsMatchSymbols ? "true" : "false") << ",\n"
            << "    \"initialA3\": \"" << Hex(evidence.bss.initialPointer) << "\",\n"
            << "    \"initialA4\": \"" << Hex(evidence.bss.initialStop) << "\",\n"
            << "    \"a3At100000\": \"" << Hex(evidence.bss.pointerAt100k) << "\",\n"
            << "    \"storesAt100000\": " << evidence.bss.storeCountAt100k << ",\n"
            << "    \"storesRetired\": " << evidence.bss.storesRetired << ",\n"
            << "    \"firstStoreAddress\": \"" << Hex(evidence.bss.firstStoreAddress) << "\",\n"
            << "    \"lastStoreAddress\": \"" << Hex(evidence.bss.lastStoreAddress) << "\",\n"
            << "    \"exitPointer\": \"" << Hex(evidence.bss.exitPointer) << "\",\n"
            << "    \"exitStop\": \"" << Hex(evidence.bss.exitStop) << "\",\n"
            << "    \"loopExitObserved\": " << (evidence.bss.loopExitObserved ? "true" : "false") << ",\n"
            << "    \"allMemoryZero\": " << (evidence.bss.allMemoryZero ? "true" : "false") << ",\n"
            << "    \"zeroRangeSha256\": \"" << evidence.bss.zeroRangeSha256 << "\",\n"
            << "    \"progressSamples\": [\n";
        for (size_t index = 0; index < evidence.bss.progress.size(); ++index) {
            const BssProgressSample& sample = evidence.bss.progress[index];
            out << "      {\"stores\":" << sample.stores << ",\"address\":\""
                << Hex(sample.address) << "\",\"zeroAfterGuestStore\":"
                << (sample.zeroAfterGuestStore ? "true" : "false") << "}"
                << (index + 1 == evidence.bss.progress.size() ? "\n" : ",\n");
        }
        out << "    ]\n  }\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-csr-state.json", std::ios::out | std::ios::trunc);
        static constexpr std::array<std::pair<const char*, uint16_t>, 12> kCsrs{{
            {"mstatus", uint16_t{0x300}}, {"sstatus", uint16_t{0x100}},
            {"mie", uint16_t{0x304}}, {"mip", uint16_t{0x344}},
            {"mtvec", uint16_t{0x305}}, {"stvec", uint16_t{0x105}},
            {"mepc", uint16_t{0x341}}, {"sepc", uint16_t{0x141}},
            {"mcause", uint16_t{0x342}}, {"scause", uint16_t{0x142}},
            {"stval", uint16_t{0x143}}, {"satp", uint16_t{0x180}},
        }};
        out << "{\n  \"schemaVersion\": 2,\n  \"pc\": \"" << Hex(cpu.getPC())
            << "\",\n  \"privilege\": \"" << PrivilegeModeToString(cpu.privilege())
            << "\",\n  \"registers\":[";
        for (size_t index = 0; index < NUM_GENERAL_REGISTERS; ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(Hex(cpu.readGR(index)));
        out << "],\n  \"csrs\": [\n";
        for (size_t index = 0; index < kCsrs.size(); ++index)
            out << "    {\"name\":" << JsonQuote(kCsrs[index].first)
                << ",\"address\":\"" << Hex(kCsrs[index].second)
                << "\",\"value\":\"" << Hex(CsrOrZero(cpu, kCsrs[index].second)) << "\"}"
                << (index + 1 == kCsrs.size() ? "\n" : ",\n");
        out << "  ],\n  \"csrChanges\": [\n";
        for (size_t index = 0; index < evidence.csrChanges.size(); ++index) {
            const LinuxCsrChange& change = evidence.csrChanges[index];
            out << "    {\"pc\":\"" << Hex(change.pc) << "\",\"privilege\":\""
                << PrivilegeModeToString(change.privilege) << "\",\"csr\":\""
                << Hex(change.csr) << "\",\"before\":\"" << Hex(change.before)
                << "\",\"after\":\"" << Hex(change.after) << "\"}"
                << (index + 1 == evidence.csrChanges.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"mtime\": " << evidence.finalMtime
            << ",\n  \"mtimecmp\": \"" << Hex(evidence.finalMtimecmp)
            << "\",\n  \"pmpEntries\": [\n";
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView entry;
            if (!cpu.pmpEntry(index, entry)) continue;
            out << "    {\"index\":" << index << ",\"active\":"
                << (entry.active ? "true" : "false") << ",\"config\":\""
                << Hex(entry.config) << "\",\"lower\":\"" << Hex(entry.lowerBound)
                << "\",\"upperExclusive\":\"" << Hex(entry.upperBound) << "\"}"
                << (index + 1 == cpu.pmpImplementedEntries() ? "\n" : ",\n");
        }
        out << "  ],\n  \"pmpUnchanged\": " << (evidence.pmpUnchanged ? "true" : "false")
            << ",\n  \"fdtUnchanged\": " << (evidence.fdtUnchanged ? "true" : "false")
            << ",\n  \"uartPrefixPreserved\": " << (evidence.uartPrefixPreserved ? "true" : "false")
            << "\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-trap-accounting.json",
                          std::ios::out | std::ios::trunc);
        out << "{\n  \"schemaVersion\": 2,\n"
            << "  \"linuxSourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
            << "  \"linuxImageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"vmlinuxSha256\": \"" << Sha256(vmlinux) << "\",\n"
            << "  \"openSbiSha256\": \"" << Sha256(firmware) << "\",\n"
            << "  \"stepCalls\": " << trapCounts.stepCalls << ",\n"
            << "  \"nonRetiringTrapTransitions\": " << trapCounts.nonRetiringTrapTransitions << ",\n"
            << "  \"totalRetired\": " << trapCounts.totalRetired << ",\n"
            << "  \"perPrivilegeRetired\": {\"U\":" << trapCounts.userRetired
            << ",\"S\":" << trapCounts.supervisorRetired
            << ",\"M\":" << trapCounts.machineRetired << "},\n"
            << "  \"normalSModeLinuxInstructions\": " << trapCounts.supervisorLinuxInstructions << ",\n"
            << "  \"sModeHandlerInstructions\": " << trapCounts.supervisorHandlerInstructions << ",\n"
            << "  \"mModeHandlerInstructions\": " << trapCounts.machineHandlerInstructions << ",\n"
            << "  \"unattributedHandlerInstructions\": " << trapCounts.unattributedHandlerInstructions << ",\n"
            << "  \"trapClasses\": {\n"
            << "    \"supervisorEcalls\":" << trapCounts.supervisorEcalls << ",\n"
            << "    \"machineEcalls\":" << trapCounts.machineEcalls << ",\n"
            << "    \"machineTimerInterrupts\":" << trapCounts.machineTimerInterrupts << ",\n"
            << "    \"supervisorTimerInterrupts\":" << trapCounts.supervisorTimerInterrupts << ",\n"
            << "    \"externalInterrupts\":" << trapCounts.externalInterrupts << ",\n"
            << "    \"otherInterrupts\":" << trapCounts.otherInterrupts << ",\n"
            << "    \"supervisorSynchronousExceptions\":" << trapCounts.supervisorSynchronousExceptions << ",\n"
            << "    \"machineSynchronousExceptions\":" << trapCounts.machineSynchronousExceptions << "\n"
            << "  },\n"
            << "  \"returns\": {\"mretInstructions\":" << trapCounts.mretInstructions
            << ",\"sretInstructions\":" << trapCounts.sretInstructions
            << ",\"handlerEntries\":" << trapCounts.handlerEntries
            << ",\"handlerReturns\":" << trapCounts.handlerReturns
            << ",\"activeAtStop\":" << accounting.activeHandlerInvocations()
            << ",\"maximumNestingDepth\":" << trapCounts.maxNestingDepth
            << ",\"largestInvocationInstructions\":" << trapCounts.largestHandlerInvocation
            << ",\"unmatchedReturns\":" << trapCounts.unmatchedReturns
            << ",\"privilegeMismatches\":" << trapCounts.privilegeMismatches
            << ",\"lifetimeUncertain\":" << (accounting.lifetimesUncertain() ? "true" : "false")
            << "},\n"
            << "  \"eventDetailsRetained\": " << accounting.events().size() << ",\n"
            << "  \"eventDetailLimit\": " << riscv_test::kTrapAccountingEventLimit << ",\n"
            << "  \"droppedEventDetails\": " << trapCounts.droppedEvents << ",\n"
            << "  \"events\": [\n";
        for (size_t index = 0; index < accounting.events().size(); ++index) {
            const riscv_test::TrapAccountingEvent& event = accounting.events()[index];
            out << "    {\"id\":" << event.id << ",\"parentId\":";
            if (event.parentId == UINT64_MAX) out << "null";
            else out << event.parentId;
            out << ",\"pc\":\"" << Hex(event.pc)
                << "\",\"origin\":\"" << PrivilegeModeToString(event.origin)
                << "\",\"target\":\"" << PrivilegeModeToString(event.target)
                << "\",\"cause\":" << event.cause
                << ",\"interrupt\":" << (event.interrupt ? "true" : "false")
                << ",\"handlerInvocation\":" << (event.handlerInvocation ? "true" : "false")
                << ",\"vector\":\"" << Hex(event.vector)
                << "\",\"handlerInstructions\":" << event.handlerInstructions
                << ",\"returned\":" << (event.returned ? "true" : "false")
                << ",\"returnInstructionPc\":\"" << Hex(event.returnInstructionPc)
                << "\",\"returnTargetPc\":\"" << Hex(event.returnTargetPc)
                << "\",\"resumed\":" << (event.resumed ? "true" : "false")
                << ",\"resumedPc\":\"" << Hex(event.resumedPc)
                << "\",\"handlerPcPrefix\":[";
            for (size_t pc = 0; pc < event.handlerPcPrefixSize; ++pc)
                out << (pc == 0 ? "" : ",") << JsonQuote(Hex(event.handlerPcPrefix[pc]));
            out << "]}" << (index + 1 == accounting.events().size() ? "\n" : ",\n");
        }
        out << "  ]\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-sbi-events.json",
                          std::ios::out | std::ios::trunc);
        out << "{\n  \"schemaVersion\": 2,\n"
            << "  \"linuxSourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
            << "  \"linuxImageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"vmlinuxSha256\": \"" << Sha256(vmlinux) << "\",\n"
            << "  \"openSbiSha256\": \"" << Sha256(firmware) << "\",\n"
            << "  \"hostSbiProviderPresent\": false,\n"
            << "  \"authenticMachineFirmware\": true,\n"
            << "  \"callCount\": " << trapCounts.sbiCalls << ",\n"
            << "  \"calls\": [\n";
        bool firstCall = true;
        for (size_t index = 0; index < accounting.events().size(); ++index) {
            const riscv_test::TrapAccountingEvent& event = accounting.events()[index];
            if (!event.sbiCall) continue;
            const riscv_test::TrapAccountingEvent* previousService = nullptr;
            uint64_t previousServiceCount = 0;
            uint64_t previousIdenticalCount = 0;
            for (size_t previous = 0; previous < index; ++previous) {
                const riscv_test::TrapAccountingEvent& candidate = accounting.events()[previous];
                if (!candidate.sbiCall || candidate.sbiEid != event.sbiEid ||
                    candidate.sbiFid != event.sbiFid) continue;
                previousService = &candidate;
                ++previousServiceCount;
                if (candidate.sbiArgs == event.sbiArgs) ++previousIdenticalCount;
            }
            const bool retiredBetween = previousService != nullptr &&
                event.totalRetiredAtTrap > previousService->totalRetiredAtTrap;
            const std::string recurrenceAssessment = !event.returned
                ? "call_open_at_execution_frontier"
                : !event.resumed
                    ? "returned_without_observed_guest_resumption"
                    : previousService != nullptr && retiredBetween
                        ? "recurring_call_returned_and_guest_retired_between_calls"
                        : previousService != nullptr
                            ? "recurring_call_returned_without_intervening_retirement"
                            : "call_returned_and_guest_resumed";
            out << (firstCall ? "" : ",\n")
                << "    {\"eventId\":" << event.id
                << ",\"linuxEcallPc\":\"" << Hex(event.pc)
                << "\",\"linuxFunction\":"
                << JsonQuote(FunctionContaining(linuxFunctions, event.pc))
                << ",\"rawInstruction\":\"0x00000073\",\"eid\":\"" << Hex(event.sbiEid)
                << "\",\"fid\":\"" << Hex(event.sbiFid) << "\",\"arguments\":[";
            for (size_t arg = 0; arg < event.sbiArgs.size(); ++arg)
                out << (arg == 0 ? "" : ",") << JsonQuote(Hex(event.sbiArgs[arg]));
            out << "],\"machineModeEntryPc\":\"" << Hex(event.machineEntryPc)
                << "\",\"machineEntrySymbol\":"
                << JsonQuote(FunctionContaining(firmwareFunctions, event.machineEntryPc))
                << ",\"handlerDispatchPath\":[";
            for (size_t pc = 0; pc < event.handlerPcPrefixSize; ++pc) {
                const uint64_t handlerPc = event.handlerPcPrefix[pc];
                out << (pc == 0 ? "" : ",") << "{\"pc\":" << JsonQuote(Hex(handlerPc))
                    << ",\"symbol\":"
                    << JsonQuote(FunctionContaining(firmwareFunctions, handlerPc)) << "}";
            }
            out << "],\"handlerPathTruncated\":"
                << (event.handlerInstructions > event.handlerPcPrefixSize ? "true" : "false")
                << ",\"returned\":" << (event.returned ? "true" : "false")
                << ",\"mretPc\":\"" << Hex(event.returnInstructionPc)
                << "\",\"sModeReturnPc\":\"" << Hex(event.returnTargetPc)
                << "\",\"sModeResumptionObserved\":" << (event.resumed ? "true" : "false")
                << ",\"sModeResumptionPc\":\"" << Hex(event.resumedPc)
                << "\",\"resultA0\":\"" << Hex(event.sbiError)
                << "\",\"resultA1\":\"" << Hex(event.sbiValue)
                << "\",\"resultCaptured\":" << (event.sbiResultCaptured ? "true" : "false")
                << ",\"sameServiceCallsPreviously\":" << previousServiceCount
                << ",\"identicalArgumentCallsPreviously\":" << previousIdenticalCount
                << ",\"recurs\":" << (previousService != nullptr ? "true" : "false")
                << ",\"guestRetiredBetweenPreviousSameService\":" << (retiredBetween ? "true" : "false")
                << ",\"recurrenceAssessment\":" << JsonQuote(recurrenceAssessment)
                << "}";
            firstCall = false;
        }
        out << "\n  ]\n}\n";
    }
    {
        std::ofstream out(dir / "riscv20-linux-frontier.json", std::ios::out | std::ios::trunc);
        const StepResult& step = run.frontier;
        const uint64_t pc = step.pc;
        uint64_t nearestValue = 0;
        std::string nearestName;
        const uint64_t linuxPhysicalEnd = symbols.physical(symbols.bssStop);
        const bool pcInLinuxVirtual = pc >= symbols.imageStart && pc < symbols.bssStop;
        const bool pcInLinuxPhysical = pc >= boot_contract::kLinuxImageEntry &&
            pc < linuxPhysicalEnd;
        const auto chooseNearest = [&](const std::map<std::string, uint64_t>& table,
                                       const auto& addressForSymbol) {
            for (const auto& [name, value] : table) {
                if (value == 0) continue;
                const uint64_t address = addressForSymbol(value);
                if (address <= pc && address >= nearestValue) {
                    nearestValue = address;
                    nearestName = name;
                }
            }
        };
        if (pcInLinuxVirtual) {
            chooseNearest(symbols.values, [](uint64_t value) { return value; });
        } else if (pcInLinuxPhysical) {
            chooseNearest(symbols.values, [&symbols](uint64_t value) {
                return symbols.physical(value);
            });
        } else if (pc >= boot_contract::kFirmwareEntry &&
                   pc < boot_contract::kFirmwareSlotEnd) {
            const std::map<std::string, uint64_t> firmwareSymbols =
                ParseElfSymbolTable(firmwareElf);
            chooseNearest(firmwareSymbols, [](uint64_t value) { return value; });
            if (nearestName.empty()) {
                nearestName = "OpenSBI firmware";
                nearestValue = boot_contract::kFirmwareEntry;
            }
        } else {
            nearestName = "outside pinned Linux and OpenSBI images";
        }
        out << "{\n  \"schemaVersion\": 2,\n  \"classification\": "
            << JsonQuote(evidence.stopClassification) << ",\n  \"stopReason\": "
            << JsonQuote(evidence.stopReason) << ",\n  \"executed\": "
            << (run.reachedFrontier ? "true" : "false") << ",\n"
            << "  \"pc\": \"" << Hex(pc) << "\",\n  \"raw\": \""
            << Hex(step.insn.rawWord) << "\",\n  \"instruction\": "
            << JsonQuote(decoder.Disassemble(step.insn)) << ",\n  \"symbol\": "
            << JsonQuote(nearestName) << ",\n  \"symbolAddress\": \""
            << Hex(nearestValue) << "\",\n  \"privilegeBefore\": \""
            << PrivilegeModeToString(run.frontierPrivilegeBefore) << "\",\n  \"privilegeAfter\": \""
            << PrivilegeModeToString(cpu.privilege()) << "\",\n  \"trapCause\": \""
            << (step.outcome == StepOutcome::Trap ? TrapCauseToString(step.trapCause) : "none")
            << "\",\n  \"trapTarget\": \"" << PrivilegeModeToString(step.trapTarget)
            << "\",\n  \"trapVector\": \"" << Hex(step.trapVector)
            << "\",\n  \"sepc\": \"" << Hex(CsrOrZero(cpu, 0x141))
            << "\",\n  \"scause\": \"" << Hex(CsrOrZero(cpu, 0x142))
            << "\",\n  \"stval\": \"" << Hex(CsrOrZero(cpu, 0x143))
            << "\",\n  \"mepc\": \"" << Hex(CsrOrZero(cpu, 0x341))
            << "\",\n  \"mcause\": \"" << Hex(CsrOrZero(cpu, 0x342))
            << "\",\n  \"mtval\": \"" << Hex(CsrOrZero(cpu, 0x343))
            << "\",\n  \"satp\": \"" << Hex(CsrOrZero(cpu, 0x180))
            << "\",\n  \"mstatus\": \"" << Hex(CsrOrZero(cpu, 0x300))
            << "\",\n  \"sstatus\": \"" << Hex(CsrOrZero(cpu, 0x100))
            << "\",\n  \"registers\":";
        std::array<uint64_t, NUM_GENERAL_REGISTERS> registers{};
        for (size_t index = 0; index < registers.size(); ++index) registers[index] = cpu.readGR(index);
        writeRegisters(out, registers);
        out << ",\n  \"prehistory\": [\n";
        for (size_t index = 0; index < run.tailTrace.size(); ++index) {
            const LinuxStepLine& line = run.tailTrace[index];
            out << "    {\"pc\":\"" << Hex(line.step.pc) << "\",\"raw\":\""
                << Hex(line.step.insn.rawWord) << "\",\"instruction\":"
                << JsonQuote(decoder.Disassemble(line.step.insn)) << ",\"outcome\":\""
                << StepOutcomeToString(line.step.outcome) << "\",\"nextPc\":\""
                << Hex(line.step.nextPc) << "\"}"
                << (index + 1 == run.tailTrace.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"traps\": [\n";
        for (size_t index = 0; index < evidence.traps.size(); ++index) {
            const LinuxTrapEvent& trap = evidence.traps[index];
            const riscv_test::TrapAccountingEvent* tracked =
                trap.accountingEventIndex < accounting.events().size()
                    ? &accounting.events()[trap.accountingEventIndex] : nullptr;
            out << "    {\"pc\":\"" << Hex(trap.pc) << "\",\"raw\":\""
                << Hex(trap.raw) << "\",\"instruction\":" << JsonQuote(trap.disassembly)
                << ",\"cause\":\""
                << (trap.interrupt ? "interrupt" : TrapCauseToString(trap.cause))
                << "\",\"origin\":\"" << PrivilegeModeToString(trap.origin)
                << "\",\"target\":\"" << PrivilegeModeToString(trap.target)
                << "\",\"vector\":\"" << Hex(trap.vector)
                << "\",\"nextPc\":\"" << Hex(trap.nextPc)
                << "\",\"scause\":\"" << Hex(trap.scause)
                << "\",\"sepc\":\"" << Hex(trap.sepc)
                << "\",\"stval\":\"" << Hex(trap.stval)
                << "\",\"satp\":\"" << Hex(trap.satp)
                << "\",\"interrupt\":" << (trap.interrupt ? "true" : "false")
                << ",\"interruptCause\":" << trap.interruptCause
                << ",\"accountingEventIndex\":" << trap.accountingEventIndex
                << ",\"handlerInvocation\":" << (tracked != nullptr && tracked->handlerInvocation ? "true" : "false")
                << ",\"progressAfterward\":" << (tracked != nullptr && tracked->resumed ? "true" : "false")
                << ",\"resumedPc\":\"" << Hex(tracked != nullptr ? tracked->resumedPc : 0)
                << "\",\"returned\":" << (tracked != nullptr && tracked->returned ? "true" : "false")
                << ",\"returnPc\":\"" << Hex(tracked != nullptr ? tracked->returnTargetPc : 0)
                << "\",\"transitionName\":" << JsonQuote(trap.transitionName)
                << ",\"architecturalTransition\":"
                << (trap.architecturalTransition ? "true" : "false")
                << ",\"mcause\":\"" << Hex(trap.mcause)
                << "\",\"mepc\":\"" << Hex(trap.mepc)
                << "\",\"mtval\":\"" << Hex(trap.mtval)
                << "\",\"mstatus\":\"" << Hex(trap.mstatus)
                << "\",\"sstatus\":\"" << Hex(trap.sstatus)
                << "\",\"registers\":";
            writeRegisters(out, trap.registers);
            out << "}"
                << (index + 1 == evidence.traps.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"firstPageWalk\": [\n";
        for (size_t index = 0; index < evidence.firstPageWalk.size(); ++index) {
            const Sv39WalkLevel& level = evidence.firstPageWalk[index];
            out << "    {\"level\":" << level.level << ",\"table\":\""
                << Hex(level.tableAddress) << "\",\"pteAddress\":\""
                << Hex(level.pteAddress) << "\",\"pte\":\"" << Hex(level.pte)
                << "\",\"readable\":" << (level.readable ? "true" : "false") << "}"
                << (index + 1 == evidence.firstPageWalk.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"pmpDeniedEvents\": [\n";
        const auto& denied = cpu.pmpDeniedTrace();
        for (size_t index = 0; index < denied.size(); ++index) {
            const PmpTraceEvent& event = denied[index];
            out << "    {\"pc\":\"" << Hex(event.pc) << "\",\"virtualAddress\":\""
                << Hex(event.virtualAddress) << "\",\"physicalAddress\":\""
                << Hex(event.physicalAddress) << "\",\"size\":" << event.size
                << ",\"access\":" << JsonQuote(event.access == AccessType::Fetch ? "fetch" :
                    event.access == AccessType::Load ? "load" : "store")
                << ",\"privilege\":\"" << PrivilegeModeToString(event.privilege)
                << "\",\"matchingEntry\":" << static_cast<int>(event.matchingEntry)
                << ",\"config\":\"" << Hex(event.config) << "\"}"
                << (index + 1 == denied.size() ? "\n" : ",\n");
        }
        out << "  ]\n}\n";
    }
}

void WriteImageContract(const std::filesystem::path& dir,
                        const RiscvLinuxImageLoadPlan& plan,
                        const std::vector<uint8_t>& image,
                        bool firmwareLoaded, bool fdtValid, bool pmpValid) {
    const uint64_t ramEnd = boot_contract::kRamBase + boot_contract::kRamSize;
    const uint64_t imageEnd = plan.loadAddress + plan.effectiveSize;
    const uint64_t fixedReserved = boot_contract::kFirmwareSlotEnd - boot_contract::kRamBase +
        boot_contract::kLinuxFdtCapacity;
    const uint64_t freeBytes = boot_contract::kRamSize - fixedReserved - plan.effectiveSize;
    std::ofstream out(dir / "riscv-linux-image-contract-v1.json",
                      std::ios::out | std::ios::trunc);
    out << "{\n  \"schemaVersion\": 1,\n"
        << "  \"sourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
        << "  \"sourceTag\": \"v6.12\",\n"
        << "  \"imageSha256\": \"" << Sha256(image) << "\",\n"
        << "  \"headerVersion\": " << plan.header.version << ",\n"
        << "  \"fileBytes\": " << plan.fileSize << ",\n"
        << "  \"effectiveBytes\": " << plan.effectiveSize << ",\n"
        << "  \"zeroInitializedTailBytes\": " << plan.effectiveSize - plan.fileSize << ",\n"
        << "  \"ram\": {\"base\": \"" << Hex(boot_contract::kRamBase)
        << "\", \"endExclusive\": \"" << Hex(ramEnd) << "\", \"bytes\": "
        << boot_contract::kRamSize << "},\n"
        << "  \"loadAddress\": \"" << Hex(plan.loadAddress) << "\",\n"
        << "  \"entryPoint\": \"" << Hex(plan.entryPoint) << "\",\n"
        << "  \"initialBranchTarget\": \"" << Hex(plan.initialBranchTarget) << "\",\n"
        << "  \"imageEndExclusive\": \"" << Hex(imageEnd) << "\",\n"
        << "  \"firmwareJumpMatchesEntry\": "
        << (firmwareLoaded && plan.entryPoint == boot_contract::kLinuxImageEntry ? "true" : "false") << ",\n"
        << "  \"fdtAddress\": \"" << Hex(boot_contract::kLinuxFdtAddress) << "\",\n"
        << "  \"fdtCapacityBytes\": " << boot_contract::kLinuxFdtCapacity << ",\n"
        << "  \"fixedFirmwareReservationBytes\": "
        << boot_contract::kFirmwareSlotEnd - boot_contract::kRamBase << ",\n"
        << "  \"remainingUnreservedBytesAfterImage\": " << freeBytes << ",\n"
        << "  \"eventualLinuxRuntimeFootprintKnown\": false,\n"
        << "  \"sourceFdtValid\": " << (fdtValid ? "true" : "false") << ",\n"
        << "  \"pmpCoverageValid\": " << (pmpValid ? "true" : "false") << ",\n"
        << "  \"pmpPolicy\": \"S-mode RWX for Image RAM; R for FDT; no catch-all; firmware/ACLINT denied\",\n"
        << "  \"devices\": [\n"
        << "    {\"name\":\"UART0\",\"base\":\"0x10000000\",\"size\":256},\n"
        << "    {\"name\":\"UART1\",\"base\":\"0x1000a000\",\"size\":256},\n"
        << "    {\"name\":\"PLIC\",\"base\":\"0x0c000000\",\"size\":4194304},\n"
        << "    {\"name\":\"ACLINT-mtime\",\"base\":\"0x0200bff8\",\"size\":8,\"owner\":\"M-mode OpenSBI\"},\n"
        << "    {\"name\":\"ACLINT-mtimecmp\",\"base\":\"0x02004000\",\"size\":8,\"owner\":\"M-mode OpenSBI\"}\n"
        << "  ]\n}\n";
}

void WriteFirmwareTrace(const std::filesystem::path& path,
                        const FirmwareRun& run, const Decoder& decoder) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    for (const auto& line : run.trace) {
        out << "{\"pc\":\"" << Hex(line.step.pc) << "\",\"raw\":\""
            << Hex(line.step.insn.rawWord) << "\",\"instruction\":"
            << JsonQuote(decoder.Disassemble(line.step.insn))
            << ",\"before\":\"" << PrivilegeModeToString(line.before)
            << "\",\"after\":\"" << PrivilegeModeToString(line.after)
            << "\",\"outcome\":\"" << StepOutcomeToString(line.step.outcome)
            << "\",\"cause\":"
            << (line.step.outcome == StepOutcome::Trap
                    ? JsonQuote(TrapCauseToString(line.step.trapCause))
                    : std::string("null"))
            << "}\n";
    }
}

} // namespace

int main(int argc, char** argv) {
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    constexpr int kExpectedArgc = 9;
    constexpr const char* kUsage = "usage: test_riscv_linux_startup_continuation fw_jump.bin fw_jump.elf Image manifest.json platform.dtb artifact_dir vmlinux config";
#else
    constexpr int kExpectedArgc = 7;
    constexpr const char* kUsage = "usage: test_riscv_linux_image_entry fw_jump.bin fw_jump.elf Image manifest.json platform.dtb artifact_dir";
#endif
    if (argc != kExpectedArgc) {
        std::cerr << kUsage << '\n';
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> image = ReadFile(argv[3]);
    const std::string manifest = AsString(ReadFile(argv[4]));
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[5]);
    const std::filesystem::path artifactDir(argv[6]);
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    const std::vector<uint8_t> vmlinux = ReadFile(argv[7]);
    const std::vector<uint8_t> config = ReadFile(argv[8]);
    const LinuxSymbols symbols = ParseLinuxVmlinux(vmlinux);
    const bool headDisassemblyMatches = VerifyHeadDisassembly(vmlinux, image, symbols);
    const std::string expectedConfigHash = Upper(JsonString(manifest, "configSha256"));
    Expect(!vmlinux.empty() && Sha256(vmlinux) ==
           "49A047DD85BAC9E8DA49F89907DFDCC845B8C9C564F9D6EB63BA76F68768D093",
           "pinned Linux v6.12 vmlinux hash matches RISCV-18 provenance");
    Expect(!config.empty() && Sha256(config) == expectedConfigHash &&
           expectedConfigHash == "5DFC1DEC7885EA79D1C6CA14262F7FF305D02AC37E74F01EE260CDD7317082AB",
           "pinned Linux v6.12 .config hash matches RISCV-18 provenance");
    Expect(symbols.valid,
           "vmlinux ELF symbol table contains the pinned BSS and startup symbols");
    Expect(headDisassemblyMatches,
           "linked vmlinux head.S bytes match the Image entry and BSS-loop instructions");
#else
    const std::vector<uint8_t> vmlinux;
    const std::vector<uint8_t> config;
    const LinuxSymbols symbols;
#endif
    std::error_code fsError;
    std::filesystem::create_directories(artifactDir, fsError);
    Expect(!fsError, "artifact output directory is created outside the source tree");

    const std::string expectedImageHash = Upper(JsonString(manifest, "imageSha256"));
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareBinSha256,
           "unchanged pinned OpenSBI v1.8.1 fw_jump.bin hash matches RISCV-9");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfSha256,
           "unchanged pinned OpenSBI v1.8.1 fw_jump.elf hash matches RISCV-9");
    Expect(!image.empty() && Sha256(image) == expectedImageHash &&
           expectedImageHash == "60CCB1172FFBE4B49CBC4447E7CCB4B705B5D60A9E92D3F10CF02A0D46BBCA2C",
           "authentic upstream Linux v6.12 Image matches the pinned manifest hash");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    Expect(firmwareLayout.valid && firmwareLayout.type == 3 &&
           firmwareLayout.machine == 243 &&
           firmwareLayout.entry == boot_contract::kFirmwareEntry &&
           firmwareLayout.lowest == boot_contract::kFirmwareEntry &&
           firmwareLayout.highest <= boot_contract::kFirmwareSlotEnd,
           "unchanged OpenSBI ELF load image remains inside its protected firmware slot");

    const FdtTree sourceTree = ParseFdt(sourceFdt);
    std::string fdtFailure;
    const bool sourceFdtValid = LinuxValidatePlatformFdt(sourceFdt, false, &fdtFailure);
    Expect(sourceFdtValid,
           "Linux-specific source FDT matches the single-hart MMU/device/reservation profile: " + fdtFailure);
    const auto reservations = LinuxExpectedReservations();
    const auto expectedPmp = LinuxExpectedPmpRegions();
    const auto options = LinuxMakeImageOptions(sourceFdt, sourceTree,
                                               reservations, expectedPmp);
    const RiscvLinuxImagePlanResult planned = planRiscvLinuxImage(image, options);
    Expect(planned.ok(), std::string("authentic Linux Image passes strict placement validation: ") +
           RiscvLinuxImageErrorToString(planned.error) + " " + planned.detail);
    if (!planned.ok() || image.empty() || firmware.empty() || sourceFdt.empty()) {
        std::ofstream outcome(artifactDir / "riscv-linux-outcome-v1.json");
        outcome << "{\"schemaVersion\":1,\"handoff\":false,\"reason\":\"preflight failed\"}\n";
        return g_failures == 0 ? 1 : 1;
    }

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    std::vector<uint8_t> ram(static_cast<size_t>(boot_contract::kRamSize), 0);
    cpu.loadProgram(boot_contract::kRamBase, ram);
    const bool firmwareLoaded = cpu.writePhysical(
        boot_contract::kFirmwareEntry, firmware.data(), firmware.size());
    const bool fdtLoaded = cpu.writePhysical(
        boot_contract::kLinuxFdtAddress, sourceFdt.data(), sourceFdt.size());
    MachineUartMapping uart0{boot_contract::kDevices[0].start, true};
    MachineUartMapping uart1{boot_contract::kDevices[1].start, true};
    MachinePlicMapping plic{boot_contract::kDevices[2].start, 96, true};
    MachineTimerMapping timer;
    timer.mtimeAddress = boot_contract::kDevices[3].start;
    timer.mtimecmpAddress = boot_contract::kDevices[4].start;
    const bool devicesConfigured = cpu.configureMachineUart(uart0) &&
        cpu.configureMachineUart1(uart1) && cpu.configureMachinePlic(plic) &&
        cpu.configureMachineTimer(timer);
    Expect(firmwareLoaded && fdtLoaded && devicesConfigured,
           "firmware, FDT, UART0/1, PLIC, and ACLINT models are loaded at declared ranges");

    const RiscvLinuxImageLoadResult loaded = loadRiscvLinuxImage(image, cpu, options);
    Expect(loaded.ok(), std::string("Linux Image transaction commits the complete effective footprint: ") +
           RiscvLinuxImageErrorToString(loaded.error) + " " + loaded.detail);
    std::vector<uint8_t> imageReadback(image.size(), 0);
    const bool imageRead = loaded.ok() && cpu.readPhysical(
        loaded.plan.loadAddress, imageReadback.data(), imageReadback.size());
    const size_t tailSize = static_cast<size_t>(planned.plan.effectiveSize - planned.plan.fileSize);
    std::vector<uint8_t> zeroTail(tailSize, 0xFF);
    const bool tailRead = tailSize == 0 || cpu.readPhysical(
        planned.plan.loadAddress + planned.plan.fileSize, zeroTail.data(), zeroTail.size());
    const bool zeroFilled = tailRead && std::all_of(zeroTail.begin(), zeroTail.end(),
        [](uint8_t value) { return value == 0; });
    Expect(imageRead && imageReadback == image && zeroFilled,
           "file bytes match the pinned Image and the header-declared runtime tail is zero-filled");

    uint8_t savedFirmware[8]{};
    const bool firmwareSaved = cpu.readPhysical(
        boot_contract::kFirmwareEntry, savedFirmware, sizeof(savedFirmware));
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(boot_contract::kFirmwareEntry);
    cpu.writeGR(10, boot_contract::kLinuxBootHartId);
    cpu.writeGR(11, boot_contract::kLinuxFdtAddress);
    Expect(cpu.sbiProvider() == nullptr && cpu.privilege() == PrivilegeMode::Machine &&
           cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
           cpu.getPC() == boot_contract::kFirmwareEntry && cpu.readGR(10) == 0 &&
           cpu.readGR(11) == boot_contract::kLinuxFdtAddress && !cpu.pagingEnabled(),
           "authentic OpenSBI begins in M-mode with a0=0, a1=FDT, Bare satp, and no host SBI provider");

    const FirmwareRun firmwareRun = RunOpenSbiToLinux(cpu);
    const uint64_t handoffPc = cpu.getPC();
    const PrivilegeMode handoffPrivilege = cpu.privilege();
    const uint64_t handoffA0 = cpu.readGR(10);
    const uint64_t handoffA1 = cpu.readGR(11);
    const uint64_t handoffSatp = CsrOrZero(cpu, 0x180);
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    const uint64_t handoffRetiredCounter = cpu.instructionsRetired();
    const uint64_t handoffStepCounter = cpu.stepsTaken();
    const uint64_t handoffMtime = cpu.mtime();
    const uint64_t handoffMtimecmp = cpu.mtimecmp();
    const uint64_t handoffMmioSequence = cpu.machineMmioEventSequence();
    const uint64_t handoffMmioReads = cpu.mmioReadCount();
    const uint64_t handoffMmioWrites = cpu.mmioWriteCount();
    const std::string handoffUartOutput = cpu.machineUartOutput();
#endif
    std::vector<std::string> failures;
    const bool actualPmpValid = ComparePmp(cpu, failures);
    Expect(firmwareSaved && firmwareRun.machineRetired > 0 && firmwareRun.handoff,
           "unchanged OpenSBI retires M-mode instructions and performs a real M-to-S Linux handoff");
    Expect(firmwareRun.mretCount > 0 && handoffPrivilege == PrivilegeMode::Supervisor &&
           handoffPc == boot_contract::kLinuxImageEntry &&
           handoffA0 == boot_contract::kLinuxBootHartId &&
           handoffA1 == boot_contract::kLinuxFdtAddress && handoffSatp == 0,
           "first Linux state is S-mode at the validated entry with a0=0, a1=FDT, satp=0");
    Expect(actualPmpValid,
           "all sixteen actual OpenSBI PMP entries exactly match the Linux policy without a catch-all");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "authentic OpenSBI banner is emitted over the guest UART");

    std::set<uint16_t> pmpAddresses;
    bool pmpConfigWrite = false;
    for (const auto& event : cpu.pmpCsrTrace()) {
        if (event.csr >= 0x3B0 && event.csr < 0x3C0) pmpAddresses.insert(event.csr);
        if (event.kind == PmpTraceKind::CsrWrite && event.csr == 0x3A0)
            pmpConfigWrite = true;
    }
    Expect(pmpAddresses.size() == 16 && pmpConfigWrite,
           "OpenSBI executes genuine PMP address probes and configuration CSR writes");

    const std::vector<uint8_t> finalFdtBytes = ReadHandoffFdt(cpu);
    const FdtTree finalTree = ParseFdt(finalFdtBytes);
    std::string finalFdtFailure;
    const bool finalFdtValid = LinuxValidatePlatformFdt(
        finalFdtBytes, true, &finalFdtFailure);
    Expect(!finalFdtBytes.empty() && finalFdtValid,
           "post-OpenSBI handoff FDT remains valid for Linux: " + finalFdtFailure);
    const bool reservationsValid = CompareReservations(finalTree, failures);
    Expect(reservationsValid,
           "fixed-up FDT retains only firmware and FDT no-map ranges, with no test/kernel reservation");

    auto actualPmp = CapturePmp(cpu);
    auto actualOptions = options;
    actualOptions.pmpRegions = actualPmp;
    actualOptions.fdtValid = finalFdtValid;
    actualOptions.fdtActualBytes = finalFdtBytes.size();
    actualOptions.bootHartEnabled = LinuxFdtBootHartEnabled(finalTree,
                                                            actualOptions.bootHartId);
    const auto postHandoffPlan = planRiscvLinuxImage(image, actualOptions);
    Expect(postHandoffPlan.ok(),
           std::string("loaded image remains valid under actual post-OpenSBI FDT/PMP state: ") +
           RiscvLinuxImageErrorToString(postHandoffPlan.error) + " " + postHandoffPlan.detail);

    const uint64_t actualMmioEvents = cpu.machineMmioEventSequence();
    const uint64_t actualMmioReads = cpu.mmioReadCount();
    const uint64_t actualMmioWrites = cpu.mmioWriteCount();
    Expect(actualMmioEvents > 0 && actualMmioReads > 0 && actualMmioWrites > 0,
           "OpenSBI initialization uses the mapped machine devices through guest MMIO");

    const uint64_t entryPc = handoffPc;
    std::array<uint8_t, 4> entryBytes{};
    const bool entryBytesRead = cpu.readPhysical(entryPc, entryBytes.data(), entryBytes.size());
    const bool entryBytesMatch = entryBytesRead && entryBytes[0] == image[0] &&
        entryBytes[1] == image[1] && entryBytes[2] == image[2] &&
        entryBytes[3] == image[3];
    Expect(entryBytesMatch,
           "first S-mode fetch bytes equal the authentic Linux Image header JAL");

    LinuxRun linuxRun;
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    ContinuationEvidence continuation;
    continuation.handoffRetiredCounter = handoffRetiredCounter;
    continuation.handoffMtime = handoffMtime;
    continuation.handoffMtimecmp = handoffMtimecmp;
    continuation.handoffMmioSequence = handoffMmioSequence;
    continuation.handoffMmioReads = handoffMmioReads;
    continuation.handoffMmioWrites = handoffMmioWrites;
    continuation.bss.expectedStart = symbols.physical(symbols.bssStart);
    continuation.bss.expectedStop = symbols.physical(symbols.bssStop);
    continuation.bss.loopStorePc = boot_contract::kLinuxImageEntry + 0x104ULL;
    continuation.bss.loopBranchPc = boot_contract::kLinuxImageEntry + 0x10CULL;
    continuation.bss.loopExitPc = boot_contract::kLinuxImageEntry + 0x110ULL;
    RecordMilestone(continuation, cpu, linuxRun, "authentic_opensbi_linux_handoff",
                    "M-mode OpenSBI MRET entered S-mode Linux with a0=0, a1=FDT, satp=Bare");
#endif
    const bool handoffStateValid = firmwareRun.handoff &&
        handoffPrivilege == PrivilegeMode::Supervisor &&
        handoffPc == boot_contract::kLinuxImageEntry &&
        handoffA0 == boot_contract::kLinuxBootHartId &&
        handoffA1 == boot_contract::kLinuxFdtAddress && handoffSatp == 0;
    if (handoffStateValid) {
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
        linuxRun = RunLinuxEntry(cpu, &continuation.bss);
#else
        linuxRun = RunLinuxEntry(cpu);
#endif
    }
    else linuxRun.stopReason = "Linux execution not started because the OpenSBI entry state failed";
    bool firstBranchRetired = false;
    uint64_t firstBranchTarget = 0;
    if (!linuxRun.trace.empty()) {
        const auto& first = linuxRun.trace.front();
        firstBranchTarget = first.step.nextPc;
        firstBranchRetired = first.before == PrivilegeMode::Supervisor &&
            first.step.outcome == StepOutcome::Ok &&
            first.step.insn.mnemonic == Mnemonic::JAL &&
            first.step.insn.rd == 0 && first.step.nextPc == planned.plan.initialBranchTarget;
    }
    Expect(linuxRun.retired >= 1 && firstBranchRetired,
           "production RV64CPU::step retires Linux's authentic entry JAL into _start_kernel");
    Expect(linuxRun.retired <= kLinuxStepBudget &&
           linuxRun.trace.size() + linuxRun.tailTrace.size() <=
               kLinuxTraceHeadLimit + kLinuxTraceTailLimit,
           "Linux entry execution and retained trace remain within explicit bounds");

#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    const uint64_t acceptedFrontierA3 = cpu.readGR(13);
    const uint64_t acceptedFrontierA4 = cpu.readGR(14);
    continuation.bss.pointerAt100k = acceptedFrontierA3;
    continuation.bss.stopAt100k = acceptedFrontierA4;
    continuation.bss.storeCountAt100k = continuation.bss.boundsMatchSymbols &&
        acceptedFrontierA3 >= continuation.bss.expectedStart
            ? (acceptedFrontierA3 - continuation.bss.expectedStart) / 8ULL : 0;
    continuation.linuxStepCalls = linuxRun.retired;
    continuation.supervisorRetired = linuxRun.retired;
    continuation.accounting.seed(linuxRun.retired, linuxRun.retired,
                                  linuxRun.retired, 0);
    const bool exactPriorFrontier = linuxRun.retired == 100000 &&
        linuxRun.traps == 0 && linuxRun.lastRetiredPc == 0x8020010CULL &&
        linuxRun.nextPc == 0x80200104ULL && !linuxRun.reachedFrontier &&
        linuxRun.frontier.pc == 0x80200104ULL &&
        linuxRun.frontier.insn.rawWord == 0x0006B023U &&
        continuation.bss.observedLoopEntry && continuation.bss.boundsMatchSymbols &&
        acceptedFrontierA4 == continuation.bss.expectedStop;
    Expect(exactPriorFrontier,
           "fresh authentic replay exactly reproduces the RISCV-18 100,000-instruction BSS frontier");
    const uint64_t expectedStores = (continuation.bss.expectedStop -
        continuation.bss.expectedStart) / 8ULL;
    Expect(continuation.bss.storeCountAt100k ==
               (acceptedFrontierA3 - continuation.bss.expectedStart) / 8ULL &&
           continuation.bss.storeCountAt100k < expectedStores,
           "guest a3 records measurable BSS-store progress at the accepted frontier");
    continuation.stages.push_back({100000, linuxRun.retired, linuxRun.retired,
        linuxRun.retired, cpu.getPC(), linuxRun.lastRetiredPc,
        continuation.bss.storesRetired});
    RecordMilestone(continuation, cpu, linuxRun, "riscv18_100000_instruction_frontier",
                    "exact PC, decoded instruction, a3/a4 and S-mode state matched the accepted run");
    const uint64_t pcAtContinuation = cpu.getPC();
    const uint64_t retiredAtContinuation = cpu.instructionsRetired();
    const uint64_t mtimeAtContinuation = cpu.mtime();
    const uint64_t a3AtContinuation = cpu.readGR(13);
    const uint64_t a4AtContinuation = cpu.readGR(14);
    Expect(pcAtContinuation == linuxRun.nextPc &&
           retiredAtContinuation - handoffRetiredCounter == 100000 &&
           mtimeAtContinuation >= handoffMtime &&
           a3AtContinuation == acceptedFrontierA3 && a4AtContinuation == acceptedFrontierA4,
           "continuation starts from the retained CPU, RAM, retirement, timer and register state");
    ContinueLinuxInStages(cpu, linuxRun, continuation, symbols);
    Expect(continuation.linuxStepCalls > 100000 &&
           cpu.stepsTaken() - handoffStepCounter == continuation.linuxStepCalls &&
           cpu.instructionsRetired() - handoffRetiredCounter == linuxRun.retired &&
           continuation.accounting.counters().totalRetired == linuxRun.retired,
           "staged continuation preserves the CPU state and accounts every subsequent step and retirement");
    const auto& trapCounts = continuation.accounting.counters();
    const uint64_t returnedTimeCalls = static_cast<uint64_t>(std::count_if(
        continuation.accounting.events().begin(), continuation.accounting.events().end(),
        [](const riscv_test::TrapAccountingEvent& event) {
            return event.sbiCall && event.sbiEid == 0x54494D45ULL && event.sbiFid == 0 &&
                event.returned && event.resumed && event.sbiResultCaptured;
        }));
    Expect(trapCounts.stepCalls == continuation.linuxStepCalls &&
           trapCounts.supervisorRetired + trapCounts.machineRetired + trapCounts.userRetired ==
               trapCounts.totalRetired && !continuation.accounting.lifetimesUncertain(),
           "per-privilege retirement counters reconcile with total guest retirements and paired transitions");
    Expect(trapCounts.maxNestingDepth != 0 &&
           trapCounts.handlerEntries - trapCounts.handlerReturns ==
               continuation.accounting.activeHandlerInvocations() &&
           trapCounts.supervisorEcalls == continuation.sbiEcalls,
           "architectural handler entries, returns, active frames, and S-mode ECALLs reconcile");
    Expect(returnedTimeCalls >= 2,
           "repeated pinned Linux SBI TIME calls complete through authentic OpenSBI and resume S-mode");
    Expect(continuation.bss.observedLoopEntry && continuation.bss.boundsMatchSymbols &&
           continuation.bss.loopExitObserved && continuation.bss.exitPointer ==
               continuation.bss.expectedStop && continuation.bss.exitStop ==
               continuation.bss.expectedStop && continuation.bss.storesRetired == expectedStores &&
           continuation.bss.allMemoryZero,
           "guest SD loop reaches its linked BSS exit and every BSS byte reads zero");
    Expect(continuation.setupVmReached || linuxRun.reachedFrontier,
           "post-BSS continuation reaches setup_vm or records a precise first architectural frontier");
    Expect(cpu.sbiProvider() == nullptr,
           "continuation leaves the host SBI provider absent while authentic firmware handles guest calls");
    std::vector<std::string> continuationFailures;
    const bool pmpAfterValid = ComparePmp(cpu, continuationFailures);
    continuation.pmpUnchanged = actualPmpValid && pmpAfterValid;
    const std::vector<uint8_t> finalFdtAfterContinuation = ReadHandoffFdt(cpu);
    continuation.fdtUnchanged = !finalFdtAfterContinuation.empty() &&
        Sha256(finalFdtAfterContinuation) == Sha256(finalFdtBytes);
    continuation.uartPrefixPreserved = cpu.machineUartOutput().size() >= handoffUartOutput.size() &&
        cpu.machineUartOutput().compare(0, handoffUartOutput.size(), handoffUartOutput) == 0;
    Expect(continuation.pmpUnchanged && continuationFailures.empty(),
           "the complete effective PMP policy remains unchanged through Linux continuation");
    Expect(continuation.fdtUnchanged && continuation.uartPrefixPreserved,
           "OpenSBI FDT contents and existing guest UART output persist through continuation");
    Expect(continuation.finalMtime >= handoffMtime &&
           cpu.instructionsRetired() - handoffRetiredCounter == linuxRun.retired,
           "deterministic virtual time and retirement state persist across all execution stages");
    const uint64_t bssFirst = continuation.bss.expectedStart;
    const uint64_t bssMiddle = continuation.bss.expectedStart +
        ((continuation.bss.expectedStop - continuation.bss.expectedStart) / 2ULL & ~7ULL);
    uint64_t firstBssWord = UINT64_MAX;
    uint64_t middleBssWord = UINT64_MAX;
    uint64_t lastBssWord = UINT64_MAX;
    const bool bssSamplesReadable = cpu.readU64(bssFirst, firstBssWord) &&
        cpu.readU64(bssMiddle, middleBssWord) &&
        cpu.readU64(continuation.bss.expectedStop - 8ULL, lastBssWord);
    Expect(bssSamplesReadable && firstBssWord == 0 && middleBssWord == 0 && lastBssWord == 0,
           "BSS beginning, midpoint and final guest-written words remain zero");
#endif

    WriteImageContract(artifactDir, planned.plan, image, firmwareLoaded,
                      sourceFdtValid, actualPmpValid);
    WriteBytes(artifactDir / "riscv18-final-fixedup.dtb", finalFdtBytes);
    {
        std::ofstream uart(artifactDir / "riscv18-uart0-output.txt",
                           std::ios::out | std::ios::trunc);
        uart << cpu.machineUartOutput();
    }
    Decoder firmwareDecoder;
    WriteFirmwareTrace(artifactDir / "riscv18-opensbi-tail-trace.jsonl",
                       firmwareRun, firmwareDecoder);

    {
        std::ofstream out(artifactDir / "riscv-reservation-comparison-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"valid\": "
            << (reservationsValid ? "true" : "false")
            << ",\n  \"sourceFdt\": \"" << Sha256(sourceFdt)
            << "\",\n  \"fixedUpFdt\": \"" << Sha256(finalFdtBytes)
            << "\",\n  \"actualFdtBytes\": " << finalFdtBytes.size()
            << ",\n  \"capacityBytes\": " << boot_contract::kLinuxFdtCapacity
            << ",\n  \"expectedLinuxReservations\": [\n";
        for (size_t index = 0; index < boot_contract::kLinuxReservations.size(); ++index) {
            const auto& range = boot_contract::kLinuxReservations[index];
            out << "    {\"name\":" << JsonQuote(std::string(range.name))
                << ",\"base\":\"" << Hex(range.start) << "\",\"size\":\""
                << Hex(range.size) << "\"}"
                << (index + 1 == boot_contract::kLinuxReservations.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"observedReservedMemoryNodes\": [\n";
        std::vector<std::string> observedNodes;
        for (const auto& [path, node] : finalTree.nodes) {
            if (path.rfind("/reserved-memory/", 0) != 0 ||
                path.find('/', 17) != std::string::npos) continue;
            const auto reg = node.properties.find("reg");
            if (reg == node.properties.end() || reg->second.size() != 16) continue;
            const bool noMap = node.properties.contains("no-map");
            std::ostringstream item;
            item << "    {\"path\":" << JsonQuote(path)
                << ",\"base\":\"" << Hex(Be64Pair(reg->second, 0))
                << "\",\"size\":\"" << Hex(Be64Pair(reg->second, 8))
                << "\",\"noMap\":" << (noMap ? "true" : "false") << "}";
            observedNodes.push_back(item.str());
        }
        for (size_t index = 0; index < observedNodes.size(); ++index)
            out << "    " << observedNodes[index]
                << (index + 1 == observedNodes.size() ? "\n" : ",\n");
        out << "  ],\n  \"validationFailures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-pmp-effective-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"valid\": "
            << (actualPmpValid ? "true" : "false") << ",\n  \"implementedEntries\": 16,\n"
            << "  \"profile\": \"StandardPmp16\",\n  \"entries\": [\n";
        for (size_t index = 0; index < actualPmp.size(); ++index) {
            const auto& entry = actualPmp[index];
            out << "    {\"index\":" << entry.index << ",\"active\":"
                << (entry.active ? "true" : "false") << ",\"config\":\""
                << Hex(entry.config) << "\",\"lower\":\"" << Hex(entry.lowerBound)
                << "\",\"upperExclusive\":\"" << Hex(entry.upperBound) << "\"}"
                << (index + 1 == actualPmp.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"policyRegions\": [\n";
        for (size_t index = 0; index < kLinuxEffectivePmp.size(); ++index) {
            const auto& entry = kLinuxEffectivePmp[index];
            out << "    {\"index\":" << index << ",\"name\":"
                << JsonQuote(entry.name) << ",\"config\":\"" << Hex(entry.config)
                << "\",\"lower\":\"" << Hex(entry.lower)
                << "\",\"upperExclusive\":\"" << Hex(entry.upper) << "\"}"
                << (index + 1 == kLinuxEffectivePmp.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"validationFailures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-opensbi-handoff-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"firmwareSha256\": \""
            << Sha256(firmware) << "\",\n  \"handoffObserved\": "
            << (firmwareRun.handoff ? "true" : "false") << ",\n"
            << "  \"firmwareSteps\": " << firmwareRun.steps << ",\n"
            << "  \"mModeRetired\": " << firmwareRun.machineRetired << ",\n"
            << "  \"sModeRetiredBeforeKernel\": " << firmwareRun.supervisorRetired << ",\n"
            << "  \"traps\": " << firmwareRun.traps << ",\n"
            << "  \"interrupts\": " << firmwareRun.interrupts << ",\n"
            << "  \"sbiEcalls\": " << firmwareRun.sbiEcalls << ",\n"
            << "  \"mretCount\": " << firmwareRun.mretCount << ",\n"
            << "  \"mmioEvents\": " << actualMmioEvents << ",\n"
            << "  \"mmioReads\": " << actualMmioReads << ",\n"
            << "  \"mmioWrites\": " << actualMmioWrites << ",\n"
            << "  \"virtualTimeTicksObserved\": " << cpu.mtime() << ",\n"
            << "  \"maxFirmwareStepsAndTicks\": " << kOpenSbiStepBudget << ",\n"
            << "  \"retainedMmioTraceEntries\": " << cpu.machineMmioTrace().size() << ",\n"
            << "  \"pc\": \"" << Hex(handoffPc) << "\",\n"
            << "  \"privilege\": \"" << PrivilegeModeToString(handoffPrivilege) << "\",\n"
            << "  \"a0\": \"" << Hex(handoffA0) << "\",\n"
            << "  \"a1\": \"" << Hex(handoffA1) << "\",\n"
            << "  \"satp\": \"" << Hex(handoffSatp) << "\",\n"
            << "  \"linuxProgressPc\": \"" << Hex(cpu.getPC()) << "\",\n"
            << "  \"linuxProgressPrivilege\": \""
            << PrivilegeModeToString(cpu.privilege()) << "\",\n"
            << "  \"hostSbiProviderPresent\": false,\n"
            << "  \"firmwareBannerObserved\": "
            << (cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos ? "true" : "false") << ",\n"
            << "  \"stopReason\": " << JsonQuote(firmwareRun.stopReason) << "\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-entry-trace-v1.json");
        Decoder decoder;
        out << "{\n  \"schemaVersion\": 1,\n  \"imageSha256\": \""
            << Sha256(image) << "\",\n  \"entryPoint\": \""
            << Hex(planned.plan.entryPoint) << "\",\n  \"branchTarget\": \""
            << Hex(planned.plan.initialBranchTarget) << "\",\n"
            << "  \"retiredInstructions\": " << linuxRun.retired << ",\n"
            << "  \"traps\": " << linuxRun.traps << ",\n"
            << "  \"interrupts\": " << linuxRun.interrupts << ",\n"
            << "  \"sbiEcalls\": " << linuxRun.sbiEcalls << ",\n"
            << "  \"lastRetiredPc\": \"" << Hex(linuxRun.lastRetiredPc) << "\",\n"
            << "  \"nextPc\": \"" << Hex(linuxRun.nextPc) << "\",\n"
            << "  \"traceHeadEntries\": " << linuxRun.trace.size() << ",\n"
            << "  \"traceTailEntries\": " << linuxRun.tailTrace.size() << ",\n"
            << "  \"stopReason\": " << JsonQuote(linuxRun.stopReason) << ",\n"
            << "  \"steps\": [\n";
        size_t emitted = 0;
        const size_t retainedTraceCount = linuxRun.trace.size() + linuxRun.tailTrace.size();
        const auto emitLine = [&](const LinuxStepLine& line) {
            out << "    {\"pc\":\"" << Hex(line.step.pc) << "\",\"raw\":\""
                << Hex(line.step.insn.rawWord) << "\",\"instruction\":"
                << JsonQuote(decoder.Disassemble(line.step.insn))
                << ",\"before\":\"" << PrivilegeModeToString(line.before)
                << "\",\"after\":\"" << PrivilegeModeToString(line.after)
                << "\",\"outcome\":\"" << StepOutcomeToString(line.step.outcome)
                << "\",\"nextPc\":\"" << Hex(line.step.nextPc)
                << "\",\"cause\":"
                << (line.step.outcome == StepOutcome::Trap
                        ? JsonQuote(TrapCauseToString(line.step.trapCause))
                        : std::string("null"))
                << "}"
                << (++emitted == retainedTraceCount ? "\n" : ",\n");
        };
        for (const auto& line : linuxRun.trace) emitLine(line);
        for (const auto& line : linuxRun.tailTrace) emitLine(line);
        out << "  ]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-frontier-v1.json");
        Decoder decoder;
        const StepResult& step = linuxRun.frontier;
        const uint64_t instructionPc = step.pc;
        const uint64_t physicalInstructionAddress = linuxRun.frontierPhysicalAddress != 0
            ? linuxRun.frontierPhysicalAddress : instructionPc;
        const auto fetch = std::find_if(cpu.pmpTrace().rbegin(), cpu.pmpTrace().rend(),
            [&](const PmpTraceEvent& event) {
                return event.kind == PmpTraceKind::PhysicalAccess &&
                    event.privilege == PrivilegeMode::Supervisor &&
                    event.access == AccessType::Fetch &&
                    event.physicalAddress == physicalInstructionAddress;
            });
        PmpEntryView matchedRegion;
        size_t matchedIndex = 0;
        bool hasMatchedRegion = false;
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView candidate;
            if (cpu.pmpEntry(index, candidate) && candidate.active &&
                physicalInstructionAddress >= candidate.lowerBound &&
                physicalInstructionAddress < candidate.upperBound) {
                matchedRegion = candidate;
                matchedIndex = index;
                hasMatchedRegion = true;
                break;
            }
        }
        out << "{\n  \"schemaVersion\": 1,\n  \"sourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
            << "  \"imageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"entryPoint\": \"" << Hex(planned.plan.entryPoint) << "\",\n"
            << "  \"lastSuccessfullyRetiredPc\": \"" << Hex(linuxRun.lastRetiredPc) << "\",\n"
            << "  \"frontierPc\": \"" << Hex(instructionPc) << "\",\n"
            << "  \"physicalInstructionAddress\": \""
            << Hex(physicalInstructionAddress) << "\",\n"
            << "  \"nextPc\": \"" << Hex(linuxRun.nextPc) << "\",\n"
            << "  \"rawInstruction\": \"" << Hex(step.insn.rawWord) << "\",\n"
            << "  \"disassembly\": " << JsonQuote(decoder.Disassemble(step.insn)) << ",\n"
            << "  \"outcome\": \""
            << (linuxRun.reachedFrontier ? StepOutcomeToString(step.outcome) : "progress_frontier")
            << "\",\n  \"instructionExecuted\": "
            << (linuxRun.reachedFrontier ? "true" : "false") << ",\n"
            << "  \"privilegeBefore\": \""
            << PrivilegeModeToString(linuxRun.frontierPrivilegeBefore)
            << "\",\n  \"privilegeAfter\": \"" << PrivilegeModeToString(cpu.privilege()) << "\",\n"
            << "  \"mcause\": \"" << Hex(CsrOrZero(cpu, 0x342)) << "\",\n"
            << "  \"mtval\": \"" << Hex(CsrOrZero(cpu, 0x343)) << "\",\n"
            << "  \"mepc\": \"" << Hex(CsrOrZero(cpu, 0x341)) << "\",\n"
            << "  \"scause\": \"" << Hex(CsrOrZero(cpu, 0x142)) << "\",\n"
            << "  \"stval\": \"" << Hex(CsrOrZero(cpu, 0x143)) << "\",\n"
            << "  \"sepc\": \"" << Hex(CsrOrZero(cpu, 0x141)) << "\",\n"
            << "  \"satp\": \"" << Hex(CsrOrZero(cpu, 0x180)) << "\",\n"
            << "  \"sstatus\": \"" << Hex(CsrOrZero(cpu, 0x100)) << "\",\n"
            << "  \"registers\": [";
        for (size_t index = 0; index < NUM_GENERAL_REGISTERS; ++index)
            out << (index == 0 ? "" : ",") << "\"" << Hex(cpu.readGR(index)) << "\"";
        out << "],\n  \"pmpFetchMatch\": ";
        if (fetch == cpu.pmpTrace().rend() && !hasMatchedRegion) out << "null,\n";
        else if (fetch == cpu.pmpTrace().rend())
            out << "{\"entry\":" << matchedIndex << ",\"config\":\""
                << Hex(matchedRegion.config) << "\",\"physicalAddress\":\""
                << Hex(physicalInstructionAddress) << "\",\"allowed\":true},\n";
        else out << "{\"entry\":" << static_cast<int>(fetch->matchingEntry)
                 << ",\"config\":\"" << Hex(fetch->config)
                 << "\",\"physicalAddress\":\"" << Hex(fetch->physicalAddress)
                 << "\",\"allowed\":" << (fetch->allowed ? "true" : "false") << "},\n";
        out << "  \"faultCause\": \""
            << (linuxRun.reachedFrontier ? TrapCauseToString(step.trapCause) : "none") << "\",\n"
            << "  \"frontierAssessment\": " << JsonQuote(linuxRun.stopReason) << "\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-fdt-boot-profile-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"sourceFdtSha256\": \""
            << Sha256(sourceFdt) << "\",\n  \"fixedUpFdtSha256\": \""
            << Sha256(finalFdtBytes) << "\",\n  \"sourceValid\": "
            << (sourceFdtValid ? "true" : "false") << ",\n  \"fixedUpValid\": "
            << (finalFdtValid ? "true" : "false") << ",\n  \"actualBytes\": "
            << finalFdtBytes.size() << ",\n  \"capacityBytes\": "
            << boot_contract::kLinuxFdtCapacity << ",\n  \"bootHartId\": 0,\n"
            << "  \"bootHartEnabled\": "
            << (LinuxFdtBootHartEnabled(finalTree, actualOptions.bootHartId) ? "true" : "false")
            << ",\n  \"ramBase\": \"0x80000000\",\n  \"ramSize\": \"0x04000000\",\n"
            << "  \"timebaseHz\": 10000000,\n  \"uart0\": \"0x10000000/0x100\",\n"
            << "  \"uart1\": \"0x1000a000/0x100\",\n  \"plic\": \"0x0c000000/0x400000, 96 sources\",\n"
            << "  \"aclintStatusAfterOpenSbi\": \"disabled; M-mode OpenSBI owns timer\"\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-outcome-v1.json");
        const bool outcomeA = g_failures == 0 && handoffStateValid &&
            firmwareRun.handoff && actualPmpValid && finalFdtValid &&
            reservationsValid && firstBranchRetired && failures.empty();
        out << "{\n  \"schemaVersion\": 1,\n  \"outcome\": \""
            << (outcomeA ? "A" : "B") << "\",\n"
            << "  \"firmwareSteps\": " << firmwareRun.steps << ",\n"
            << "  \"linuxRetiredInstructions\": " << linuxRun.retired << ",\n"
            << "  \"linuxFrontierOutcome\": \""
            << (linuxRun.reachedFrontier
                    ? StepOutcomeToString(linuxRun.frontier.outcome)
                    : "progress_frontier")
            << "\",\n  \"failures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }

#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    WriteContinuationArtifacts(artifactDir, firmware, firmwareElf, image, vmlinux, config,
        finalFdtBytes, symbols, firmwareRun, linuxRun, continuation, cpu);
#endif

    std::cout << "OpenSBI: steps=" << firmwareRun.steps
              << " M-retired=" << firmwareRun.machineRetired
              << " S-retired-before-kernel=" << firmwareRun.supervisorRetired
              << " traps=" << firmwareRun.traps << " interrupts=" << firmwareRun.interrupts
              << " SBI-ECALLs=" << firmwareRun.sbiEcalls
              << " MMIO=" << actualMmioEvents
              << " handoff=" << firmwareRun.handoff << " stop=" << firmwareRun.stopReason << '\n'
              << "Linux: retired=" << linuxRun.retired
              << " entry=0x" << std::hex << planned.plan.entryPoint
              << " branch-target=0x" << firstBranchTarget
              << " frontier=0x" << linuxRun.nextPc << std::dec
              << " stop=" << linuxRun.stopReason << '\n'
              << "Artifacts: " << artifactDir.string() << '\n';
    if (g_failures != 0) {
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
        std::cerr << "RISCV-19 Linux startup continuation checks=" << g_checks
                  << " failures=" << g_failures << '\n';
#else
        std::cerr << "RISCV-18 Linux entry checks=" << g_checks
                  << " failures=" << g_failures << '\n';
#endif
        return 1;
    }
#if defined(RISCV_LINUX_STARTUP_CONTINUATION)
    std::cout << "RISCV-19 Linux startup continuation checks=" << g_checks << " failures=0\n";
#else
    std::cout << "RISCV-18 Linux entry checks=" << g_checks << " failures=0\n";
#endif
    return 0;
}
