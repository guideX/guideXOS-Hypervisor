#include "riscv_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr uint64_t kRamSize = 0x00400000ULL;
constexpr uint64_t kGuestEntry = kRamBase + 0x200000ULL;
constexpr uint64_t kTrapVector = kGuestEntry + 0x100ULL;
constexpr uint64_t kPmpCfg0 = 0x3A0;
constexpr uint64_t kPmpCfg2 = 0x3A2;
constexpr uint64_t kPmpAddr0 = 0x3B0;
constexpr uint64_t kMstatus = 0x300;
constexpr uint64_t kMepc = 0x341;
constexpr uint64_t kMedeleg = 0x302;
constexpr uint64_t kStvec = 0x105;
constexpr uint64_t kSatp = 0x180;
constexpr uint64_t kAllAccessFaultDelegation = (1ULL << 1) | (1ULL << 2) |
    (1ULL << 5) | (1ULL << 7) | (1ULL << 12) | (1ULL << 13) | (1ULL << 15);
constexpr uint8_t kR = 1U;
constexpr uint8_t kW = 2U;
constexpr uint8_t kX = 4U;
constexpr uint8_t kTor = 1U << 3;
constexpr uint8_t kNa4 = 2U << 3;
constexpr uint8_t kNapot = 3U << 3;
constexpr uint8_t kLock = 1U << 7;

unsigned checks = 0;
unsigned failures = 0;

void Expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectEq(uint64_t expected, uint64_t actual, const std::string& message) {
    Expect(expected == actual, message + " expected=" + std::to_string(expected) +
           " actual=" + std::to_string(actual));
}

uint32_t EncodeI(int32_t imm, unsigned rs1, unsigned funct3, unsigned rd,
                 unsigned opcode) {
    return ((static_cast<uint32_t>(imm) & 0xFFFU) << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

uint32_t EncodeS(int32_t imm, unsigned rs2, unsigned rs1, unsigned funct3) {
    const uint32_t uimm = static_cast<uint32_t>(imm) & 0xFFFU;
    return ((uimm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
           ((uimm & 0x1FU) << 7) | 0x23U;
}

uint32_t EncodeU(uint32_t imm20, unsigned rd) {
    return (imm20 & 0xFFFFF000U) | (rd << 7) | 0x37U;
}

uint32_t EncodeCsr(unsigned funct3, unsigned rd, unsigned rs1, unsigned csr) {
    return (csr << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x73U;
}

uint32_t EncodeAmo(unsigned funct5, unsigned rs2, unsigned rs1, unsigned funct3,
                   unsigned rd) {
    return (funct5 << 27) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
           (rd << 7) | 0x2FU;
}

std::vector<uint8_t> MakeRam(size_t bytes = static_cast<size_t>(kRamSize)) {
    return std::vector<uint8_t>(bytes, 0);
}

void WriteWord(RV64CPU& cpu, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{{
        static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)}};
    (void)cpu.writePhysical(address, bytes.data(), bytes.size());
}

void WriteU64(RV64CPU& cpu, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8U));
    }
    (void)cpu.writePhysical(address, bytes.data(), bytes.size());
}

void SetPmpEntry(RV64CPU& cpu, size_t index, uint8_t config, uint64_t address) {
    const uint64_t csr = kPmpAddr0 + index;
    cpu.debugWriteCsr(static_cast<uint32_t>(csr), address);
    const uint64_t cfgCsr = index < 8 ? kPmpCfg0 : kPmpCfg2;
    uint64_t cfg = 0;
    (void)cpu.readCsr(static_cast<uint32_t>(cfgCsr), cfg);
    const size_t field = index & 7U;
    cfg = (cfg & ~(0xFFULL << (field * 8U))) |
          (static_cast<uint64_t>(config) << (field * 8U));
    cpu.debugWriteCsr(static_cast<uint32_t>(cfgCsr), cfg);
}

void AllowRam(RV64CPU& cpu, uint64_t base = kRamBase, unsigned order = 22) {
    const unsigned trailingOnes = order - 3U;
    const uint64_t mask = trailingOnes == 0 ? 0ULL : ((1ULL << trailingOnes) - 1ULL);
    SetPmpEntry(cpu, 1, static_cast<uint8_t>(kR | kW | kX | kNapot),
                (base >> 2) | mask);
}

void EnterSupervisor(RV64CPU& cpu, uint64_t entry = kGuestEntry,
                     uint64_t trapVector = kTrapVector) {
    WriteWord(cpu, kRamBase, 0x30200073U); // genuine MRET instruction
    cpu.debugWriteCsr(static_cast<uint32_t>(kMedeleg), kAllAccessFaultDelegation);
    cpu.debugWriteCsr(static_cast<uint32_t>(kStvec), trapVector);
    cpu.debugWriteCsr(static_cast<uint32_t>(kMstatus), 1ULL << 11);
    cpu.debugWriteCsr(static_cast<uint32_t>(kMepc), entry);
    cpu.setPC(kRamBase);
    const StepResult step = cpu.step();
    Expect(step.outcome == StepOutcome::Ok && cpu.privilege() == PrivilegeMode::Supervisor &&
           cpu.getPC() == entry, "real MRET enters the S-mode test payload");
}

void TestProfilesAndCsrSemantics() {
    RV64CPU legacy;
    Expect(legacy.pmpImplementedEntries() == 0, "legacy profile has no PMP entries");
    uint64_t value = 0;
    Expect(!legacy.readCsr(static_cast<uint32_t>(kPmpCfg0), value),
           "PMP CSRs remain absent in the compatibility profile");

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    Expect(cpu.pmpImplementedEntries() == 16, "standard profile implements exactly 16 entries");
    Expect(cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16,
           "PMP machine profile selection is observable");
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpCfg0), value) && value == 0,
           "pmpcfg0 resets to all OFF");
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpCfg2), value) && value == 0,
           "pmpcfg2 resets to all OFF");
    for (uint32_t index = 0; index < 16; ++index) {
        Expect(cpu.readCsr(static_cast<uint32_t>(kPmpAddr0 + index), value) && value == 0,
               "implemented pmpaddr resets to zero");
    }
    Expect(!cpu.readCsr(static_cast<uint32_t>(kPmpAddr0 + 16), value),
           "pmpaddr16 is unimplemented and traps");
    Expect(!cpu.readCsr(0x3A1, value), "odd RV64 pmpcfg1 is illegal");
    Expect(!cpu.readCsr(0x3A4, value), "pmpcfg4 is unimplemented for this profile");

    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpAddr0), UINT64_MAX);
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpAddr0), value) &&
           value == ((1ULL << 54) - 1ULL),
           "pmpaddr WARL mask reports the implemented 54 address bits");
    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpAddr0), 0);
    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpCfg0), 0x2ULL);
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpCfg0), value) && (value & 0xFFU) == 0,
           "reserved R=0,W=1 configuration deterministically maps to no R/W permission");

    // The six CSR instruction forms use the ordinary Zicsr read-modify-write
    // path. A write to pmpaddr0 through CSRRW is checked alongside readback.
    cpu.loadProgram(kRamBase, MakeRam(0x2000));
    WriteWord(cpu, kRamBase, EncodeI(1, 0, 0, 1, 0x13));       // addi x1,x0,1
    WriteWord(cpu, kRamBase + 4, EncodeCsr(1, 2, 1, 0x3B0)); // csrrw x2,pmpaddr0,x1
    WriteWord(cpu, kRamBase + 8, EncodeCsr(2, 3, 0, 0x3B0)); // csrrs x3,pmpaddr0,x0
    Expect(cpu.step().outcome == StepOutcome::Ok && cpu.step().outcome == StepOutcome::Ok &&
           cpu.step().outcome == StepOutcome::Ok, "guest CSRRW/CSRRS PMP instructions retire in M-mode");
    Expect(cpu.readGR(2) == 0 && cpu.readGR(3) == 1,
           "CSRRW returns the old value and CSRRS x0 reads without writing");
    Expect(cpu.pmpCsrTrace().size() >= 3,
           "bounded diagnostics retain PMP CSR probes, writes, and readbacks");

    cpu.reset();
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpCfg0), value) && value == 0 &&
           cpu.readCsr(static_cast<uint32_t>(kPmpAddr0), value) && value == 0,
           "reset clears PMP permissions, addresses, and locks");
}

