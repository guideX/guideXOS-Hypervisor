// PPC64 Phase 22 MTMSRD decode, MSR transition, and authentic Linux replay.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64Platform.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                          \
        }                                                                      \
    } while (0)

constexpr uint32_t kLinuxMtmsrd = 0x7D600164U;
constexpr uint64_t kKernelSize = 49'045'944ULL;
constexpr uint64_t kRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kKernelEnd = 0x029C2158ULL;
constexpr uint64_t kEntry = 0x10000ULL;
constexpr uint32_t kRfid = 0x4C000024U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kSprDEC = 22;
constexpr uint64_t kPTable = 0x00200000ULL;
constexpr uint64_t kRoot = 0x00240000ULL;
constexpr uint64_t kLevel1 = 0x00260000ULL;
constexpr uint64_t kLevel2 = 0x00261000ULL;
constexpr uint64_t kPageTable = 0x00262000ULL;
constexpr uint64_t kCodeGRA = 0x00300000ULL;
constexpr uint64_t kDataGRA = 0x00310000ULL;
constexpr uint64_t kBackingBase = 0x00400000ULL;
constexpr uint64_t kPartitionSize = 0x00400000ULL;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;
constexpr uint64_t kPteRead = 0x4ULL;
constexpr uint64_t kPteWrite = 0x2ULL;
constexpr uint64_t kPteExecute = 0x1ULL;
constexpr uint64_t kExpectedMTMSRDWritableMask =
    0x8000000000000000ULL | 0x000000000000EF32ULL;
constexpr uint64_t kExpectedMTMSRDLimitedMask = 0x0000000000008002ULL;

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

