// PPC64 Phase 12 production DEC and asynchronous interrupt acceptance.

#include "VirtualMachine.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "VMSnapshot.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
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
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kCounter = 0x2000;
constexpr uint64_t kMessage = 0x3000;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kSc1 = 0x44000022U;
constexpr uint64_t kSprDEC = 22;
constexpr uint64_t kMSREE = MSR_EE;
constexpr uint64_t kMSRPR = MSR_PR;

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeAddis(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (15U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeOri(uint8_t rs, uint8_t ra, uint16_t immediate) {
    return (24U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) | immediate;
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(reg) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t spr) {
    return encodeXfx(rt, spr, 339);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, 467);
}

uint32_t encodeMfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}

uint32_t encodeMtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

std::vector<uint8_t> image(const std::vector<uint32_t>& words) {
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

PPC64PlatformConfiguration config(uint64_t ticks = 1,
                                  uint64_t initial = 0,
                                  bool terminal = false) {
    PPC64PlatformConfiguration value;
    value.timebaseFrequency = 512'000'000ULL;
    value.ticksPerInstruction = ticks;
    value.initialTimebase = initial;
    value.virtualTerminalEnabled = terminal;
    return value;
}

std::vector<uint32_t> withTail(std::vector<uint32_t> words) {
    for (size_t i = 0; i < 48; ++i) {
        words.push_back(encodeAddi(10, 10, 1));
    }
    return words;
}

bool prepare(VirtualMachine& vm, std::vector<uint32_t> words) {
    if (words.empty()) words.push_back(encodeOri(0, 0, 0));
    const auto guestImage = image(withTail(std::move(words)));
    if (!vm.init()) {
        std::printf("Phase 12 VM init failed\n");
        return false;
    }
    std::string error;
    if (!vm.preparePPC64Platform(guestImage, kEntry, kEntry, &error)) {
        std::printf("Phase 12 platform preparation failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        writeWord(memory, address + i * 4U, words[i]);
    }
}

uint32_t readBE32(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

bool step(VirtualMachine& vm) {
    const bool result = vm.step();
    CHECK(result);
    return result;
}

bool steps(VirtualMachine& vm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!step(vm)) return false;
    }
    return true;
}

std::vector<uint32_t> timerProgram(uint16_t decValue = 8,
                                  bool problemState = false) {
    std::vector<uint32_t> words{
        encodeAddi(3, 0, static_cast<int16_t>(decValue)),
        encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeMfmsr(4),
        encodeOri(4, 4, static_cast<uint16_t>(kMSREE |
                                              (problemState ? kMSRPR : 0))),
        encodeMtmsr(4)};
    for (size_t i = 0; i < 20; ++i) {
        words.push_back(encodeAddi(10, 10, 1));
    }
    return words;
}

std::vector<uint32_t> counterHandler(bool rearm, uint64_t counter = kCounter) {
    std::vector<uint32_t> words{
        encodeAddi(8, 0, static_cast<int16_t>(counter)),
        encodeD(32, 9, 8, 0),
        encodeAddi(9, 9, 1),
        encodeD(36, 9, 8, 0)};
    if (rearm) {
        words.push_back(encodeAddi(7, 0, 100));
        words.push_back(encodeMtspr(7, static_cast<uint16_t>(kSprDEC)));
    }
    words.push_back(kRfid);
    return words;
}

void programA_DECReadWriteAndWidth() {
    std::printf("[phase12-A-dec-read-write]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeAddis(3, 0, 0x1234), encodeOri(3, 3, 0x5678),
        encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeMfspr(4, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(3, 0, 0), encodeMtspr(3, static_cast<uint16_t>(kSprDEC))}));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin != nullptr);
    CHECK(steps(vm, 3));
    CHECK(plugin->getDEC() == 0x12345677U);
    CHECK(plugin->getInterruptSnapshot().decrementerPending == false);
    CHECK(step(vm));
    CHECK(plugin->readGPR(4) == 0x12345677U);
    CHECK(plugin->getDEC() == 0x12345676U);
    CHECK(plugin->serialize_state().size() == STATE_SIZE_V5);
    CHECK(steps(vm, 2));
    CHECK(plugin->getDEC() == 0xffffffffU);
    CHECK(plugin->getInterruptSnapshot().decrementerPending);

    PPC64ISAState serialized;
    auto bytes = plugin->serialize_state();
    serialized.deserializeV5(bytes.data());
    CHECK(serialized.dec == 0xffffffffU && serialized.decArmed);
    CHECK((plugin->getDEC() & 0x80000000U) != 0U);
    auto impossibleCPUState = bytes;
    impossibleCPUState[STATE_SIZE_V4 + 4U] = 0;
    bool rejectedImpossibleCPUState = false;
    try {
        serialized.deserializeV5(impossibleCPUState.data());
    } catch (const std::invalid_argument&) {
        rejectedImpossibleCPUState = true;
    }
    CHECK(rejectedImpossibleCPUState);

    VirtualMachine wrapped(kRamSize, 1, "PPC64", config(0xffffffffULL));
    CHECK(prepare(wrapped, {
        encodeAddi(3, 0, 1), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeMfspr(4, static_cast<uint16_t>(kSprDEC))}));
    CHECK(steps(wrapped, 2));
    CHECK(wrapped.getPPC64ISAPlugin()->getDEC() == 2U);
    CHECK(!wrapped.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(step(wrapped));
    CHECK(wrapped.getPPC64ISAPlugin()->readGPR(4) == 2U);
    CHECK(wrapped.getPPC64ISAPlugin()->getDEC() == 3U);

    // Problem-state access to DEC is a guest Program exception, not an
    // unprivileged read. The only supported LPCR profile keeps LD disabled.
    VirtualMachine privilege(kRamSize, 1, "PPC64", config());
    CHECK(prepare(privilege, {
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_PR)),
        encodeMtmsr(4), encodeMfspr(5, static_cast<uint16_t>(kSprDEC))}));
    privilege.getPPC64ISAPlugin()->setGuestDeliveryEnabled(true);
    CHECK(steps(privilege, 3));
    CHECK(step(privilege));
    CHECK(privilege.getPPC64ISAPlugin()->getNIP() == 0x700U);
    CHECK(privilege.getPPC64ISAPlugin()->getSRR0() == kEntry + 12U);

    VirtualMachine unsupported(kRamSize, 1, "PPC64", config());
    CHECK(prepare(unsupported, {
        encodeAddi(3, 0, 1), encodeMtspr(3, 318)})); // LPCR: no host profile API
    CHECK(step(unsupported));
    CHECK(!unsupported.step()); // guest LPCR/LD configuration is unsupported
    CHECK(unsupported.getState() == VMState::ERROR);
}