void TestAddressMatchingAndPriority() {
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kRamBase, MakeRam());
    const uint64_t region = kRamBase + 0x3000;
    WriteU64(cpu, region, 0x123456789ABCDEF0ULL);

    // All OFF is distinct from a machine with no PMP implementation.
    EnterSupervisor(cpu);
    std::array<uint8_t, 4> bytes{};
    Expect(!cpu.readPhysical(region, bytes.data(), bytes.size()),
           "unmatched S-mode physical loads are denied when PMP exists but entries are OFF");

    // NA4 and a partial access overlap.
    SetPmpEntry(cpu, 0, static_cast<uint8_t>(kR | kNa4), region >> 2);
    AllowRam(cpu);
    Expect(cpu.readPhysical(region, bytes.data(), 4), "NA4 permits its exact four-byte range");
    Expect(!cpu.readPhysical(region + 2, bytes.data(), 4),
           "partial NA4 coverage faults even when a lower-priority region is permissive");

    // TOR uses the preceding address as its lower bound even when the
    // preceding entry is OFF.
    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpCfg0), 0);
    SetPmpEntry(cpu, 0, 0, region >> 2);
    SetPmpEntry(cpu, 1, static_cast<uint8_t>(kR | kTor), (region + 0x1000) >> 2);
    Expect(cpu.readPhysical(region, bytes.data(), 1), "TOR includes its lower bound");
    Expect(cpu.readPhysical(region + 0xFFC, bytes.data(), 1), "TOR excludes no byte below its top");
    Expect(!cpu.readPhysical(region + 0x1000, bytes.data(), 1), "TOR upper bound is exclusive");
    Expect(!cpu.readPhysical(region - 1, bytes.data(), 1), "TOR lower bound is inclusive");

    // NAPOT range encodings for 8, 16, and 32 bytes.
    for (unsigned order : {3U, 4U, 5U}) {
        cpu.debugWriteCsr(static_cast<uint32_t>(kPmpCfg0), 0);
        const unsigned trailing = order - 3U;
        const uint64_t low = trailing == 0 ? 0ULL : ((1ULL << trailing) - 1ULL);
        SetPmpEntry(cpu, 0, static_cast<uint8_t>(kR | kNapot), (region >> 2) | low);
        const PmpEntryView view = [&]() { PmpEntryView out; (void)cpu.pmpEntry(0, out); return out; }();
        Expect(view.lowerBound == region && view.upperBound == region + (1ULL << order),
               "NAPOT decodes aligned region size " + std::to_string(1ULL << order));
        Expect(cpu.readPhysical(region + (1ULL << order) - 1ULL, bytes.data(), 1),
               "NAPOT includes its final byte");
        Expect(!cpu.readPhysical(region + (1ULL << order), bytes.data(), 1),
               "NAPOT excludes the byte after its range");
    }

    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpCfg2), 0);
    SetPmpEntry(cpu, 15, static_cast<uint8_t>(kR | kNapot), (1ULL << 54) - 1ULL);
    PmpEntryView high;
    Expect(cpu.pmpEntry(15, high) && high.lowerBound == 0 && high.upperBound == (1ULL << 57),
           "largest NAPOT encoding decodes without shift overflow");

    // Lowest-numbered matching entry is final, even if a later entry permits.
    cpu.debugWriteCsr(static_cast<uint32_t>(kPmpCfg0), 0);
    SetPmpEntry(cpu, 0, kNa4, region >> 2);
    SetPmpEntry(cpu, 1, static_cast<uint8_t>(kR | kNapot),
                (region >> 2) | 1ULL); // 16-byte NAPOT
    Expect(!cpu.readPhysical(region, bytes.data(), 4),
           "an earlier restrictive match wins over a later permissive match");

    RV64CPU user(PmpMachineProfile::StandardPmp16);
    user.loadProgram(kRamBase, MakeRam());
    AllowRam(user);
    WriteWord(user, kRamBase, 0x30200073U); // genuine MRET into U-mode
    user.debugWriteCsr(static_cast<uint32_t>(kMstatus), 0);
    user.debugWriteCsr(static_cast<uint32_t>(kMepc), kGuestEntry);
    user.setPC(kRamBase);
    const StepResult enterUser = user.step();
    Expect(enterUser.outcome == StepOutcome::Ok && user.privilege() == PrivilegeMode::User,
           "genuine MRET enters U-mode with the RAM entry authorized");
    constexpr uint64_t unmatchedUserAddress = 0x50000000ULL;
    Expect(!user.readPhysical(unmatchedUserAddress, bytes.data(), bytes.size()) &&
           !user.pmpTrace().empty() && !user.pmpTrace().back().allowed &&
           user.pmpTrace().back().privilege == PrivilegeMode::User &&
           user.pmpTrace().back().matchingEntry == -1,
           "unmatched U-mode physical access is denied by PMP independently of RAM bounds");
}