void writeDoubleword(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint32_t encodeMTMSRD(uint8_t rs, bool l = false) {
    return (31U << 26) | (static_cast<uint32_t>(rs & 31U) << 21) |
           (l ? (1U << 15) : 0U) | (178U << 1);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeLwz(uint8_t rt, uint8_t ra, int16_t displacement) {
    return (32U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(reg) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, 467);
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

void testDecodeAndReservedFields() {
    std::puts("[phase22-decode-and-reserved-fields]");
    const auto decoded = PPC64ISAPlugin::decodeWord(kLinuxMtmsrd);
    CHECK(decoded.opcode == 31);
    CHECK(((kLinuxMtmsrd >> 21) & 0x1fU) == 11);
    CHECK(((kLinuxMtmsrd >> 1) & 0x3ffU) == 178);
    CHECK(((kLinuxMtmsrd >> 15) & 1U) == 0);
    CHECK((kLinuxMtmsrd & 1U) == 0);
    CHECK(decoded.kind == DecodedKind::MTMSRD);
    CHECK(decoded.supported);
    CHECK(decoded.rt == 11);
    CHECK(!decoded.l);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0xD328) == "mtmsrd r11");

    const auto limited = PPC64ISAPlugin::decodeWord(encodeMTMSRD(7, true));
    CHECK(limited.kind == DecodedKind::MTMSRD);
    CHECK(limited.supported);
    CHECK(limited.rt == 7);
    CHECK(limited.l);
    CHECK(PPC64ISAPlugin::disassembleDecoded(limited, 0) == "mtmsrd r7,1");

    for (uint32_t bad : {encodeMTMSRD(3) | (1U << 16),
                         encodeMTMSRD(3) | (1U << 14),
                         encodeMTMSRD(3) | 1U}) {
        const auto invalid = PPC64ISAPlugin::decodeWord(bad);
        CHECK(invalid.kind == DecodedKind::MTMSRD);
        CHECK(!invalid.supported);
    }
    CHECK(encodeMTMSRD(11) == kLinuxMtmsrd);
}

struct DirectResult {
    PPC64ISAState before;
    PPC64ISAState after;
    ISAExecutionResult outcome = ISAExecutionResult::EXCEPTION;
    PPC64FaultCategory fault = PPC64FaultCategory::NONE;
};

DirectResult runDirect(uint32_t instruction, uint8_t rs, uint64_t source,
                       uint64_t oldMSR, bool guestDelivery = false) {
    Memory memory(4096);
    writeWord(memory, 0, instruction);
    PPC64ISAPlugin cpu;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    state.nip = 0;
    state.msr = oldMSR;
    state.cr = 0xA5B6C7D8U;
    state.xer = 0xE1234567U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    for (unsigned reg = 0; reg < state.gpr.size(); ++reg) {
        state.gpr[reg] = 0x0102030405060708ULL * (reg + 1U);
    }
    state.gpr[rs] = source;
    cpu.setGuestDeliveryEnabled(guestDelivery);
    DirectResult result;
    result.before = state;
    result.outcome = cpu.step(memory);
    result.after = dynamic_cast<const PPC64ISAState&>(cpu.getState());
    if (cpu.hasFault()) result.fault = cpu.getLastFault().category;
    return result;
}

void checkPreservedNonMSR(const DirectResult& run, uint8_t sourceRegister) {
    for (size_t reg = 0; reg < run.before.gpr.size(); ++reg) {
        if (reg != sourceRegister) CHECK(run.after.gpr[reg] == run.before.gpr[reg]);
    }
    CHECK(run.after.cr == run.before.cr);
    CHECK(run.after.xer == run.before.xer);
    CHECK(run.after.lr == run.before.lr);
    CHECK(run.after.ctr == run.before.ctr);
    CHECK(run.after.srr0 == run.before.srr0);
    CHECK(run.after.srr1 == run.before.srr1);
}

void testGeneralAndLimitedMSRUpdates() {
    std::puts("[phase22-msr-mask-and-l-forms]");
    constexpr uint64_t reservedOld = 1ULL << 30;
    const uint64_t oldMSR = MSR_SF | MSR_ME | MSR_S | MSR_RI | reservedOld;
    const uint64_t source = MSR_SF | MSR_EE | MSR_FP | MSR_FE0 | MSR_SE |
        MSR_BE | MSR_FE1 | MSR_RI | MSR_HV | MSR_LE | (1ULL << 29);
    const DirectResult general = runDirect(encodeMTMSRD(4), 4, source, oldMSR);
    CHECK(MSR_MTMSRD_WRITABLE == kExpectedMTMSRDWritableMask);
    const uint64_t expected = (oldMSR & ~kExpectedMTMSRDWritableMask) |
                              (source & kExpectedMTMSRDWritableMask);
    CHECK(general.outcome == ISAExecutionResult::CONTINUE);
    CHECK(general.after.msr == expected);
    CHECK((general.after.msr & MSR_HV) == 0);  // no guest privilege elevation
    CHECK((general.after.msr & MSR_S) != 0);    // secure state preserved
    CHECK((general.after.msr & MSR_ME) != 0);   // machine-check state preserved
    CHECK((general.after.msr & MSR_LE) == 0);   // BE profile preserved
    CHECK((general.after.msr & reservedOld) != 0);
    CHECK((general.after.msr & (1ULL << 29)) == 0); // source reserved bit ignored
    CHECK(general.after.nip == 4);
    checkPreservedNonMSR(general, 4);

    const uint64_t limitedOld = MSR_SF | MSR_ME | MSR_S | MSR_FP | MSR_FE0 |
                                MSR_BE | MSR_RI;
    const DirectResult limitedEnable = runDirect(
        encodeMTMSRD(5, true), 5, MSR_EE, limitedOld);
    CHECK(MSR_EE == 0x0000000000008000ULL &&
          MSR_RI == 0x0000000000000002ULL);
    CHECK(limitedEnable.outcome == ISAExecutionResult::CONTINUE);
    CHECK(limitedEnable.after.msr ==
          ((limitedOld & ~kExpectedMTMSRDLimitedMask) |
           (MSR_EE & kExpectedMTMSRDLimitedMask)));
    CHECK(limitedEnable.after.nip == 4);
    checkPreservedNonMSR(limitedEnable, 5);

    const DirectResult limitedDisable = runDirect(
        encodeMTMSRD(5, true), 5, MSR_RI, limitedOld | MSR_EE);
    CHECK(limitedDisable.outcome == ISAExecutionResult::CONTINUE);
    CHECK(limitedDisable.after.msr ==
          (((limitedOld | MSR_EE) & ~kExpectedMTMSRDLimitedMask) |
           (MSR_RI & kExpectedMTMSRDLimitedMask)));
    CHECK((limitedDisable.after.msr & (MSR_SF | MSR_HV | MSR_PR | MSR_IR |
          MSR_DR | MSR_ME | MSR_LE | MSR_S)) ==
          (limitedOld & (MSR_SF | MSR_HV | MSR_PR | MSR_IR |
          MSR_DR | MSR_ME | MSR_LE | MSR_S)));
}

void testGPR0PrivilegeAndRejectedTransitions() {
    std::puts("[phase22-gpr0-privilege-and-rejection]");
    const DirectResult gpr0 = runDirect(encodeMTMSRD(0, true), 0,
                                        MSR_SF | MSR_EE | MSR_RI, MSR_SF);
    CHECK(gpr0.outcome == ISAExecutionResult::CONTINUE);
    CHECK(gpr0.after.msr == (MSR_SF | MSR_EE | MSR_RI));
    CHECK(gpr0.after.gpr[0] == gpr0.before.gpr[0]);

    const DirectResult problem = runDirect(encodeMTMSRD(4), 4,
        MSR_SF | MSR_EE | MSR_RI, MSR_SF | MSR_PR, true);
    CHECK(problem.outcome == ISAExecutionResult::CONTINUE);
    CHECK(problem.fault == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    CHECK(problem.after.nip == EXC_VECTOR_BASE + VEC_PROG);
    CHECK(problem.after.srr0 == 0);
    CHECK((problem.after.srr1 & SRR1_PROGPRIV) != 0);
    CHECK((problem.after.srr1 & MSR_PR) != 0);
    CHECK((problem.after.msr & MSR_PR) == 0); // exception entered supervisor state
    CHECK(problem.after.gpr[4] == problem.before.gpr[4]);
    CHECK(problem.after.cr == problem.before.cr);
    CHECK(problem.after.xer == problem.before.xer);

    const DirectResult badSF = runDirect(encodeMTMSRD(4), 4, 0, MSR_SF);
    CHECK(badSF.outcome == ISAExecutionResult::EXCEPTION);
    CHECK(badSF.fault == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(badSF.after.msr == badSF.before.msr);
    CHECK(badSF.after.nip == 0);

    const DirectResult noRadix = runDirect(encodeMTMSRD(4), 4,
        MSR_SF | MSR_IR | MSR_DR, MSR_SF);
    CHECK(noRadix.outcome == ISAExecutionResult::EXCEPTION);
    CHECK(noRadix.fault == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(noRadix.after.msr == noRadix.before.msr);
    CHECK(noRadix.after.nip == 0);

    for (uint32_t bad : {encodeMTMSRD(4) | (1U << 16),
                         encodeMTMSRD(4) | (1U << 14),
                         encodeMTMSRD(4) | 1U}) {
        const DirectResult invalid = runDirect(bad, 4, MSR_SF, MSR_SF);
        CHECK(invalid.outcome == ISAExecutionResult::EXCEPTION);
        CHECK(invalid.fault == PPC64FaultCategory::INVALID_ENCODING);
        CHECK(invalid.after.msr == invalid.before.msr);
        CHECK(invalid.after.nip == 0);
    }
    const DirectResult invalidGuest = runDirect(
        encodeMTMSRD(4) | (1U << 16), 4, MSR_SF, MSR_SF, true);
    CHECK(invalidGuest.outcome == ISAExecutionResult::CONTINUE);
    CHECK(invalidGuest.fault == PPC64FaultCategory::INVALID_ENCODING);
    CHECK(invalidGuest.after.nip == EXC_VECTOR_BASE + VEC_PROG);
    CHECK((invalidGuest.after.srr1 & SRR1_PROGILL) != 0);

    Memory hvMemory(0x10000);
    writeWord(hvMemory, 0, encodeMTMSRD(3));
    PPC64ISAPlugin hypervisor;
    std::string hvReason;
    CHECK(hypervisor.configureHypervisorMemory(hvMemory, 0, 0x10000, hvReason));
    auto& hvState = dynamic_cast<PPC64ISAState&>(hypervisor.getState());
    hvState.nip = 0;
    hvState.msr = MSR_SF | MSR_HV;
    hvState.gpr[3] = MSR_SF | MSR_HV | MSR_EE | MSR_RI | MSR_ME;
    CHECK(hypervisor.step(hvMemory) == ISAExecutionResult::CONTINUE);
    CHECK(hypervisor.getMSR() == (MSR_SF | MSR_HV | MSR_EE | MSR_RI));
    CHECK(hypervisor.getNIP() == 4);

    Memory secureHvMemory(0x10000);
    writeWord(secureHvMemory, 0, encodeMTMSRD(3));
    PPC64ISAPlugin secureHypervisor;
    CHECK(secureHypervisor.configureHypervisorMemory(
        secureHvMemory, 0, 0x10000, hvReason));
    auto& secureState =
        dynamic_cast<PPC64ISAState&>(secureHypervisor.getState());
    secureState.nip = 0;
    secureState.msr = MSR_SF | MSR_HV | MSR_S;
    secureState.gpr[3] = MSR_SF | MSR_IR | MSR_DR;
    CHECK(secureHypervisor.step(secureHvMemory) == ISAExecutionResult::CONTINUE);
    CHECK(secureHypervisor.getMSR() == (MSR_SF | MSR_HV | MSR_S));
    CHECK(secureHypervisor.getNIP() == 4);
}

uint64_t rootWord(uint64_t rootBase) {
    constexpr uint64_t rts = 21;
    const uint64_t encodedRTS = ((rts & 0x18ULL) << 58) |
                                ((rts & 0x7ULL) << 5);
    return (rootBase & 0x0FFFFFFFFFFFFF00ULL) | encodedRTS | 13ULL;
}

void initializeRadixTree(Memory& memory) {
    const auto writeGuestReal = [&memory](uint64_t address, uint64_t value) {
        writeDoubleword(memory, kBackingBase + address, value);
    };
    writeGuestReal(kPTable, rootWord(kRoot));
    writeGuestReal(kRoot, kPteValid | kLevel1 | 9ULL);
    writeGuestReal(kLevel1, kPteValid | kLevel2 | 9ULL);
    writeGuestReal(kLevel2, kPteValid | kPageTable | 9ULL);
}

void mapRadixPage(Memory& memory, uint64_t ea, uint64_t guestReal,
                  uint64_t access) {
    const uint64_t index = (ea >> 12) & 0x1FFULL;
    const uint64_t pte = kPteValid | kPteLeaf |
        (guestReal & 0x01FFFFFFFFFFF000ULL) | access |
        kPteReference | kPteChange;
    writeDoubleword(memory, kBackingBase + kPageTable + index * 8ULL, pte);
}

bool configureRadix(PPC64ISAPlugin& cpu, Memory& memory) {
    const PPC64RadixConfiguration config{kPTable, 5, 0, kBackingBase,
                                         kPartitionSize};
    std::string reason;
    const bool configured = cpu.configureRadixTranslation(config, memory, reason);
    if (!configured) std::printf("radix configuration failed: %s\n", reason.c_str());
    return configured;
}

void testTranslationTransitions() {
    std::puts("[phase22-radix-ir-dr-and-problem-state]");
    {
        Memory memory(0x00800000);
        initializeRadixTree(memory);
        mapRadixPage(memory, 0, kCodeGRA, kPteRead | kPteExecute);
        // A configured POWER9 radix contract also maps guest-real fetches via
        // hostBackingBase while IR=0, so place the first instruction there.
        writeWord(memory, kBackingBase, encodeMTMSRD(3));
        writeWord(memory, kBackingBase + kCodeGRA + 4, encodeAddi(9, 0, 0x1234));
        PPC64ISAPlugin cpu;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
        state.nip = 0;
        state.msr = MSR_SF;
        state.gpr[3] = MSR_SF | MSR_PR | MSR_EE;
        CHECK(configureRadix(cpu, memory));
        const auto transition = cpu.step(memory);
        if (transition != ISAExecutionResult::CONTINUE && cpu.hasFault()) {
            std::printf("IR/PR transition fault: %s\n", cpu.getLastFault().reason.c_str());
        }
        CHECK(transition == ISAExecutionResult::CONTINUE);
        CHECK(cpu.getNIP() == 4);
        CHECK(cpu.getMSR() == (MSR_SF | MSR_PR | MSR_EE | MSR_IR | MSR_DR));
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.readGPR(9) == 0x1234);
        CHECK(cpu.getNIP() == 8);
    }
    {
        Memory memory(0x00800000);
        initializeRadixTree(memory);
        mapRadixPage(memory, 0x3000, kDataGRA,
                     kPteRead | kPteWrite);
        writeWord(memory, kBackingBase, encodeMTMSRD(3));
        writeWord(memory, kBackingBase + 4, encodeLwz(4, 5, 0));
        writeWord(memory, kBackingBase + kDataGRA, 0xAABBCCDDU);
        PPC64ISAPlugin cpu;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
        state.nip = 0;
        state.msr = MSR_SF;
        state.gpr[3] = MSR_SF | MSR_DR;
        state.gpr[5] = 0x3000;
        CHECK(configureRadix(cpu, memory));
        const auto transition = cpu.step(memory);
        if (transition != ISAExecutionResult::CONTINUE && cpu.hasFault()) {
            std::printf("DR transition fault: %s\n", cpu.getLastFault().reason.c_str());
        }
        CHECK(transition == ISAExecutionResult::CONTINUE);
        CHECK((cpu.getMSR() & MSR_DR) != 0);
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.readGPR(4) == 0xAABBCCDDU);
        CHECK(cpu.getNIP() == 8);
    }
}

PPC64PlatformConfiguration linuxPlatform() {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.requestedFdtAddress = 0;
    config.maximumFdtSize = 64U * 1024U;
    return config;
}

bool prepareVM(VirtualMachine& vm, const std::vector<uint32_t>& program,
               const PPC64PlatformConfiguration&) {
    if (!vm.init()) return false;
    const auto bytes = wordsToBytes(program);
    std::string reason;
    return vm.preparePPC64Platform(bytes, kEntry, kEntry, &reason);
}

void testVMExecutionAndXICS() {
    std::puts("[phase22-vm-retirement-xics-and-rfid]");
    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.ticksPerInstruction = 1;
    VirtualMachine vm(2U << 20, 1, "PPC64", config);
    CHECK(prepareVM(vm, {encodeMTMSRD(5, true), encodeAddi(4, 0, 0xff),
                         encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)),
                         kSc1, encodeAddi(10, 10, 1), encodeMTMSRD(5, true),
                         encodeAddi(10, 10, 1)}, config));
    writeWord(vm.getMemory(), 0x500, encodeAddi(3, 0,
              static_cast<int16_t>(PPC64_H_XIRR)));
    writeWord(vm.getMemory(), 0x504, kSc1);
    writeWord(vm.getMemory(), 0x508, encodeAddi(8, 4, 0));
    writeWord(vm.getMemory(), 0x50C, encodeAddi(7, 0, 0x2000));
    writeWord(vm.getMemory(), 0x510, (36U << 26) | (8U << 21) | (7U << 16));
    writeWord(vm.getMemory(), 0x514, encodeAddi(3, 0,
              static_cast<int16_t>(PPC64_H_EOI)));
    writeWord(vm.getMemory(), 0x518, kSc1);
    writeWord(vm.getMemory(), 0x51C, kRfid);
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin != nullptr);
    if (plugin == nullptr) return;
    plugin->setMSR(MSR_SF | MSR_EE | MSR_RI);
    plugin->writeGPR(5, MSR_RI); // L=1 disables EE, preserves RI.
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(PPC64_XICS_FIRST_SOURCE,
          PPC64XICSSourceType::EDGE, 0x40, 0, 0, true, &error));
    CHECK((plugin->getMSR() & MSR_EE) != 0);
    CHECK(vm.step());
    CHECK((plugin->getMSR() & MSR_EE) == 0);
    CHECK(plugin->getNIP() == kEntry + 4);
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(vm.step()); // Establish CPPR=0xff with EE masked.
    CHECK(vm.assertPPC64InterruptSource(PPC64_XICS_FIRST_SOURCE, 0, &error));
    CHECK(vm.step()); // Pending XICS remains masked while EE=0.
    CHECK(plugin->getNIP() == kEntry + 20);
    CHECK(plugin->readGPR(10) == 1);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    plugin->writeGPR(5, MSR_EE | MSR_RI);
    CHECK(vm.step()); // MTMSRD retires before the pending interrupt is entered.
    CHECK((plugin->getMSR() & MSR_EE) != 0);
    CHECK(plugin->getNIP() == kEntry + 24);
    const uint64_t clockBeforeDelivery = vm.getPPC64Timebase();
    CHECK(vm.step());
    CHECK(plugin->getNIP() == 0x500);
    CHECK(plugin->getSRR0() == kEntry + 24);
    CHECK(plugin->getSRR1() == (MSR_SF | MSR_EE | MSR_RI));
    CHECK(plugin->getMSR() == MSR_SF);
    CHECK(vm.getPPC64Timebase() == clockBeforeDelivery);
    for (unsigned i = 0; i < 8; ++i) CHECK(vm.step());
    CHECK(plugin->getNIP() == kEntry + 24);
    CHECK((plugin->getMSR() & MSR_EE) != 0);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
}

