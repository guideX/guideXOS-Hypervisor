#include "PPC64HostConsole.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64XICS.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

using namespace ia64;
using namespace ia64::ppc64;

namespace {
int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        try {                                                                  \
            if (!(condition)) {                                                \
                ++failures;                                                    \
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                            #condition);                                       \
            }                                                                  \
        } catch (const std::exception& exception) {                            \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s threw %s\n", __FILE__, __LINE__,    \
                        #condition, exception.what());                         \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;

uint32_t addi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | static_cast<uint16_t>(immediate);
}
uint32_t addis(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (15U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | static_cast<uint16_t>(immediate);
}
uint32_t ori(uint8_t rs, uint8_t ra, uint16_t immediate) {
    return (24U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) | immediate;
}
uint32_t mfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}
uint32_t mtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}
std::vector<uint8_t> image(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> result;
    result.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        result.push_back(static_cast<uint8_t>(word >> 24));
        result.push_back(static_cast<uint8_t>(word >> 16));
        result.push_back(static_cast<uint8_t>(word >> 8));
        result.push_back(static_cast<uint8_t>(word));
    }
    return result;
}

PPC64PlatformConfiguration configuration() {
    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.virtualTerminalEnabled = true;
    return config;
}

bool prepare(VirtualMachine& vm) {
    std::vector<uint32_t> words{
        addi(4, 0, 0xff), addi(3, 0, PPC64_H_CPPR), kSc1,
        mfmsr(6), ori(6, 6, static_cast<uint16_t>(MSR_EE)), mtmsr(6),
    };
    for (size_t i = 0; i < 512; ++i) words.push_back(addi(10, 10, 1));
    if (!vm.init()) return false;
    std::string error;
    return vm.preparePPC64Platform(image(words), kEntry, kEntry, &error);
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        const std::array<uint8_t, 4> bytes{
            static_cast<uint8_t>(word >> 24),
            static_cast<uint8_t>(word >> 16),
            static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
        memory.Write(address + i * 4U, bytes.data(), bytes.size());
    }
}

std::vector<uint32_t> echoHandler() {
    return {addi(3, 0, PPC64_H_XIRR), kSc1,
            addi(8, 4, 0), addis(4, 0, 0x3000),
            addi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            addi(9, 4, 0), addi(10, 5, 0), addi(11, 6, 0),
            addis(4, 0, 0x3000), addi(5, 9, 0), addi(6, 10, 0),
            addi(7, 11, 0), addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            addi(4, 8, 0), addi(3, 0, PPC64_H_EOI), kSc1, kRfid};
}

std::vector<uint32_t> drainingHandler(size_t getCalls) {
    std::vector<uint32_t> words{addi(3, 0, PPC64_H_XIRR), kSc1,
                                addi(8, 4, 0)};
    for (size_t i = 0; i < getCalls; ++i) {
        words.push_back(addis(4, 0, 0x3000));
        words.push_back(addi(3, 0, PPC64_H_GET_TERM_CHAR));
        words.push_back(kSc1);
    }
    words.insert(words.end(), {addi(4, 8, 0), addi(3, 0, PPC64_H_EOI),
                               kSc1, kRfid});
    return words;
}

std::vector<uint32_t> acknowledgeOnlyHandler() {
    return {addi(3, 0, PPC64_H_XIRR), kSc1, addi(8, 4, 0),
            addi(4, 8, 0), addi(3, 0, PPC64_H_EOI), kSc1, kRfid};
}

bool step(VirtualMachine& vm) { return vm.step(); }

bool runInterrupt(VirtualMachine& vm, PPC64ConsoleBridge& bridge,
                  size_t handlerInstructions, size_t maxSteps = 3000) {
    if (!vm.step()) return false; // asynchronous vector entry
    bridge.poll();
    for (size_t i = 0; i < handlerInstructions && i < maxSteps; ++i) {
        if (!vm.step()) return false;
        bridge.poll();
    }
    return true;
}

void drainSink(const std::shared_ptr<MemoryHostConsoleTransport>& transport,
               std::vector<uint8_t>& bytes) {
    auto current = transport->takeOutput();
    bytes.insert(bytes.end(), current.begin(), current.end());
}