void TestPermissionsLockingAndPrivilege() {
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kRamBase, MakeRam());
    const uint64_t data = kRamBase + 0x8000;
    WriteU64(cpu, data, 0xAABBCCDDEEFF0011ULL);
    AllowRam(cpu);
    EnterSupervisor(cpu);
    std::array<uint8_t, 4> bytes{};

    // Entry zero denies the target; entry one gives the remainder of RAM to S.
    SetPmpEntry(cpu, 0, kNa4, data >> 2);
    Expect(!cpu.readPhysical(data, bytes.data(), bytes.size()), "OFF/zero-permission entry denies S read");
    SetPmpEntry(cpu, 0, static_cast<uint8_t>(kR | kNa4), data >> 2);
    Expect(cpu.readPhysical(data, bytes.data(), bytes.size()), "R permission grants S loads");
    Expect(!cpu.writePhysical(data, bytes.data(), bytes.size()), "missing W permission denies S stores");
    SetPmpEntry(cpu, 0, static_cast<uint8_t>(kW | kNa4), data >> 2);
    Expect(!cpu.readPhysical(data, bytes.data(), bytes.size()), "missing R permission denies S loads");

    // Unlocked matching permissions do not restrict ordinary M-mode; L makes
    // the same permission restriction apply to M-mode.
    RV64CPU machine(PmpMachineProfile::StandardPmp16);
    machine.loadProgram(kRamBase, MakeRam());
    WriteU64(machine, data, 0xAABBCCDDEEFF0011ULL);
    SetPmpEntry(machine, 0, kNa4, data >> 2);
    std::array<uint8_t, 4> mbytes{};
    Expect(machine.readPhysical(data, mbytes.data(), mbytes.size()),
           "unlocked matching PMP entry ignores R/W/X permissions in M-mode");
    SetPmpEntry(machine, 0, static_cast<uint8_t>(kNa4 | kLock), data >> 2);
    Expect(!machine.readPhysical(data, mbytes.data(), mbytes.size()),
           "locked zero-permission entry also denies M-mode loads");
    const uint64_t lockedAddr = data >> 2;
    machine.debugWriteCsr(static_cast<uint32_t>(kPmpAddr0), lockedAddr + 1);
    uint64_t readback = 0;
    Expect(machine.readCsr(static_cast<uint32_t>(kPmpAddr0), readback) && readback == lockedAddr,
           "locked address register ignores writes");
    machine.debugWriteCsr(static_cast<uint32_t>(kPmpCfg0), 0);
    Expect(machine.readCsr(static_cast<uint32_t>(kPmpCfg0), readback) &&
           (readback & 0xFFU) == static_cast<uint8_t>(kNa4 | kLock),
           "locked configuration ignores writes");
    machine.reset();
    SetPmpEntry(machine, 0, 0, data >> 2);
    SetPmpEntry(machine, 1, static_cast<uint8_t>(kTor | kLock), (data + 0x1000) >> 2);
    machine.debugWriteCsr(static_cast<uint32_t>(kPmpAddr0), (data + 4) >> 2);
    Expect(machine.readCsr(static_cast<uint32_t>(kPmpAddr0), readback) && readback == (data >> 2),
           "locked TOR entry prevents changing its predecessor address");
    machine.reset();
    Expect(machine.readCsr(static_cast<uint32_t>(kPmpCfg0), readback) && readback == 0 &&
           machine.readCsr(static_cast<uint32_t>(kPmpAddr0), readback) && readback == 0,
           "reset unlocks entries and clears PMP addresses");
}