void testDECEnableBoundary() {
    std::puts("[phase22-dec-enable-boundary]");
    PPC64PlatformConfiguration config;
    config.ticksPerInstruction = 1;
    VirtualMachine vm(2U << 20, 1, "PPC64", config);
    CHECK(prepareVM(vm, {encodeMTMSRD(5, true), encodeAddi(3, 0, 2),
                         encodeMtspr(3, kSprDEC), encodeAddi(10, 10, 1),
                         encodeAddi(10, 10, 1), encodeMTMSRD(5, true),
                         encodeAddi(10, 10, 1)}, config));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin != nullptr);
    if (plugin == nullptr) return;
    plugin->writeGPR(5, MSR_RI);
    CHECK(vm.step()); // Mask EE.
    CHECK(vm.step());
    CHECK(vm.step()); // Arm DEC=2; one deterministic virtual tick elapses.
    CHECK(vm.step()); // DEC becomes pending while EE is disabled.
    CHECK(vm.step()); // Sign bit becomes set on deterministic wrap to 0xffffffff.
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    const uint64_t timeBeforeEnable = vm.getPPC64Timebase();
    plugin->writeGPR(5, MSR_EE | MSR_RI);
    CHECK(vm.step()); // L=1 enable retires at this boundary.
    CHECK(plugin->getNIP() == kEntry + 24);
    CHECK((plugin->getMSR() & MSR_EE) != 0);
    CHECK(vm.getPPC64Timebase() == timeBeforeEnable + 1);
    const uint64_t timeBeforeDelivery = vm.getPPC64Timebase();
    CHECK(vm.step());
    CHECK(plugin->getNIP() == 0x900);
    CHECK(plugin->getSRR0() == kEntry + 24);
    CHECK(plugin->getInterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_DECREMENTER);
    CHECK(vm.getPPC64Timebase() == timeBeforeDelivery);
}