void programA_HostToGuestEcho() {
    std::puts("[phase15-A-host-to-guest-echo]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, echoHandler());
    auto transport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge bridge(vm, transport);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    const std::vector<uint8_t> input{'g','u','e','s','t',0,'!'};
    CHECK(transport->offerInput(input) == input.size());
    bridge.poll();
    CHECK(vm.getPPC64TerminalPendingInputBytes() == input.size());
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(bridge.status().hostInputBytesRead == input.size());
    CHECK(bridge.status().guestInputBytesAccepted == input.size());
    CHECK(runInterrupt(vm, bridge, echoHandler().size()));
    bridge.poll();
    CHECK(transport->takeOutput() == input);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
}

void programB_PartialGuestAcceptance() {
    std::puts("[phase15-B-partial-guest-acceptance]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, drainingHandler(257));
    auto transport = std::make_shared<MemoryHostConsoleTransport>(64, 32);
    transport->setReadLimit(12);
    PPC64ConsoleBridge bridge(vm, transport, 0, 64, 256);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    constexpr size_t seedCount = PPC64_TERMINAL_QUEUE_CAPACITY - 6;
    std::vector<uint8_t> seed(seedCount, 's');
    CHECK(vm.injectPPC64TerminalInputWithStatus(seed) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(transport->offerInput(std::vector<uint8_t>(12, 't')) == 12);
    bridge.poll();
    CHECK(bridge.status().hostInputBytesRead == 12);
    CHECK(bridge.status().guestInputBytesAccepted == 6);
    CHECK(bridge.stagedInputBytes() == 6);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == PPC64_TERMINAL_QUEUE_CAPACITY);
    const auto xicsBefore = vm.getPPC64XICSSnapshot();
    CHECK(xicsBefore.sources.size() == 1 && xicsBefore.sources[0].pending);
    CHECK(runInterrupt(vm, bridge, drainingHandler(257).size()));
    CHECK(bridge.stagedInputBytes() == 0);
    CHECK(bridge.status().guestInputBytesAccepted == 12);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
}

void programC_HostStagingBackpressure() {
    std::puts("[phase15-C-host-staging-backpressure]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, acknowledgeOnlyHandler());
    auto transport = std::make_shared<MemoryHostConsoleTransport>(16, 8);
    PPC64ConsoleBridge bridge(vm, transport, 0, 8, 8);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    CHECK(vm.injectPPC64TerminalInputWithStatus(
              std::vector<uint8_t>(PPC64_TERMINAL_QUEUE_CAPACITY, 'q')) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(runInterrupt(vm, bridge, acknowledgeOnlyHandler().size()));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == PPC64_TERMINAL_QUEUE_CAPACITY);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(transport->offerInput(std::vector<uint8_t>(8, 'a')) == 8);
    bridge.poll();
    CHECK(bridge.stagedInputBytes() == 8);
    CHECK(bridge.status().hostInputBytesRead == 8);
    CHECK(bridge.status().guestInputBytesAccepted == 0);
    CHECK(transport->offerInput(std::vector<uint8_t>(8, 'b')) == 8);
    bridge.poll();
    CHECK(bridge.stagedInputBytes() == 8);
    CHECK(bridge.status().hostInputBytesRead == 8);
    CHECK(transport->pendingInput() == 8);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    const uint64_t pollingPC = kEntry + 24U;
    writeWords(vm.getMemory(), pollingPC,
        {addis(4, 0, 0x3000), addi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1});
    for (size_t i = 0; i < 3; ++i) {
        CHECK(step(vm));
        bridge.poll();
    }
    CHECK(bridge.status().guestInputBytesAccepted == 16);
    CHECK(bridge.stagedInputBytes() == 0);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == PPC64_TERMINAL_QUEUE_CAPACITY);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);

    CHECK(transport->offerInput(std::vector<uint8_t>(4, 'r')) == 4);
    bridge.poll();
    CHECK(bridge.stagedInputBytes() == 4);
    bool snapshotRejected = false;
    try { (void)vm.captureVMSnapshot(); }
    catch (const std::logic_error&) { snapshotRejected = true; }
    CHECK(snapshotRejected);
    CHECK(bridge.detach(false, &error));
    CHECK(bridge.status().stagedInputBytes == 4);
    CHECK(!vm.writeDiagnosticCheckpoint(
        (std::filesystem::temp_directory_path() / "phase15-detached.gxcp").string(),
        "phase15", &error));
    auto replacement = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge reattached(vm, replacement, 0, 8, 8);
    CHECK(reattached.attach(error));
    CHECK(reattached.stagedInputBytes() == 4);
    CHECK(replacement->offerInput({'s', 't'}) == 2);
    CHECK(replacement->pendingInput() == 2);
    vm.reset();
    reattached.poll();
    CHECK(reattached.stagedInputBytes() == 0);
    CHECK(replacement->pendingInput() == 0);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
}

void programD_OutputBackpressure() {
    std::puts("[phase15-D-output-partial-write]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, echoHandler());
    auto transport = std::make_shared<MemoryHostConsoleTransport>(32, 5);
    transport->setWriteLimit(3);
    transport->setWriteBlocked(true);
    PPC64ConsoleBridge bridge(vm, transport);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    const std::vector<uint8_t> input{'0','1','2','3','4','5','6','7',0,'x'};
    CHECK(transport->offerInput(input) == input.size());
    bridge.poll();
    CHECK(vm.getPPC64TerminalPendingInputBytes() == input.size());
    CHECK(runInterrupt(vm, bridge, echoHandler().size()));
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == input.size());
    std::vector<uint8_t> output;
    transport->setWriteBlocked(false);
    for (size_t i = 0; i < 8 && vm.getPPC64TerminalPendingOutputBytes() != 0; ++i) {
        bridge.poll(0, 4);
        drainSink(transport, output);
    }
    CHECK(output == input);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.status().guestOutputBytesAcceptedByHost == input.size());
}