void programBAndC_TimerDeliveryAndRealHandler() {
    std::printf("[phase12-B-C-timer-handler-rfid]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, timerProgram()));
    writeWords(vm.getMemory(), 0x900, counterHandler(true));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(steps(vm, 10));
    CHECK(plugin->getNIP() == kEntry + 40U);
    CHECK(plugin->getDEC() == 0xffffffffU);
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    CHECK(plugin->readGPR(10) == 5U);
    const uint64_t cyclesBeforeEntry = vm.getCyclesExecuted();
    const uint64_t clockBeforeEntry = vm.getPPC64Timebase();
    CHECK(step(vm));
    CHECK(vm.getCyclesExecuted() == cyclesBeforeEntry);
    CHECK(vm.getPPC64Timebase() == clockBeforeEntry);
    CHECK(plugin->getNIP() == 0x900U);
    CHECK(plugin->getSRR0() == kEntry + 40U);
    CHECK(plugin->getSRR1() == (MSR_SF | MSR_EE));
    CHECK(plugin->getMSR() == MSR_SF);
    CHECK(plugin->readGPR(10) == 5U);
    CHECK(plugin->getInterruptSnapshot().lastDeliveryVector == 0x900U);
    CHECK(plugin->getInterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_DECREMENTER);
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(plugin->getNIP() == kEntry + 40U);
    CHECK(plugin->getMSR() == (MSR_SF | MSR_EE));
    CHECK(plugin->getDEC() == 98U);
    CHECK(!plugin->getInterruptSnapshot().decrementerPending);
    CHECK(step(vm));
    CHECK(plugin->getNIP() == kEntry + 44U);
    CHECK(plugin->readGPR(10) == 6U);

    VirtualMachine problem(kRamSize, 1, "PPC64", config());
    CHECK(prepare(problem, timerProgram(8, true)));
    writeWords(problem.getMemory(), 0x900, counterHandler(true));
    auto* problemPlugin = problem.getPPC64ISAPlugin();
    CHECK(steps(problem, 10));
    CHECK(problemPlugin->getNIP() == kEntry + 40U);
    CHECK(problemPlugin->getMSR() == (MSR_SF | MSR_EE | MSR_PR));
    CHECK(step(problem));
    CHECK(problemPlugin->getSRR0() == kEntry + 40U);
    CHECK(problemPlugin->getSRR1() == (MSR_SF | MSR_EE | MSR_PR));
    CHECK(steps(problem, 7));
    CHECK(problemPlugin->getNIP() == kEntry + 40U);
    CHECK(problemPlugin->getMSR() == (MSR_SF | MSR_EE | MSR_PR));
    CHECK(problem.readGR(10) == 5U);
    CHECK(step(problem));
    CHECK(problem.readGR(10) == 6U);

    VirtualMachine boundedRun(kRamSize, 1, "PPC64", config());
    CHECK(prepare(boundedRun, timerProgram()));
    writeWords(boundedRun.getMemory(), 0x900, counterHandler(true));
    CHECK(steps(boundedRun, 10));
    const uint64_t runCyclesBefore = boundedRun.getCyclesExecuted();
    CHECK(boundedRun.run(1) == 1U); // zero-cycle entry, then one handler instruction
    CHECK(boundedRun.getCyclesExecuted() == runCyclesBefore + 1U);
    CHECK(boundedRun.getPPC64ISAPlugin()->getNIP() == 0x904U);
    CHECK(boundedRun.getPPC64ISAPlugin()->getSRR0() == kEntry + 40U);
}

void programD_MaskedExpiryAndUnmask() {
    std::printf("[phase12-D-mask-unmask]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeAddi(3, 0, 3), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(10, 10, 1), encodeAddi(10, 10, 1),
        encodeAddi(10, 10, 1), encodeMfmsr(4),
        encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(4)}));
    writeWords(vm.getMemory(), 0x900, counterHandler(true));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(steps(vm, 5));
    CHECK(plugin->getNIP() == kEntry + 20U);
    CHECK(plugin->getDEC() == 0xffffffffU);
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    CHECK((plugin->getMSR() & MSR_EE) == 0);
    CHECK(steps(vm, 3));
    CHECK(plugin->getNIP() == kEntry + 32U);
    CHECK((plugin->getMSR() & MSR_EE) != 0);
    const uint64_t before = vm.getCyclesExecuted();
    CHECK(step(vm));
    CHECK(vm.getCyclesExecuted() == before);
    CHECK(plugin->getNIP() == 0x900U);
    CHECK(plugin->getSRR0() == kEntry + 32U);
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(plugin->getNIP() == kEntry + 32U);
}

