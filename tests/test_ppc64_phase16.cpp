#include "PPC64HostConsole.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64XICS.h"
#include "VirtualMachine.h"
#include "logger.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
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
uint32_t stw(uint8_t rs, uint8_t ra, int16_t displacement) {
    return (36U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}
uint32_t mfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}
uint32_t mtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}

std::vector<uint8_t> encode(const std::vector<uint32_t>& words) {
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

bool prepare(VirtualMachine& vm, const std::vector<uint32_t>& mainCode) {
    if (!vm.init()) return false;
    std::string error;
    return vm.preparePPC64Platform(encode(mainCode), kEntry, kEntry, &error);
}

std::vector<uint32_t> echoMain() {
    std::vector<uint32_t> words{
        addi(4, 0, 0xff), addi(3, 0, PPC64_H_CPPR), kSc1,
        mfmsr(6), ori(6, 6, static_cast<uint16_t>(MSR_EE)), mtmsr(6),
    };
    for (size_t i = 0; i < 512; ++i) words.push_back(addi(10, 10, 1));
    words.push_back(0x4bfffffcU);
    return words;
}

std::vector<uint32_t> echoHandler() {
    return {addi(3, 0, PPC64_H_XIRR), kSc1, addi(8, 4, 0),
            addis(4, 0, 0x3000), addi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            addi(9, 4, 0), addi(10, 5, 0), addi(11, 6, 0),
            addis(4, 0, 0x3000), addi(5, 9, 0), addi(6, 10, 0),
            addi(7, 11, 0), addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            addi(4, 8, 0), addi(3, 0, PPC64_H_EOI), kSc1, kRfid};
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

std::vector<uint32_t> putProgram(size_t calls) {
    std::vector<uint32_t> words{
        addis(4, 0, 0x3000), addi(5, 0, 16),
        addi(6, 0, 0), addi(7, 0, 0),
    };
    for (size_t i = 0; i < calls; ++i) {
        words.insert(words.end(), {
            addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            stw(3, 0, 0x300), addi(6, 6, 1),
        });
    }
    return words;
}

std::vector<uint32_t> putRetryProgram(size_t calls) {
    std::vector<uint32_t> words{
        addis(4, 0, 0x3000), addi(5, 0, 16),
        addi(6, 0, 0), addi(7, 0, 0),
    };
    for (size_t i = 0; i < calls; ++i) {
        const size_t retryStart = words.size();
        words.insert(words.end(), {
            addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            stw(3, 0, 0x300),
            (11U << 26) | (3U << 16) | PPC64_H_BUSY,
        });
        const int32_t displacement = -static_cast<int32_t>(
            (words.size() - retryStart) * 4U);
        words.push_back((16U << 26) | (12U << 21) | (2U << 16) |
                        (static_cast<uint32_t>(displacement) & 0xfffcU));
        words.push_back(addi(6, 6, 1));
    }
    return words;
}

uint32_t readGuestWord(IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

class ControlledAsyncTransport final : public IHostConsoleTransport,
                                       public IAsyncHostConsoleOutput {
public:
    explicit ControlledAsyncTransport(size_t capacity = 4096)
        : capacity_(capacity) {}

    bool attach(std::string& error) override {
        if (shutdown_) { error = "transport shut down"; return false; }
        attached_ = true;
        error.clear();
        return true;
    }
    void detach() noexcept override { attached_ = false; }
    void resetInput() noexcept override { input_.clear(); }
    HostConsoleIOResult readInput(uint8_t* destination,
                                  size_t maximumBytes) override {
        if (!attached_) return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
        const size_t count = std::min(maximumBytes, input_.size());
        for (size_t i = 0; i < count; ++i) {
            destination[i] = input_.front();
            input_.pop_front();
        }
        return count == 0
            ? HostConsoleIOResult{HostConsoleIOStatus::WOULD_BLOCK, 0, 0}
            : HostConsoleIOResult{HostConsoleIOStatus::OK, count, 0};
    }
    HostConsoleIOResult writeOutput(const uint8_t*, size_t) override {
        ++synchronousWrites_;
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
    }
    void shutdown() noexcept override {
        attached_ = false;
        shutdown_ = true;
    }
    bool isAttached() const noexcept override { return attached_ && !shutdown_; }
    bool usesAsyncOutput() const noexcept override { return true; }

    HostConsoleOutputSubmission submitOutput(const uint8_t* source,
                                              size_t count) override {
        if (!attached_ || shutdown_)
            return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
        if (count == 0 || source == nullptr)
            return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
        if (count > capacity_ || pending_ || completion_.ready)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        pendingBytes_.assign(source, source + count);
        pending_ = true;
        state_ = HostConsoleOutputWorkerState::WRITING;
        ++submissions_;
        return {HostConsoleIOStatus::OK, count, 0};
    }

    HostConsoleOutputCompletion pollOutputCompletion() override {
        if (!completion_.ready) return {};
        auto result = completion_;
        completion_ = {};
        pending_ = false;
        pendingBytes_.clear();
        if (stopped_) state_ = HostConsoleOutputWorkerState::STOPPED;
        else if (result.status == HostConsoleIOStatus::DISCONNECTED ||
                 result.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR)
            state_ = HostConsoleOutputWorkerState::FAILED;
        else state_ = HostConsoleOutputWorkerState::IDLE;
        return result;
    }

    HostConsoleOutputWorkerSnapshot outputWorkerSnapshot() const noexcept override {
        return {state_, capacity_, pending_ ? pendingBytes_.size() : 0};
    }

    HostConsoleOutputStopResult stopOutput(
        bool cancel, std::chrono::milliseconds) noexcept override {
        if (completion_.ready) {
            stopped_ = true;
            return {true, cancel || cancellationRequested_, 0};
        }
        if (!pending_) {
            stopped_ = true;
            state_ = HostConsoleOutputWorkerState::STOPPED;
            return {true, cancel, 0};
        }
        if (cancel) {
            cancellationRequested_ = true;
            state_ = HostConsoleOutputWorkerState::STOPPING;
            if (completeCancellation_) {
                finish(0, pendingBytes_.size(), 0,
                       HostConsoleIOStatus::SHUTDOWN);
                stopped_ = true;
                return {true, true, 0};
            }
        }
        return {false, cancel, 0};
    }

    bool waitForOutputCompletion(std::chrono::milliseconds) override {
        return completion_.ready;
    }

    void finish(size_t confirmed, size_t uncertain, size_t notAttempted,
                HostConsoleIOStatus status = HostConsoleIOStatus::OK,
                size_t uncertainActuallyDelivered = 0) {
        CHECK(pending_);
        CHECK(confirmed + uncertain + notAttempted == pendingBytes_.size());
        const size_t delivered = std::min(uncertain, uncertainActuallyDelivered);
        sinkBytes_.insert(sinkBytes_.end(), pendingBytes_.begin(),
                          pendingBytes_.begin() + static_cast<std::ptrdiff_t>(
                              confirmed + delivered));
        completion_ = {true, confirmed, uncertain, notAttempted, status, 0};
        state_ = status == HostConsoleIOStatus::DISCONNECTED ||
                 status == HostConsoleIOStatus::UNRECOVERABLE_ERROR
            ? HostConsoleOutputWorkerState::FAILED
            : HostConsoleOutputWorkerState::COMPLETED;
    }

    void setCancellationCompletes(bool value) { completeCancellation_ = value; }
    size_t submissions() const { return submissions_; }
    size_t synchronousWrites() const { return synchronousWrites_; }
    bool hasPending() const { return pending_; }
    const std::vector<uint8_t>& sinkBytes() const { return sinkBytes_; }

private:
    size_t capacity_;
    bool attached_ = false;
    bool shutdown_ = false;
    bool pending_ = false;
    bool stopped_ = false;
    bool cancellationRequested_ = false;
    bool completeCancellation_ = true;
    size_t submissions_ = 0;
    size_t synchronousWrites_ = 0;
    std::deque<uint8_t> input_;
    std::vector<uint8_t> pendingBytes_;
    std::vector<uint8_t> sinkBytes_;
    HostConsoleOutputCompletion completion_;
    HostConsoleOutputWorkerState state_ = HostConsoleOutputWorkerState::IDLE;
};

void pumpSteps(VirtualMachine& vm, PPC64ConsoleBridge& bridge, size_t count,
               size_t outputChunk = 16) {
    for (size_t i = 0; i < count; ++i) {
        if (!vm.step()) break;
        bridge.poll(0, outputChunk);
    }
}

void testBoundedWorkerAccountingAndBackpressure() {
    std::puts("[phase16-A-through-F-bounded-output-accounting]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm, putProgram(300)));
    auto transport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 16);
    std::string error;
    CHECK(bridge.attach(error));
    pumpSteps(vm, bridge, 4U + 300U * 4U);

    const auto blocked = bridge.status();
    CHECK(transport->submissions() == 1);
    CHECK(transport->synchronousWrites() == 0);
    CHECK(blocked.outputHandoffCapacity == 16);
    CHECK(blocked.outputHandoffBytesPending == 16);
    CHECK(blocked.outputBytesHandedToWorker == 16);
    CHECK(blocked.guestOutputBytesAcceptedByHost == 0);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() ==
          PPC64_TERMINAL_QUEUE_CAPACITY);
    CHECK(readGuestWord(vm.getMemory(), 0x300) == PPC64_H_BUSY);

    const uint64_t cycles = vm.getCyclesExecuted();
    std::string checkpointError;
    CHECK(!bridge.writeCheckpoint("phase16-pending-output.chk", "phase16",
                                  &checkpointError));
    CHECK(checkpointError.find("stdout") != std::string::npos);
    CHECK(!vm.writeDiagnosticCheckpoint("phase16-pending-output-direct.chk",
                                         "phase16", &checkpointError));
    CHECK(vm.getPPC64TerminalPendingOutputBytes() ==
          PPC64_TERMINAL_QUEUE_CAPACITY);
    CHECK(vm.pause());
    CHECK(!vm.step());
    bridge.poll(0, 16);
    CHECK(vm.getCyclesExecuted() == cycles);
    CHECK(transport->submissions() == 1);
    CHECK(vm.resume());

    std::vector<uint8_t> expected;
    expected.reserve(PPC64_TERMINAL_QUEUE_CAPACITY);
    for (uint64_t value = 0; value < 256; ++value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            expected.push_back(static_cast<uint8_t>(value >> shift));
        expected.insert(expected.end(), 8, 0);
    }
    for (size_t block = 0; block < 256; ++block) {
        CHECK(transport->hasPending());
        transport->finish(16, 0, 0);
        bridge.poll(0, 16);
    }
    if (transport->sinkBytes() != expected || transport->submissions() != 256 ||
        bridge.status().guestOutputBytesAcceptedByHost != expected.size() ||
        vm.getPPC64TerminalPendingOutputBytes() != 0) {
        const auto recoveredStatus = bridge.status();
        std::printf("simulated recovery diagnostics: submitted=%zu sink=%zu confirmed=%llu terminal=%zu state=%u\n",
                    transport->submissions(), transport->sinkBytes().size(),
                    static_cast<unsigned long long>(
                        recoveredStatus.guestOutputBytesAcceptedByHost),
                    vm.getPPC64TerminalPendingOutputBytes(),
                    static_cast<unsigned>(recoveredStatus.state));
    }
    CHECK(transport->sinkBytes() == expected);
    CHECK(transport->submissions() == 256);
    CHECK(bridge.status().guestOutputBytesAcceptedByHost == expected.size());
    CHECK(bridge.status().outputBytesWithUncertainDelivery == 0);
    CHECK(bridge.status().outputBytesReportedNotAttempted == 0);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));
}