void TestMachinePermissionRecovery() {
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kRamBase, MakeRam());
    constexpr uint64_t data = kRamBase + 0x18000;
    constexpr uint64_t machineVector = kGuestEntry + 0x200;
    WriteU64(cpu, data, 0x11223344ULL);
    WriteWord(cpu, kGuestEntry, EncodeI(0, 1, 2, 2, 0x03)); // lw x2,0(x1)
    WriteWord(cpu, kGuestEntry + 4, 0x00000073U); // S-mode ECALL asks M-mode to update PMP
    WriteWord(cpu, kGuestEntry + 8, EncodeI(0, 1, 2, 2, 0x03)); // retry lw
    WriteWord(cpu, kTrapVector, EncodeCsr(2, 5, 0, 0x141)); // csrrs x5,sepc,x0
    WriteWord(cpu, kTrapVector + 4, EncodeI(4, 5, 0, 5, 0x13)); // addi x5,x5,4
    WriteWord(cpu, kTrapVector + 8, EncodeCsr(1, 0, 5, 0x141)); // csrrw x0,sepc,x5
    WriteWord(cpu, kTrapVector + 12, 0x10200073U); // sret skips denied load
    WriteWord(cpu, machineVector, EncodeCsr(2, 3, 0, static_cast<unsigned>(kPmpCfg0)));
    WriteWord(cpu, machineVector + 4, EncodeI(1, 3, 0, 3, 0x13)); // permit R in entry 0
    WriteWord(cpu, machineVector + 8, EncodeCsr(1, 0, 3, static_cast<unsigned>(kPmpCfg0)));
    WriteWord(cpu, machineVector + 12, EncodeU(0x00802000U, 4)); // x4 = 0x00802000
    WriteWord(cpu, machineVector + 16, EncodeI(8, 4, 1, 4, 0x13)); // slli x4,x4,8
    WriteWord(cpu, machineVector + 20, EncodeI(8, 4, 0, 4, 0x13)); // addi x4,x4,8
    WriteWord(cpu, machineVector + 24, EncodeCsr(1, 0, 4, static_cast<unsigned>(kMepc)));
    WriteWord(cpu, machineVector + 28, 0x30200073U); // mret retries the load
    AllowRam(cpu);
    SetPmpEntry(cpu, 0, kNa4, data >> 2); // entry 0 matches but denies all permissions
    cpu.debugWriteCsr(0x305, machineVector);
    EnterSupervisor(cpu);
    cpu.writeGR(1, data);

    const StepResult denied = cpu.step();
    uint64_t cause = 0, tval = 0;
    (void)cpu.readCsr(0x142, cause);
    (void)cpu.readCsr(0x143, tval);
    Expect(denied.outcome == StepOutcome::Trap && denied.trapCause == TrapCause::LoadAccessFault &&
           cause == 5 && tval == data,
           "guest first observes the precise S-mode access fault before recovery");
    for (unsigned i = 0; i < 4; ++i) {
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "S-mode trap handler skips the denied load and returns to its ECALL");
    }
    Expect(cpu.getPC() == kGuestEntry + 4 && cpu.privilege() == PrivilegeMode::Supervisor,
           "guest resumes at the machine-service ECALL after its access-fault handler");

    const StepResult ecall = cpu.step();
    Expect(ecall.outcome == StepOutcome::Trap && ecall.trapCause == TrapCause::EcallFromS &&
           cpu.privilege() == PrivilegeMode::Machine && cpu.getPC() == machineVector,
           "S-mode ECALL enters the real M-mode PMP recovery routine");
    for (unsigned i = 0; i < 8; ++i) {
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "M-mode handler changes PMP and returns to the guest retry address");
    }
    Expect(cpu.privilege() == PrivilegeMode::Supervisor && cpu.getPC() == kGuestEntry + 8,
           "MRET returns to the guest load after the unlocked PMP update");
    const StepResult recovered = cpu.step();
    Expect(recovered.outcome == StepOutcome::Ok && cpu.readGR(2) == 0x11223344ULL,
           "previously denied S-mode load succeeds after M-mode grants the region");
    uint64_t cfg = 0;
    Expect(cpu.readCsr(static_cast<uint32_t>(kPmpCfg0), cfg) &&
           (cfg & 0xFFU) == static_cast<uint8_t>(kR | kNa4),
           "M-mode recovery programmed the selected unlocked PMP entry");
    Expect(std::any_of(cpu.pmpCsrTrace().begin(), cpu.pmpCsrTrace().end(),
                       [](const PmpTraceEvent& event) {
                           return event.kind == PmpTraceKind::CsrWrite &&
                                  event.csr == kPmpCfg0 && event.csrValue == 0x1F11ULL;
                       }),
           "bounded CSR trace records the guest-executed PMP permission update");
}

