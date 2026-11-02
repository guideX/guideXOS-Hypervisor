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
                          uint32_t compareValue) {
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
    boot.push_back(encI(0x28, t0, t1, 16));   // FCR: 16-byte FIFO, trigger 1.
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
            payload.push_back(encI(0x09, 0, t1, 1));
            payload.push_back(encI(0x28, t0, t1, 8)); // guest enables IER.RDI.
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
    handler.push_back(encI(0x09, 0, t5, 2000));
    handler.push_back(encCp0(4, t5, 11)); // Compare acknowledges timer only.
    const size_t afterTimer = handler.size();
    patchBranch(handler, noTimerBranch, afterTimer, 0x04, t4, 0);
    handler.push_back(encI(0x0F, 0, t0, 0xBF00));
    handler.push_back(encI(0x0D, t0, t0, 0x0900));
    handler.push_back(encI(0x24, t0, t3, 16)); // lbu IIR.
    handler.push_back(encI(0x28, 19, t3, 0x208)); // save IIR.
    handler.push_back(encI(0x0C, t3, t4, 0x0E)); // isolate IIR source id.
    handler.push_back(encI(0x09, 0, t5, 4));
    const size_t noRxBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    const size_t rxLoop = handler.size();
    handler.push_back(encI(0x24, t0, t4, 40)); // lbu LSR.
    handler.push_back(encI(0x0C, t4, t4, 1));  // LSR.DR.
    const size_t emptyBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    handler.push_back(encI(0x24, t0, t1, 0)); // lbu RBR consumes a byte.
    handler.push_back(encI(0x28, 16, t1, 0)); // store into guest RAM.
    handler.push_back(encI(0x09, 16, 16, 1)); // advance receive pointer.
    handler.push_back(encI(0x28, t0, t1, 0)); // echo through guest THR.
    const size_t loopBranch = handler.size();
    handler.push_back(0);
    handler.push_back(0);
    patchBranch(handler, loopBranch, rxLoop, 0x04, 0, 0);
    const size_t done = handler.size();
    patchBranch(handler, noRxBranch, done, 0x05, t4, t5);
    patchBranch(handler, emptyBranch, done, 0x04, t4, 0);
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
                                       size_t cpuCount = 1) {
    GuestImage image = makeGuestImage(endian, initialIer, initialStatus,
                                      releaseStatus, waitForRelease,
                                      enableIerOnRelease, compareValue);
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
               Mips64Endianness endian, bool expectTimerCause = false) {
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
    CHECK(readByte(vm, kIirPhysical) == 0xC4);
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
    badQueue.uart.receiveFifo.resize(Ns16550Uart::kReceiveFifoCapacity + 1, 0xAA);
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

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "guest") testProductionGuestEchoBothEndiansAndMaskedSources();
    else if (mode == "snapshot") testTransactionalReceiveSnapshotReplay();
    else return 2;
    std::cout << "MIPS64 IRQ VM[" << mode << "]: " << gPass
              << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
