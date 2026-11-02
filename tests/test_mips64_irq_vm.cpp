#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ia64;
using namespace mips64;

static unsigned gPass = 0;
static unsigned gFail = 0;
#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

namespace {

constexpr uint64_t kGuestBufferPhysical = 0x3000;
constexpr uint64_t kCausePhysical = 0x3200;
constexpr uint64_t kIirPhysical = 0x3208;
constexpr uint64_t kHandlerCountPhysical = 0x320C;
constexpr uint64_t kReleaseFlagPhysical = 0x3210;
constexpr uint64_t kWorkCounterPhysical = 0x3220;
constexpr uint32_t kStatusIm4Ie = 0x00001101;
constexpr uint32_t kStatusIm4MaskedIe = 0x00000101;
constexpr uint32_t kStatusIm4Im7IeMasked = 0x00009100;
constexpr uint32_t kStatusIm4Im7Ie = 0x00009101;

uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t shamt, uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | funct;
}

uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel = 0) {
    return (0x10U << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | (uint32_t(rd) << 11) | sel;
}

void patchBranch(std::vector<uint32_t>& words, size_t branchIndex,
                 size_t targetIndex, uint8_t opcode, uint8_t rs, uint8_t rt) {
    const auto offset = static_cast<int16_t>(static_cast<int32_t>(targetIndex) -
                                             static_cast<int32_t>(branchIndex + 1));
    words[branchIndex] = encI(opcode, rs, rt, static_cast<uint16_t>(offset));
}

std::vector<uint8_t> encodeWords(const std::vector<uint32_t>& words,
                                Mips64Endianness endianness) {
    std::vector<uint8_t> bytes(words.size() * 4);
    const bool little = endianness == Mips64Endianness::Little;
    for (size_t index = 0; index < words.size(); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            const unsigned shift = 8 * (little ? byte : 3 - byte);
            bytes[index * 4 + byte] = static_cast<uint8_t>(words[index] >> shift);
        }
    }
    return bytes;
}

struct GuestImage {
    std::vector<uint8_t> bootRom;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> handler;
};

GuestImage makeGuestImage(Mips64Endianness endian, uint8_t initialIer,
                          uint32_t initialStatus, uint32_t releaseStatus,
                          bool waitForRelease, bool enableIerOnRelease,
                          uint32_t compareValue, uint8_t fcrValue = 1,
                          uint8_t expectedRxIirId = 4,
                          bool recordLineStatus = false) {
    constexpr uint8_t t0 = 8, t1 = 9, t2 = 10;
    std::vector<uint32_t> boot;
    boot.push_back(encI(0x0F, 0, t0, 0xBF00)); // CKSEG1 UART base.
    boot.push_back(encI(0x0D, t0, t0, 0x0900));
    boot.push_back(encI(0x09, 0, t1, 0x80));
    boot.push_back(encI(0x28, t0, t1, 0x18)); // LCR: DLAB.
    boot.push_back(encI(0x09, 0, t1, 1));
    boot.push_back(encI(0x28, t0, t1, 0));    // DLL.
    boot.push_back(encI(0x09, 0, t1, 0));
    boot.push_back(encI(0x28, t0, t1, 8));    // DLM.
    boot.push_back(encI(0x09, 0, t1, 3));
    boot.push_back(encI(0x28, t0, t1, 0x18)); // LCR: 8N1, DLAB clear.
    boot.push_back(encI(0x09, 0, t1, 1));
    boot.push_back(encI(0x09, 0, t1, fcrValue)); // FCR profile supplied by test.
    boot.push_back(encI(0x28, t0, t1, 16));
    boot.push_back(encI(0x09, 0, t1, initialIer));
    boot.push_back(encI(0x28, t0, t1, 8));    // IER.
    boot.push_back(encI(0x09, 0, t1, 0));
    boot.push_back(encCp0(4, t1, 9));          // mtc0 Count, 0.
    boot.push_back(encI(0x09, 0, t1, static_cast<uint16_t>(compareValue)));
    boot.push_back(encCp0(4, t1, 11));         // mtc0 Compare.
    boot.push_back(encI(0x0F, 0, t1, 0x8000));
    boot.push_back(encCp0(4, t1, 15, 1));      // guest sets EBase=0x80000000.
    boot.push_back(encI(0x0D, 0, t2, static_cast<uint16_t>(initialStatus)));
    boot.push_back(encCp0(4, t2, 12));         // mtc0 Status; BEV/ERL clear.
    boot.push_back(encI(0x0F, 0, t1, 0x8000));
    boot.push_back(encI(0x0D, t1, t1, 0x1000));
    boot.push_back(encR(t1, 0, 0, 0, 0x08));  // jr payload.
    boot.push_back(0);                         // branch-delay slot.

    std::vector<uint32_t> payload;
    payload.push_back(encI(0x0F, 0, 19, 0x8000));
    payload.push_back(encI(0x0D, 19, 19, 0x3000)); // $s3 = RAM base.
    payload.push_back(encI(0x0F, 0, 16, 0x8000));
    payload.push_back(encI(0x0D, 16, 16, 0x3000)); // $s0 = receive pointer.
    payload.push_back(encI(0x09, 0, 17, 0));       // $s1 = IRQ count.
    payload.push_back(encI(0x09, 0, 18, 0));       // $s2 = work counter.
    if (waitForRelease) {
        const size_t waitLoop = payload.size();
        payload.push_back(encI(0x23, 19, 12, 0x210)); // lw $t4, release flag.
        const size_t waitBranch = payload.size();
        payload.push_back(0);
        payload.push_back(0);
        patchBranch(payload, waitBranch, waitLoop, 0x04, 12, 0); // beq $t4,$zero,wait.
        if (enableIerOnRelease) {
            payload.push_back(encI(0x0F, 0, t0, 0xBF00));
            payload.push_back(encI(0x0D, t0, t0, 0x0900));
            payload.push_back(encI(0x09, 0, t1, recordLineStatus ? 5 : 1));
            payload.push_back(encI(0x28, t0, t1, 8)); // guest enables requested RX sources.
        }
        payload.push_back(encI(0x0D, 0, t2, static_cast<uint16_t>(releaseStatus)));
        payload.push_back(encCp0(4, t2, 12)); // guest unmasks/IE enables.
    }
    const size_t workLoop = payload.size();
    payload.push_back(encI(0x09, 18, 18, 1));
    payload.push_back(encI(0x2B, 19, 18, 0x220)); // publish resumed work.
    const size_t workBranch = payload.size();
    payload.push_back(0);
    payload.push_back(0);
    patchBranch(payload, workBranch, workLoop, 0x04, 0, 0);

    constexpr uint8_t k1 = 27, t3 = 11, t4 = 12, t5 = 13;
    std::vector<uint32_t> handler;
    handler.push_back(encCp0(1, k1, 13)); // dmfc0 $k1,Cause.
    handler.push_back(encI(0x2B, 19, k1, 0x200)); // preserve full Cause low word.
    handler.push_back(encCp0(1, 26, 14)); // dmfc0 $k0,EPC.
    handler.push_back(encI(0x3F, 19, 26, 0x230));
    handler.push_back(encCp0(1, 26, 12)); // dmfc0 $k0,Status.
    handler.push_back(encI(0x3F, 19, 26, 0x238));
    handler.push_back(encI(0x0C, k1, t4, 0x8000)); // isolate Cause.IP7 for timer ack.
    const size_t noTimerBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    handler.push_back(encI(0x09, 0, t5, recordLineStatus ? 60000 : 2000));
    handler.push_back(encCp0(4, t5, 11)); // Compare acknowledges timer only.
    const size_t afterTimer = handler.size();
    patchBranch(handler, noTimerBranch, afterTimer, 0x04, t4, 0);
    handler.push_back(encI(0x0F, 0, t0, 0xBF00));
    handler.push_back(encI(0x0D, t0, t0, 0x0900));
    handler.push_back(encI(0x24, t0, t3, 16)); // lbu IIR.
    handler.push_back(encI(0x28, 19, t3, 0x208)); // save IIR.
    handler.push_back(encI(0x0C, t3, t4, 0x0E)); // isolate IIR source id.
    size_t noRxBranch = 0;
    if (!recordLineStatus) {
        handler.push_back(encI(0x09, 0, t5, expectedRxIirId));
        noRxBranch = handler.size();
        handler.push_back(0);
        handler.push_back(0);
    }
    const size_t rxLoop = handler.size();
    handler.push_back(encI(0x24, t0, t4, 40)); // lbu LSR.
    const uint8_t readyReg = recordLineStatus ? 14 : t4;
    handler.push_back(encI(0x0C, t4, readyReg, 1)); // LSR.DR, retaining full LSR if recording.
    const size_t emptyBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    if (recordLineStatus) {
        handler.push_back(encI(0x28, 16, t4, 1)); // save LSR beside this character.
    }
    handler.push_back(encI(0x24, t0, t1, 0)); // lbu RBR consumes a byte.
    handler.push_back(encI(0x28, 16, t1, 0)); // store into guest RAM.
    handler.push_back(encI(0x09, 16, 16, recordLineStatus ? 2 : 1)); // record stride.
    handler.push_back(encI(0x28, t0, t1, 0)); // echo through guest THR.
    const size_t loopBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    patchBranch(handler, loopBranch, rxLoop, 0x04, 0, 0);
    const size_t done = handler.size();
    if (!recordLineStatus) patchBranch(handler, noRxBranch, done, 0x05, t4, t5);
    patchBranch(handler, emptyBranch, done, 0x04, readyReg, 0);
    handler.push_back(encI(0x09, 17, 17, 1)); // one interrupt completed.
    handler.push_back(encI(0x2B, 19, 17, 0x20C));
    handler.push_back(0x42000018U);             // eret.

    GuestImage image;
    image.bootRom = encodeWords(boot, endian);
    image.payload = encodeWords(payload, endian);
    image.handler = encodeWords(handler, endian);
    return image;
}

