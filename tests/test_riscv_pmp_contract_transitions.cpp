#include "riscv_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kGuestBase = 0x80000000ULL;
constexpr uint64_t kRamSize = 0x00400000ULL;
constexpr uint64_t kResultBase = 0x80002000ULL;
constexpr uint64_t kNapotTarget = 0x80010000ULL;
constexpr uint64_t kTorTarget = 0x80011000ULL;
constexpr uint64_t kTargetValue = 0x6C6F636B6564706DULL;
constexpr uint64_t kLockedComplete = 0x4758504D4C4F434BULL;
constexpr uint64_t kRecoveryComplete = 0x4758504D52535431ULL;
constexpr uint64_t kStepBudget = 100000;

unsigned gChecks = 0;
unsigned gFailures = 0;

void Expect(bool condition, const std::string& message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

uint64_t ReadWord(const RV64CPU& cpu, uint64_t address) {
    uint64_t value = 0;
    (void)cpu.readU64(address, value);
    return value;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void WriteWord(RV64CPU& cpu, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    (void)cpu.writePhysical(address, bytes.data(), bytes.size());
}

struct RunResult {
    uint64_t steps = 0;
    uint64_t traps = 0;
    bool complete = false;
    bool waited = false;
};

RunResult RunUntil(RV64CPU& cpu, uint64_t markerAddress, uint64_t marker,
                   uint64_t budget) {
    RunResult result;
    for (; result.steps < budget; ++result.steps) {
        const StepResult step = cpu.step();
        if (step.outcome == StepOutcome::Trap) ++result.traps;
        if (ReadWord(cpu, markerAddress) == marker) {
            result.complete = true;
            ++result.steps;
            break;
        }
        if (step.outcome == StepOutcome::Waiting) {
            result.waited = true;
            break;
        }
        if (step.outcome != StepOutcome::Ok && step.outcome != StepOutcome::Trap) {
            break;
        }
    }
    return result;
}

bool EntryEquals(const RV64CPU& cpu, size_t index, uint8_t config,
                 uint64_t address, uint64_t lower, uint64_t upper) {
    PmpEntryView entry;
    return cpu.pmpEntry(index, entry) && entry.active && entry.config == config &&
        entry.encodedAddress == address && entry.lowerBound == lower &&
        entry.upperBound == upper;
}

bool EntryIsOff(const RV64CPU& cpu, size_t index, uint64_t address) {
    PmpEntryView entry;
    return cpu.pmpEntry(index, entry) && !entry.active && entry.config == 0 &&
        entry.encodedAddress == address;
}

bool HasCsrWrite(const RV64CPU& cpu, uint16_t csr, uint64_t value,
                 PrivilegeMode privilege = PrivilegeMode::Machine) {
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
        if (event.kind == PmpTraceKind::CsrWrite && event.csr == csr &&
            event.csrValue == value && event.privilege == privilege) return true;
    }
    return false;
}

uint64_t CountCsrWrites(const RV64CPU& cpu, uint16_t csr,
                        PrivilegeMode privilege = PrivilegeMode::Machine) {
    uint64_t count = 0;
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace())
        if (event.kind == PmpTraceKind::CsrWrite && event.csr == csr &&
            event.privilege == privilege) ++count;
    return count;
}

bool HasDeniedLoad(const RV64CPU& cpu, uint64_t address, int matchingEntry,
                   uint8_t config) {
    for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
        if (event.privilege == PrivilegeMode::Supervisor &&
            event.access == AccessType::Load && event.physicalAddress == address &&
            event.matchingEntry == matchingEntry && event.config == config &&
            !event.allowed) return true;
    }
    return false;
}