void testTwoVMAndCheckpointState() {
    std::puts("[phase22-two-vm-and-checkpoint-state]");
    Memory firstMemory(4096);
    Memory secondMemory(4096);
    writeWord(firstMemory, 0, encodeMTMSRD(3, true));
    writeWord(firstMemory, 4, encodeAddi(9, 0, 0x2468));
    writeWord(secondMemory, 0, encodeMTMSRD(3, true));
    PPC64ISAPlugin first;
    PPC64ISAPlugin second;
    auto& firstState = dynamic_cast<PPC64ISAState&>(first.getState());
    auto& secondState = dynamic_cast<PPC64ISAState&>(second.getState());
    firstState.msr = MSR_SF | MSR_RI;
    firstState.gpr[3] = MSR_SF | MSR_EE | MSR_RI;
    secondState.msr = MSR_SF | MSR_ME;
    secondState.gpr[3] = MSR_SF | MSR_RI;
    CHECK(first.step(firstMemory) == ISAExecutionResult::CONTINUE);
    CHECK(first.getMSR() == (MSR_SF | MSR_EE | MSR_RI));
    CHECK(second.getMSR() == (MSR_SF | MSR_ME));
    CHECK(second.getNIP() == 0);
    const auto checkpoint = first.serialize_state_v5();
    PPC64ISAPlugin restored;
    CHECK(restored.deserialize_state_v5(checkpoint));
    CHECK(restored.getMSR() == first.getMSR());
    CHECK(restored.getNIP() == first.getNIP());
    CHECK(restored.step(firstMemory) == ISAExecutionResult::CONTINUE);
    CHECK(restored.readGPR(9) == 0x2468);
    CHECK(restored.getNIP() == 8);

    PPC64PlatformConfiguration vmConfig;
    vmConfig.ticksPerInstruction = 1;
    VirtualMachine firstVM(2U << 20, 1, "PPC64", vmConfig);
    VirtualMachine secondVM(2U << 20, 1, "PPC64", vmConfig);
    CHECK(prepareVM(firstVM, {encodeMTMSRD(6), encodeAddi(8, 0, 0x1357)},
                    vmConfig));
    CHECK(prepareVM(secondVM, {encodeAddi(9, 0, 0x5678)}, vmConfig));
    PPC64ISAPlugin* firstVMCPU = firstVM.getPPC64ISAPlugin();
    PPC64ISAPlugin* secondVMCPU = secondVM.getPPC64ISAPlugin();
    CHECK(firstVMCPU != nullptr && secondVMCPU != nullptr);
    if (firstVMCPU != nullptr && secondVMCPU != nullptr) {
        firstVMCPU->setMSR(MSR_SF | MSR_RI);
        firstVMCPU->writeGPR(6, MSR_SF | MSR_EE | MSR_RI);
        secondVMCPU->setMSR(MSR_SF | MSR_ME);
        secondVMCPU->writeGPR(6, MSR_SF | MSR_RI);
        const uint64_t secondVMNIP = secondVMCPU->getNIP();
        const uint64_t secondVMGPR6 = secondVMCPU->readGPR(6);
        CHECK(firstVM.step());
        CHECK(firstVMCPU->getMSR() == (MSR_SF | MSR_EE | MSR_RI));
        CHECK(firstVMCPU->getNIP() == kEntry + 4);
        CHECK(secondVMCPU->getMSR() == (MSR_SF | MSR_ME));
        CHECK(secondVMCPU->getNIP() == secondVMNIP);
        CHECK(secondVMCPU->readGPR(6) == secondVMGPR6);
    }
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input.good() || input.eof();
}

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path outputRoot() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE22_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase22)";
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::string sha256File(const std::filesystem::path& input,
                       const std::filesystem::path& output) {
    const std::string command =
        "python -c \"import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())\" " +
        quote(input) + " > " + quote(output) + " 2>&1";
    if (std::system(command.c_str()) != 0) return {};
    std::ifstream hashFile(output);
    std::string hash;
    std::getline(hashFile, hash);
    return hash;
}