void programE_PauseResume() {
    std::puts("[phase15-E-pause-resume]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, echoHandler());
    auto transport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge bridge(vm, transport);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    CHECK(vm.pause());
    const uint64_t cycles = vm.getCyclesExecuted();
    const std::vector<uint8_t> input{'p','a','u','s','e'};
    CHECK(transport->offerInput(input) == input.size());
    bridge.poll();
    CHECK(vm.getState() == VMState::PAUSED);
    CHECK(vm.getCyclesExecuted() == cycles);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == input.size());
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.resume());
    CHECK(runInterrupt(vm, bridge, echoHandler().size()));
    bridge.poll();
    CHECK(transport->takeOutput() == input);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
}

void programF_PausedOutputAndProgramG_EOFDisconnect() {
    std::puts("[phase15-FG-paused-output-eof-disconnect]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, echoHandler());
    auto transport = std::make_shared<MemoryHostConsoleTransport>();
    transport->setWriteBlocked(true);
    PPC64ConsoleBridge bridge(vm, transport);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    const std::vector<uint8_t> input{'e','o','f'};
    CHECK(transport->offerInput(input) == input.size());
    transport->closeInput();
    bridge.poll();
    bridge.poll();
    CHECK(bridge.status().state == PPC64ConsoleSessionState::INPUT_EOF);
    CHECK(runInterrupt(vm, bridge, echoHandler().size()));
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == input.size());
    CHECK(vm.pause());
    bridge.poll(); // policy: the sink may drain output while the VM is paused
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == input.size());
    CHECK(vm.resume());
    transport->setWriteBlocked(false);
    bridge.poll();
    CHECK(transport->takeOutput() == input);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));

    auto broken = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge disconnected(vm, broken);
    CHECK(disconnected.attach(error));
    const std::vector<uint8_t> secondInput{'b','a','d'};
    CHECK(broken->offerInput(secondInput) == secondInput.size());
    broken->closeInput();
    disconnected.poll();
    disconnected.poll();
    CHECK(disconnected.status().state == PPC64ConsoleSessionState::INPUT_EOF);
    broken->disconnectOutput();
    CHECK(runInterrupt(vm, disconnected, echoHandler().size()));
    CHECK(disconnected.status().state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == secondInput.size());
    CHECK(vm.pause());
    CHECK(vm.resume());
    CHECK(disconnected.detach(false, &error));
}