void TestArchitecturalFaultsAndAtomics() {
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kRamBase, MakeRam());
    const uint64_t data = kRamBase + 0x9000;
    WriteU64(cpu, data, 0x1122334455667788ULL);
    WriteWord(cpu, kGuestEntry, EncodeI(0, 1, 3, 2, 0x03)); // ld x2,0(x1)
    WriteWord(cpu, kGuestEntry + 4, EncodeS(0, 2, 1, 3));   // sd x2,0(x1)
    AllowRam(cpu);
    EnterSupervisor(cpu);
    SetPmpEntry(cpu, 0, kNa4, data >> 2);
    cpu.writeGR(1, data);

    const uint64_t retired = cpu.instructionsRetired();
    StepResult load = cpu.step();
    Expect(load.outcome == StepOutcome::Trap && load.trapCause == TrapCause::LoadAccessFault,
           "denied S-mode load raises load access fault");
    uint64_t value = 0;
    Expect(cpu.readCsr(0x142, value) && value == 5, "load access fault writes scause=5");
    Expect(cpu.readCsr(0x143, value) && value == data, "load access fault writes precise stval");
    Expect(cpu.readCsr(0x141, value) && value == kGuestEntry, "sepc identifies the denied load");
    Expect(cpu.instructionsRetired() == retired, "faulting load does not retire");

    cpu.setPC(kGuestEntry + 4);
    StepResult store = cpu.step();
    Expect(store.outcome == StepOutcome::Trap && store.trapCause == TrapCause::StoreAccessFault,
           "denied S-mode store raises store access fault");
    Expect(cpu.readCsr(0x143, value) && value == data, "store access fault reports the store address");

    // LR succeeds with R, while SC and AMO need W and must not alter RAM when
    // that permission is denied.
    cpu.loadProgram(kRamBase, MakeRam());
    WriteU64(cpu, data, 0x0102030405060708ULL);
    WriteWord(cpu, kGuestEntry, EncodeAmo(0x02, 0, 1, 2, 3)); // lr.w x3,(x1)
    WriteWord(cpu, kGuestEntry + 4, EncodeAmo(0x03, 2, 1, 2, 4)); // sc.w x4,x2,(x1)
    WriteWord(cpu, kGuestEntry + 8, EncodeAmo(0x00, 2, 1, 2, 5)); // amoadd.w x5,x2,(x1)
    WriteWord(cpu, kGuestEntry + 12, EncodeAmo(0x02, 0, 1, 3, 6)); // lr.d x6,(x1)
    WriteWord(cpu, kGuestEntry + 16, EncodeAmo(0x03, 2, 1, 3, 7)); // sc.d x7,x2,(x1)
    WriteWord(cpu, kGuestEntry + 20, EncodeAmo(0x00, 2, 1, 3, 8)); // amoadd.d x8,x2,(x1)
    AllowRam(cpu);
    SetPmpEntry(cpu, 0, static_cast<uint8_t>(kR | kNapot), data >> 2);
    EnterSupervisor(cpu);
    cpu.writeGR(1, data);
    cpu.writeGR(2, 0xDEADBEEFULL);
    Expect(cpu.step().outcome == StepOutcome::Ok, "LR.W succeeds with read permission");
    cpu.setPC(kGuestEntry + 4);
    const StepResult sc = cpu.step();
    Expect(sc.outcome == StepOutcome::Trap && sc.trapCause == TrapCause::StoreAccessFault,
           "SC.W checks write permission even with a live reservation");
    cpu.setPC(kGuestEntry + 8);
    const StepResult amo = cpu.step();
    Expect(amo.outcome == StepOutcome::Trap && amo.trapCause == TrapCause::StoreAccessFault,
           "AMO.W requires both read and write permission");
    cpu.setPC(kGuestEntry + 12);
    Expect(cpu.step().outcome == StepOutcome::Ok, "LR.D succeeds with read permission");
    cpu.setPC(kGuestEntry + 16);
    const StepResult scd = cpu.step();
    Expect(scd.outcome == StepOutcome::Trap && scd.trapCause == TrapCause::StoreAccessFault,
           "SC.D checks write permission with a live reservation");
    cpu.setPC(kGuestEntry + 20);
    const StepResult amod = cpu.step();
    Expect(amod.outcome == StepOutcome::Trap && amod.trapCause == TrapCause::StoreAccessFault,
           "AMO.D requires both read and write permission");
    std::array<uint8_t, 8> raw{};
    Expect(cpu.readPhysical(data, raw.data(), raw.size()),
           "S-mode retains read permission after atomic faults");
    Expect(raw[0] == 0x08 && raw[1] == 0x07 && raw[2] == 0x06 && raw[3] == 0x05 &&
           raw[4] == 0x04 && raw[5] == 0x03 && raw[6] == 0x02 && raw[7] == 0x01,
           "denied atomics do not partially update RAM");

    // A real S-mode CSR instruction is illegal even though host CSR probes are
    // intentionally privilege-neutral.
    RV64CPU csrCpu(PmpMachineProfile::StandardPmp16);
    csrCpu.loadProgram(kRamBase, MakeRam(0x210000));
    WriteWord(csrCpu, kGuestEntry, EncodeCsr(2, 5, 0, static_cast<unsigned>(kPmpCfg0)));
    AllowRam(csrCpu);
    EnterSupervisor(csrCpu);
    const StepResult illegal = csrCpu.step();
    Expect(illegal.outcome == StepOutcome::Trap && illegal.trapCause == TrapCause::IllegalInstruction,
           "S-mode PMP CSR read traps as illegal instruction");
    Expect(csrCpu.readCsr(0x142, value) && value == 2, "S-mode CSR denial records scause=2");
}