void testZeroProgressRetry() {
    std::puts("[phase16-D-zero-progress-retry]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm, putProgram(1)));
    auto transport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 16);
    std::string error;
    CHECK(bridge.attach(error));
    pumpSteps(vm, bridge, 8);
    CHECK(transport->submissions() == 1);

    transport->finish(0, 0, 16, HostConsoleIOStatus::WOULD_BLOCK);
    bridge.poll(0, 16);
    CHECK(bridge.status().guestOutputBytesAcceptedByHost == 0);
    CHECK(bridge.status().outputBytesReportedNotAttempted == 16);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 16);

    const auto retryDeadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(1);
    while (transport->submissions() == 1 &&
           std::chrono::steady_clock::now() < retryDeadline) {
        bridge.poll(0, 16);
        std::this_thread::yield();
    }
    CHECK(transport->submissions() == 2);
    CHECK(transport->hasPending());
    transport->finish(16, 0, 0);
    bridge.poll(0, 16);
    CHECK(bridge.status().guestOutputBytesAcceptedByHost == 16);
    CHECK(bridge.status().outputBytesReportedNotAttempted == 16);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(transport->sinkBytes() == std::vector<uint8_t>(16, 0));
    CHECK(bridge.detach(false, &error));
}

void testGuestHBusyRetryAfterRecovery() {
    std::puts("[phase16-C-guest-retries-H_BUSY-after-reader-recovery]");
    constexpr size_t calls = 300;
    const auto program = putRetryProgram(calls);
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm, program));
    auto transport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 16);
    std::string error;
    CHECK(bridge.attach(error));

    const uint64_t endNIP = kEntry + program.size() * 4U;
    bool observedBusy = false;
    bool releasedReader = false;
    size_t busyControlSteps = 0;
    bool completed = false;
    for (size_t step = 0; step < 100000; ++step) {
        auto* plugin = vm.getPPC64ISAPlugin();
        if (plugin->getNIP() < endNIP && !vm.step()) break;
        bridge.poll(0, 16);

        if (vm.getPPC64TerminalPendingOutputBytes() ==
                PPC64_TERMINAL_QUEUE_CAPACITY &&
            readGuestWord(vm.getMemory(), 0x300) == PPC64_H_BUSY) {
            observedBusy = true;
            if (!releasedReader && busyControlSteps < 32) {
                ++busyControlSteps;
                continue;
            }
            releasedReader = true;
        }

        if (releasedReader && transport->hasPending()) {
            transport->finish(16, 0, 0);
            bridge.poll(0, 16);
        }
        if (releasedReader && plugin->getNIP() >= endNIP &&
            !transport->hasPending() &&
            vm.getPPC64TerminalPendingOutputBytes() == 0) {
            completed = true;
            break;
        }
    }

    std::vector<uint8_t> expected;
    expected.reserve(calls * 16U);
    for (uint64_t value = 0; value < calls; ++value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            expected.push_back(static_cast<uint8_t>(value >> shift));
        expected.insert(expected.end(), 8, 0);
    }
    CHECK(observedBusy);
    CHECK(busyControlSteps == 32);
    CHECK(releasedReader);
    CHECK(completed);
    CHECK(transport->sinkBytes() == expected);
    CHECK(transport->submissions() == calls);
    CHECK(readGuestWord(vm.getMemory(), 0x300) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));
}