void programH_TwoVMIsolationAndLifetime() {
    std::puts("[phase15-H-two-VM-isolation-lifetime]");
    VirtualMachine first(kRamSize, 1, "PPC64", configuration());
    VirtualMachine second(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(first)); CHECK(prepare(second));
    writeWords(first.getMemory(), 0x500, echoHandler());
    writeWords(second.getMemory(), 0x500, echoHandler());
    auto t1 = std::make_shared<MemoryHostConsoleTransport>();
    auto t2 = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge b1(first, t1), b2(second, t2);
    PPC64ConsoleBridge duplicate(first,
        std::make_shared<MemoryHostConsoleTransport>());
    std::string error;
    CHECK(b1.attach(error)); CHECK(b2.attach(error));
    CHECK(!duplicate.attach(error)); // one consumer lease per VM/LPID
    for (size_t i = 0; i < 6; ++i) { CHECK(step(first)); CHECK(step(second)); }
    CHECK(t1->offerInput({'1'}) == 1);
    CHECK(t2->offerInput({'2'}) == 1);
    b1.poll(); b2.poll();
    CHECK(first.getPPC64TerminalPendingInputBytes() == 1);
    CHECK(second.getPPC64TerminalPendingInputBytes() == 1);
    CHECK(runInterrupt(first, b1, echoHandler().size()));
    CHECK(runInterrupt(second, b2, echoHandler().size()));
    b1.poll(); b2.poll();
    CHECK(t1->takeOutput() == std::vector<uint8_t>{'1'});
    CHECK(t2->takeOutput() == std::vector<uint8_t>{'2'});

    auto* firstMemory = dynamic_cast<Memory*>(&first.getMemory());
    CHECK(firstMemory != nullptr);
    CHECK(firstMemory != nullptr && first.getPPC64PartitionManager()->
        configurePartitionTable(*firstMemory, 0x40000, 4, error));
    CHECK(first.getPPC64PartitionManager()->createPartition(7, error) != nullptr);
    CHECK(first.attachPPC64TerminalInterrupt(7, 0x30, &error));
    auto partitionTransport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge partitionBridge(first, partitionTransport, 7);
    CHECK(partitionBridge.attach(error));
    CHECK(partitionTransport->offerInput({'L'}) == 1);
    partitionBridge.poll();
    CHECK(first.getPPC64TerminalPendingInputBytes(0) == 0);
    CHECK(first.getPPC64TerminalPendingInputBytes(7) == 1);
    CHECK(first.getPPC64XICSSnapshot().sources[1].ownerLPID == 7);
    CHECK(first.getPPC64XICSSnapshot().sources[1].pending);
    CHECK(first.getPPC64PartitionManager()->destroyPartition(7));
    CHECK(first.getPPC64TerminalPendingInputBytes(7) == 0);
    CHECK(partitionTransport->offerInput({'?'}) == 1);
    partitionBridge.poll();
    CHECK(partitionBridge.status().state ==
          PPC64ConsoleSessionState::INVALID_BINDING);
    CHECK(partitionBridge.detach(true, &error));

    std::unique_ptr<VirtualMachine> doomed =
        std::make_unique<VirtualMachine>(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(*doomed));
    auto doomedTransport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge doomedBridge(*doomed, doomedTransport);
    CHECK(doomedBridge.attach(error));
    doomed.reset();
    doomedBridge.poll();
    CHECK(doomedBridge.status().state == PPC64ConsoleSessionState::VM_DESTROYED);
    CHECK(!doomedTransport->isAttached());
}