Mips64ISAPlugin* plugin(VirtualMachine& vm, int cpu = 0) {
    CPUContext* context = vm.getCPUContext(cpu);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

std::unique_ptr<VirtualMachine> makeVm(Mips64Endianness endian,
                                       uint8_t initialIer,
                                       uint32_t initialStatus,
                                       uint32_t releaseStatus,
                                       bool waitForRelease,
                                       bool enableIerOnRelease,
                                       uint32_t compareValue = 0,
                                       size_t cpuCount = 1,
                                       uint8_t fcrValue = 1,
                                       uint8_t expectedRxIirId = 4,
                                       bool recordLineStatus = false) {
    GuestImage image = makeGuestImage(endian, initialIer, initialStatus,
                                      releaseStatus, waitForRelease,
                                      enableIerOnRelease, compareValue,
                                      fcrValue, expectedRxIirId, recordLineStatus);
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.useBuiltInDemoRom = false;
    config.bootRom = image.bootRom;
    auto vm = std::make_unique<VirtualMachine>(2 * 1024 * 1024, cpuCount, "MIPS64");
    vm->configureMips64MaltaBootstrap(config);
    if (!vm->init()) return {};
    if (!vm->loadProgram(image.payload.data(), image.payload.size(), 0x1000) ||
        !vm->loadProgram(image.handler.data(), image.handler.size(), 0x180)) return {};
    return vm;
}

void writeWord(VirtualMachine& vm, uint64_t address, uint32_t value,
               Mips64Endianness endian) {
    uint8_t bytes[4]{};
    const bool little = endian == Mips64Endianness::Little;
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes[byte] = static_cast<uint8_t>(value >> (8 * (little ? byte : 3 - byte)));
    }
    vm.getMemory().Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(VirtualMachine& vm, uint64_t address, Mips64Endianness endian) {
    uint8_t bytes[4]{};
    vm.getMemory().Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    const bool little = endian == Mips64Endianness::Little;
    for (unsigned byte = 0; byte < 4; ++byte) {
        value |= uint32_t(bytes[byte]) << (8 * (little ? byte : 3 - byte));
    }
    return value;
}

uint8_t readByte(VirtualMachine& vm, uint64_t address) {
    uint8_t value = 0;
    vm.getMemory().Read(address, &value, 1);
    return value;
}

uint64_t readDoubleWord(VirtualMachine& vm, uint64_t address, Mips64Endianness endian) {
    uint8_t bytes[8]{};
    vm.getMemory().Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    const bool little = endian == Mips64Endianness::Little;
    for (unsigned byte = 0; byte < 8; ++byte) {
        value |= uint64_t(bytes[byte]) << (8 * (little ? byte : 7 - byte));
    }
    return value;
}

void releaseGuest(VirtualMachine& vm, Mips64Endianness endian) {
    writeWord(vm, kReleaseFlagPhysical, 1, endian);
}

void runToReady(VirtualMachine& vm) {
    CHECK(vm.run(100) == 100);
    CHECK(vm.getState() == VMState::STOPPED);
    CHECK(plugin(vm) != nullptr);
    CHECK(!plugin(vm)->core().statusExl());
}

void checkEcho(VirtualMachine& vm, const std::vector<uint8_t>& input,
               Mips64Endianness endian, bool expectTimerCause = false,
               uint8_t expectedIir = 0xC4) {
    CHECK(vm.getState() == VMState::STOPPED);
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    CHECK(board != nullptr);
    if (!board) return;
    CHECK(board->ConsoleSink()->Bytes() == input);
    std::vector<uint8_t> received(input.size());
    for (size_t index = 0; index < received.size(); ++index) {
        vm.getMemory().Read(kGuestBufferPhysical + index, &received[index], 1);
    }
    CHECK(received == input);
    CHECK(readByte(vm, kIirPhysical) == expectedIir);
    CHECK(readWord(vm, kHandlerCountPhysical, endian) == 1);
    const uint32_t cause = readWord(vm, kCausePhysical, endian);
    CHECK((cause & kCauseIp4Bit) != 0);
    CHECK(((cause & kCauseExcCodeMask) >> kCauseExcCodeShift) ==
          static_cast<uint32_t>(Mips64ExcCode::Int));
    CHECK(((cause & kCauseBitTi) != 0) == expectTimerCause);
    const uint64_t savedEpc = readDoubleWord(vm, 0x3230, endian);
    const uint64_t savedStatus = readDoubleWord(vm, 0x3238, endian);
    CHECK(savedEpc >= 0xFFFFFFFF80001000ULL && savedEpc < 0xFFFFFFFF80002000ULL);
    CHECK((savedStatus & kStatusBitExl) != 0);
    CHECK((plugin(vm)->core().cp0Cause() & kCauseIp4Bit) == 0);
    CHECK(!plugin(vm)->core().statusExl());
    CHECK(readWord(vm, kWorkCounterPhysical, endian) != 0); // guest resumed normal work.
}

bool sameReplayState(const Mips64FullVmSnapshot& a, const Mips64FullVmSnapshot& b) {
    if (a.formatVersion != b.formatVersion || a.cpuCount != b.cpuCount ||
        a.totalVirtualCycles != b.totalVirtualCycles ||
        a.activeCpuIndex != b.activeCpuIndex || a.scheduler.lastCPUIndex != b.scheduler.lastCPUIndex ||
        a.scheduler.numCPUs != b.scheduler.numCPUs ||
        a.scheduler.quantumSize != b.scheduler.quantumSize ||
        a.scheduler.cpuBundleCounters != b.scheduler.cpuBundleCounters ||
        a.fpga.ledBar != b.fpga.ledBar || a.fpga.asciiDisplay != b.fpga.asciiDisplay ||
        a.fpga.breakReset != b.fpga.breakReset || a.fpga.resetPending != b.fpga.resetPending ||
        a.fpga.resetRequestCount != b.fpga.resetRequestCount ||
        a.uart.receiveFifo != b.uart.receiveFifo ||
        a.uart.pendingHostInput != b.uart.pendingHostInput ||
        a.uart.ier != b.uart.ier || a.uart.fcrLastWrite != b.uart.fcrLastWrite ||
        a.uart.fifoEnabled != b.uart.fifoEnabled ||
        a.uart.receiveTriggerLevel != b.uart.receiveTriggerLevel ||
        a.uart.receiveOverrun != b.uart.receiveOverrun ||
        a.uart.virtualTimeCycle != b.uart.virtualTimeCycle ||
        a.uart.receiveTimeoutDeadlineCycle != b.uart.receiveTimeoutDeadlineCycle ||
        a.uart.receiveTimeoutArmed != b.uart.receiveTimeoutArmed ||
        a.uart.receiveTimeoutPending != b.uart.receiveTimeoutPending ||
        a.uart.transmitCount != b.uart.transmitCount ||
        a.uartInterruptTargetCpu != b.uartInterruptTargetCpu ||
        a.uartHardwareInterruptLine != b.uartHardwareInterruptLine ||
        a.uartCauseIp != b.uartCauseIp ||
        a.routedUartInterruptLevel != b.routedUartInterruptLevel ||
        a.console.bytes != b.console.bytes || a.console.sequence != b.console.sequence ||
        a.pendingDeferredBoardReset != b.pendingDeferredBoardReset ||
        a.cpus.size() != b.cpus.size() ||
        a.physicalMemory.regions.size() != b.physicalMemory.regions.size()) {
        return false;
    }
    for (size_t i = 0; i < a.cpus.size(); ++i) {
        if (a.cpus[i].architecturalState != b.cpus[i].architecturalState ||
            a.cpus[i].cyclesExecuted != b.cpus[i].cyclesExecuted ||
            a.cpus[i].instructionsExecuted != b.cpus[i].instructionsExecuted) return false;
    }
    for (size_t i = 0; i < a.physicalMemory.regions.size(); ++i) {
        if (a.physicalMemory.regions[i].denseBytes != b.physicalMemory.regions[i].denseBytes ||
            a.physicalMemory.regions[i].sparseBytes != b.physicalMemory.regions[i].sparseBytes ||
            a.physicalMemory.regions[i].info.base != b.physicalMemory.regions[i].info.base ||
            a.physicalMemory.regions[i].info.size != b.physicalMemory.regions[i].info.size ||
            a.physicalMemory.regions[i].backendKind != b.physicalMemory.regions[i].backendKind) return false;
    }
    return true;
}

void testProductionGuestEchoBothEndiansAndMaskedSources() {
    const std::vector<uint8_t> expected{
        'g','u','i','d','e','X','O','S',' ','M','I','P','S','6','4',' ',
        'i','n','p','u','t',' ','O','K'};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        auto vm = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                         false, false);
        CHECK(vm != nullptr);
        if (!vm) continue;
        runToReady(*vm);
        CHECK(vm->enqueueMips64UartInput(expected));
        CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK(vm->run(1200) == 1200);
        checkEcho(*vm, expected, endian);
    }

    // Scenario A: data and IP4 remain pending while Status.IM4 is clear; the
    // guest later enables the mask and handles the input itself.
    const std::vector<uint8_t> maskedBytes{'m','a','s','k',0,'e','d'};
    auto masked = makeVm(Mips64Endianness::Little, 1, kStatusIm4MaskedIe,
                         kStatusIm4Ie, true, false);
    CHECK(masked != nullptr);
    if (masked) {
        runToReady(*masked);
        CHECK(masked->enqueueMips64UartInput(maskedBytes));
        CHECK((plugin(*masked)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK((plugin(*masked)->core().cp0Status() & (1ULL << 12)) == 0);
        CHECK((masked->getMips64MaltaBootstrapBoard()->Uart()->InspectState().at("rx_fifo_bytes")) > 0);
        CHECK(masked->run(20) == 20);
        CHECK(!plugin(*masked)->core().statusExl());
        releaseGuest(*masked, Mips64Endianness::Little);
        CHECK(masked->run(800) == 800);
        checkEcho(*masked, maskedBytes, Mips64Endianness::Little);
    }

    // Scenario B: UART retains data with IER.RDI disabled and asserts IRQ only
    // after the guest enables IER through MMIO.
    const std::vector<uint8_t> disabledBytes{'I','E','R','!'};
    auto disabled = makeVm(Mips64Endianness::Little, 0, kStatusIm4Ie,
                           kStatusIm4Ie, true, true);
    CHECK(disabled != nullptr);
    if (disabled) {
        runToReady(*disabled);
        CHECK(disabled->enqueueMips64UartInput(disabledBytes));
        CHECK((disabled->getMips64MaltaBootstrapBoard()->Uart()->InterruptSignal().value()) == false);
        CHECK((plugin(*disabled)->core().cp0Cause() & kCauseIp4Bit) == 0);
        uint8_t lsr = 0;
        disabled->getMemory().Read(kMaltaUartBase + 40, &lsr, 1);
        CHECK((lsr & 1) != 0);
        releaseGuest(*disabled, Mips64Endianness::Little);
        CHECK(disabled->run(800) == 800);
        checkEcho(*disabled, disabledBytes, Mips64Endianness::Little);
    }

    // The VM-scoped route asserts CPU0 INT2 only in a two-vCPU Malta profile.
    auto twoCpu = makeVm(Mips64Endianness::Little, 1, kStatusIm4Ie,
                         kStatusIm4Ie, true, false, 0, 2);
    CHECK(twoCpu != nullptr);
    if (twoCpu) {
        CHECK(twoCpu->run(200) == 200);
        CHECK(twoCpu->enqueueMips64UartInput(std::vector<uint8_t>{'0'}));
        CHECK((plugin(*twoCpu, 0)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK((plugin(*twoCpu, 1)->core().cp0Cause() & kCauseIp4Bit) == 0);
        CHECK(twoCpu->coldResetMips64Malta());
        CHECK((plugin(*twoCpu, 0)->core().cp0Cause() & kCauseIp4Bit) == 0);
        CHECK(twoCpu->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == 0);
        CHECK(twoCpu->getMips64MaltaBootstrapBoard()->Uart()->PendingHostInputSize() == 0);
    }
}

void testTransactionalReceiveSnapshotReplay() {
    const std::vector<uint8_t> input{'R',0,'X','-',0xFF,'Q','!'};
    const auto endian = Mips64Endianness::Little;
    auto vm = makeVm(endian, 1, kStatusIm4MaskedIe, kStatusIm4Ie,
                     true, false);
    CHECK(vm != nullptr);
    if (!vm) return;
    runToReady(*vm);

    // Replay 1: waiting before input, inject the same binary bytes after restore.
    const Mips64FullVmSnapshot beforeInput = vm->captureMips64FullSnapshot();
    CHECK(vm->enqueueMips64UartInput(input));
    releaseGuest(*vm, endian);
    CHECK(vm->run(1000) == 1000);
    const Mips64FullVmSnapshot firstComplete = vm->captureMips64FullSnapshot();
    CHECK(vm->restoreMips64FullSnapshot(beforeInput));
    CHECK(vm->enqueueMips64UartInput(input));
    releaseGuest(*vm, endian);
    CHECK(vm->run(1000) == 1000);
    CHECK(sameReplayState(firstComplete, vm->captureMips64FullSnapshot()));

    // Replay 2: snapshot pending-but-masked input, then have guest MTC0 unmask.
    CHECK(vm->restoreMips64FullSnapshot(beforeInput));
    CHECK(vm->enqueueMips64UartInput(input));
    CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) != 0);
    CHECK((plugin(*vm)->core().cp0Status() & (1ULL << 12)) == 0);
    const Mips64FullVmSnapshot pendingMasked = vm->captureMips64FullSnapshot();
    CHECK(pendingMasked.uart.receiveFifo.size() == input.size());
    CHECK(pendingMasked.routedUartInterruptLevel);
    releaseGuest(*vm, endian);
    CHECK(vm->run(1000) == 1000);
    const Mips64FullVmSnapshot maskedComplete = vm->captureMips64FullSnapshot();
    CHECK(vm->restoreMips64FullSnapshot(pendingMasked));
    CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) != 0);
    releaseGuest(*vm, endian);
    CHECK(vm->run(1000) == 1000);
    CHECK(sameReplayState(maskedComplete, vm->captureMips64FullSnapshot()));

    // Replay 3: consume a prefix from FIFO + host staging, snapshot the partial
    // queue, finish, restore, and finish the same remaining bytes again.
    CHECK(vm->restoreMips64FullSnapshot(beforeInput));
    std::vector<uint8_t> longInput(40);
    for (size_t i = 0; i < longInput.size(); ++i) longInput[i] = static_cast<uint8_t>(i + 1);
    CHECK(vm->enqueueMips64UartInput(longInput));
    releaseGuest(*vm, endian);
    unsigned partialSteps = 0;
    while (vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size() < 1) {
        CHECK(vm->run(1) == 1);
        ++partialSteps;
        CHECK(partialSteps < 100);
    }
    const Mips64FullVmSnapshot partial = vm->captureMips64FullSnapshot();
    CHECK(!partial.uart.receiveFifo.empty());
    CHECK(!partial.uart.pendingHostInput.empty());
    CHECK(partial.uart.receiveFifo.size() + partial.uart.pendingHostInput.size() < longInput.size());
    const std::vector<uint8_t> partialConsole = vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(partialConsole.size() == 1 && partialConsole[0] == longInput[0]);
    CHECK(vm->run(1000) == 1000);
    const Mips64FullVmSnapshot longComplete = vm->captureMips64FullSnapshot();
    CHECK(vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == longInput);
    CHECK(vm->restoreMips64FullSnapshot(partial));
    CHECK(vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == partialConsole);
    CHECK(vm->run(1000) == 1000);
    CHECK(sameReplayState(longComplete, vm->captureMips64FullSnapshot()));
    CHECK(vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == longInput);

    // Replay 4: timer IP7 and UART IP4 are both pending in a full VM snapshot.
    auto overlap = makeVm(endian, 1, kStatusIm4Im7IeMasked,
                          kStatusIm4Im7Ie, true, false, 180);
    CHECK(overlap != nullptr);
    if (!overlap) return;
    runToReady(*overlap);
    const uint64_t compare = 180;
    const uint64_t count = plugin(*overlap)->core().cp0Count();
    if (count < compare - 1) CHECK(overlap->run(compare - 1 - count) == compare - 1 - count);
    CHECK(plugin(*overlap)->core().cp0Count() == compare - 1);
    CHECK(overlap->enqueueMips64UartInput(input));
    CHECK(overlap->run(1) == 1); // Count reaches Compare while IE is clear.
    const uint64_t both = plugin(*overlap)->core().cp0Cause();
    CHECK((both & (kCauseIp4Bit | kCauseBitTi | kCauseIp7Bit)) ==
          (kCauseIp4Bit | kCauseBitTi | kCauseIp7Bit));
    const Mips64FullVmSnapshot bothPending = overlap->captureMips64FullSnapshot();
    releaseGuest(*overlap, endian);
    CHECK(overlap->run(1000) == 1000);
    checkEcho(*overlap, input, endian, true);
    const Mips64FullVmSnapshot overlapComplete = overlap->captureMips64FullSnapshot();
    CHECK(overlap->restoreMips64FullSnapshot(bothPending));
    CHECK((plugin(*overlap)->core().cp0Cause() &
          (kCauseIp4Bit | kCauseBitTi | kCauseIp7Bit)) ==
          (kCauseIp4Bit | kCauseBitTi | kCauseIp7Bit));
    releaseGuest(*overlap, endian);
    CHECK(overlap->run(1000) == 1000);
    CHECK(sameReplayState(overlapComplete, overlap->captureMips64FullSnapshot()));

    // Replay 5: malformed queue length, UART version, and route identity reject
    // before changing any live CPU/device/memory/output/scheduler state.
    CHECK(overlap->restoreMips64FullSnapshot(bothPending));
    const Mips64FullVmSnapshot beforeBadRestore = overlap->captureMips64FullSnapshot();
    auto checkBad = [&](Mips64FullVmSnapshot bad) {
        std::string error;
        CHECK(bad.RefreshIntegrity());
        CHECK(!overlap->restoreMips64FullSnapshot(bad, &error));
        CHECK(!error.empty());
        CHECK(overlap->captureMips64FullSnapshot().integrityChecksum ==
              beforeBadRestore.integrityChecksum);
    };
    Mips64FullVmSnapshot badQueue = bothPending;
    badQueue.uart.receiveFifo.resize(Ns16550Uart::kReceiveFifoCapacity + 1,
                                    Ns16550RxCharacter{0xAA, 0, false});
    checkBad(std::move(badQueue));
    Mips64FullVmSnapshot badVersion = bothPending;
    badVersion.uartComponentVersion = 99;
    checkBad(std::move(badVersion));
    Mips64FullVmSnapshot badRoute = bothPending;
    badRoute.uartHardwareInterruptLine = 3;
    checkBad(std::move(badRoute));

    // Replay 6: a matching independent VM restores without mutating the source.
    const uint64_t sourceChecksum = overlap->captureMips64FullSnapshot().integrityChecksum;
    auto independent = makeVm(endian, 1, kStatusIm4Im7IeMasked,
                              kStatusIm4Im7Ie, true, false, 180);
    CHECK(independent != nullptr);
    if (independent) {
        CHECK(independent->restoreMips64FullSnapshot(bothPending));
        CHECK((plugin(*independent)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK(independent->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == input.size());
        CHECK(overlap->captureMips64FullSnapshot().integrityChecksum == sourceChecksum);
    }
}

uint8_t readUartRegister(VirtualMachine& vm, unsigned reg) {
    uint8_t value = 0xFF;
    vm.getMemory().Read(kMaltaUartBase + reg * 8, &value, 1);
    return value;
}

void testProductionTimeoutGuestAndThresholdSemantics() {
    // Seven bytes are strictly below the eight-byte trigger. The prompt's
    // example "guideXOS" is eight ASCII bytes, so it is covered below at the
    // fourteen-byte trigger and at the eight-byte normal-RDA boundary.
    const std::vector<uint8_t> sevenBytes{'g','u','i','d','e','X','O'};
    const std::vector<uint8_t> eightBytes{'g','u','i','d','e','X','O','S'};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        auto timeoutVm = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                                false, false, 0, 1, 0x87, 12);
        CHECK(timeoutVm != nullptr);
        if (!timeoutVm) continue;
        runToReady(*timeoutVm);
        auto uart = timeoutVm->getMips64MaltaBootstrapBoard()->Uart();
        CHECK(uart->ReceiveTimeoutDeadline() == 0);
        CHECK(timeoutVm->enqueueMips64UartInput(sevenBytes));
        const uint64_t deadline = uart->ReceiveTimeoutDeadline();
        const uint64_t start = timeoutVm->getCyclesExecuted();
        CHECK(deadline - start == 174);
        CHECK(uart->ReceiveFifoSize() == sevenBytes.size());
        CHECK(readUartRegister(*timeoutVm, 2) == 0xC1);
        CHECK(!uart->ReceiveTimeoutPending());
        CHECK((plugin(*timeoutVm)->core().cp0Cause() & kCauseIp4Bit) == 0);

        CHECK(timeoutVm->run(deadline - start - 1) == deadline - start - 1);
        CHECK(timeoutVm->getCyclesExecuted() == deadline - 1);
        CHECK(readUartRegister(*timeoutVm, 2) == 0xC1);
        CHECK(!uart->ReceiveTimeoutPending());
        CHECK((plugin(*timeoutVm)->core().cp0Cause() & kCauseIp4Bit) == 0);
        CHECK(timeoutVm->run(1) == 1);
        CHECK(timeoutVm->getCyclesExecuted() == deadline);
        CHECK(uart->ReceiveTimeoutPending());
        CHECK(readUartRegister(*timeoutVm, 2) == 0xCC);
        CHECK((plugin(*timeoutVm)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK(timeoutVm->run(1000) == 1000);
        checkEcho(*timeoutVm, sevenBytes, endian, false, 0xCC);

        // The example's final byte is deliberately sent after the first
        // sub-trigger timeout. It demonstrates the full guideXOS echo at the
        // eight-byte trigger without misclassifying an eight-byte FIFO as a
        // timeout condition.
        CHECK(timeoutVm->enqueueMips64UartInput(std::vector<uint8_t>{'S'}));
        CHECK(readUartRegister(*timeoutVm, 2) == 0xC1);
        const uint64_t lastByteDeadline = uart->ReceiveTimeoutDeadline();
        const uint64_t lastByteRemaining = lastByteDeadline - timeoutVm->getCyclesExecuted();
        CHECK(timeoutVm->run(lastByteRemaining) == lastByteRemaining);
        CHECK(readUartRegister(*timeoutVm, 2) == 0xCC);
        CHECK(timeoutVm->run(1000) == 1000);
        CHECK(timeoutVm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == eightBytes);
        for (size_t index = 0; index < eightBytes.size(); ++index) {
            CHECK(readByte(*timeoutVm, kGuestBufferPhysical + index) == eightBytes[index]);
        }
        CHECK(readWord(*timeoutVm, kHandlerCountPhysical, endian) == 2);
        CHECK(readByte(*timeoutVm, kIirPhysical) == 0xCC);
        CHECK(!plugin(*timeoutVm)->core().statusExl());

        // A long bounded run cannot redeliver a drained timeout or duplicate
        // logical UART output.
        CHECK(timeoutVm->run(4096) == 4096);
        CHECK(readWord(*timeoutVm, kHandlerCountPhysical, endian) == 2);
        CHECK(timeoutVm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == eightBytes);

        // Eight-byte guideXOS is below trigger 14 and times out normally.
        auto fourteenTrigger = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                                      false, false, 0, 1, 0xC7, 12);
        CHECK(fourteenTrigger != nullptr);
        if (!fourteenTrigger) continue;
        runToReady(*fourteenTrigger);
        CHECK(fourteenTrigger->enqueueMips64UartInput(eightBytes));
        CHECK(readUartRegister(*fourteenTrigger, 2) == 0xC1);
        CHECK((plugin(*fourteenTrigger)->core().cp0Cause() & kCauseIp4Bit) == 0);
        const uint64_t fourteenDeadline = fourteenTrigger->getMips64MaltaBootstrapBoard()
            ->Uart()->ReceiveTimeoutDeadline();
        const uint64_t fourteenRemaining = fourteenDeadline - fourteenTrigger->getCyclesExecuted();
        CHECK(fourteenTrigger->run(fourteenRemaining) == fourteenRemaining);
        CHECK(readUartRegister(*fourteenTrigger, 2) == 0xCC);
        CHECK(fourteenTrigger->run(1000) == 1000);
        checkEcho(*fourteenTrigger, eightBytes, endian, false, 0xCC);

        // At trigger eight, the same eight bytes assert ordinary RDA (0x04),
        // and no character-timeout state is created.
        auto exactlyEight = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                                   false, false, 0, 1, 0x87, 4);
        CHECK(exactlyEight != nullptr);
        if (!exactlyEight) continue;
        runToReady(*exactlyEight);
        CHECK(exactlyEight->enqueueMips64UartInput(eightBytes));
        CHECK(readUartRegister(*exactlyEight, 2) == 0xC4);
        CHECK(!exactlyEight->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK((plugin(*exactlyEight)->core().cp0Cause() & kCauseIp4Bit) != 0);
        CHECK(exactlyEight->run(1000) == 1000);
        checkEcho(*exactlyEight, eightBytes, endian, false, 0xC4);
    }

    // The VM clock advances once per round-robin scheduler step, including
    // when two CPUs are enabled. It is not counted once per vCPU.
    auto twoCpu = makeVm(Mips64Endianness::Little, 1, kStatusIm4Ie,
                         kStatusIm4Ie, false, false, 0, 2, 0x87, 12);
    CHECK(twoCpu != nullptr);
    if (twoCpu) {
        CPUContext* second = twoCpu->getCPUContext(1);
        CHECK(second != nullptr);
        if (second) {
            second->enabled = true;
            second->state = CPUExecutionState::RUNNING;
        }
        CHECK(twoCpu->run(200) == 200);
        CHECK(twoCpu->getCPUContext(0)->cyclesExecuted > 0);
        CHECK(twoCpu->getCPUContext(1)->cyclesExecuted > 0);
        CHECK(twoCpu->enqueueMips64UartInput(sevenBytes));
        const uint64_t twoCpuDeadline = twoCpu->getMips64MaltaBootstrapBoard()->Uart()
            ->ReceiveTimeoutDeadline();
        CHECK(twoCpuDeadline - twoCpu->getCyclesExecuted() == 174);
        CHECK(twoCpu->run(173) == 173);
        CHECK(!twoCpu->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(twoCpu->run(1) == 1);
        CHECK(twoCpu->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(twoCpu->run(1200) == 1200);
        checkEcho(*twoCpu, sevenBytes, Mips64Endianness::Little, false, 0xCC);
    }
}

void testTransactionalTimeoutSnapshotReplay() {
    const auto endian = Mips64Endianness::Little;
    const std::vector<uint8_t> input{'g','u','i','d','e','X','O'};
    auto vm = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                     false, false, 0, 1, 0x87, 12);
    CHECK(vm != nullptr);
    if (!vm) return;
    runToReady(*vm);
    CHECK(vm->enqueueMips64UartInput(input));
    const uint64_t deadline = vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutDeadline();
    const uint64_t start = vm->getCyclesExecuted();
    const uint64_t half = (deadline - start) / 2;
    CHECK(vm->run(half) == half);
    const Mips64FullVmSnapshot armed = vm->captureMips64FullSnapshot();
    CHECK(armed.uart.version == 4 && armed.uartComponentVersion == 4);
    CHECK(armed.uart.virtualTimeCycle == armed.totalVirtualCycles);
    CHECK(armed.uart.receiveTimeoutArmed && !armed.uart.receiveTimeoutPending);
    CHECK(armed.uart.receiveTimeoutDeadlineCycle == deadline);

    uint64_t remaining = deadline - vm->getCyclesExecuted();
    CHECK(vm->run(remaining) == remaining);
    CHECK(vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    const Mips64FullVmSnapshot boundary = vm->captureMips64FullSnapshot();
    CHECK(boundary.uart.receiveTimeoutPending && !boundary.uart.receiveTimeoutArmed);
    CHECK(readUartRegister(*vm, 2) == 0xCC);
    CHECK(vm->run(1000) == 1000);
    checkEcho(*vm, input, endian, false, 0xCC);
    const Mips64FullVmSnapshot firstComplete = vm->captureMips64FullSnapshot();

    // Replay A: restore halfway-armed state and reach the same exact edge.
    CHECK(vm->restoreMips64FullSnapshot(armed));
    CHECK(vm->getCyclesExecuted() == armed.totalVirtualCycles);
    CHECK(vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutDeadline() == deadline);
    CHECK(vm->run(remaining) == remaining);
    const Mips64FullVmSnapshot replayBoundary = vm->captureMips64FullSnapshot();
    CHECK(replayBoundary.uart.receiveTimeoutPending);
    CHECK(sameReplayState(boundary, replayBoundary));
    CHECK(vm->run(1000) == 1000);
    CHECK(sameReplayState(firstComplete, vm->captureMips64FullSnapshot()));

    // Replay B: one unit before expiry remains quiet; one more unit expires.
    CHECK(vm->restoreMips64FullSnapshot(armed));
    const uint64_t toOneBefore = deadline - vm->getCyclesExecuted() - 1;
    CHECK(vm->run(toOneBefore) == toOneBefore);
    CHECK(vm->getCyclesExecuted() == deadline - 1);
    CHECK(!vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    CHECK(readUartRegister(*vm, 2) == 0xC1);
    CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) == 0);
    const Mips64FullVmSnapshot oneBefore = vm->captureMips64FullSnapshot();
    CHECK(vm->restoreMips64FullSnapshot(oneBefore));
    CHECK(vm->run(1) == 1);
    CHECK(vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    CHECK(sameReplayState(boundary, vm->captureMips64FullSnapshot()));

    // Replay C: IER keeps the device source enabled while CPU Status.IM4 masks
    // delivery. The pending source survives a snapshot and guest unmask.
    auto masked = makeVm(endian, 1, kStatusIm4MaskedIe, kStatusIm4Ie,
                         true, false, 0, 1, 0x87, 12);
    CHECK(masked != nullptr);
    if (!masked) return;
    runToReady(*masked);
    CHECK(masked->enqueueMips64UartInput(input));
    const uint64_t maskedDeadline = masked->getMips64MaltaBootstrapBoard()->Uart()
        ->ReceiveTimeoutDeadline();
    const uint64_t maskedRemaining = maskedDeadline - masked->getCyclesExecuted();
    CHECK(masked->run(maskedRemaining) == maskedRemaining);
    CHECK(masked->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    CHECK((plugin(*masked)->core().cp0Cause() & kCauseIp4Bit) != 0);
    CHECK((plugin(*masked)->core().cp0Status() & kCauseIp4Bit) == 0);
    CHECK(readUartRegister(*masked, 2) == 0xCC);
    const Mips64FullVmSnapshot pendingMasked = masked->captureMips64FullSnapshot();
    CHECK(pendingMasked.routedUartInterruptLevel);
    releaseGuest(*masked, endian);
    CHECK(masked->run(1000) == 1000);
    checkEcho(*masked, input, endian, false, 0xCC);
    const Mips64FullVmSnapshot maskedComplete = masked->captureMips64FullSnapshot();
    CHECK(masked->restoreMips64FullSnapshot(pendingMasked));
    releaseGuest(*masked, endian);
    CHECK(masked->run(1000) == 1000);
    CHECK(sameReplayState(maskedComplete, masked->captureMips64FullSnapshot()));

    // Replay D: real RBR register accesses leave an ordered sub-trigger FIFO;
    // its rearmed timeout and suffix reproduce through the production guest.
    auto partialVm = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                            false, false, 0, 1, 0x87, 12);
    CHECK(partialVm != nullptr);
    if (!partialVm) return;
    runToReady(*partialVm);
    CHECK(partialVm->enqueueMips64UartInput(input));
    CHECK(readUartRegister(*partialVm, 0) == input[0]);
    CHECK(readUartRegister(*partialVm, 0) == input[1]);
    const std::vector<uint8_t> suffix(input.begin() + 2, input.end());
    const Mips64FullVmSnapshot partial = partialVm->captureMips64FullSnapshot();
    CHECK(partial.uart.receiveFifo.size() == suffix.size());
    for (size_t index = 0; index < suffix.size(); ++index) {
        CHECK(partial.uart.receiveFifo[index].data == suffix[index]);
    }
    CHECK(partial.uart.receiveTimeoutArmed && !partial.uart.receiveTimeoutPending);
    const uint64_t partialRemaining = partial.uart.receiveTimeoutDeadlineCycle -
        partialVm->getCyclesExecuted();
    CHECK(partialVm->run(partialRemaining) == partialRemaining);
    CHECK(partialVm->run(1000) == 1000);
    checkEcho(*partialVm, suffix, endian, false, 0xCC);
    const Mips64FullVmSnapshot partialComplete = partialVm->captureMips64FullSnapshot();
    CHECK(partialVm->restoreMips64FullSnapshot(partial));
    CHECK(partialVm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == suffix.size());
    CHECK(partialVm->run(partialRemaining) == partialRemaining);
    CHECK(partialVm->run(1000) == 1000);
    CHECK(sameReplayState(partialComplete, partialVm->captureMips64FullSnapshot()));

    // Replay E: timer IP7 and UART timeout IP4 are pending at the same
    // boundary; guest acknowledges Compare and then the UART through RBR.
    auto overlap = makeVm(endian, 1, kStatusIm4Im7IeMasked,
                          kStatusIm4Im7Ie, true, false, 20, 1, 0x87, 12);
    CHECK(overlap != nullptr);
    if (!overlap) return;
    runToReady(*overlap);
    uint64_t guard = 0;
    while ((plugin(*overlap)->core().cp0Cause() & kCauseBitTi) == 0 && guard < 512) {
        CHECK(overlap->run(1) == 1);
        ++guard;
    }
    CHECK(guard < 512 && (plugin(*overlap)->core().cp0Cause() & kCauseBitTi) != 0);
    CHECK(overlap->enqueueMips64UartInput(input));
    const uint64_t overlapDeadline = overlap->getMips64MaltaBootstrapBoard()->Uart()
        ->ReceiveTimeoutDeadline();
    const uint64_t overlapRemaining = overlapDeadline - overlap->getCyclesExecuted();
    CHECK(overlap->run(overlapRemaining) == overlapRemaining);
    const uint64_t bothPendingCause = plugin(*overlap)->core().cp0Cause();
    CHECK((bothPendingCause & (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi)) ==
          (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi));
    CHECK(readUartRegister(*overlap, 2) == 0xCC);
    const Mips64FullVmSnapshot bothPending = overlap->captureMips64FullSnapshot();
    releaseGuest(*overlap, endian);
    CHECK(overlap->run(1200) == 1200);
    checkEcho(*overlap, input, endian, true, 0xCC);
    const Mips64FullVmSnapshot overlapComplete = overlap->captureMips64FullSnapshot();
    CHECK(overlap->restoreMips64FullSnapshot(bothPending));
    releaseGuest(*overlap, endian);
    CHECK(overlap->run(1200) == 1200);
    CHECK(sameReplayState(overlapComplete, overlap->captureMips64FullSnapshot()));

    // Replay F: malformed deadline/clock/length/version/IRQ records all fail
    // before any destination CPU, memory, output, or device mutation.
    const Mips64FullVmSnapshot beforeFailedRestore = overlap->captureMips64FullSnapshot();
    auto rejectWithoutMutation = [&](Mips64FullVmSnapshot bad, bool refresh) {
        if (refresh) CHECK(bad.RefreshIntegrity());
        std::string error;
        CHECK(!overlap->restoreMips64FullSnapshot(bad, &error));
        CHECK(!error.empty());
        CHECK(overlap->captureMips64FullSnapshot().integrityChecksum ==
              beforeFailedRestore.integrityChecksum);
    };
    Mips64FullVmSnapshot badDeadline = bothPending;
    badDeadline.uart.receiveTimeoutDeadlineCycle = badDeadline.uart.virtualTimeCycle + 1;
    rejectWithoutMutation(std::move(badDeadline), true);
    Mips64FullVmSnapshot badUartClock = bothPending;
    ++badUartClock.uart.virtualTimeCycle;
    rejectWithoutMutation(std::move(badUartClock), true);
    Mips64FullVmSnapshot badVmClock = bothPending;
    ++badVmClock.totalVirtualCycles;
    rejectWithoutMutation(std::move(badVmClock), true);
    Mips64FullVmSnapshot oldUartVersion = bothPending;
    oldUartVersion.uartComponentVersion = 2;
    rejectWithoutMutation(std::move(oldUartVersion), true);
    Mips64FullVmSnapshot badLength = bothPending;
    ++badLength.deviceComponentLength;
    rejectWithoutMutation(std::move(badLength), false);
    Mips64FullVmSnapshot badIrqRoute = bothPending;
    badIrqRoute.uartHardwareInterruptLine = 3;
    rejectWithoutMutation(std::move(badIrqRoute), true);

    auto coldReset = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                            false, false, 0, 1, 0x87, 12);
    CHECK(coldReset != nullptr);
    if (coldReset) {
        runToReady(*coldReset);
        CHECK(coldReset->enqueueMips64UartInput(input));
        const uint64_t coldDeadline = coldReset->getMips64MaltaBootstrapBoard()->Uart()
            ->ReceiveTimeoutDeadline();
        const uint64_t coldRemaining = coldDeadline - coldReset->getCyclesExecuted();
        CHECK(coldReset->run(coldRemaining) == coldRemaining);
        CHECK(coldReset->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(coldReset->coldResetMips64Malta());
        CHECK(coldReset->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == 0);
        CHECK(!coldReset->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(coldReset->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutDeadline() == 0);
        CHECK(coldReset->getMips64MaltaBootstrapBoard()->Uart()->VirtualTimeCycle() == 0);
    }

    // Malta FPGA SOFTRES is applied at the next VM boundary and resets all
    // timeout state while preserving the established VM cycle count policy.
    CHECK(overlap->enqueueMips64UartInput(input));
    const uint64_t resetDeadline = overlap->getMips64MaltaBootstrapBoard()->Uart()
        ->ReceiveTimeoutDeadline();
    const uint64_t resetRemaining = resetDeadline - overlap->getCyclesExecuted();
    CHECK(overlap->run(resetRemaining) == resetRemaining);
    CHECK(overlap->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    writeWord(*overlap, kMaltaFpgaBase + 0x500, 0x42, endian);
    CHECK(overlap->getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
    CHECK(overlap->step());
    CHECK(overlap->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == 0);
    CHECK(!overlap->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
    CHECK(overlap->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutDeadline() == 0);
    CHECK(overlap->getMips64MaltaBootstrapBoard()->Uart()->VirtualTimeCycle() ==
          overlap->getCyclesExecuted());
    CHECK((plugin(*overlap)->core().cp0Cause() & kCauseIp4Bit) == 0);

    // Cross-VM restore resumes from the exact armed deadline and keeps the
    // source VM unchanged.
    auto independent = makeVm(endian, 1, kStatusIm4Ie, kStatusIm4Ie,
                              false, false, 0, 1, 0x87, 12);
    CHECK(independent != nullptr);
    if (independent) {
        CHECK(independent->restoreMips64FullSnapshot(armed));
        const uint64_t sourceChecksum = vm->captureMips64FullSnapshot().integrityChecksum;
        CHECK(independent->getCyclesExecuted() == armed.totalVirtualCycles);
        const uint64_t independentRemaining = armed.uart.receiveTimeoutDeadlineCycle -
            independent->getCyclesExecuted();
        CHECK(independent->run(independentRemaining) == independentRemaining);
        CHECK(independent->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(independent->run(1000) == 1000);
        checkEcho(*independent, input, endian, false, 0xCC);
        CHECK(vm->captureMips64FullSnapshot().integrityChecksum == sourceChecksum);
    }
}

void testProductionGuestLineStatusAndTimeout() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        auto vm = makeVm(endian, 0x05, kStatusIm4Im7IeMasked,
                         kStatusIm4Im7Ie, true, false, 20, 1, 0x41, 6, true);
        CHECK(vm != nullptr);
        if (!vm) continue;
        runToReady(*vm);
        CHECK((plugin(*vm)->core().cp0Cause() &
               (kCauseIp7Bit | kCauseBitTi)) == (kCauseIp7Bit | kCauseBitTi));

        const std::vector<Ns16550RxInput> firstBatch{
            {'A', Ns16550Uart::kLsrParityError},
            {'B', 0},
            {'C', Ns16550Uart::kLsrFramingError},
            {'X', Ns16550Uart::kLsrBreakIndication}};
        CHECK(vm->enqueueMips64UartReceiverInput(firstBatch));
        CHECK(readUartRegister(*vm, 2) == 0xC6);
        CHECK((plugin(*vm)->core().cp0Cause() &
               (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi)) ==
              (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi));
        releaseGuest(*vm, endian);
        CHECK(vm->run(1600) == 1600);

        const std::vector<uint8_t> firstBytes{'A', 'B', 'C', 0};
        CHECK(vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == firstBytes);
        CHECK(readWord(*vm, kHandlerCountPhysical, endian) == 1);
        CHECK(readByte(*vm, kIirPhysical) == 0xC6);
        const uint32_t firstCause = readWord(*vm, kCausePhysical, endian);
        CHECK((firstCause & (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi)) ==
              (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi));
        CHECK(((firstCause & kCauseExcCodeMask) >> kCauseExcCodeShift) ==
              static_cast<uint32_t>(Mips64ExcCode::Int));
        const std::vector<uint8_t> firstLsr{0xE5, 0xE1, 0xE9, 0xF1};
        for (size_t index = 0; index < firstBytes.size(); ++index) {
            CHECK(readByte(*vm, kGuestBufferPhysical + index * 2) == firstBytes[index]);
            CHECK(readByte(*vm, kGuestBufferPhysical + index * 2 + 1) == firstLsr[index]);
        }
        const uint64_t firstEpc = readDoubleWord(*vm, 0x3230, endian);
        const uint64_t firstStatus = readDoubleWord(*vm, 0x3238, endian);
        CHECK(firstEpc >= 0xFFFFFFFF80001000ULL && firstEpc < 0xFFFFFFFF80002000ULL);
        CHECK((firstStatus & kStatusBitExl) != 0);
        CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) == 0);
        CHECK(!plugin(*vm)->core().statusExl());
        CHECK(readWord(*vm, kWorkCounterPhysical, endian) != 0);

        const std::vector<Ns16550RxInput> ordinary{{'Z', 0}};
        CHECK(vm->enqueueMips64UartReceiverInput(ordinary));
        CHECK(readUartRegister(*vm, 2) == 0xC1); // Below trigger, timeout not yet due.
        const uint64_t timeoutDeadline = vm->getMips64MaltaBootstrapBoard()->Uart()
            ->ReceiveTimeoutDeadline();
        const uint64_t untilTimeout = timeoutDeadline - vm->getCyclesExecuted();
        CHECK(vm->run(untilTimeout) == untilTimeout);
        CHECK(vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveTimeoutPending());
        CHECK(readUartRegister(*vm, 2) == 0xCC);
        CHECK(vm->run(1000) == 1000);

        CHECK(vm->getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
              std::vector<uint8_t>({'A', 'B', 'C', 0, 'Z'}));
        CHECK(readWord(*vm, kHandlerCountPhysical, endian) == 2);
        CHECK(readByte(*vm, kIirPhysical) == 0xCC);
        CHECK(readByte(*vm, kGuestBufferPhysical + 8) == 'Z');
        CHECK(readByte(*vm, kGuestBufferPhysical + 9) == 0x61);
        CHECK((plugin(*vm)->core().cp0Cause() & kCauseIp4Bit) == 0);
        CHECK(!plugin(*vm)->core().statusExl());
        CHECK(readWord(*vm, kWorkCounterPhysical, endian) != 0);
        CHECK(vm->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == 0);
        CHECK(!*vm->getMips64MaltaBootstrapBoard()->Uart()->InterruptSignal());

        // The UART condition remains pending under both architectural
        // exception-level suppressors and is delivered after guest MTC0 Status.
        for (const uint64_t exceptionLevel : {kStatusBitExl, kStatusBitErl}) {
            auto suppressed = makeVm(endian, 0x05,
                static_cast<uint32_t>(kStatusIm4Ie | exceptionLevel),
                kStatusIm4Ie, true, false, 0, 1, 1, 6, true);
            CHECK(suppressed != nullptr);
            if (!suppressed) continue;
            CHECK(suppressed->run(100) == 100);
            CHECK(plugin(*suppressed)->core().statusExl() ==
                  (exceptionLevel == kStatusBitExl));
            CHECK(plugin(*suppressed)->core().statusErl() ==
                  (exceptionLevel == kStatusBitErl));
            CHECK(suppressed->enqueueMips64UartReceiverInput(
                {{'E', Ns16550Uart::kLsrParityError}}));
            CHECK(readUartRegister(*suppressed, 2) == 0xC6);
            CHECK((plugin(*suppressed)->core().cp0Cause() & kCauseIp4Bit) != 0);
            CHECK(suppressed->run(50) == 50);
            CHECK(readWord(*suppressed, kHandlerCountPhysical, endian) == 0);
            CHECK(plugin(*suppressed)->core().cp0Cause() & kCauseIp4Bit);
            releaseGuest(*suppressed, endian);
            CHECK(suppressed->run(700) == 700);
            CHECK(readWord(*suppressed, kHandlerCountPhysical, endian) == 1);
            CHECK(readByte(*suppressed, kGuestBufferPhysical) == 'E');
            CHECK(readByte(*suppressed, kGuestBufferPhysical + 1) == 0xE5);
            CHECK((plugin(*suppressed)->core().cp0Cause() & kCauseIp4Bit) == 0);
            CHECK(!plugin(*suppressed)->core().statusExl());
            CHECK(!plugin(*suppressed)->core().statusErl());
        }
    }
}

void testTransactionalReceiverErrorSnapshotReplay() {
    const auto endian = Mips64Endianness::Little;

    // IER[2]-masked PE survives a full-VM snapshot. Guest code enables it,
    // handles the resulting line-status interrupt, and repeats identically.
    auto masked = makeVm(endian, 0x01, kStatusIm4MaskedIe, kStatusIm4Ie,
                         true, true, 0, 1, 0x41, 6, true);
    CHECK(masked != nullptr);
    if (!masked) return;
    runToReady(*masked);
    CHECK(masked->enqueueMips64UartReceiverInput({{'M', Ns16550Uart::kLsrParityError}}));
    CHECK(readUartRegister(*masked, 2) == 0xC1);
    const Mips64FullVmSnapshot maskedPending = masked->captureMips64FullSnapshot();
    CHECK(maskedPending.formatVersion == 2 && maskedPending.uartComponentVersion == 4);
    CHECK(maskedPending.uart.receiveFifo.size() == 1);
    CHECK(maskedPending.uart.receiveFifo[0].errorFlags == Ns16550Uart::kLsrParityError);
    CHECK(!maskedPending.routedUartInterruptLevel);
    releaseGuest(*masked, endian);
    CHECK(masked->run(600) == 600);
    const Mips64FullVmSnapshot maskedComplete = masked->captureMips64FullSnapshot();
    CHECK(readByte(*masked, kIirPhysical) == 0xC6);
    CHECK(readByte(*masked, kGuestBufferPhysical) == 'M');
    CHECK(readByte(*masked, kGuestBufferPhysical + 1) == 0xE5);
    CHECK(masked->restoreMips64FullSnapshot(maskedPending));
    releaseGuest(*masked, endian);
    CHECK(masked->run(600) == 600);
    CHECK(sameReplayState(maskedComplete, masked->captureMips64FullSnapshot()));

    // A line-status error at FIFO head competes with RDA and timer IP7. The
    // genuine guest sees IIR C6, acknowledges both independent sources, and
    // replays the same byte/error records after restoration.
    auto head = makeVm(endian, 0x05, kStatusIm4Im7IeMasked,
                       kStatusIm4Im7Ie, true, false, 20, 1, 1, 6, true);
    CHECK(head != nullptr);
    if (!head) return;
    runToReady(*head);
    const std::vector<Ns16550RxInput> headBatch{
        {'A', Ns16550Uart::kLsrParityError},
        {'B', 0},
        {'C', Ns16550Uart::kLsrFramingError},
        {'X', Ns16550Uart::kLsrBreakIndication}};
    CHECK(head->enqueueMips64UartReceiverInput(headBatch));
    CHECK(readUartRegister(*head, 2) == 0xC6);
    CHECK((plugin(*head)->core().cp0Cause() &
           (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi)) ==
          (kCauseIp4Bit | kCauseIp7Bit | kCauseBitTi));
    const Mips64FullVmSnapshot atErrorHead = head->captureMips64FullSnapshot();
    CHECK(atErrorHead.uart.receiveFifo.front().errorFlags == Ns16550Uart::kLsrParityError);
    releaseGuest(*head, endian);
    CHECK(head->run(1000) == 1000);
    const Mips64FullVmSnapshot headComplete = head->captureMips64FullSnapshot();
    CHECK(readWord(*head, kHandlerCountPhysical, endian) == 1);
    CHECK(readByte(*head, kIirPhysical) == 0xC6);
    CHECK(readWord(*head, kCausePhysical, endian) & kCauseIp7Bit);
    CHECK(head->restoreMips64FullSnapshot(atErrorHead));
    releaseGuest(*head, endian);
    CHECK(head->run(1000) == 1000);
    CHECK(sameReplayState(headComplete, head->captureMips64FullSnapshot()));

    // A clean head with a later FE keeps bit 7 set without prematurely
    // raising RLS. Popping the clean byte exposes FE at the new FIFO head.
    auto deep = makeVm(endian, 0x05, kStatusIm4MaskedIe, kStatusIm4Ie,
                       true, false, 0, 1, 0x41, 6, true);
    CHECK(deep != nullptr);
    if (!deep) return;
    runToReady(*deep);
    CHECK(deep->enqueueMips64UartReceiverInput({
        {'A', 0}, {'C', Ns16550Uart::kLsrFramingError}}));
    CHECK(readUartRegister(*deep, 2) == 0xC1);
    CHECK(readUartRegister(*deep, 5) == 0xE1);
    const Mips64FullVmSnapshot errorDeeper = deep->captureMips64FullSnapshot();
    CHECK(readUartRegister(*deep, 0) == 'A');
    CHECK(deep->run(1) == 1); // Publishes the newly exposed error to CPU0 INT2.
    CHECK(readUartRegister(*deep, 2) == 0xC6);
    const Mips64FullVmSnapshot exposedError = deep->captureMips64FullSnapshot();
    CHECK(deep->restoreMips64FullSnapshot(errorDeeper));
    CHECK(readUartRegister(*deep, 0) == 'A');
    CHECK(deep->run(1) == 1);
    CHECK(sameReplayState(exposedError, deep->captureMips64FullSnapshot()));
    releaseGuest(*deep, endian);
    CHECK(deep->run(600) == 600);
    const Mips64FullVmSnapshot deepComplete = deep->captureMips64FullSnapshot();
    CHECK(readByte(*deep, kGuestBufferPhysical) == 'C');
    CHECK(readByte(*deep, kGuestBufferPhysical + 1) == 0xE9);
    CHECK(deep->restoreMips64FullSnapshot(exposedError));
    releaseGuest(*deep, endian);
    CHECK(deep->run(600) == 600);
    CHECK(sameReplayState(deepComplete, deep->captureMips64FullSnapshot()));

    // Full hardware FIFO and OE latch replay with data preservation. The
    // bounded host staging rejection remains a separate atomic policy.
    auto overrun = makeVm(endian, 0x04, kStatusIm4MaskedIe, kStatusIm4Ie,
                          true, false, 0, 1, 1, 6, false);
    CHECK(overrun != nullptr);
    if (!overrun) return;
    runToReady(*overrun);
    for (uint8_t value = 0; value < Ns16550Uart::kReceiveFifoCapacity; ++value) {
        CHECK(overrun->receiveMips64UartCharacter({value, 0}));
    }
    CHECK(!overrun->receiveMips64UartCharacter({0xEE, 0}));
    CHECK(readUartRegister(*overrun, 2) == 0xC6);
    CHECK((plugin(*overrun)->core().cp0Cause() & kCauseIp4Bit) != 0);
    const Mips64FullVmSnapshot overrunPending = overrun->captureMips64FullSnapshot();
    CHECK(overrunPending.uart.receiveOverrun);
    CHECK(overrunPending.routedUartInterruptLevel);
    CHECK(readUartRegister(*overrun, 5) == 0x63);
    CHECK(readUartRegister(*overrun, 0) == 0);
    CHECK(overrun->run(1) == 1); // UART level deasserts independently of data.
    const Mips64FullVmSnapshot overrunComplete = overrun->captureMips64FullSnapshot();
    CHECK(overrun->restoreMips64FullSnapshot(overrunPending));
    CHECK(readUartRegister(*overrun, 5) == 0x63);
    CHECK(readUartRegister(*overrun, 0) == 0);
    CHECK(overrun->run(1) == 1);
    CHECK(sameReplayState(overrunComplete, overrun->captureMips64FullSnapshot()));

    auto stagingOverflow = makeVm(endian, 0x04, kStatusIm4MaskedIe, kStatusIm4Ie,
                                  true, false, 0, 1, 1, 6, false);
    CHECK(stagingOverflow != nullptr);
    if (!stagingOverflow) return;
    runToReady(*stagingOverflow);
    const std::vector<Ns16550RxInput> tooMany(
        Ns16550Uart::kHostInputCapacity + 1, Ns16550RxInput{0x55, 0});
    CHECK(!stagingOverflow->enqueueMips64UartReceiverInput(tooMany));
    CHECK(stagingOverflow->getMips64MaltaBootstrapBoard()->Uart()->ReceiveFifoSize() == 0);
    CHECK(stagingOverflow->getMips64MaltaBootstrapBoard()->Uart()->PendingHostInputSize() == 0);
    CHECK(readUartRegister(*stagingOverflow, 2) == 0xC6);
    CHECK(readUartRegister(*stagingOverflow, 5) == 0x62);
    CHECK((plugin(*stagingOverflow)->core().cp0Cause() & kCauseIp4Bit) != 0);

    // Break snapshots retain the zero character and BI metadata across a
    // matching independent VM; rejected metadata leaves destination intact.
    auto breakSource = makeVm(endian, 0, kStatusIm4Ie, kStatusIm4Ie,
                              false, false, 0, 1, 1, 4, false);
    CHECK(breakSource != nullptr);
    if (!breakSource) return;
    runToReady(*breakSource);
    CHECK(breakSource->enqueueMips64UartReceiverInput(
        {Ns16550RxInput::BreakEvent()}));
    const Mips64FullVmSnapshot breakPending = breakSource->captureMips64FullSnapshot();
    CHECK(breakPending.uart.receiveFifo.front().data == 0);
    CHECK(breakPending.uart.receiveFifo.front().errorFlags == Ns16550Uart::kLsrBreakIndication);
    auto independent = makeVm(endian, 0, kStatusIm4Ie, kStatusIm4Ie,
                              false, false, 0, 1, 1, 4, false);
    CHECK(independent != nullptr);
    if (!independent) return;
    CHECK(independent->restoreMips64FullSnapshot(breakPending));
    const uint64_t sourceChecksum = breakSource->captureMips64FullSnapshot().integrityChecksum;
    CHECK(readUartRegister(*independent, 5) == 0xF1);
    CHECK(readUartRegister(*independent, 0) == 0);
    const Mips64FullVmSnapshot breakComplete = independent->captureMips64FullSnapshot();
    CHECK(independent->restoreMips64FullSnapshot(breakPending));
    CHECK(readUartRegister(*independent, 5) == 0xF1);
    CHECK(readUartRegister(*independent, 0) == 0);
    CHECK(sameReplayState(breakComplete, independent->captureMips64FullSnapshot()));
    CHECK(breakSource->captureMips64FullSnapshot().integrityChecksum == sourceChecksum);

    const uint64_t beforeBadRestore = independent->captureMips64FullSnapshot().integrityChecksum;
    auto rejectWithoutMutation = [&](Mips64FullVmSnapshot bad) {
        CHECK(bad.RefreshIntegrity());
        std::string error;
        CHECK(!independent->restoreMips64FullSnapshot(bad, &error));
        CHECK(!error.empty());
        CHECK(independent->captureMips64FullSnapshot().integrityChecksum == beforeBadRestore);
    };
    Mips64FullVmSnapshot badFlags = breakPending;
    badFlags.uart.receiveFifo[0].errorFlags = 0x80;
    rejectWithoutMutation(std::move(badFlags));
    Mips64FullVmSnapshot badBreakData = breakPending;
    badBreakData.uart.receiveFifo[0].data = 'X';
    rejectWithoutMutation(std::move(badBreakData));
    Mips64FullVmSnapshot oldUartVersion = breakPending;
    oldUartVersion.uartComponentVersion = 3;
    rejectWithoutMutation(std::move(oldUartVersion));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "guest") testProductionGuestEchoBothEndiansAndMaskedSources();
    else if (mode == "snapshot") testTransactionalReceiveSnapshotReplay();
    else if (mode == "timeout_guest") testProductionTimeoutGuestAndThresholdSemantics();
    else if (mode == "timeout_snapshot") testTransactionalTimeoutSnapshotReplay();
    else if (mode == "line_status_guest") testProductionGuestLineStatusAndTimeout();
    else if (mode == "line_status_snapshot") testTransactionalReceiverErrorSnapshotReplay();
    else return 2;
    std::cout << "MIPS64 IRQ VM[" << mode << "]: " << gPass
              << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