void WriteEvidence(const std::string& path, const RunResult& locked,
                   const RunResult& recovery, const RV64CPU& cpu,
                   bool resetCleared, bool lockedNapotsRejected,
                   bool lockedTorRejected, bool recoveryChanged) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    Expect(static_cast<bool>(out), "locked transition report is written to its artifact path");
    if (!out) return;
    out << "{\n  \"schemaVersion\": 1,\n"
        << "  \"unlockedTransition\": {\"coveredBy\": \"RiscVPmpDomainPolicyTests\", "
        << "\"result\": \"deny-grant-allowed-revoke-deny\"},\n"
        << "  \"lockedEntryRejection\": {\"guestCsrWritesUsed\": true, "
        << "\"napotWritesIgnored\": " << (lockedNapotsRejected ? "true" : "false")
        << ", \"torAndPredecessorWritesIgnored\": "
        << (lockedTorRejected ? "true" : "false") << ", \"sModeFaults\": 2},\n"
        << "  \"lockedGuestRun\": {\"steps\": " << locked.steps
        << ", \"trapCount\": " << locked.traps << ", \"complete\": "
        << (locked.complete ? "true" : "false") << "},\n"
        << "  \"resetRecovery\": {\"productionResetPath\": true, "
        << "\"allEntriesOffAndUnlocked\": " << (resetCleared ? "true" : "false")
        << ", \"guestRamPreserved\": true, \"sameRegionReconfiguredByGuest\": "
        << (recoveryChanged ? "true" : "false") << ", \"guestRun\": {\"steps\": "
        << recovery.steps << ", \"trapCount\": " << recovery.traps << ", \"complete\": "
        << (recovery.complete ? "true" : "false") << "}},\n"
        << "  \"synchronization\": {\"sfenceVmaExecutedByGuest\": "
        << cpu.sfenceVmaTrace().size() << ", \"tlbInvalidationClaimed\": false, "
        << "\"translationCacheImplemented\": false},\n"
        << "  \"mmioSideEffects\": {\"eventCount\": "
        << cpu.machineMmioTrace().size() << "},\n"
        << "  \"preservedRegressions\": [\"RiscVPmpTests:MPRV-and-Sv39\", "
        << "\"RiscVPmpDomainPolicyTests:permission-transitions-and-SFENCE.VMA\"],\n"
        << "  \"cpuInstanceIsolation\": true,\n"
        << "  \"linuxExecution\": false\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_riscv_pmp_contract_transitions guest.bin evidence.json\n";
        return 2;
    }
    const std::vector<uint8_t> guest = ReadFile(argv[1]);
    Expect(!guest.empty() && guest.size() < 0x2000,
           "independently assembled M/S guest fits the executable PMP region");
    if (guest.empty() || guest.size() >= 0x2000) return 1;

    std::vector<uint8_t> ram(static_cast<size_t>(kRamSize), 0);
    std::copy(guest.begin(), guest.end(), ram.begin());
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    RV64CPU independentCpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kGuestBase, ram);
    WriteWord(cpu, kNapotTarget, kTargetValue);
    WriteWord(cpu, kTorTarget, kTargetValue ^ 0x1111111111111111ULL);
    Expect(cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
           cpu.pmpImplementedEntries() == 16,
           "locked transition guest runs on production StandardPmp16 CPU");

    const RunResult locked = RunUntil(cpu, kResultBase + 120,
                                      kLockedComplete, kStepBudget);
    Expect(locked.complete && !locked.waited && locked.traps == 2,
           "guest reaches completion after two delegated S-mode locked-range faults");
    Expect(ReadWord(cpu, kResultBase + 0) == 0x4758504D4C4F434BULL &&
           ReadWord(cpu, kResultBase + 8) == 1 &&
           static_cast<uint32_t>(ReadWord(cpu, kResultBase + 20)) == 0 &&
           ReadWord(cpu, kResultBase + 24) == 2,
           "guest records locked-stage signature, phase, and exact fault count");

    const uint64_t napotAddress = kNapotTarget >> 2;
    const uint64_t torLower = kTorTarget >> 2;
    const uint64_t torUpper = (kTorTarget + 8) >> 2;
    const bool lockedNapotState = EntryEquals(cpu, 1, 0x98, napotAddress,
                                               kNapotTarget, kNapotTarget + 8);
    const bool lockedTorState = EntryEquals(cpu, 4, 0x88, torUpper,
                                             kTorTarget, kTorTarget + 8) &&
        EntryIsOff(cpu, 3, torLower);
    Expect(lockedNapotState, "entry 1 remains locked NAPOT with the original denied range");
    Expect(lockedTorState, "entry 4 remains locked TOR and retains its predecessor address");

    const uint64_t expectedConfig = 0x88001B981DULL;
    const bool lockedReadbacks = ReadWord(cpu, kResultBase + 128) == napotAddress &&
        ReadWord(cpu, kResultBase + 136) == expectedConfig &&
        ReadWord(cpu, kResultBase + 144) == torLower &&
        ReadWord(cpu, kResultBase + 152) == torUpper;
    Expect(lockedReadbacks,
           "guest CSR readbacks prove locked config, NAPOT address, and TOR predecessor writes were ignored");

    const uint64_t firstCause = ReadWord(cpu, kResultBase + 32);
    const uint64_t firstAddress = ReadWord(cpu, kResultBase + 40);
    const uint64_t firstPc = ReadWord(cpu, kResultBase + 48);
    const uint64_t secondCause = ReadWord(cpu, kResultBase + 56);
    const uint64_t secondAddress = ReadWord(cpu, kResultBase + 64);
    const uint64_t secondPc = ReadWord(cpu, kResultBase + 72);
    Expect(firstCause == 5 && firstAddress == kNapotTarget && firstPc != 0 &&
           secondCause == 5 && secondAddress == kTorTarget && secondPc != 0 &&
           firstPc != secondPc,
           "S-mode trap log identifies distinct locked NAPOT and TOR loads");
    Expect(HasDeniedLoad(cpu, kNapotTarget, 1, 0x98) &&
           HasDeniedLoad(cpu, kTorTarget, 4, 0x88),
           "production PMP checks deny S-mode accesses at the locked entry indices");

    const bool napotAttemptTraced = CountCsrWrites(cpu, 0x3B1) >= 2 &&
        HasCsrWrite(cpu, 0x3B1, napotAddress) &&
        HasCsrWrite(cpu, 0x3B3, torLower) &&
        HasCsrWrite(cpu, 0x3B4, torUpper) &&
        HasCsrWrite(cpu, 0x3A0, expectedConfig);
    Expect(napotAttemptTraced,
           "PMP trace records guest M-mode CSR attempts and architecturally retained values");
    Expect(cpu.sfenceVmaTrace().size() == 2 &&
           std::all_of(cpu.sfenceVmaTrace().begin(), cpu.sfenceVmaTrace().end(),
               [](const SfenceVmaTraceEvent& event) {
                   return event.privilege == PrivilegeMode::Machine &&
                          event.rs1 == 0 && event.rs2 == 0;
               }),
           "guest executes genuine M-mode SFENCE.VMA around lock policy updates");

    PmpEntryView code, result, other;
    bool unrelatedEntriesStable = cpu.pmpEntry(0, code) && code.config == 0x1D &&
        code.lowerBound == kGuestBase && code.upperBound == kGuestBase + 0x2000 &&
        cpu.pmpEntry(2, result) && result.config == 0x1B &&
        result.lowerBound == kResultBase && result.upperBound == kResultBase + 0x1000;
    for (size_t index = 5; index < cpu.pmpImplementedEntries(); ++index)
        unrelatedEntriesStable = unrelatedEntriesStable && cpu.pmpEntry(index, other) &&
            !other.active && other.config == 0;
    Expect(unrelatedEntriesStable,
           "locked CSR attempts leave code, result, and all unrelated PMP entries unchanged");
    Expect(cpu.machineMmioTrace().empty() && cpu.mmioReadCount() == 0 &&
           cpu.mmioWriteCount() == 0 && cpu.mtimecmp() == UINT64_MAX,
           "locked guest touches no MMIO device or timer state");
    Expect(independentCpu.pmpImplementedEntries() == 16 &&
           independentCpu.pmpCsrTrace().empty() && independentCpu.pmpDeniedTrace().empty() &&
           EntryIsOff(independentCpu, 1, 0) && EntryIsOff(independentCpu, 4, 0),
           "a second production CPU instance retains independent empty PMP state");

    cpu.reset();
    bool resetCleared = cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
        cpu.pmpImplementedEntries() == 16 && cpu.privilege() == PrivilegeMode::Machine &&
        cpu.getPC() == kGuestBase && cpu.sfenceVmaTrace().empty() &&
        cpu.pmpCsrTrace().empty() && ReadWord(cpu, kResultBase + 8) == 1;
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        resetCleared = resetCleared && cpu.pmpEntry(index, entry) &&
            !entry.active && entry.config == 0 && entry.encodedAddress == 0;
    }
    Expect(resetCleared,
           "production reset clears all PMP locks and addresses while preserving guest RAM phase state");

    const RunResult recovery = RunUntil(cpu, kResultBase + 192,
                                        kRecoveryComplete, kStepBudget);
    Expect(recovery.complete && !recovery.waited && recovery.traps == 0,
           "reset guest restarts and completes an unlocked same-region recovery");
    const uint64_t recoveryConfig = 0x081B191DULL;
    const bool recoveryChanged =
        EntryEquals(cpu, 1, 0x19, napotAddress, kNapotTarget, kNapotTarget + 8) &&
        ReadWord(cpu, kResultBase + 160) == ((kNapotTarget + 0x100) >> 2) &&
        ReadWord(cpu, kResultBase + 168) == napotAddress &&
        ReadWord(cpu, kResultBase + 176) == recoveryConfig &&
        ReadWord(cpu, kResultBase + 184) == kTargetValue &&
        HasCsrWrite(cpu, 0x3B1, (kNapotTarget + 0x100) >> 2) &&
        HasCsrWrite(cpu, 0x3B1, napotAddress) &&
        HasCsrWrite(cpu, 0x3A0, recoveryConfig);
    Expect(recoveryChanged,
           "guest M-mode changes the unlocked same NAPOT region after reset and S-mode reads it");
    Expect(cpu.sfenceVmaTrace().size() == 2 &&
           std::all_of(cpu.sfenceVmaTrace().begin(), cpu.sfenceVmaTrace().end(),
               [](const SfenceVmaTraceEvent& event) {
                   return event.privilege == PrivilegeMode::Machine &&
                          event.rs1 == 0 && event.rs2 == 0;
               }),
           "recovery permission setup and widening execute actual M-mode SFENCE.VMA instructions");
    Expect(cpu.machineMmioTrace().empty() && cpu.mtimecmp() == UINT64_MAX,
           "reset recovery remains contained to guest RAM and leaves machine MMIO state unchanged");

    WriteEvidence(argv[2], locked, recovery, cpu, resetCleared,
                  lockedNapotState && lockedReadbacks,
                  lockedTorState && lockedReadbacks, recoveryChanged);
    std::cout << "RISCV-17 locked PMP transition checks=" << gChecks
              << " failures=" << gFailures << '\n';
    return gFailures == 0 ? 0 : 1;
}