void programI_CheckpointBoundaryAndRestore() {
    std::puts("[phase15-I-checkpoint-quiesce-restore]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm));
    writeWords(vm.getMemory(), 0x500, drainingHandler(2));
    auto transport = std::make_shared<MemoryHostConsoleTransport>(32, 8);
    PPC64ConsoleBridge bridge(vm, transport, 0, 8, 8);
    std::string error;
    CHECK(bridge.attach(error));
    for (size_t i = 0; i < 6; ++i) CHECK(step(vm));
    CHECK(vm.injectPPC64TerminalInputWithStatus(
              std::vector<uint8_t>(PPC64_TERMINAL_QUEUE_CAPACITY, 'c')) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(transport->offerInput(std::vector<uint8_t>(4, 'd')) == 4);
    bridge.poll();
    CHECK(bridge.stagedInputBytes() == 4);
    const auto pendingBefore = vm.getPPC64TerminalPendingInputBytes();
    const auto path = std::filesystem::temp_directory_path() /
                      "guidexos-phase15-quiesce.gxcp";
    CHECK(!vm.writeDiagnosticCheckpoint(path.string(), "phase15", &error));
    CHECK(!bridge.writeCheckpoint(path.string(), "phase15", &error));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == pendingBefore);
    CHECK(bridge.stagedInputBytes() == 4);
    CHECK(runInterrupt(vm, bridge, drainingHandler(2).size()));
    CHECK(bridge.stagedInputBytes() == 0);
    CHECK(bridge.writeCheckpoint(path.string(), "phase15", &error));
    const auto beforeRestore = vm.getPPC64XICSSnapshot();
    const size_t restoredPending = vm.getPPC64TerminalPendingInputBytes();
    CHECK(vm.readDiagnosticCheckpoint(path.string(), "phase15", &error));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == restoredPending);
    CHECK(vm.getPPC64XICSSnapshot().sources.size() ==
          beforeRestore.sources.size());
    const auto events = vm.getPPC64XICSSnapshot().sources[0].pending;
    const auto malformedPath = std::filesystem::temp_directory_path() /
                               "guidexos-phase15-malformed.gxcp";
    std::error_code copyError;
    CHECK(std::filesystem::copy_file(path, malformedPath,
        std::filesystem::copy_options::overwrite_existing, copyError));
    std::fstream malformed(malformedPath,
        std::ios::binary | std::ios::in | std::ios::out);
    char magicByte = 0;
    malformed.read(&magicByte, 1);
    CHECK(malformed.gcount() == 1);
    malformed.clear();
    malformed.seekp(0);
    magicByte ^= 0x01;
    malformed.write(&magicByte, 1);
    malformed.close();
    CHECK(!vm.readDiagnosticCheckpoint(malformedPath.string(), "phase15", &error));
    CHECK(!error.empty());
    CHECK(vm.getPPC64TerminalPendingInputBytes() == restoredPending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending == events);
    bridge.poll();
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending == events);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(malformedPath, ignored);
}