void testFailureAccountingAndPendingTeardown() {
    std::puts("[phase16-GH-failure-cancellation-and-teardown]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm, putProgram(1)));
    auto transport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 16);
    std::string error;
    CHECK(bridge.attach(error));
    pumpSteps(vm, bridge, 8);
    CHECK(transport->hasPending());
    transport->finish(4, 4, 8, HostConsoleIOStatus::DISCONNECTED, 2);
    bridge.poll(0, 16);
    const auto status = bridge.status();
    CHECK(status.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED);
    CHECK(status.guestOutputBytesAcceptedByHost == 4);
    CHECK(status.outputBytesWithUncertainDelivery == 4);
    CHECK(status.outputBytesReportedNotAttempted == 8);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 8);
    const size_t submissions = transport->submissions();
    bridge.poll(0, 16);
    CHECK(transport->submissions() == submissions);
    CHECK(bridge.detach(false, &error));

    VirtualMachine pendingVM(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(pendingVM, putProgram(1)));
    auto pendingTransport = std::make_shared<ControlledAsyncTransport>(16);
    pendingTransport->setCancellationCompletes(false);
    PPC64ConsoleBridge pendingBridge(pendingVM, pendingTransport, 0, 4096, 16);
    CHECK(pendingBridge.attach(error));
    pumpSteps(pendingVM, pendingBridge, 8);
    CHECK(!pendingBridge.detach(false, &error));
    CHECK(pendingBridge.status().state ==
          PPC64ConsoleSessionState::OUTPUT_STOP_PENDING);
    CHECK(pendingBridge.status().outputHandoffBytesPending == 16);
    CHECK(pendingVM.drainPPC64TerminalOutput(0, 1).empty());
    pendingTransport->finish(8, 8, 0, HostConsoleIOStatus::SHUTDOWN, 3);
    CHECK(pendingBridge.detach(false, &error));
    CHECK(pendingBridge.status().outputBytesWithUncertainDelivery == 8);
    CHECK(pendingVM.getPPC64TerminalPendingOutputBytes() == 0);
}