void programE_ExternalInterruptInjection() {
    std::printf("[phase12-E-external-input]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(4), encodeAddi(10, 10, 1)}));
    writeWords(vm.getMemory(), 0x500, counterHandler(false));
    auto* plugin = vm.getPPC64ISAPlugin();
    std::string error;
    CHECK(!vm.assertPPC64ExternalInterrupt(1, &error));
    CHECK(steps(vm, 3));
    const uint64_t cycles = vm.getCyclesExecuted();
    CHECK(vm.assertPPC64ExternalInterrupt(0, &error));
    CHECK(vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(vm.getPPC64InterruptSnapshot().externalAsserted);
    const VMStateSnapshot assertedSource = vm.captureVMSnapshot();
    CHECK(vm.deassertPPC64ExternalInterrupt(0, &error));
    CHECK(!vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(vm.restoreVMSnapshot(assertedSource));
    plugin = vm.getPPC64ISAPlugin();
    CHECK(vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(vm.getPPC64InterruptSnapshot().externalAsserted);
    CHECK(step(vm));
    CHECK(vm.getCyclesExecuted() == cycles);
    CHECK(plugin->getNIP() == 0x500U);
    CHECK(plugin->getSRR0() == kEntry + 12U);
    CHECK(plugin->getSRR1() == (MSR_SF | MSR_EE));
    CHECK(plugin->getMSR() == MSR_SF);
    CHECK(plugin->getInterruptSnapshot().lastDeliveryVector == 0x500U);
    CHECK(steps(vm, 4));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(vm.deassertPPC64ExternalInterrupt(0, &error));
    CHECK(!vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(step(vm)); // authentic rfid
    CHECK(plugin->getNIP() == kEntry + 12U);
    CHECK(plugin->getMSR() == (MSR_SF | MSR_EE));
    CHECK(step(vm));
    CHECK(plugin->getNIP() == kEntry + 16U);
    CHECK(plugin->readGPR(10) == 1U);

    CHECK(vm.assertPPC64ExternalInterrupt(0, &error));
    vm.halt();
    CHECK(!vm.assertPPC64ExternalInterrupt(0, &error));
    CHECK(!vm.step());
    CHECK(vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(vm.deassertPPC64ExternalInterrupt(0, &error));

    VirtualMachine uninitialized(kRamSize, 1, "PPC64", config());
    CHECK(!uninitialized.assertPPC64ExternalInterrupt(0, &error));
}

void programF_SimultaneousPriorityAndRearmIndependence() {
    std::printf("[phase12-F-simultaneous-priority]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeAddi(3, 0, 0), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(4), encodeAddi(10, 10, 1)}));
    writeWords(vm.getMemory(), 0x500, counterHandler(false, kCounter + 4U));
    writeWords(vm.getMemory(), 0x900, counterHandler(true));
    std::string error;
    CHECK(vm.assertPPC64ExternalInterrupt(0, &error));
    CHECK(steps(vm, 5));
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(vm.getPPC64InterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_EXTERNAL);
    CHECK(steps(vm, 4));
    CHECK(vm.deassertPPC64ExternalInterrupt(0, &error));
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(!vm.getPPC64InterruptSnapshot().externalPending);
    CHECK(step(vm)); // handler rfid, then DEC is eligible at the next boundary
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 20U);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(vm.getPPC64InterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_DECREMENTER);
    CHECK(vm.getPPC64InterruptSnapshot().deliveryCount == 2U);
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(readBE32(vm.getMemory(), kCounter + 4U) == 1U);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 20U);
}

void programG_PauseSnapshotAndCheckpoint() {
    std::printf("[phase12-G-pause-checkpoint]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeAddi(3, 0, 4), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(10, 10, 1), encodeAddi(10, 10, 1), encodeMfmsr(4),
        encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(4)}));
    writeWords(vm.getMemory(), 0x900, counterHandler(true));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(steps(vm, 4)); // DEC reads one tick; two main ticks remain to expiry
    CHECK(plugin->getDEC() == 1U);
    const VMStateSnapshot beforeExpiry = vm.captureVMSnapshot();
    const uint64_t frozenTB = vm.getPPC64Timebase();
    CHECK(vm.pause());
    const auto path = std::filesystem::temp_directory_path() /
                      "guidexos-phase12-dec.gxcp";
    const auto corruptPath = std::filesystem::temp_directory_path() /
                             "guidexos-phase12-dec-corrupt.gxcp";
    const auto beforePath = std::filesystem::temp_directory_path() /
                            "guidexos-phase12-dec-before.gxcp";
    const auto handlerPath = std::filesystem::temp_directory_path() /
                             "guidexos-phase12-dec-handler.gxcp";
    const auto returnedPath = std::filesystem::temp_directory_path() /
                              "guidexos-phase12-dec-returned.gxcp";
    std::string error;
    CHECK(vm.writeDiagnosticCheckpoint(beforePath.string(), "phase12-before",
                                       &error));

    VirtualMachine other(kRamSize, 1, "PPC64", config(7, 200));
    CHECK(prepare(other, {encodeAddi(10, 10, 1)}));
    CHECK(steps(other, 5));
    CHECK(vm.getPPC64Timebase() == frozenTB);
    CHECK(vm.readDiagnosticCheckpoint(beforePath.string(), "phase12-before",
                                      &error));
    plugin = vm.getPPC64ISAPlugin();
    CHECK(vm.getPPC64Timebase() == frozenTB);
    CHECK(vm.getState() == VMState::PAUSED);
    CHECK(vm.resume());
    CHECK(steps(vm, 2));
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    CHECK((plugin->getMSR() & MSR_EE) == 0);
    CHECK(vm.writeDiagnosticCheckpoint(path.string(), "phase12-dec", &error));
    std::ifstream in(path, std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(file.size() > 32U);
    if (!file.empty()) file.back() ^= 0x01;
    {
        std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
        out.write(file.data(), static_cast<std::streamsize>(file.size()));
    }
    CHECK(!vm.readDiagnosticCheckpoint(corruptPath.string(), "phase12-dec",
                                       &error));
    CHECK(vm.readDiagnosticCheckpoint(path.string(), "phase12-dec", &error));
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK((vm.getPPC64ISAPlugin()->getMSR() & MSR_EE) == 0);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(corruptPath, ignored);
    plugin = vm.getPPC64ISAPlugin();
    CHECK(steps(vm, 1)); // guest code unmasks the saved pending level
    CHECK(step(vm));
    CHECK(plugin->getNIP() == 0x900U);
    const VMStateSnapshot inHandler = vm.captureVMSnapshot();
    CHECK(vm.writeDiagnosticCheckpoint(handlerPath.string(), "phase12-handler",
                                       &error));
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(plugin->getNIP() == kEntry + 28U);
    CHECK(plugin->getDEC() == 98U);
    const VMStateSnapshot afterReturn = vm.captureVMSnapshot();
    CHECK(vm.writeDiagnosticCheckpoint(returnedPath.string(), "phase12-returned",
                                       &error));
    CHECK(vm.readDiagnosticCheckpoint(handlerPath.string(), "phase12-handler",
                                      &error));
    plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin->getNIP() == 0x900U);
    CHECK(readBE32(vm.getMemory(), kCounter) == 0U);
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(vm.readDiagnosticCheckpoint(returnedPath.string(), "phase12-returned",
                                      &error));
    plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin->getNIP() == kEntry + 28U);
    CHECK(plugin->getDEC() == 98U);
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(vm.restoreVMSnapshot(beforeExpiry));
    plugin = vm.getPPC64ISAPlugin();
    CHECK(vm.getPPC64Timebase() == frozenTB);
    CHECK(steps(vm, 2));
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    CHECK(steps(vm, 1));
    CHECK(step(vm));
    CHECK(plugin->getNIP() == 0x900U);

    VMStateSnapshot impossible = vm.captureVMSnapshot();
    impossible.ppc64Interrupts.decrementerPending = false;
    CHECK(!vm.restoreVMSnapshot(impossible));
    std::filesystem::remove(beforePath, ignored);
    std::filesystem::remove(handlerPath, ignored);
    std::filesystem::remove(returnedPath, ignored);
}

void programH_TwoVMIsolationAndLPIDBinding() {
    std::printf("[phase12-H-vm-and-lpid-isolation]\n");
    VirtualMachine first(kRamSize, 1, "PPC64", config(2, 17));
    VirtualMachine second(kRamSize, 1, "PPC64", config(5, 91));
    CHECK(prepare(first, timerProgram(10)));
    CHECK(prepare(second, timerProgram(20)));
    writeWords(first.getMemory(), 0x500, counterHandler(false, kCounter + 4U));
    writeWords(first.getMemory(), 0x900, counterHandler(true));
    writeWords(second.getMemory(), 0x900, counterHandler(true));
    std::string error;
    CHECK(first.assertPPC64ExternalInterrupt(0, &error));
    CHECK(!second.getPPC64InterruptSnapshot().externalPending);
    CHECK(first.getPPC64Timebase() == 17U);
    CHECK(second.getPPC64Timebase() == 91U);
    CHECK(steps(first, 5));
    CHECK(steps(second, 5));
    CHECK(first.getPPC64Timebase() == 27U);
    CHECK(second.getPPC64Timebase() == 116U);
    CHECK(first.getPPC64ISAPlugin()->getDEC() == 2U);
    CHECK(second.getPPC64ISAPlugin()->getDEC() == 0U);
    CHECK(first.step());
    CHECK(first.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(first.getPPC64InterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_EXTERNAL);
    CHECK(second.step()); // executes main code; the second VM is not injected
    CHECK(second.getPPC64ISAPlugin()->getNIP() == kEntry + 24U);
    CHECK(second.getPPC64InterruptSnapshot().deliveryCount == 0U);
    CHECK(second.step());
    CHECK(second.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(second.getPPC64InterruptSnapshot().lastDeliverySource ==
          PPC64_INTERRUPT_SOURCE_DECREMENTER);
    CHECK(steps(first, 4));
    CHECK(first.deassertPPC64ExternalInterrupt(0, &error));
    CHECK(first.step()); // external handler rfid
    CHECK(first.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(first.step());
    CHECK(first.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(steps(first, 7));
    CHECK(steps(second, 7));
    CHECK(readBE32(first.getMemory(), kCounter) == 1U);
    CHECK(readBE32(first.getMemory(), kCounter + 4U) == 1U);
    CHECK(readBE32(second.getMemory(), kCounter) == 1U);
    CHECK(!second.getPPC64InterruptSnapshot().externalAsserted);
    CHECK(first.getPPC64InterruptSnapshot().deliveryCount == 2U);
    CHECK(second.getPPC64InterruptSnapshot().deliveryCount == 1U);
    CHECK(!first.assertPPC64ExternalInterrupt(1, &error));
    CHECK(first.getPPC64InterruptSnapshot().externalLPID == 0U);
    CHECK(!second.getPPC64InterruptSnapshot().externalAsserted);
    CHECK(first.deassertPPC64ExternalInterrupt(0, &error));
    CHECK(!first.getPPC64InterruptSnapshot().externalPending);
    CHECK(first.assertPPC64ExternalInterrupt(0, &error));
    CHECK(first.reset());
    CHECK(first.getPPC64Timebase() == 17U);
    CHECK(first.getPPC64ISAPlugin()->getDEC() == 0U);
    CHECK(!first.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(!first.getPPC64InterruptSnapshot().externalPending);
}

void programI_GuestGeneratedTerminalMessage() {
    std::printf("[phase12-I-guest-terminal-proof]\n");
    const std::string message = "PPC64 timer interrupt handled\n";
    VirtualMachine vm(kRamSize, 1, "PPC64", config(1, 0, true));
    CHECK(prepare(vm, timerProgram()));
    std::vector<uint8_t> messageBytes(message.begin(), message.end());
    vm.getMemory().Write(kMessage, messageBytes.data(), messageBytes.size());

    std::vector<uint32_t> handler{
        encodeAddi(8, 0, static_cast<int16_t>(kCounter)),
        encodeD(32, 9, 8, 0), encodeAddi(9, 9, 1), encodeD(36, 9, 8, 0),
        encodeAddi(8, 0, static_cast<int16_t>(kMessage)),
        encodeDS(58, 6, 8, 0), encodeDS(58, 7, 8, 8),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_PUT_TERM_CHAR)),
        encodeAddis(4, 0, 0x3000), encodeAddi(5, 0, 16), kSc1,
        encodeAddi(8, 8, 16), encodeDS(58, 6, 8, 0),
        encodeDS(58, 7, 8, 8),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_PUT_TERM_CHAR)),
        encodeAddis(4, 0, 0x3000),
        encodeAddi(5, 0, static_cast<int16_t>(message.size() - 16U)), kSc1,
        encodeAddi(7, 0, 100),
        encodeMtspr(7, static_cast<uint16_t>(kSprDEC)), kRfid};
    writeWords(vm.getMemory(), 0x900, handler);
    CHECK(steps(vm, 10));
    CHECK(step(vm));
    CHECK(steps(vm, handler.size()));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    const auto output = vm.readPPC64TerminalOutput();
    CHECK(std::string(output.begin(), output.end()) == message);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 40U);
    CHECK(vm.getPPC64ISAPlugin()->getMSR() == (MSR_SF | MSR_EE));
}

void programJ_DSIOrderingAndFaultTermination() {
    std::printf("[phase12-J-sync-exception-ordering]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", config());
    CHECK(prepare(vm, {
        encodeAddi(3, 0, 2), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddis(5, 0, 0x0100), encodeAddi(10, 10, 1),
        encodeD(32, 6, 5, 0), // DSI while DEC is pending but EE is masked
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(4), encodeAddi(10, 10, 1)}));
    vm.getPPC64ISAPlugin()->setGuestDeliveryEnabled(true);
    writeWords(vm.getMemory(), 0x300, {
        encodeMfspr(11, 26), encodeAddi(11, 11, 4), encodeMtspr(11, 26), kRfid});
    writeWords(vm.getMemory(), 0x900, counterHandler(true));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(steps(vm, 5));
    CHECK(plugin->getNIP() == 0x300U);
    CHECK(plugin->getSRR0() == kEntry + 16U);
    CHECK(plugin->getInterruptSnapshot().decrementerPending);
    CHECK(steps(vm, 4)); // DSI handler advances SRR0 and returns authentically
    CHECK(plugin->getNIP() == kEntry + 20U);
    CHECK(steps(vm, 3)); // enable EE; DEC must win before the next instruction
    CHECK(plugin->getNIP() == kEntry + 32U);
    CHECK(step(vm));
    CHECK(plugin->getNIP() == 0x900U);
    CHECK(plugin->getSRR0() == kEntry + 32U);
    CHECK(steps(vm, 7));
    CHECK(readBE32(vm.getMemory(), kCounter) == 1U);
    CHECK(plugin->getNIP() == kEntry + 32U);

    // A synchronous sc 0 syscall may run while a masked DEC level is pending;
    // after its real guest handler returns, enabling EE admits the timer.
    VirtualMachine syscall(kRamSize, 1, "PPC64", config());
    CHECK(prepare(syscall, {
        encodeAddi(3, 0, 1), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(10, 10, 1), 0x44000002U,
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(4), encodeAddi(10, 10, 1)}));
    syscall.getPPC64ISAPlugin()->setGuestDeliveryEnabled(true);
    writeWords(syscall.getMemory(), 0xC00,
               counterHandler(false, kCounter + 8U));
    writeWords(syscall.getMemory(), 0x900, counterHandler(true));
    auto* syscallPlugin = syscall.getPPC64ISAPlugin();
    CHECK(steps(syscall, 3));
    CHECK(syscallPlugin->getInterruptSnapshot().decrementerPending);
    CHECK(step(syscall));
    CHECK(syscallPlugin->getNIP() == 0xC00U);
    CHECK(syscallPlugin->getSRR0() == kEntry + 16U);
    CHECK(syscallPlugin->getInterruptSnapshot().decrementerPending);
    CHECK(steps(syscall, 5));
    CHECK(readBE32(syscall.getMemory(), kCounter + 8U) == 1U);
    CHECK(syscallPlugin->getNIP() == kEntry + 16U);
    CHECK(steps(syscall, 3));
    CHECK(syscallPlugin->getNIP() == kEntry + 28U);
    CHECK(step(syscall));
    CHECK(syscallPlugin->getNIP() == 0x900U);
    CHECK(syscallPlugin->getSRR0() == kEntry + 28U);

    // A supported PAPR terminal call can complete with a masked timer pending;
    // the source remains pending until the guest later enables EE.
    VirtualMachine papr(kRamSize, 1, "PPC64", config(1, 0, true));
    CHECK(prepare(papr, {
        encodeAddi(3, 0, 1), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(10, 10, 1),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_PUT_TERM_CHAR)),
        encodeAddis(4, 0, 0x3000), encodeAddi(5, 0, 0), kSc1,
        encodeMfmsr(4), encodeOri(4, 4, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(4), encodeAddi(10, 10, 1)}));
    papr.getPPC64ISAPlugin()->setGuestDeliveryEnabled(true);
    writeWords(papr.getMemory(), 0x900, counterHandler(true));
    auto* paprPlugin = papr.getPPC64ISAPlugin();
    CHECK(steps(papr, 3));
    CHECK(paprPlugin->getInterruptSnapshot().decrementerPending);
    CHECK(steps(papr, 4));
    CHECK(paprPlugin->getNIP() == kEntry + 28U);
    CHECK(paprPlugin->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(paprPlugin->getInterruptSnapshot().decrementerPending);
    CHECK(steps(papr, 3));
    CHECK(paprPlugin->getNIP() == kEntry + 40U);
    CHECK(step(papr));
    CHECK(paprPlugin->getNIP() == 0x900U);
    CHECK(paprPlugin->getSRR0() == kEntry + 40U);

    // A faulty timer vector remains a terminal bounded guest fault; delivery
    // itself does not execute the interrupted instruction or claim progress.
    VirtualMachine badVector(kRamSize, 1, "PPC64", config());
    CHECK(prepare(badVector, timerProgram(8)));
    writeWord(badVector.getMemory(), 0x900, 0xffffffffU);
    CHECK(steps(badVector, 10));
    CHECK(step(badVector));
    CHECK(badVector.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(!badVector.step());
    CHECK(badVector.getState() == VMState::ERROR);

    // Repeated DSI faults in the timer and DSI handlers hit the existing
    // bounded nested-exception guard instead of spinning indefinitely.
    VirtualMachine repeatedFault(kRamSize, 1, "PPC64", config());
    std::vector<uint32_t> repeatedProgram{
        encodeAddis(5, 0, 0x0100)};
    const auto timerSetup = timerProgram(2);
    repeatedProgram.insert(repeatedProgram.end(), timerSetup.begin(),
                           timerSetup.end());
    CHECK(prepare(repeatedFault, std::move(repeatedProgram)));
    repeatedFault.getPPC64ISAPlugin()->setGuestDeliveryEnabled(true);
    const uint32_t faultingLoad = encodeD(32, 6, 5, 0);
    writeWord(repeatedFault.getMemory(), 0x900, faultingLoad);
    writeWord(repeatedFault.getMemory(), 0x300, faultingLoad);
    CHECK(steps(repeatedFault, 6));
    CHECK(repeatedFault.getPPC64ISAPlugin()->getNIP() == kEntry + 24U);
    CHECK(step(repeatedFault));
    CHECK(repeatedFault.getPPC64ISAPlugin()->getNIP() == 0x900U);
    bool stopped = false;
    for (size_t i = 0; i < MAX_NESTED_EXCEPTIONS + 4U && !stopped; ++i) {
        stopped = !repeatedFault.step();
    }
    CHECK(stopped);
    CHECK(repeatedFault.getState() == VMState::ERROR);
}

} // namespace

int main() {
    std::printf("PPC64 Phase 12 POWER9 DEC and asynchronous interrupt acceptance\n");
    programA_DECReadWriteAndWidth();
    programBAndC_TimerDeliveryAndRealHandler();
    programD_MaskedExpiryAndUnmask();
    programE_ExternalInterruptInjection();
    programF_SimultaneousPriorityAndRearmIndependence();
    programG_PauseSnapshotAndCheckpoint();
    programH_TwoVMIsolationAndLPIDBinding();
    programI_GuestGeneratedTerminalMessage();
    programJ_DSIOrderingAndFaultTermination();
    std::printf("PPC64 Phase 12: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