void programJ_StdioAdapterAndProgramK_Compatibility() {
    std::puts("[phase15-JK-stdio-and-compatibility]");
    auto memoryTransport = std::make_shared<MemoryHostConsoleTransport>(3, 4);
    std::string error;
    CHECK(memoryTransport->attach(error));
    CHECK(memoryTransport->offerInput({0, 1, 2, 3, 4}) == 3);
    std::array<uint8_t, 8> input{};
    const auto partialRead = memoryTransport->readInput(input.data(), input.size());
    CHECK(partialRead.status == HostConsoleIOStatus::OK);
    CHECK(partialRead.bytes == 3);
    CHECK(input[0] == 0 && input[1] == 1 && input[2] == 2);
    CHECK(memoryTransport->readInput(input.data(), input.size()).status ==
          HostConsoleIOStatus::WOULD_BLOCK);
    memoryTransport->closeInput();
    CHECK(memoryTransport->readInput(input.data(), input.size()).status ==
          HostConsoleIOStatus::EOF_REACHED);
    const std::array<uint8_t, 6> output{9, 8, 7, 6, 5, 4};
    const auto partialWrite = memoryTransport->writeOutput(output.data(), output.size());
    CHECK(partialWrite.status == HostConsoleIOStatus::OK);
    CHECK(partialWrite.bytes == 4);
    CHECK(memoryTransport->writeOutput(output.data() + 4, 2).status ==
          HostConsoleIOStatus::WOULD_BLOCK);
    CHECK(memoryTransport->takeOutput() ==
          std::vector<uint8_t>({9, 8, 7, 6}));
    memoryTransport->disconnectInput();
    CHECK(memoryTransport->readInput(input.data(), input.size()).status ==
          HostConsoleIOStatus::DISCONNECTED);
    memoryTransport->shutdown();
    CHECK(!memoryTransport->isAttached());

    StdioHostConsoleTransport stdio;
    const bool attached = stdio.attach(error);
    CHECK(attached || !error.empty());
    if (attached) {
        CHECK(stdio.isAttached());
        CHECK(!stdio.capabilityNote().empty());
        stdio.detach();
        CHECK(!stdio.isAttached());
    }
#ifdef _WIN32
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    HANDLE nullOutput = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool pipeCreated = CreatePipe(&pipeRead, &pipeWrite, nullptr, 4096) != 0;
    bool stdioAttached = false;
    bool staleWrite = false;
    bool freshWrite = false;
    HostConsoleIOResult staleRead;
    HostConsoleIOResult freshRead;
    std::array<uint8_t, 8> pipeBytes{};
    const std::array<uint8_t, 4> staleBytes{'o', 'l', 'd', 0};
    const std::array<uint8_t, 3> freshBytes{'n', 'e', 'w'};
    if (pipeCreated && nullOutput != INVALID_HANDLE_VALUE &&
        SetStdHandle(STD_INPUT_HANDLE, pipeRead) &&
        SetStdHandle(STD_OUTPUT_HANDLE, nullOutput)) {
        StdioHostConsoleTransport pipeTransport;
        std::string pipeError;
        stdioAttached = pipeTransport.attach(pipeError);
        if (stdioAttached) {
            DWORD written = 0;
            staleWrite = WriteFile(pipeWrite, staleBytes.data(),
                static_cast<DWORD>(staleBytes.size()), &written, nullptr) != 0 &&
                written == staleBytes.size();
            pipeTransport.resetInput();
            staleRead = pipeTransport.readInput(pipeBytes.data(), pipeBytes.size());
            freshWrite = WriteFile(pipeWrite, freshBytes.data(),
                static_cast<DWORD>(freshBytes.size()), &written, nullptr) != 0 &&
                written == freshBytes.size();
            freshRead = pipeTransport.readInput(pipeBytes.data(), pipeBytes.size());
            pipeTransport.detach();
        }
    }
    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (pipeRead != INVALID_HANDLE_VALUE) CloseHandle(pipeRead);
    if (pipeWrite != INVALID_HANDLE_VALUE) CloseHandle(pipeWrite);
    if (nullOutput != INVALID_HANDLE_VALUE) CloseHandle(nullOutput);
    CHECK(pipeCreated);
    CHECK(nullOutput != INVALID_HANDLE_VALUE);
    CHECK(stdioAttached);
    CHECK(staleWrite);
    CHECK(staleRead.status == HostConsoleIOStatus::WOULD_BLOCK);
    CHECK(freshWrite);
    CHECK(freshRead.status == HostConsoleIOStatus::OK);
    CHECK(freshRead.bytes == freshBytes.size());
    CHECK(std::equal(freshBytes.begin(), freshBytes.end(), pipeBytes.begin()));
#endif
    CHECK(PPC64VirtualTerminal::validTerminalId(
        PPC64_DEFAULT_VIRTUAL_TERMINAL_ID));
    CHECK(PPC64_TERMINAL_QUEUE_CAPACITY == 4096);
    VirtualMachine polling(kRamSize, 1, "PPC64", [] {
        auto config = configuration();
        config.interruptMode = PPC64InterruptMode::LEGACY_EXTERNAL;
        return config;
    }());
    CHECK(prepare(polling));
    CHECK(polling.getPPC64PlatformCapabilities().virtualTerminal);
    CHECK(!polling.getPPC64PlatformCapabilities().interruptController);
}

} // namespace

int main() {
    std::puts("PPC64 Phase 15 host console bridge acceptance");
    programA_HostToGuestEcho();
    programB_PartialGuestAcceptance();
    programC_HostStagingBackpressure();
    programD_OutputBackpressure();
    programE_PauseResume();
    programF_PausedOutputAndProgramG_EOFDisconnect();
    programH_TwoVMIsolationAndLifetime();
    programI_CheckpointBoundaryAndRestore();
    programJ_StdioAdapterAndProgramK_Compatibility();
    std::printf("PPC64 Phase 15: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