void testResetGenerationAndTwoVMIsolation() {
    std::puts("[phase16-FI-generation-and-two-VM-isolation]");
    VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(vm, putProgram(2)));
    auto transport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 16);
    std::string error;
    CHECK(bridge.attach(error));
    pumpSteps(vm, bridge, 8);
    CHECK(transport->hasPending());
    vm.reset();
    pumpSteps(vm, bridge, 8);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 16);
    transport->finish(16, 0, 0);
    bridge.poll(0, 16);
    CHECK(bridge.status().staleGenerationOutputCompletions == 1);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 16);
    CHECK(transport->hasPending()); // The new generation remains queued.
    transport->finish(16, 0, 0);
    bridge.poll(0, 16);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));

    VirtualMachine first(kRamSize, 1, "PPC64", configuration());
    VirtualMachine second(kRamSize, 1, "PPC64", configuration());
    CHECK(prepare(first, putProgram(1)));
    CHECK(prepare(second, putProgram(1)));
    auto firstTransport = std::make_shared<ControlledAsyncTransport>(16);
    auto secondTransport = std::make_shared<ControlledAsyncTransport>(16);
    PPC64ConsoleBridge firstBridge(first, firstTransport, 0, 4096, 16);
    PPC64ConsoleBridge secondBridge(second, secondTransport, 0, 4096, 16);
    CHECK(firstBridge.attach(error));
    CHECK(secondBridge.attach(error));
    pumpSteps(first, firstBridge, 8);
    pumpSteps(second, secondBridge, 8);
    CHECK(firstTransport->hasPending());
    CHECK(secondTransport->hasPending());
    firstTransport->finish(16, 0, 0);
    firstBridge.poll(0, 16);
    CHECK(first.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(second.getPPC64TerminalPendingOutputBytes() == 16);
    secondTransport->finish(16, 0, 0);
    secondBridge.poll(0, 16);
    CHECK(second.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(firstBridge.detach(false, &error));
    CHECK(secondBridge.detach(false, &error));
}

#ifdef _WIN32
void testCharacterDeviceOutputClassification() {
    std::puts("[phase16-handle-character-device-classification]");
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool handlesMade = nullInput != INVALID_HANDLE_VALUE &&
                             nullOutput != INVALID_HANDLE_VALUE;
    bool handlesSet = false;
    bool attached = false;
    bool classified = false;
    bool writeSucceeded = false;
    bool detached = false;
    if (handlesMade) {
        handlesSet = SetStdHandle(STD_INPUT_HANDLE, nullInput) != 0 &&
                     SetStdHandle(STD_OUTPUT_HANDLE, nullOutput) != 0;
        if (handlesSet) {
            StdioHostConsoleTransport transport;
            std::string error;
            attached = transport.attach(error);
            if (attached) {
                classified = !transport.usesAsyncOutput() &&
                    transport.capabilityNote().find(
                        "character-device output") != std::string::npos;
                const std::array<uint8_t, 3> bytes{'N', 0, 'L'};
                const auto result = transport.writeOutput(bytes.data(),
                                                           bytes.size());
                writeSucceeded = result.status == HostConsoleIOStatus::OK &&
                                 result.bytes == bytes.size();
                transport.detach();
                detached = !transport.isAttached();
            }
        }
    }
    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (nullOutput != INVALID_HANDLE_VALUE) CloseHandle(nullOutput);
    CHECK(handlesMade);
    CHECK(handlesSet);
    CHECK(attached);
    CHECK(classified);
    CHECK(writeSucceeded);
    CHECK(detached);
}

void testRealPipeGuestEcho() {
    std::puts("[phase16-A-L-authentic-guest-over-real-stdio-pipes]");
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE inputRead = INVALID_HANDLE_VALUE;
    HANDLE inputWrite = INVALID_HANDLE_VALUE;
    HANDLE outputRead = INVALID_HANDLE_VALUE;
    HANDLE outputWrite = INVALID_HANDLE_VALUE;
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const bool pipesMade =
        CreatePipe(&inputRead, &inputWrite, &security, 4096) != 0 &&
        CreatePipe(&outputRead, &outputWrite, &security, 4096) != 0;
    const std::vector<uint8_t> expected{'P', 0, 'P', 'C', 0xff, '6', '4'};
    DWORD written = 0;
    const bool inputWritten = pipesMade &&
        WriteFile(inputWrite, expected.data(), static_cast<DWORD>(expected.size()),
                  &written, nullptr) && written == expected.size();
    if (inputWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(inputWrite);
        inputWrite = INVALID_HANDLE_VALUE;
    }
    const bool stdHandlesSet = pipesMade && nullInput != INVALID_HANDLE_VALUE &&
        SetStdHandle(STD_INPUT_HANDLE, inputRead) &&
        SetStdHandle(STD_OUTPUT_HANDLE, outputWrite);

    bool attached = false;
    bool exactOutput = false;
    bool leaseExclusive = false;
    bool boundedWorker = false;
    bool prepared = false;
    bool countsCorrect = false;
    bool guestQueueDrained = false;
    bool detached = false;
    DWORD observedAvailable = 0;
    DWORD observedRead = 0;
    bool workerCompleted = false;
    if (stdHandlesSet && inputWritten) {
        Logger::getInstance().setLogLevel(LogLevel::ERROR);
        std::cout.rdbuf(std::cerr.rdbuf());
        VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
        prepared = prepare(vm, echoMain());
        const auto handler = echoHandler();
        if (prepared) writeWords(vm.getMemory(), 0x500, handler);
        auto transport = std::make_shared<StdioHostConsoleTransport>(4096);
        PPC64ConsoleBridge bridge(vm, transport);
        std::string error;
        attached = prepared && bridge.attach(error);
        if (attached) {
            StdioHostConsoleTransport duplicate(4096);
            std::string duplicateError;
            leaseExclusive = !duplicate.attach(duplicateError) &&
                duplicateError.find("owns") != std::string::npos;
            boundedWorker = transport->usesAsyncOutput() &&
                transport->outputWorkerSnapshot().capacity == 4096;
            bool ran = true;
            for (size_t i = 0; i < 6 && ran; ++i) {
                ran = vm.step();
                if (ran) bridge.poll();
            }
            if (ran) {
                ran = vm.step(); // asynchronous XICS vector entry
                if (ran) bridge.poll();
            }
            for (size_t i = 0; i < handler.size() && ran; ++i) {
                ran = vm.step();
                if (ran) bridge.poll();
            }
            workerCompleted = ran &&
                (bridge.status().guestOutputBytesAcceptedByHost == expected.size() ||
                 transport->waitForOutputCompletion(std::chrono::seconds(2)));
            if (workerCompleted) bridge.poll();
            std::array<uint8_t, 64> actual{};
            if (workerCompleted && PeekNamedPipe(outputRead, nullptr, 0, nullptr,
                                                  &observedAvailable, nullptr) &&
                observedAvailable <= actual.size() &&
                ReadFile(outputRead, actual.data(), observedAvailable,
                         &observedRead, nullptr)) {
                exactOutput = observedRead == expected.size() &&
                    std::equal(expected.begin(), expected.end(), actual.begin());
            }
            countsCorrect = bridge.status().guestOutputBytesAcceptedByHost ==
                            expected.size();
            guestQueueDrained = vm.getPPC64TerminalPendingOutputBytes() == 0;
            detached = bridge.detach(false, &error);
        }
    }

    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (inputRead != INVALID_HANDLE_VALUE) CloseHandle(inputRead);
    if (inputWrite != INVALID_HANDLE_VALUE) CloseHandle(inputWrite);
    if (outputRead != INVALID_HANDLE_VALUE) CloseHandle(outputRead);
    if (outputWrite != INVALID_HANDLE_VALUE) CloseHandle(outputWrite);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CHECK(pipesMade);
    CHECK(inputWritten);
    CHECK(stdHandlesSet);
    CHECK(attached);
    CHECK(prepared);
    CHECK(leaseExclusive);
    CHECK(boundedWorker);
    CHECK(countsCorrect);
    CHECK(guestQueueDrained);
    CHECK(detached);
    if (!exactOutput) {
        std::printf("real pipe echo diagnostics: worker=%d available=%lu read=%lu expected=%zu\n",
                    workerCompleted ? 1 : 0,
                    static_cast<unsigned long>(observedAvailable),
                    static_cast<unsigned long>(observedRead), expected.size());
    }
    CHECK(exactOutput);
}

void testRealPipeStallRecoveryAndCancellation() {
    std::puts("[phase16-BCH-stalled-reader-recovery-and-cancel]");
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const bool made = nullInput != INVALID_HANDLE_VALUE &&
        CreatePipe(&pipeRead, &pipeWrite, &security, 4096) != 0;
    bool attachOK = false;
    bool stalled = false;
    bool recovered = false;
    bool canceledSafely = false;
    if (made && SetStdHandle(STD_INPUT_HANDLE, nullInput) &&
        SetStdHandle(STD_OUTPUT_HANDLE, pipeWrite)) {
        StdioHostConsoleTransport transport(4096);
        std::string error;
        attachOK = transport.attach(error) && transport.usesAsyncOutput();
        if (attachOK) {
            constexpr size_t capacity = 4096;
            constexpr size_t maximumBlocks = 64;
            std::array<uint8_t, capacity> block{};
            std::vector<uint8_t> expected;
            std::vector<uint8_t> actual;
            size_t completeBlocks = 0;
            bool foundBlockedWrite = false;
            for (; completeBlocks < maximumBlocks; ++completeBlocks) {
                block.fill(static_cast<uint8_t>(completeBlocks));
                const auto queued = transport.submitOutput(block.data(), block.size());
                if (queued.queuedBytes != block.size()) break;
                if (!transport.waitForOutputCompletion(
                        std::chrono::milliseconds(100))) {
                    const auto snapshot = transport.outputWorkerSnapshot();
                    foundBlockedWrite =
                        snapshot.state == HostConsoleOutputWorkerState::WRITING &&
                        snapshot.bufferedBytes == capacity;
                    ++completeBlocks; // This write is in flight, not completed.
                    break;
                }
                const auto completion = transport.pollOutputCompletion();
                if (!completion.ready || completion.status != HostConsoleIOStatus::OK ||
                    completion.confirmedBytes != capacity) break;
            }
            stalled = foundBlockedWrite;

            if (completeBlocks != 0) {
                const size_t target = completeBlocks * capacity;
                const auto readerDeadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(3);
                std::array<uint8_t, 2048> chunk{};
                while (actual.size() < target &&
                       std::chrono::steady_clock::now() < readerDeadline) {
                    DWORD available = 0;
                    if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr,
                                       &available, nullptr)) break;
                    if (available == 0) {
                        Sleep(1);
                        continue;
                    }
                    DWORD count = 0;
                    const DWORD request = static_cast<DWORD>(std::min<size_t>(
                        {chunk.size(), target - actual.size(),
                         static_cast<size_t>(available)}));
                    if (!ReadFile(pipeRead, chunk.data(), request, &count,
                                  nullptr) || count == 0) break;
                    actual.insert(actual.end(), chunk.begin(),
                                  chunk.begin() + count);
                }
                const bool writeCompletedAfterRead = !foundBlockedWrite ||
                    transport.waitForOutputCompletion(std::chrono::seconds(3));
                if (foundBlockedWrite && writeCompletedAfterRead) {
                    const auto completion = transport.pollOutputCompletion();
                    CHECK(completion.ready);
                    CHECK(completion.status == HostConsoleIOStatus::OK);
                    CHECK(completion.confirmedBytes == capacity);
                }
                for (size_t i = 0; i < completeBlocks; ++i)
                    expected.insert(expected.end(), capacity,
                                    static_cast<uint8_t>(i));
                recovered = writeCompletedAfterRead && actual == expected;
            }

            // Refill the pipe after recovery, then verify stop waits for the
            // actual worker's cancellation completion.
            bool cancelWritePending = false;
            if (recovered) {
                for (size_t i = 0; i < maximumBlocks; ++i) {
                    block.fill(0xa5);
                    const auto queued = transport.submitOutput(block.data(),
                                                               block.size());
                    if (queued.queuedBytes != block.size()) break;
                    if (!transport.waitForOutputCompletion(
                            std::chrono::milliseconds(100))) {
                        cancelWritePending =
                            transport.outputWorkerSnapshot().state ==
                                HostConsoleOutputWorkerState::WRITING;
                        break;
                    }
                    (void)transport.pollOutputCompletion();
                }
            }
            if (cancelWritePending) {
                const auto stop = transport.stopOutput(true,
                    std::chrono::seconds(2));
                if (stop.completed) {
                    const auto completion = transport.pollOutputCompletion();
                    canceledSafely = completion.ready &&
                        completion.confirmedBytes + completion.uncertainBytes +
                            completion.notAttemptedBytes == capacity;
                }
            }
            transport.shutdown();
        }
    }
    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (pipeRead != INVALID_HANDLE_VALUE) CloseHandle(pipeRead);
    if (pipeWrite != INVALID_HANDLE_VALUE) CloseHandle(pipeWrite);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CHECK(made);
    CHECK(attachOK);
    CHECK(stalled);
    CHECK(recovered);
    CHECK(canceledSafely);
}