bool verifyArtifact(const std::filesystem::path& kernel,
                    const std::filesystem::path& output) {
    const std::filesystem::path source = PPC64_PHASE22_SOURCE_DIR;
    const auto manifest = source / "docs" / "ppc64" /
        "PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json";
    const auto artifactRoot = kernel.parent_path().parent_path();
    const std::string command = "python " +
        quote(source / "tools" / "ppc64_linux_artifact_verify.py") +
        " --kernel " + quote(kernel) + " --manifest " + quote(manifest) +
        " --artifact-root " + quote(artifactRoot) + " > " + quote(output) +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

struct ProbeResult {
    bool prepared = false;
    uint64_t stopNip = 0;
    uint64_t retired = 0;
    uint64_t attempted = 0;
    std::vector<uint8_t> fdt;
    std::string trace;
    std::string frontier;
    std::string memoryPlan;
    std::string entryRegisters;
    std::string stopReason;
};

bool verifyLoad(const VirtualMachine& vm, const PPC64ElfImage& elf) {
    const auto* memory = dynamic_cast<const Memory*>(&vm.getMemory());
    if (memory == nullptr) return false;
    std::array<uint8_t, 4096> block{};
    for (const auto& segment : elf.loadSegments) {
        for (uint64_t done = 0; done < segment.fileSize;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                block.size(), segment.fileSize - done));
            memory->ReadGuestReal(segment.physicalAddress + done,
                                  block.data(), count);
            if (!std::equal(block.begin(), block.begin() +
                            static_cast<std::ptrdiff_t>(count),
                            segment.fileBytes.begin() +
                            static_cast<std::ptrdiff_t>(done))) return false;
            done += count;
        }
        for (uint64_t done = segment.fileSize; done < segment.memorySize;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                block.size(), segment.memorySize - done));
            memory->ReadGuestReal(segment.physicalAddress + done,
                                  block.data(), count);
            if (!std::all_of(block.begin(), block.begin() +
                             static_cast<std::ptrdiff_t>(count),
                             [](uint8_t byte) { return byte == 0; })) return false;
            done += count;
        }
    }
    return true;
}