void TestMprvAndMmio() {
    RV64CPU mprv(PmpMachineProfile::StandardPmp16);
    mprv.loadProgram(kRamBase, MakeRam());
    const uint64_t data = kRamBase + 0xA000;
    WriteU64(mprv, data, 0x8877665544332211ULL);
    WriteWord(mprv, kRamBase, EncodeI(0, 1, 3, 2, 0x03));
    SetPmpEntry(mprv, 0, kNa4, data >> 2);
    AllowRam(mprv);
    mprv.writeGR(1, data);
    mprv.debugWriteCsr(static_cast<uint32_t>(kMstatus), (1ULL << 17) | (1ULL << 11));
    const StepResult mprvLoad = mprv.step();
    uint64_t cause = 0, tval = 0;
    Expect(mprvLoad.outcome == StepOutcome::Trap && mprvLoad.trapCause == TrapCause::LoadAccessFault &&
           mprv.privilege() == PrivilegeMode::Machine,
           "MPRV with MPP=S enforces S-mode PMP on an M-mode data load");
    (void)mprv.readCsr(0x342, cause);
    (void)mprv.readCsr(0x343, tval);
    Expect(cause == 5 && tval == data, "MPRV fault is precise in M-mode trap state");

    RV64CPU uart(PmpMachineProfile::StandardPmp16);
    uart.loadProgram(kRamBase, MakeRam());
    MachineUartMapping mapping;
    mapping.baseAddress = 0x10000000ULL;
    mapping.enabled = true;
    Expect(uart.configureMachineUart(mapping), "UART mapping configured for PMP side-effect test");
    Expect(uart.injectMachineUartReceiveByte(0x5A), "UART receive byte is queued");
    WriteWord(uart, kGuestEntry, EncodeI(0, 1, 4, 2, 0x03)); // lbu x2,0(x1)
    AllowRam(uart);
    EnterSupervisor(uart);
    uart.writeGR(1, mapping.baseAddress);
    const StepResult denied = uart.step();
    Expect(denied.outcome == StepOutcome::Trap && denied.trapCause == TrapCause::LoadAccessFault,
           "unmatched S-mode UART read is denied before device dispatch");
    Expect(uart.machineUartReceiveDataReady(), "denied UART RBR read does not consume input");
    SetPmpEntry(uart, 0, static_cast<uint8_t>(kR | kW | kNapot), mapping.baseAddress >> 2);
    uart.setPC(kGuestEntry);
    const StepResult permitted = uart.step();
    Expect(permitted.outcome == StepOutcome::Ok && uart.readGR(2) == 0x5A,
           "authorized S-mode UART read reaches the device");
    Expect(!uart.machineUartReceiveDataReady(), "authorized UART RBR read consumes the queued byte");

    RV64CPU plic(PmpMachineProfile::StandardPmp16);
    plic.loadProgram(kRamBase, MakeRam());
    constexpr uint64_t plicBase = 0x0C000000ULL;
    constexpr uint64_t supervisorClaim = plicBase + 0x201004ULL;
    Expect(plic.configureMachinePlic(MachinePlicMapping{plicBase, 96, true}),
           "PLIC configured for denied-claim side-effect test");
    WriteWord(plic, plicBase + 4, 1); // source 1 priority
    WriteWord(plic, plicBase + 0x2080, 1U << 1); // S context enable
    WriteWord(plic, plicBase + 0x201000, 0); // S context threshold
    Expect(plic.setMachinePlicSourceInputForTest(1, true), "PLIC source 1 is pending before read");
    WriteWord(plic, kGuestEntry, EncodeI(0, 1, 2, 2, 0x03)); // lw x2,0(x1)
    AllowRam(plic);
    EnterSupervisor(plic);
    plic.writeGR(1, supervisorClaim);
    const uint64_t claimsBefore = plic.machinePlicDiagnostics().claimReads;
    const StepResult deniedClaim = plic.step();
    Expect(deniedClaim.outcome == StepOutcome::Trap &&
           deniedClaim.trapCause == TrapCause::LoadAccessFault,
           "unmatched S-mode PLIC claim read raises load access fault");
    Expect(plic.machinePlicDiagnostics().claimReads == claimsBefore &&
           plic.machinePlicSourcePending(1) &&
           plic.machinePlicOutput(MachinePlicContext::Supervisor),
           "denied PLIC claim read does not count, claim, or clear the pending source");
    SetPmpEntry(plic, 0, static_cast<uint8_t>(kR | kNapot), (supervisorClaim & ~7ULL) >> 2);
    plic.setPC(kGuestEntry);
    const StepResult allowedClaim = plic.step();
    Expect(allowedClaim.outcome == StepOutcome::Ok && plic.readGR(2) == 1,
           "S-mode PLIC claim succeeds after its exact 8-byte PMP region is allowed");
    Expect(plic.machinePlicDiagnostics().claimReads == claimsBefore + 1 &&
           !plic.machinePlicSourcePending(1),
           "authorized PLIC claim alone consumes the pending source");

    RV64CPU timer(PmpMachineProfile::StandardPmp16);
    timer.loadProgram(kRamBase, MakeRam());
    const uint64_t deadline = 1000;
    WriteU64(timer, timer.machineTimerMapping().mtimecmpAddress, deadline);
    WriteWord(timer, kGuestEntry, EncodeS(0, 2, 1, 3)); // sd x2,0(x1)
    AllowRam(timer);
    EnterSupervisor(timer);
    timer.writeGR(1, timer.machineTimerMapping().mtimecmpAddress);
    timer.writeGR(2, 5);
    const auto timerWritesBefore = std::count_if(
        timer.machineMmioTrace().begin(), timer.machineMmioTrace().end(),
        [](const MachineMmioEvent& event) {
            return event.device == MachineMmioDevice::Timer &&
                   event.operation == MachineMmioOperation::Write;
        });
    const StepResult deniedTimerWrite = timer.step();
    Expect(deniedTimerWrite.outcome == StepOutcome::Trap &&
           deniedTimerWrite.trapCause == TrapCause::StoreAccessFault,
           "unmatched S-mode timer comparator write raises store access fault");
    ExpectEq(deadline, timer.mtimecmp(),
             "denied timer comparator write leaves the programmed deadline unchanged");
    const auto timerWritesAfter = std::count_if(
        timer.machineMmioTrace().begin(), timer.machineMmioTrace().end(),
        [](const MachineMmioEvent& event) {
            return event.device == MachineMmioDevice::Timer &&
                   event.operation == MachineMmioOperation::Write;
        });
    Expect(timerWritesAfter == timerWritesBefore,
           "denied timer write does not add a device MMIO dispatch event");

    RV64CPU isolatedA(PmpMachineProfile::StandardPmp16);
    RV64CPU isolatedB(PmpMachineProfile::StandardPmp16);
    SetPmpEntry(isolatedA, 0, static_cast<uint8_t>(kR | kNa4), data >> 2);
    PmpEntryView configuredEntry;
    PmpEntryView untouchedEntry;
    Expect(isolatedA.pmpEntry(0, configuredEntry) &&
           isolatedB.pmpEntry(0, untouchedEntry) &&
           configuredEntry.config == static_cast<uint8_t>(kR | kNa4) &&
           untouchedEntry.config == 0,
           "PMP configuration is isolated per CPU instance");
}