void testRealGuestStallRecoveryAndControl() {
    std::puts("[phase16-BCE-authentic-guest-stall-recovery-control-checkpoint]");
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const bool made = nullInput != INVALID_HANDLE_VALUE &&
        CreatePipe(&pipeRead, &pipeWrite, &security, 4096) != 0;

    bool completedGuestSteps = false;
    bool reachedBlockedWorker = false;
    bool guestFIFOFull = false;
    bool checkpointRejectedPromptly = false;
    bool pauseResponsive = false;
    bool exactRecovery = false;
    bool detached = false;
    if (made && SetStdHandle(STD_INPUT_HANDLE, nullInput) &&
        SetStdHandle(STD_OUTPUT_HANDLE, pipeWrite)) {
        constexpr size_t guestCalls = 4096;
        VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
        const bool prepared = prepare(vm, putProgram(guestCalls));
        CHECK(prepared);
        auto transport = std::make_shared<StdioHostConsoleTransport>(4096);
        PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 4096);
        std::string error;
        const bool attached = prepared && bridge.attach(error);
        CHECK(attached);
        if (attached) {
            const auto start = std::chrono::steady_clock::now();
            const size_t instructionCount = 4U + guestCalls * 4U;
            size_t executed = 0;
            for (; executed < instructionCount; ++executed) {
                if (!vm.step()) break;
                bridge.poll(0, 4096);
            }
            const auto executionTime = std::chrono::steady_clock::now() - start;
            const auto blocked = bridge.status();
            completedGuestSteps = executed == instructionCount &&
                executionTime < std::chrono::seconds(5);
            reachedBlockedWorker = blocked.outputWorkerState ==
                    HostConsoleOutputWorkerState::WRITING &&
                blocked.outputHandoffBytesPending != 0 &&
                blocked.outputHandoffCapacity == 4096;
            guestFIFOFull = vm.getPPC64TerminalPendingOutputBytes() ==
                    PPC64_TERMINAL_QUEUE_CAPACITY &&
                blocked.terminalOutputBytesPending +
                    blocked.outputHandoffBytesPending ==
                    PPC64_TERMINAL_QUEUE_CAPACITY &&
                readGuestWord(vm.getMemory(), 0x300) == PPC64_H_BUSY;

            std::string checkpointError;
            const auto checkpointStart = std::chrono::steady_clock::now();
            const bool checkpointWritten = bridge.writeCheckpoint(
                "phase16-real-pipe-blocked.chk", "phase16-real-pipe",
                &checkpointError);
            checkpointRejectedPromptly = !checkpointWritten &&
                checkpointError.find("stdout") != std::string::npos &&
                std::chrono::steady_clock::now() - checkpointStart <
                    std::chrono::seconds(1);

            const uint64_t cyclesBeforePause = vm.getCyclesExecuted();
            const bool paused = vm.pause();
            bridge.poll(0, 4096);
            pauseResponsive = paused && !vm.step() &&
                vm.getCyclesExecuted() == cyclesBeforePause &&
                bridge.status().outputWorkerState ==
                    HostConsoleOutputWorkerState::WRITING;
            CHECK(vm.resume());

            const size_t expectedSize =
                static_cast<size_t>(blocked.guestOutputBytesAcceptedByHost) +
                blocked.outputHandoffBytesPending +
                blocked.terminalOutputBytesPending;
            std::vector<uint8_t> expected;
            expected.reserve(expectedSize);
            for (uint64_t value = 0; expected.size() < expectedSize; ++value) {
                for (int shift = 56; shift >= 0; shift -= 8)
                    expected.push_back(static_cast<uint8_t>(value >> shift));
                expected.insert(expected.end(), 8, 0);
            }
            std::vector<uint8_t> actual;
            actual.reserve(expectedSize);
            const auto recoveryDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::seconds(10);
            std::array<uint8_t, 2048> chunk{};
            while (actual.size() < expectedSize &&
                   std::chrono::steady_clock::now() < recoveryDeadline) {
                bridge.poll(0, 4096);
                DWORD available = 0;
                if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &available,
                                   nullptr)) break;
                if (available == 0) {
                    std::this_thread::yield();
                    continue;
                }
                const DWORD request = static_cast<DWORD>(std::min<size_t>(
                    {chunk.size(), expectedSize - actual.size(),
                     static_cast<size_t>(available)}));
                DWORD count = 0;
                if (!ReadFile(pipeRead, chunk.data(), request, &count, nullptr) ||
                    count == 0) break;
                actual.insert(actual.end(), chunk.begin(), chunk.begin() + count);
            }
            const auto settledDeadline = std::chrono::steady_clock::now() +
                                         std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < settledDeadline) {
                bridge.poll(0, 4096);
                const auto settled = bridge.status();
                if (settled.guestOutputBytesAcceptedByHost == expectedSize &&
                    settled.outputHandoffBytesPending == 0 &&
                    settled.terminalOutputBytesPending == 0) break;
                std::this_thread::yield();
            }
            const auto finalStatus = bridge.status();
            exactRecovery = actual == expected &&
                finalStatus.guestOutputBytesAcceptedByHost == expectedSize &&
                finalStatus.outputBytesWithUncertainDelivery == 0 &&
                finalStatus.outputHandoffBytesPending == 0 &&
                finalStatus.terminalOutputBytesPending == 0;
            if (!exactRecovery) {
                const auto mismatch = std::mismatch(actual.begin(), actual.end(),
                                                    expected.begin(), expected.end());
                std::printf("real guest recovery diagnostics: actual=%zu expected=%zu confirmed=%llu uncertain=%llu handoff=%zu terminal=%zu state=%u worker=%u mismatch=%zu\n",
                    actual.size(), expected.size(),
                    static_cast<unsigned long long>(
                        finalStatus.guestOutputBytesAcceptedByHost),
                    static_cast<unsigned long long>(
                        finalStatus.outputBytesWithUncertainDelivery),
                    finalStatus.outputHandoffBytesPending,
                    finalStatus.terminalOutputBytesPending,
                    static_cast<unsigned>(finalStatus.state),
                    static_cast<unsigned>(finalStatus.outputWorkerState),
                    static_cast<size_t>(mismatch.first - actual.begin()));
            }
            detached = bridge.detach(false, &error);
        }
    }
    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (pipeRead != INVALID_HANDLE_VALUE) CloseHandle(pipeRead);
    if (pipeWrite != INVALID_HANDLE_VALUE) CloseHandle(pipeWrite);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CHECK(made);
    CHECK(completedGuestSteps);
    CHECK(reachedBlockedWorker);
    CHECK(guestFIFOFull);
    CHECK(checkpointRejectedPromptly);
    CHECK(pauseResponsive);
    CHECK(exactRecovery);
    CHECK(detached);
}