ProbeResult runAuthenticProbe(const std::vector<uint8_t>& kernel) {
    ProbeResult result;
    VirtualMachine vm(static_cast<size_t>(kRamSize), 1, "PPC64", linuxPlatform());
    if (!vm.init()) {
        result.stopReason = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    std::string error;
    if (!loadPPC64Elf(vm, kernel, elf, error)) {
        result.stopReason = "ELF load failed: " + error;
        return result;
    }
    result.prepared = true;
    result.fdt = vm.getPPC64FDT();
    if (!verifyLoad(vm, elf)) {
        result.prepared = false;
        result.stopReason = "PT_LOAD or BSS readback mismatch";
        return result;
    }
    const auto platform = vm.getPPC64PlatformSnapshot();
    const auto& layout = platform.memoryLayout;
    std::ostringstream plan;
    plan << "ram=[0x" << std::hex << layout.ramBase << ",0x"
         << layout.ramBase + layout.ramSize << ")\nkernel=[0x"
         << layout.imageBase << ",0x" << layout.imageBase + layout.imageSize
         << ")\nentry=0x" << layout.entryAddress << "\nfdt=[0x"
         << layout.fdtBase << ",0x" << layout.fdtBase + layout.fdtSize << ")\n";
    result.memoryPlan = plan.str();
    auto registers = vm.getPPC64RegisterView();
    std::ostringstream entry;
    entry << "nip=0x" << std::hex << registers.nip << " msr=0x" << registers.msr
          << " r3=0x" << registers.gpr[3] << " r4=0x" << registers.gpr[4]
          << " r5=0x" << registers.gpr[5] << " cr=0x" << registers.cr
          << " xer=0x" << registers.xer << "\n";
    result.entryRegisters = entry.str();

    std::ostringstream trace;
    trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
          << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
          << "entry_mode=POWER9_PSERIES_FOUNDATION direct-FDT; RTAS=disabled\n"
          << "initial=" << result.entryRegisters;
    constexpr uint64_t instructionBudget = 32;
    uint64_t lastNip = registers.nip;
    uint32_t lastWord = 0;
    for (uint64_t attempt = 0; attempt < instructionBudget; ++attempt) {
        const auto before = vm.getPPC64RegisterView();
        std::array<uint8_t, 4> bytes{};
        vm.getMemory().Read(before.nip, bytes.data(), bytes.size());
        const uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
        const std::string decoded = vm.getPPC64ISAPlugin()->disassemble(
            vm.getMemory(), before.nip);
        const bool advanced = vm.step();
        const auto after = vm.getPPC64RegisterView();
        ++result.attempted;
        if (advanced) {
            ++result.retired;
            lastNip = before.nip;
            lastWord = word;
        }
        trace << "step=" << std::dec << attempt << " nip=0x" << std::hex
              << before.nip << " raw=0x" << std::setw(8) << std::setfill('0')
              << word << " decoded=" << decoded << " retired="
              << (advanced ? 1 : 0) << " next_nip=0x" << after.nip
              << " r11_before=0x" << before.gpr[11]
              << " r11_after=0x" << after.gpr[11]
              << " r12_before=0x" << before.gpr[12]
              << " r12_after=0x" << after.gpr[12]
              << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
              << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
              << " msr_before=0x" << before.msr << " msr_after=0x"
              << after.msr << "\n";
        if (!advanced) {
            const auto* plugin = vm.getPPC64ISAPlugin();
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : after.nip;
            result.stopReason = plugin->getLastExecutionDiagnostic();
            std::ostringstream frontier;
            frontier << "category="
                     << (plugin->hasFault()
                             ? std::to_string(static_cast<unsigned>(
                                   plugin->getLastFault().category))
                             : "bounded-stop")
                     << "\nstop_nip=0x" << std::hex << result.stopNip
                     << "\nraw_instruction=0x" << std::setw(8)
                     << std::setfill('0') << word << "\ndecoder_text="
                     << decoded << "\nstop_reason=" << result.stopReason
                     << "\npre_msr=0x" << before.msr << "\ncr=0x" << before.cr
                     << "\nxer=0x" << before.xer << "\nretired="
                     << std::dec << result.retired << "\nattempted="
                     << result.attempted << "\npre_gprs=";
            for (size_t reg = 0; reg < 32; ++reg) {
                frontier << (reg == 0 ? "" : ",") << "r" << reg << "=0x"
                         << std::hex << before.gpr[reg];
            }
            frontier << "\nlast_successful_nip=0x" << std::hex << lastNip
                     << "\nlast_successful_raw=0x" << std::setw(8)
                     << std::setfill('0') << lastWord << "\nfinal_msr=0x"
                     << after.msr << "\nfinal_nip=0x" << after.nip
                     << "\nvirtual_time=0x" << vm.getPPC64Timebase() << "\n";
            result.frontier = frontier.str();
            break;
        }
    }
    if (result.stopReason.empty()) {
        result.stopNip = vm.getPPC64RegisterView().nip;
        result.stopReason = "bounded execution limit (32 production VM steps)";
        result.frontier = "category=bounded-execution-limit\nstep_budget=32\n";
    }
    trace << "retired=" << std::dec << result.retired
          << " attempted=" << result.attempted << "\nstop_nip=0x"
          << std::hex << result.stopNip << "\nstop_reason=" << result.stopReason
          << "\n";
    result.trace = trace.str();
    return result;
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output << text;
    return output.good();
}

void testAuthenticLinuxReplay() {
    std::puts("[phase22-authentic-linux-replay]");
    const auto kernelFile = kernelPath();
    std::vector<uint8_t> kernel;
    CHECK(readFile(kernelFile, kernel));
    if (kernel.empty()) return;
    CHECK(kernel.size() == kKernelSize);

    const auto root = outputRoot();
    std::filesystem::create_directories(root);
    auto runId = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto runDir = root / ("run-" + std::to_string(runId));
    while (std::filesystem::exists(runDir)) {
        runDir = root / ("run-" + std::to_string(++runId));
    }
    std::filesystem::create_directories(runDir);
    CHECK(verifyArtifact(kernelFile, runDir / "artifact-verification.txt"));

    PPC64ElfImage elf;
    std::string parseReason;
    CHECK(parsePPC64Elf64BE(kernel, elf, parseReason));
    CHECK(elf.physicalEntry == 0);
    CHECK(elf.physicalBase == 0);
    CHECK(elf.physicalEnd == kKernelEnd);
    CHECK(elf.loadSegments.size() == 1);
    if (elf.loadSegments.empty()) return;
    CHECK(elf.loadSegments[0].fileSize == 0x028865e4ULL);
    CHECK(elf.loadSegments[0].memorySize == kKernelEnd);

    const ProbeResult first = runAuthenticProbe(kernel);
    const ProbeResult second = runAuthenticProbe(kernel);
    CHECK(first.prepared && second.prepared);
    CHECK(first.trace == second.trace);
    CHECK(first.frontier == second.frontier);
    CHECK(first.fdt == second.fdt);
    CHECK(first.memoryPlan == second.memoryPlan);
    CHECK(first.entryRegisters == second.entryRegisters);
    CHECK(first.retired == second.retired);
    CHECK(first.attempted == second.attempted);
    CHECK(first.stopNip == second.stopNip);
    CHECK(first.trace.find("nip=0x0 raw=0x08000048 decoded=tdi 0,r0,72 retired=1") !=
          std::string::npos);
    CHECK(first.trace.find("nip=0xd320 raw=0x798c07c6 decoded=rldicr r12,r12,32,31 retired=1") !=
          std::string::npos);
    CHECK(first.trace.find("nip=0xd324") != std::string::npos);
    CHECK(first.trace.find("nip=0xd328 raw=0x7d600164 decoded=mtmsrd r11 retired=1 next_nip=0xd32c") !=
          std::string::npos);
    CHECK(first.trace.find("msr_before=0x8000000000000000 msr_after=0x8000000000000000") !=
          std::string::npos);
    CHECK(first.retired >= 9);
    CHECK(first.stopNip != 0xD328);

    const auto fdtFile = runDir / "entry-fdt.dtb";
    {
        std::ofstream fdt(fdtFile, std::ios::binary | std::ios::out);
        fdt.write(reinterpret_cast<const char*>(first.fdt.data()),
                  static_cast<std::streamsize>(first.fdt.size()));
        CHECK(fdt.good());
    }
    CHECK(writeText(runDir / "bounded-linux-trace.txt", first.trace));
    CHECK(writeText(runDir / "frontier-summary.txt", first.frontier));
    CHECK(writeText(runDir / "physical-memory-plan.txt", first.memoryPlan));
    CHECK(writeText(runDir / "entry-registers.txt", first.entryRegisters));
    CHECK(writeText(runDir / "replay-comparison.txt",
        "fresh_vm_runs=2\ntrace_identical=true\nfdt_identical=true\n"
        "memory_plan_identical=true\nentry_registers_identical=true\n"
        "retired_instructions=" + std::to_string(first.retired) +
        "\nattempted_instructions=" + std::to_string(first.attempted) +
        "\nnext_frontier_nip=0x" + [&] { std::ostringstream o; o << std::hex << first.stopNip; return o.str(); }() +
        "\n"));
    CHECK(writeText(runDir / "instruction-decode-evidence.txt",
        "original_frontier_nip=0xd328\nraw_word=0x7d600164\n"
        "primary=(word>>26)&0x3f=31\nrs=(word>>21)&0x1f=11\n"
        "xo=(word>>1)&0x3ff=178\nL=(word>>15)&1=0\n"
        "reserved_RA=(word>>16)&0x1f=0\nreserved_RB=(word>>11)&0xf=0\nRc=0\n"
        "decoded=mtmsrd r11\npost_msr=0x8000000000000000\npost_nip=0xd32c\n"));
    const std::string kernelHash = sha256File(kernelFile,
                                               runDir / "kernel-sha256.txt");
    const std::string fdtHash = sha256File(fdtFile, runDir / "fdt-sha256.txt");
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    std::printf("Phase22 Linux artifacts: %s\n", runDir.string().c_str());
    std::printf("Phase22 Linux replay: retired=%llu stop=0x%llx (%s)\n",
        static_cast<unsigned long long>(first.retired),
        static_cast<unsigned long long>(first.stopNip), first.stopReason.c_str());
}

} // namespace

int main() {
    std::puts("PPC64 Phase 22 MTMSRD acceptance suite");
    testDecodeAndReservedFields();
    testGeneralAndLimitedMSRUpdates();
    testGPR0PrivilegeAndRejectedTransitions();
    testTranslationTransitions();
    testVMExecutionAndXICS();
    testDECEnableBoundary();
    testTwoVMAndCheckpointState();
    testAuthenticLinuxReplay();
    std::printf("Phase22 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