void TestSv39Pmp() {
    constexpr uint64_t root = kRamBase + 0x300000;
    constexpr uint64_t l1Code = kRamBase + 0x301000;
    constexpr uint64_t l1Alias = kRamBase + 0x302000;
    constexpr uint64_t l0Alias = kRamBase + 0x303000;
    constexpr uint64_t virtualAlias = 0x40000000ULL;
    constexpr uint64_t firmwarePpn = kRamBase >> 12;

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kRamBase, MakeRam());
    const uint64_t physicalRamLeaf = ((kRamBase + 0x200000) >> 12) << 10;
    WriteU64(cpu, root + 2 * 8, ((l1Code >> 12) << 10) | kPteV);
    WriteU64(cpu, l1Code + 1 * 8, physicalRamLeaf | kPteV | kPteR | kPteW | kPteX | kPteA | kPteD);
    WriteU64(cpu, root + 1 * 8, ((l1Alias >> 12) << 10) | kPteV);
    WriteU64(cpu, l1Alias, ((l0Alias >> 12) << 10) | kPteV);
    WriteU64(cpu, l0Alias, (firmwarePpn << 10) | kPteV | kPteR | kPteW | kPteX | kPteA | kPteD);
    WriteU64(cpu, l0Alias + 8, (firmwarePpn << 10) | kPteV | kPteR | kPteW | kPteX | kPteA | kPteD);
    WriteWord(cpu, kGuestEntry, EncodeI(0, 1, 3, 2, 0x03)); // ld x2,0(x1)
    WriteWord(cpu, kGuestEntry + 4, EncodeAmo(0x02, 0, 1, 3, 2)); // lr.d x2,(x1)
    WriteWord(cpu, kGuestEntry + 8, EncodeAmo(0x03, 2, 1, 3, 2)); // sc.d x2,x2,(x1)
    WriteWord(cpu, kGuestEntry + 12, EncodeAmo(0x00, 2, 1, 3, 2)); // amoadd.d x2,x2,(x1)

    SetPmpEntry(cpu, 0, kNapot, (kRamBase >> 2) | ((1ULL << 18) - 1ULL)); // 2 MiB deny
    AllowRam(cpu, kRamBase, 26); // 64 MiB guest RAM
    cpu.debugWriteCsr(static_cast<uint32_t>(kSatp), (8ULL << 60) | (root >> 12));
    EnterSupervisor(cpu);
    cpu.writeGR(1, virtualAlias);

    const StepResult denied = cpu.step();
    uint64_t cause = 0, tval = 0;
    (void)cpu.readCsr(0x142, cause);
    (void)cpu.readCsr(0x143, tval);
    Expect(denied.outcome == StepOutcome::Trap && denied.trapCause == TrapCause::LoadAccessFault,
           "valid Sv39 mapping to PMP-denied firmware raises access fault");
    Expect(cause == 5 && tval == virtualAlias,
           "translated PMP denial keeps the original VA in stval");

    cpu.setPC(kGuestEntry);
    cpu.writeGR(1, virtualAlias + 0x1000);
    const StepResult alias = cpu.step();
    Expect(alias.outcome == StepOutcome::Trap && alias.trapCause == TrapCause::LoadAccessFault,
           "a second virtual alias to the same physical firmware obeys the same PMP rule");

    cpu.writeGR(1, virtualAlias);
    cpu.setPC(kGuestEntry + 4);
    const StepResult translatedLr = cpu.step();
    uint64_t atomicCause = 0, atomicTval = 0;
    (void)cpu.readCsr(0x142, atomicCause);
    (void)cpu.readCsr(0x143, atomicTval);
    Expect(translatedLr.outcome == StepOutcome::Trap &&
           translatedLr.trapCause == TrapCause::LoadAccessFault && atomicCause == 5 &&
           atomicTval == virtualAlias,
           "LR.D checks PMP after Sv39 translation and reports the original virtual address");
    cpu.setPC(kGuestEntry + 8);
    const StepResult translatedSc = cpu.step();
    Expect(translatedSc.outcome == StepOutcome::Trap &&
           translatedSc.trapCause == TrapCause::StoreAccessFault,
           "SC.D checks write PMP permission after Sv39 translation");
    cpu.setPC(kGuestEntry + 12);
    const StepResult translatedAmo = cpu.step();
    Expect(translatedAmo.outcome == StepOutcome::Trap &&
           translatedAmo.trapCause == TrapCause::StoreAccessFault,
           "AMO.D checks read/write PMP permissions after Sv39 translation");

    // Page-table physical denial is classified as an access fault for the
    // original load. The host invokes the production walker directly here so
    // the failed walk can be inspected without routing its vector through the
    // deliberately inaccessible table.
    cpu.debugWriteCsr(static_cast<uint32_t>(kSatp), (8ULL << 60) | firmwarePpn);
    const Sv39Translation deniedWalk = cpu.translateLoad(virtualAlias, 8);
    Expect(!deniedWalk.ok && deniedWalk.cause == TrapCause::LoadAccessFault,
           "PMP-denied page-table read is classified as the original load access fault");

    // Repeat the denied next-level page-table read through a genuine S-mode
    // load so trap delegation, scause, sepc, and stval are checked architecturally.
    RV64CPU trappedWalk(PmpMachineProfile::StandardPmp16);
    trappedWalk.loadProgram(kRamBase, MakeRam());
    WriteU64(trappedWalk, root + 2 * 8, ((l1Code >> 12) << 10) | kPteV);
    WriteU64(trappedWalk, l1Code + 1 * 8,
             (((kRamBase + 0x200000) >> 12) << 10) | kPteV | kPteR | kPteW |
                 kPteX | kPteA | kPteD);
    WriteU64(trappedWalk, root + 1 * 8, ((l1Alias >> 12) << 10) | kPteV);
    WriteU64(trappedWalk, l1Alias, ((l0Alias >> 12) << 10) | kPteV);
    WriteWord(trappedWalk, kGuestEntry, EncodeI(0, 1, 3, 2, 0x03)); // ld x2,0(x1)
    SetPmpEntry(trappedWalk, 0, kNapot, (l0Alias >> 2) | 0x1FFULL); // deny its 4 KiB page
    AllowRam(trappedWalk, kRamBase, 22);
    trappedWalk.debugWriteCsr(static_cast<uint32_t>(kSatp), (8ULL << 60) | (root >> 12));
    EnterSupervisor(trappedWalk);
    trappedWalk.writeGR(1, virtualAlias);
    const StepResult deniedWalkTrap = trappedWalk.step();
    uint64_t walkCause = 0, walkTval = 0, walkEpc = 0;
    (void)trappedWalk.readCsr(0x142, walkCause);
    (void)trappedWalk.readCsr(0x143, walkTval);
    (void)trappedWalk.readCsr(0x141, walkEpc);
    Expect(deniedWalkTrap.outcome == StepOutcome::Trap &&
           deniedWalkTrap.trapCause == TrapCause::LoadAccessFault && walkCause == 5,
           "denied implicit PTE read enters the guest S-mode load access-fault handler");
    Expect(walkTval == virtualAlias && walkEpc == kGuestEntry,
           "page-table PMP fault preserves the original VA in stval and faulting PC in sepc");

    // An Sv39 leaf permission failure remains a page fault.
    WriteU64(cpu, l0Alias, (firmwarePpn << 10) | kPteV | kPteX | kPteA);
    cpu.debugWriteCsr(static_cast<uint32_t>(kSatp), (8ULL << 60) | (root >> 12));
    const Sv39Translation pageFault = cpu.translateLoad(virtualAlias, 8);
    Expect(!pageFault.ok && pageFault.cause == TrapCause::LoadPageFault,
           "Sv39 page-level permission failure remains a page fault");

    // A/D-bit write denied after the PTE read maps to the original access fault.
    constexpr uint64_t protectedLeafPage = l0Alias;
    WriteU64(cpu, l0Alias, (((kRamBase + 0x200000) >> 12) << 10) |
                            kPteV | kPteR | kPteW | kPteX | kPteD);
    SetPmpEntry(cpu, 0, static_cast<uint8_t>(kR | kNapot), protectedLeafPage >> 2 | 0x1FFULL);
    SetPmpEntry(cpu, 2, static_cast<uint8_t>(kR | kW | kX | kNapot),
                (kRamBase >> 2) | ((1ULL << 23) - 1ULL));
    const Sv39Translation adFault = cpu.translateLoad(virtualAlias, 8);
    Expect(!adFault.ok && adFault.cause == TrapCause::LoadAccessFault,
           "PMP-denied implicit A-bit update raises an access fault");
}

} // namespace

int main() {
    TestProfilesAndCsrSemantics();
    TestAddressMatchingAndPriority();
    TestPermissionsLockingAndPrivilege();
    TestMachinePermissionRecovery();
    TestArchitecturalFaultsAndAtomics();
    TestMprvAndMmio();
    TestSv39Pmp();
    std::cout << "RISCV-15 PMP checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