void testRealBrokenPipeClassification() {
    std::puts("[phase16-G-real-broken-pipe]");
    HANDLE oldInput = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE oldOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE pipeRead = INVALID_HANDLE_VALUE;
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const bool made = nullInput != INVALID_HANDLE_VALUE &&
        CreatePipe(&pipeRead, &pipeWrite, &security, 4096) != 0;
    bool disconnected = false;
    bool noRetry = false;
    bool safeDetach = false;
    if (made && SetStdHandle(STD_INPUT_HANDLE, nullInput) &&
        SetStdHandle(STD_OUTPUT_HANDLE, pipeWrite)) {
        VirtualMachine vm(kRamSize, 1, "PPC64", configuration());
        const bool prepared = prepare(vm, putProgram(1));
        auto transport = std::make_shared<StdioHostConsoleTransport>(4096);
        PPC64ConsoleBridge bridge(vm, transport);
        std::string error;
        const bool attached = prepared && bridge.attach(error);
        CHECK(prepared);
        CHECK(attached);
        if (attached) {
            CloseHandle(pipeRead);
            pipeRead = INVALID_HANDLE_VALUE;
            pumpSteps(vm, bridge, 8, 4096);
            const bool completionSignaled =
                transport->waitForOutputCompletion(std::chrono::seconds(2));
            bridge.poll(0, 4096);
            const auto status = bridge.status();
            const bool completionObserved = completionSignaled ||
                status.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED;
            disconnected = completionObserved &&
                status.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED &&
                status.lastOutputStatus == HostConsoleIOStatus::DISCONNECTED &&
                status.guestOutputBytesAcceptedByHost +
                        status.outputBytesWithUncertainDelivery +
                        status.outputBytesReportedNotAttempted == 16;
            if (!disconnected) {
                std::printf("broken pipe diagnostics: observed=%d state=%u output=%u confirmed=%llu uncertain=%llu not-attempted=%llu handed=%llu worker=%u error=%d\n",
                    completionObserved ? 1 : 0,
                    static_cast<unsigned>(status.state),
                    static_cast<unsigned>(status.lastOutputStatus),
                    static_cast<unsigned long long>(
                        status.guestOutputBytesAcceptedByHost),
                    static_cast<unsigned long long>(
                        status.outputBytesWithUncertainDelivery),
                    static_cast<unsigned long long>(
                        status.outputBytesReportedNotAttempted),
                    static_cast<unsigned long long>(
                        status.outputBytesHandedToWorker),
                    static_cast<unsigned>(status.outputWorkerState),
                    status.lastSystemError);
            }
            const uint64_t handed = status.outputBytesHandedToWorker;
            bridge.poll(0, 4096);
            noRetry = bridge.status().outputBytesHandedToWorker == handed;
            safeDetach = bridge.detach(false, &error);
        }
    }
    SetStdHandle(STD_INPUT_HANDLE, oldInput);
    SetStdHandle(STD_OUTPUT_HANDLE, oldOutput);
    if (pipeRead != INVALID_HANDLE_VALUE) CloseHandle(pipeRead);
    if (pipeWrite != INVALID_HANDLE_VALUE) CloseHandle(pipeWrite);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CHECK(made);
    CHECK(disconnected);
    CHECK(noRetry);
    CHECK(safeDetach);
}

void testChildDemoPipeline(const char* demoPath) {
    std::puts("[phase16-L-slow-reader-child-process-pipeline]");
    CHECK(demoPath != nullptr && demoPath[0] != '\0');
    if (demoPath == nullptr || demoPath[0] == '\0') return;

    HANDLE childInputRead = INVALID_HANDLE_VALUE;
    HANDLE parentInputWrite = INVALID_HANDLE_VALUE;
    HANDLE parentOutputRead = INVALID_HANDLE_VALUE;
    HANDLE childOutputWrite = INVALID_HANDLE_VALUE;
    HANDLE nullError = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const bool inputPipe = CreatePipe(&childInputRead, &parentInputWrite,
                                      &security, 4096) != 0;
    const bool outputPipe = inputPipe &&
        CreatePipe(&parentOutputRead, &childOutputWrite, &security, 256) != 0;
    if (outputPipe) {
        SetHandleInformation(parentInputWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(parentOutputRead, HANDLE_FLAG_INHERIT, 0);
        nullError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    PROCESS_INFORMATION process{};
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childInputRead;
    startup.hStdOutput = childOutputWrite;
    startup.hStdError = nullError;
    std::string command = std::string("\"") + demoPath +
                          "\" --max-steps 100000000";
    std::vector<char> commandLine(command.begin(), command.end());
    commandLine.push_back('\0');
    const bool launched = outputPipe && nullError != INVALID_HANDLE_VALUE &&
        CreateProcessA(demoPath, commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (childInputRead != INVALID_HANDLE_VALUE) {
        CloseHandle(childInputRead);
        childInputRead = INVALID_HANDLE_VALUE;
    }
    if (childOutputWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(childOutputWrite);
        childOutputWrite = INVALID_HANDLE_VALUE;
    }

    bool exactBytes = false;
    bool slowReaderStayedLive = false;
    bool writerSucceeded = false;
    bool cleanExit = false;
    std::vector<uint8_t> expected(4096);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<uint8_t>((i * 37U + (i >> 4U)) & 0xffU);
    std::vector<uint8_t> actual;
    if (launched) {
        std::atomic<bool> writerOK{false};
        std::thread writer([&] {
            size_t offset = 0;
            bool ok = true;
            while (offset < expected.size()) {
                const DWORD request = static_cast<DWORD>(std::min<size_t>(
                    1024, expected.size() - offset));
                DWORD count = 0;
                if (!WriteFile(parentInputWrite, expected.data() + offset,
                               request, &count, nullptr) || count == 0) {
                    ok = false;
                    break;
                }
                offset += count;
            }
            writerOK.store(ok && offset == expected.size());
            CloseHandle(parentInputWrite);
        });

        const auto stallDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(10);
        DWORD available = 0;
        while (std::chrono::steady_clock::now() < stallDeadline) {
            if (!PeekNamedPipe(parentOutputRead, nullptr, 0, nullptr,
                               &available, nullptr)) break;
            if (available != 0 ||
                WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
                break;
            std::this_thread::yield();
        }
        if (available != 0) {
            // Deliberately stop consuming after output reaches the pipe. The
            // worker and guest continue until bounded backpressure applies.
            Sleep(100);
            slowReaderStayedLive =
                WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT;
        }

        actual.reserve(expected.size());
        const auto drainDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(45);
        std::array<uint8_t, 1024> chunk{};
        bool timedOut = false;
        for (;;) {
            if (std::chrono::steady_clock::now() >= drainDeadline) {
                timedOut = true;
                break;
            }
            available = 0;
            if (!PeekNamedPipe(parentOutputRead, nullptr, 0, nullptr,
                               &available, nullptr)) break;
            if (available != 0) {
                const DWORD request = static_cast<DWORD>(std::min<size_t>(
                    {chunk.size(), static_cast<size_t>(available),
                     expected.size() - std::min(expected.size(), actual.size())}));
                if (request == 0) break;
                DWORD count = 0;
                if (!ReadFile(parentOutputRead, chunk.data(), request, &count,
                              nullptr) || count == 0) break;
                actual.insert(actual.end(), chunk.begin(), chunk.begin() + count);
                Sleep(1); // controlled slow reader, with bounded total time
                continue;
            }
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
                break;
            std::this_thread::yield();
        }
        if (timedOut) TerminateProcess(process.hProcess, 124);
        writer.join();
        writerSucceeded = writerOK.load();
        const DWORD exitWait = WaitForSingleObject(process.hProcess, 10000);
        DWORD exitCode = 0xffffffffU;
        if (exitWait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess,
                                                             &exitCode))
            cleanExit = exitCode == 0;
        exactBytes = actual == expected;
        if (!exactBytes || !cleanExit) {
            const auto mismatch = std::mismatch(actual.begin(), actual.end(),
                                                expected.begin(), expected.end());
            std::printf("child pipeline diagnostics: actual=%zu expected=%zu writer=%d slow-live=%d exit-wait=%lu exit-code=%lu mismatch=%zu\n",
                actual.size(), expected.size(), writerSucceeded ? 1 : 0,
                slowReaderStayedLive ? 1 : 0,
                static_cast<unsigned long>(exitWait),
                static_cast<unsigned long>(exitCode),
                static_cast<size_t>(mismatch.first - actual.begin()));
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        if (parentInputWrite != INVALID_HANDLE_VALUE)
            CloseHandle(parentInputWrite);
    }
    if (parentOutputRead != INVALID_HANDLE_VALUE) CloseHandle(parentOutputRead);
    if (nullError != INVALID_HANDLE_VALUE) CloseHandle(nullError);
    CHECK(outputPipe);
    CHECK(launched);
    CHECK(slowReaderStayedLive);
    CHECK(writerSucceeded);
    CHECK(exactBytes);
    CHECK(cleanExit);
}
#endif
} // namespace

int runPPC64Phase16Tests(const char* demoPath) {
    checks = 0;
    failures = 0;
    std::puts("PPC64 Phase 16 nonblocking stdout transport acceptance");
    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    std::cout.rdbuf(std::cerr.rdbuf());

    testBoundedWorkerAccountingAndBackpressure();
    testZeroProgressRetry();
    testGuestHBusyRetryAfterRecovery();
    testFailureAccountingAndPendingTeardown();
    testResetGenerationAndTwoVMIsolation();
#ifdef _WIN32
    testCharacterDeviceOutputClassification();
    testRealPipeGuestEcho();
    testRealPipeStallRecoveryAndCancellation();
    testRealGuestStallRecoveryAndControl();
    testRealBrokenPipeClassification();
    if (demoPath != nullptr) testChildDemoPipeline(demoPath);
#endif
    std::printf("PPC64 Phase 16: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
