#include "PPC64HostConsole.h"
#include "PPC64ISAPlugin.h"
#include "PPC64VirtualTerminal.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
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
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                        #condition);                                           \
        }                                                                      \
    } while (0)

constexpr size_t kRamSize = 2U << 20U;
constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kResult = 0x2000;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint16_t kSprDEC = 22;

uint32_t addi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t addis(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (15U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t ld(uint8_t rt, uint8_t ra, int16_t displacement) {
    return (58U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

uint32_t stw(uint8_t rs, uint8_t ra, int16_t displacement) {
    return (36U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}

uint32_t ori(uint8_t rs, uint8_t ra, uint16_t immediate) {
    return (24U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) | immediate;
}

uint32_t mtspr(uint8_t rs, uint16_t spr) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (467U << 1);
}

uint32_t mfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}

uint32_t mtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}

std::vector<uint8_t> encode(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (const uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

PPC64PlatformConfiguration platformConfiguration() {
    PPC64PlatformConfiguration configuration;
    configuration.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    configuration.virtualTerminalEnabled = true;
    return configuration;
}

std::vector<uint32_t> putTextProgram(const std::string& first,
                                     const std::string& second = {}) {
    auto appendCall = [](std::vector<uint32_t>& words,
                         const std::string& text, int16_t address) {
        words.push_back(addis(4, 0, 0x3000));
        words.push_back(addi(5, 0, static_cast<int16_t>(text.size())));
        words.push_back(addi(8, 0, address));
        words.push_back(ld(6, 8, 0));
        words.push_back(ld(7, 8, 8));
        words.push_back(addi(3, 0, PPC64_H_PUT_TERM_CHAR));
        words.push_back(kSc1);
    };
    std::vector<uint32_t> result;
    appendCall(result, first, 0x4000);
    if (!second.empty()) appendCall(result, second, 0x4010);
    return result;
}

bool prepare(VirtualMachine& vm, const std::vector<uint32_t>& guest) {
    if (!vm.init()) return false;
    std::string error;
    return vm.preparePPC64Platform(encode(guest), kEntry, kEntry, &error);
}

bool prepareTextProgram(VirtualMachine& vm, const std::string& first,
                        const std::string& second = {}) {
    if (!prepare(vm, putTextProgram(first, second))) return false;
    std::array<uint8_t, 16> firstBytes{};
    std::array<uint8_t, 16> secondBytes{};
    std::copy(first.begin(), first.end(), firstBytes.begin());
    std::copy(second.begin(), second.end(), secondBytes.begin());
    vm.getMemory().Write(0x4000, firstBytes.data(), firstBytes.size());
    if (!second.empty())
        vm.getMemory().Write(0x4010, secondBytes.data(), secondBytes.size());
    return true;
}

std::string temporaryCheckpointPath(const char* name) {
    const auto unique = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    return (std::filesystem::temp_directory_path() /
            (std::string("guideXOS-ppc64-phase17-") + name + "-" +
             std::to_string(unique) + ".gxcp")).string();
}

void execute(VirtualMachine& vm, PPC64ConsoleBridge& bridge, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!vm.step()) break;
        bridge.poll(0, 4096);
    }
}

class ControlledAsyncTransport final : public IHostConsoleTransport,
                                       public IAsyncHostConsoleOutput {
public:
    explicit ControlledAsyncTransport(size_t capacity = 4096)
        : capacity_(capacity) {}

    bool attach(std::string& error) override {
        attached_ = true;
        stopped_ = false;
        error.clear();
        return true;
    }

    void detach() noexcept override {
        attached_ = false;
        stopped_ = true;
        state_ = HostConsoleOutputWorkerState::STOPPED;
    }

    void resetInput() noexcept override {}
    HostConsoleIOResult readInput(uint8_t*, size_t) override {
        return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    }
    HostConsoleIOResult writeOutput(const uint8_t*, size_t) override {
        return {HostConsoleIOStatus::DISCONNECTED, 0, 109};
    }
    void shutdown() noexcept override { detach(); }
    bool isAttached() const noexcept override { return attached_; }
    HostConsoleSinkType sinkType() const noexcept override {
        return HostConsoleSinkType::MEMORY;
    }

    bool usesAsyncOutput() const noexcept override { return true; }
    HostConsoleOutputSubmission submitOutput(const uint8_t* source,
                                             size_t maximumBytes) override {
        if (!attached_ || stopped_)
            return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
        if (pending_ || completionReady_ || maximumBytes > capacity_)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        pendingBytes_.assign(source, source + maximumBytes);
        pending_ = true;
        state_ = HostConsoleOutputWorkerState::QUEUED;
        return {HostConsoleIOStatus::OK, maximumBytes, 0};
    }

    HostConsoleOutputCompletion pollOutputCompletion() override {
        if (!completionReady_) return {};
        HostConsoleOutputCompletion result = completion_;
        completion_ = {};
        completionReady_ = false;
        pending_ = false;
        pendingBytes_.clear();
        if (result.status == HostConsoleIOStatus::DISCONNECTED ||
            result.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR)
            state_ = HostConsoleOutputWorkerState::FAILED;
        else
            state_ = stopped_ ? HostConsoleOutputWorkerState::STOPPED
                              : HostConsoleOutputWorkerState::IDLE;
        return result;
    }

    HostConsoleOutputWorkerSnapshot outputWorkerSnapshot() const noexcept override {
        return {state_, capacity_, pending_ ? pendingBytes_.size() : 0};
    }

    HostConsoleOutputStopResult stopOutput(
        bool cancel, std::chrono::milliseconds) noexcept override {
        if (completionReady_) {
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
        }
        return {false, cancel, 0};
    }

    bool waitForOutputCompletion(std::chrono::milliseconds) override {
        return completionReady_;
    }

    void finish(size_t confirmed, size_t uncertain, size_t notAttempted,
                HostConsoleIOStatus status, int error = 0,
                size_t uncertainDelivered = 0) {
        CHECK(pending_);
        CHECK(confirmed + uncertain + notAttempted == pendingBytes_.size());
        const size_t safeConfirmed = std::min(confirmed, pendingBytes_.size());
        sinkBytes_.insert(sinkBytes_.end(), pendingBytes_.begin(),
                          pendingBytes_.begin() +
                              static_cast<std::ptrdiff_t>(safeConfirmed));
        const size_t actualUncertain = std::min(
            {uncertain, uncertainDelivered,
             pendingBytes_.size() - safeConfirmed});
        sinkBytes_.insert(sinkBytes_.end(),
                          pendingBytes_.begin() + static_cast<std::ptrdiff_t>(safeConfirmed),
                          pendingBytes_.begin() +
                              static_cast<std::ptrdiff_t>(safeConfirmed + actualUncertain));
        const bool partial = confirmed != 0 &&
                             confirmed < pendingBytes_.size();
        completion_ = {true, confirmed, uncertain, notAttempted, status, error,
                       partial};
        completionReady_ = true;
        state_ = status == HostConsoleIOStatus::DISCONNECTED ||
                         status == HostConsoleIOStatus::UNRECOVERABLE_ERROR
                     ? HostConsoleOutputWorkerState::FAILED
                     : HostConsoleOutputWorkerState::COMPLETED;
    }

    bool hasPending() const noexcept { return pending_; }
    const std::vector<uint8_t>& sinkBytes() const noexcept { return sinkBytes_; }

private:
    size_t capacity_;
    bool attached_ = false;
    bool stopped_ = false;
    bool pending_ = false;
    bool completionReady_ = false;
    bool cancellationRequested_ = false;
    HostConsoleOutputWorkerState state_ = HostConsoleOutputWorkerState::IDLE;
    std::vector<uint8_t> pendingBytes_;
    std::vector<uint8_t> sinkBytes_;
    HostConsoleOutputCompletion completion_;
};

void testStateModelAndBoundedEvents() {
    std::puts("[phase17-A-state-model-and-diagnostics]");
    PPC64OutputSessionStateMachine model;
    CHECK(model.state() == PPC64OutputSessionState::DETACHED);
    CHECK(model.transition(PPC64OutputSessionState::CONNECTED));
    CHECK(model.transition(PPC64OutputSessionState::PERMANENT_FAILURE));
    CHECK(!model.transition(PPC64OutputSessionState::CONNECTED));
    CHECK(model.state() == PPC64OutputSessionState::PERMANENT_FAILURE);
    CHECK(model.transition(PPC64OutputSessionState::RECONNECT_ELIGIBLE));
    CHECK(model.transition(PPC64OutputSessionState::RECONNECTING));
    CHECK(model.transition(PPC64OutputSessionState::CONNECTED));

    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepare(vm, {addi(10, 0, 0x1234)}));
    auto transport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge bridge(vm, transport);
    std::string error;
    for (size_t i = 0; i < 40; ++i) {
        CHECK(bridge.attach(error));
        CHECK(bridge.detach(false, &error));
    }
    const auto status = bridge.status();
    const auto events = bridge.diagnosticEvents();
    CHECK(status.vmIdentity != 0);
    CHECK(status.sessionGeneration == 40);
    CHECK(status.outputState == PPC64OutputSessionState::CLEANLY_STOPPED);
    CHECK(events.size() == 128);
    CHECK(!events.empty() && events.front().sequence > 1);
    CHECK(std::is_sorted(events.begin(), events.end(),
        [](const auto& a, const auto& b) { return a.sequence < b.sequence; }));
}

void testHealthyOutputAndCleanReconnect() {
    std::puts("[phase17-B-C-healthy-diagnostics-and-clean-reconnect]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(vm, "ABCDEFGH"));
    auto oldSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge bridge(vm, oldSink, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    CHECK(oldSink->hasPending());
    CHECK(bridge.status().writeAttempts == 1);
    oldSink->finish(4, 0, 4, HostConsoleIOStatus::DISCONNECTED, 109);
    bridge.poll(0, 8);
    auto status = bridge.status();
    CHECK(status.outputState == PPC64OutputSessionState::RECONNECT_ELIGIBLE);
    CHECK(status.lastFailure == HostConsoleFailureClass::BROKEN_PIPE);
    CHECK(status.lastSystemError == 109);
    CHECK(status.partialWrites == 1);
    CHECK(status.guestOutputBytesAcceptedByHost == 4);
    CHECK(status.outputBytesReportedNotAttempted == 4);
    CHECK(status.knownNotAttemptedBytes == 4);
    CHECK(status.terminalId == vm.getPPC64VirtualTerminalId());
    CHECK(status.outputLeaseHeld);
    const std::string failedCheckpointPath =
        temporaryCheckpointPath("failed-boundary");
    const bool failedBoundaryCheckpoint = bridge.writeCheckpoint(
        failedCheckpointPath, "phase17-failed", &error);
    if (!failedBoundaryCheckpoint)
        std::printf("failed-boundary checkpoint diagnostic: %s\n", error.c_str());
    CHECK(failedBoundaryCheckpoint);
    if (failedBoundaryCheckpoint)
        std::filesystem::remove(failedCheckpointPath);

    auto newSink = std::make_shared<ControlledAsyncTransport>(8);
    const uint64_t oldGeneration = status.sessionGeneration;
    CHECK(bridge.reconnect(newSink, error));
    status = bridge.status();
    CHECK(status.sessionGeneration == oldGeneration + 1);
    CHECK(status.outputState == PPC64OutputSessionState::CONNECTED);
    CHECK(status.writeAttempts == 1); // Worker submission is not a completion.
    bridge.poll(0, 8);
    CHECK(newSink->hasPending());
    newSink->finish(4, 0, 0, HostConsoleIOStatus::OK);
    bridge.poll(0, 8);
    status = bridge.status();
    CHECK(status.writeCompletions == 2);
    CHECK(status.guestOutputBytesAcceptedByHost == 8);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(std::string(oldSink->sinkBytes().begin(), oldSink->sinkBytes().end()) == "ABCD");
    CHECK(std::string(newSink->sinkBytes().begin(), newSink->sinkBytes().end()) == "EFGH");
    const std::string recoveredCheckpointPath =
        temporaryCheckpointPath("recovered");
    const bool recoveredCheckpoint = bridge.writeCheckpoint(
        recoveredCheckpointPath, "phase17-recovered", &error);
    if (!recoveredCheckpoint)
        std::printf("recovered checkpoint diagnostic: %s\n", error.c_str());
    CHECK(recoveredCheckpoint);
    if (recoveredCheckpoint)
        std::filesystem::remove(recoveredCheckpointPath);
    const auto events = bridge.diagnosticEvents();
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == PPC64ConsoleEventType::RECONNECT_COMPLETED;
    }));
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == PPC64ConsoleEventType::PARTIAL_WRITE;
    }));
    CHECK(bridge.detach(false, &error));
}

void testUncertaintyResolutionAndCheckpoint() {
    std::puts("[phase17-D-E-K-uncertainty-resolution-and-checkpoint]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(vm, "ABCDEFGH"));
    auto oldSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge bridge(vm, oldSink, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    oldSink->finish(2, 3, 3, HostConsoleIOStatus::DISCONNECTED, 109, 1);
    bridge.poll(0, 8);
    auto status = bridge.status();
    CHECK(status.outputState == PPC64OutputSessionState::DELIVERY_UNCERTAIN);
    CHECK(status.unresolvedUncertainBytes == 3);
    CHECK(status.uncertainSegmentId != 0);
    CHECK(status.lastFailure == HostConsoleFailureClass::UNCERTAIN_DELIVERY);
    CHECK(status.outputBytesWithUncertainDelivery == 3);
    CHECK(status.knownNotAttemptedBytes == 3);

    auto newSink = std::make_shared<ControlledAsyncTransport>(8);
    CHECK(!bridge.reconnect(newSink, error));
    CHECK(bridge.status().lastFailure == HostConsoleFailureClass::UNCERTAIN_DELIVERY);
    CHECK(bridge.resolveOutputUncertainty(
        PPC64OutputUncertaintyResolution::KEEP_UNRESOLVED, 0, error));
    auto events = bridge.diagnosticEvents();
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == PPC64ConsoleEventType::UNCERTAINTY_KEPT_UNRESOLVED;
    }));
    CHECK(!bridge.resolveOutputUncertainty(
        PPC64OutputUncertaintyResolution::DISCARD_UNCERTAIN_SEGMENT,
        status.uncertainSegmentId + 1, error));
    CHECK(!bridge.writeCheckpoint("build/phase17-uncertain.gxcp",
                                  "phase17", &error));
    CHECK(bridge.resolveOutputUncertainty(
        PPC64OutputUncertaintyResolution::DISCARD_UNCERTAIN_SEGMENT,
        status.uncertainSegmentId, error));
    events = bridge.diagnosticEvents();
    CHECK(std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.type == PPC64ConsoleEventType::UNCERTAINTY_SEGMENT_DISCARDED &&
               event.uncertainSegmentId == status.uncertainSegmentId;
    }));
    CHECK(bridge.status().unresolvedUncertainBytes == 0);
    CHECK(bridge.status().diagnostic.find("not lossless") != std::string::npos);
    CHECK(bridge.reconnect(newSink, error));
    bridge.poll(0, 8);
    CHECK(newSink->hasPending());
    newSink->finish(3, 0, 0, HostConsoleIOStatus::OK);
    bridge.poll(0, 8);
    CHECK(std::string(newSink->sinkBytes().begin(), newSink->sinkBytes().end()) == "FGH");
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));
}

void testCancellationPendingAndPauseResume() {
    std::puts("[phase17-F-H-cancellation-and-pause-resume]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(vm, "ABCDEFGH"));
    auto sink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge bridge(vm, sink, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    CHECK(!bridge.detach(false, &error));
    auto status = bridge.status();
    CHECK(status.state == PPC64ConsoleSessionState::OUTPUT_STOP_PENDING);
    CHECK(status.outputState == PPC64OutputSessionState::CANCELLATION_PENDING);
    CHECK(status.cancellationRequests == 1);
    CHECK(status.inFlightBytes == 0);
    CHECK(!bridge.writeCheckpoint("build/phase17-cancel-pending.gxcp",
                                  "phase17-cancel-pending", &error));
    sink->finish(0, 0, 8, HostConsoleIOStatus::SHUTDOWN);
    CHECK(bridge.detach(false, &error));
    CHECK(bridge.status().cancellationCompletions != 0);

    VirtualMachine quarantineVM(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(quarantineVM, "QUARANT!"));
    auto quarantineSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge quarantineBridge(
        quarantineVM, quarantineSink, 0, 4096, 8);
    CHECK(quarantineBridge.attach(error));
    execute(quarantineVM, quarantineBridge, 7);
    quarantineBridge.shutdown();
    status = quarantineBridge.status();
    CHECK(status.outputState == PPC64OutputSessionState::QUARANTINED);
    CHECK(status.workerQuarantined);
    CHECK(!quarantineVM.writeDiagnosticCheckpoint(
        "build/phase17-quarantined.gxcp", "phase17-quarantined", &error));

    VirtualMachine pausedVM(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(pausedVM, "PAUSED!!"));
    auto failedSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge pausedBridge(pausedVM, failedSink, 0, 4096, 8);
    CHECK(pausedBridge.attach(error));
    execute(pausedVM, pausedBridge, 7);
    failedSink->finish(8, 0, 0, HostConsoleIOStatus::DISCONNECTED, 109);
    pausedBridge.poll(0, 8);
    CHECK(pausedVM.pause());
    const uint64_t cycles = pausedVM.getCyclesExecuted();
    auto recoveredSink = std::make_shared<ControlledAsyncTransport>(8);
    CHECK(pausedBridge.reconnect(recoveredSink, error));
    CHECK(pausedVM.getCyclesExecuted() == cycles);
    CHECK(pausedBridge.status().sessionGeneration == 2);
    CHECK(pausedVM.resume());
    CHECK(pausedBridge.detach(false, &error));
}

void testResetIsolationAndOwnership() {
    std::puts("[phase17-I-J-reset-generation-and-two-VM-isolation]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(vm, "ABCDEFGH", "IJKLMNOP"));
    auto sink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge bridge(vm, sink, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    CHECK(sink->hasPending());
    CHECK(vm.reset());
    execute(vm, bridge, 7);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 8);
    sink->finish(8, 0, 0, HostConsoleIOStatus::OK);
    bridge.poll(0, 8);
    CHECK(bridge.status().staleGenerationOutputCompletions == 1);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 8);
    CHECK(sink->hasPending());
    sink->finish(8, 0, 0, HostConsoleIOStatus::OK);
    bridge.poll(0, 8);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.detach(false, &error));

    VirtualMachine first(kRamSize, 1, "PPC64", platformConfiguration());
    VirtualMachine second(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(first, "FIRST!!!"));
    CHECK(prepareTextProgram(second, "SECOND!!"));
    auto firstSink = std::make_shared<ControlledAsyncTransport>(8);
    auto secondSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge firstBridge(first, firstSink, 0, 4096, 8);
    PPC64ConsoleBridge secondBridge(second, secondSink, 0, 4096, 8);
    CHECK(firstBridge.attach(error));
    CHECK(secondBridge.attach(error));
    execute(first, firstBridge, 7);
    execute(second, secondBridge, 7);
    firstSink->finish(8, 0, 0, HostConsoleIOStatus::DISCONNECTED, 109);
    firstBridge.poll(0, 8);
    secondSink->finish(8, 0, 0, HostConsoleIOStatus::OK);
    secondBridge.poll(0, 8);
    CHECK(firstBridge.status().state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED);
    CHECK(firstBridge.status().vmIdentity != secondBridge.status().vmIdentity);
    CHECK(firstBridge.status().lpid == 0);
    CHECK(secondBridge.status().lpid == 0);
    CHECK(firstBridge.status().sessionGeneration == 1);
    CHECK(secondBridge.status().sessionGeneration == 1);
    CHECK(firstBridge.status().outputLeaseHeld);
    CHECK(secondBridge.status().outputLeaseHeld);
    CHECK(firstBridge.status().cancellationRequests == 0);
    CHECK(secondBridge.status().cancellationRequests == 0);
    CHECK(first.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(second.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(std::string(firstSink->sinkBytes().begin(), firstSink->sinkBytes().end()) == "FIRST!!!");
    CHECK(std::string(secondSink->sinkBytes().begin(), secondSink->sinkBytes().end()) == "SECOND!!");
    CHECK(firstBridge.detach(false, &error));
    CHECK(secondBridge.detach(false, &error));
}

#ifdef _WIN32
void testRealPipeVMIsolation() {
    std::puts("[phase17-J-real-pipe-failure-isolated-from-second-VM]");
    VirtualMachine first(kRamSize, 1, "PPC64", platformConfiguration());
    VirtualMachine second(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(first, "PIPE-A!!"));
    CHECK(prepareTextProgram(second, "PIPE-B!!"));

    HANDLE input = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE reader = INVALID_HANDLE_VALUE;
    HANDLE writer = INVALID_HANDLE_VALUE;
    const bool pipeMade = input != INVALID_HANDLE_VALUE &&
        CreatePipe(&reader, &writer, nullptr, 4096) != 0;
    CHECK(pipeMade);

    auto pipeTransport = std::make_shared<StdioHostConsoleTransport>(4096);
    auto memoryTransport = std::make_shared<MemoryHostConsoleTransport>();
    PPC64ConsoleBridge firstBridge(first, pipeTransport, 0, 4096, 8);
    PPC64ConsoleBridge secondBridge(second, memoryTransport, 0, 4096, 8);
    std::string error;
    const bool pipeAttached = pipeMade &&
        pipeTransport->attachToHandles(input, writer, error);
    CHECK(pipeAttached);
    const bool firstAttached = pipeAttached && firstBridge.attach(error);
    const bool secondAttached = secondBridge.attach(error);
    CHECK(firstAttached);
    CHECK(secondAttached);

    if (firstAttached && secondAttached) {
        CloseHandle(reader);
        reader = INVALID_HANDLE_VALUE;
        execute(first, firstBridge, 7);
        execute(second, secondBridge, 7);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            firstBridge.poll(0, 8);
            const auto failed = firstBridge.status();
            if (failed.outputState ==
                    PPC64OutputSessionState::DELIVERY_UNCERTAIN ||
                failed.outputState ==
                    PPC64OutputSessionState::RECONNECT_ELIGIBLE)
                break;
            std::this_thread::yield();
        }

        const auto failed = firstBridge.status();
        const auto healthy = secondBridge.status();
        const auto events = firstBridge.diagnosticEvents();
        CHECK(failed.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED);
        CHECK(failed.outputState ==
              PPC64OutputSessionState::DELIVERY_UNCERTAIN);
        CHECK(failed.sinkType == HostConsoleSinkType::WINDOWS_PIPE);
        CHECK(failed.lastFailure == HostConsoleFailureClass::UNCERTAIN_DELIVERY);
        CHECK(failed.outputBytesWithUncertainDelivery == 8);
        CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.type == PPC64ConsoleEventType::BROKEN_PIPE;
        }));
        const auto secondOutput = memoryTransport->takeOutput();
        CHECK(std::string(secondOutput.begin(), secondOutput.end()) == "PIPE-B!!");
        CHECK(healthy.guestOutputBytesAcceptedByHost == 8);
        CHECK(healthy.state == PPC64ConsoleSessionState::ACTIVE);
        CHECK(failed.vmIdentity != healthy.vmIdentity);
        CHECK(failed.sessionGeneration == 1 && healthy.sessionGeneration == 1);
        CHECK(failed.outputLeaseHeld && healthy.outputLeaseHeld);
        CHECK(failed.cancellationRequests == 0 &&
              healthy.cancellationRequests == 0);
        CHECK(failed.lpid == 0 && healthy.lpid == 0);
        CHECK(firstBridge.detach(false, &error));
        CHECK(secondBridge.detach(false, &error));
    }

    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
}
#endif

void testBoundedWorkerAdmission() {
    std::puts("[phase17-G-bounded-worker-and-quarantine-admission]");
    CHECK(StdioHostConsoleTransport::configureWorkerLimits(4, 1, 4096));
    auto limits = StdioHostConsoleTransport::workerResourceSnapshot();
    CHECK(limits.maximumActiveWorkers == 4);
    CHECK(limits.maximumQuarantinedWorkers == 1);
    CHECK(limits.maximumQuarantinedBufferCapacityBytes == 4096);
#ifdef _WIN32
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE readA = INVALID_HANDLE_VALUE, writeA = INVALID_HANDLE_VALUE;
    HANDLE readB = INVALID_HANDLE_VALUE, writeB = INVALID_HANDLE_VALUE;
    const bool pipesMade = input != INVALID_HANDLE_VALUE &&
        CreatePipe(&readA, &writeA, nullptr, 4096) != 0 &&
        CreatePipe(&readB, &writeB, nullptr, 4096) != 0;
    CHECK(pipesMade);
    if (pipesMade) {
        StdioHostConsoleTransport first(4096), second(4096);
        std::string error;
        CHECK(first.attachToHandles(input, writeA, error));
        CHECK(!second.attachToHandles(input, writeB, error));
        limits = StdioHostConsoleTransport::workerResourceSnapshot();
        CHECK(limits.activeWorkers == 1);
        first.detach();
        CHECK(StdioHostConsoleTransport::workerResourceSnapshot().activeWorkers == 0);
    }
    if (readA != INVALID_HANDLE_VALUE) CloseHandle(readA);
    if (writeA != INVALID_HANDLE_VALUE) CloseHandle(writeA);
    if (readB != INVALID_HANDLE_VALUE) CloseHandle(readB);
    if (writeB != INVALID_HANDLE_VALUE) CloseHandle(writeB);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
#endif
    CHECK(StdioHostConsoleTransport::configureWorkerLimits(
        16, 4, 4U * 1024U * 1024U));
}

void testAuthenticGuestPAPROutput() {
    std::puts("[phase17-N-authentic-guest-hvterm1-PAPR-output]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    CHECK(prepareTextProgram(vm, "GUEST123"));
    auto transport = std::make_shared<MemoryHostConsoleTransport>(4096, 4096);
    PPC64ConsoleBridge bridge(vm, transport, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    bridge.poll(0, 8);
    const auto output = transport->takeOutput();
    CHECK(std::string(output.begin(), output.end()) == "GUEST123");
    const auto status = bridge.status();
    CHECK(status.guestOutputBytesAcceptedByHost == 8);
    CHECK(status.writeAttempts == 1);
    CHECK(status.writeCompletions == 1);
    CHECK(status.lastFailure == HostConsoleFailureClass::NONE);
    CHECK(bridge.detach(false, &error));
}

void testIntegratedGuestAfterCleanReconnect() {
    std::puts("[phase17-N-integrated-guest-after-clean-reconnect]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfiguration());
    auto guest = putTextProgram("RECOVER!");
    guest.insert(guest.end(), {
        addi(3, 0, 0), mtspr(3, kSprDEC),
        addi(4, 0, 0xff), addi(3, 0, PPC64_H_CPPR), kSc1,
        mfmsr(6), ori(6, 6, static_cast<uint16_t>(MSR_EE)), mtmsr(6)});
    CHECK(prepare(vm, guest));
    const std::array<uint8_t, 8> payload{'R','E','C','O','V','E','R','!'};
    vm.getMemory().Write(0x4000, payload.data(), payload.size());
    const std::vector<uint32_t> xicsHandler{
        addi(3, 0, PPC64_H_XIRR), kSc1,
        addi(8, 4, 0), addis(4, 0, 0x3000),
        addi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
        addi(9, 4, 0), addi(10, 5, 0),
        addi(7, 0, static_cast<int16_t>(kResult)),
        stw(8, 7, 0), stw(9, 7, 4),
        (62U << 26) | (10U << 21) | (7U << 16) | 8U,
        addi(4, 8, 0), addi(3, 0, PPC64_H_EOI), kSc1, kRfid};
    const auto xicsBytes = encode(xicsHandler);
    vm.getMemory().Write(0x500, xicsBytes.data(), xicsBytes.size());
    const auto decBytes = encode({addi(7, 0, 100), mtspr(7, kSprDEC), kRfid});
    vm.getMemory().Write(0x900, decBytes.data(), decBytes.size());

    auto oldSink = std::make_shared<ControlledAsyncTransport>(8);
    PPC64ConsoleBridge bridge(vm, oldSink, 0, 4096, 8);
    std::string error;
    CHECK(bridge.attach(error));
    execute(vm, bridge, 7);
    CHECK(oldSink->hasPending());
    oldSink->finish(0, 0, 8, HostConsoleIOStatus::DISCONNECTED, 109);
    bridge.poll(0, 8);
    CHECK(bridge.status().knownNotAttemptedBytes == 8);

    auto replacement = std::make_shared<ControlledAsyncTransport>(8);
    CHECK(bridge.reconnect(replacement, error));
    bridge.poll(0, 8);
    CHECK(replacement->hasPending());
    replacement->finish(8, 0, 0, HostConsoleIOStatus::OK);
    bridge.poll(0, 8);
    CHECK(std::string(replacement->sinkBytes().begin(),
                      replacement->sinkBytes().end()) == "RECOVER!");
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0);
    CHECK(bridge.status().sessionGeneration == 2);

    const uint32_t terminalSource =
        vm.getPPC64PlatformConfiguration().virtualTerminalInterruptSourceId;
    CHECK(vm.configurePPC64InterruptSource(
        terminalSource + 1U, PPC64XICSSourceType::EDGE, 0x10, 0, 0,
        true, &error));
    CHECK(vm.assertPPC64InterruptSource(terminalSource + 1U, 0, &error));
    execute(vm, bridge, 7);
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(vm.injectPPC64TerminalInputWithStatus({'t'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources.size() >= 2);
    CHECK(vm.step()); // Guest enables external interrupts.
    bridge.poll(0, 8);
    CHECK(vm.step()); // XICS vector entry.
    bridge.poll(0, 8);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500);
    for (size_t i = 0; i < xicsHandler.size(); ++i) {
        CHECK(vm.step());
        bridge.poll(0, 8);
    }
    CHECK(vm.step()); // Return from the first XICS delivery.
    bridge.poll(0, 8);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500);
    for (size_t i = 0; i < xicsHandler.size(); ++i) {
        CHECK(vm.step());
        bridge.poll(0, 8);
    }
    CHECK(vm.step()); // DEC exception entry after XICS work is acknowledged.
    bridge.poll(0, 8);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x900);
    CHECK(vm.getPPC64InterruptSnapshot().lastDeliveryVector == 0x900);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(vm.step());
        bridge.poll(0, 8);
    }
    CHECK(!vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() ==
          kEntry + guest.size() * sizeof(uint32_t));
    const auto xics = vm.getPPC64XICSSnapshot();
    CHECK(std::none_of(xics.sources.begin(), xics.sources.end(),
                       [](const auto& source) { return source.pending; }));
    CHECK(bridge.detach(false, &error));
}

#ifdef _WIN32
void testRealPipeQuarantineBounds() {
    std::puts("[phase17-real-Windows-pipe-quarantine-bounds]");
    constexpr size_t kWorkerCapacity = 1U << 20U;
    CHECK(StdioHostConsoleTransport::configureWorkerLimits(
        2, 1, kWorkerCapacity));
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE reader = INVALID_HANDLE_VALUE, writer = INVALID_HANDLE_VALUE;
    const bool pipeMade = input != INVALID_HANDLE_VALUE &&
        CreatePipe(&reader, &writer, nullptr, 4096) != 0;
    CHECK(pipeMade);
    if (pipeMade) {
        StdioHostConsoleTransport transport(kWorkerCapacity);
        std::string error;
        CHECK(transport.attachToHandles(input, writer, error));
        std::vector<uint8_t> bytes(kWorkerCapacity, 0x5a);
        const auto submitted = transport.submitOutput(bytes.data(), bytes.size());
        CHECK(submitted.status == HostConsoleIOStatus::OK);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        bool observedWriting = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (transport.outputWorkerSnapshot().state ==
                HostConsoleOutputWorkerState::WRITING) {
                observedWriting = true;
                break;
            }
            std::this_thread::yield();
        }
        CHECK(observedWriting);
        transport.detach(); // Bounded cancellation; unresolved storage is reaped.
        auto snapshot = StdioHostConsoleTransport::workerResourceSnapshot();
        CHECK(snapshot.quarantinedWorkers <= 1);
        CHECK(snapshot.quarantinedBufferCapacityBytes <= kWorkerCapacity);
        if (reader != INVALID_HANDLE_VALUE) {
            CloseHandle(reader);
            reader = INVALID_HANDLE_VALUE;
        }
        const auto reapDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < reapDeadline &&
               StdioHostConsoleTransport::workerResourceSnapshot().activeWorkers != 0)
            std::this_thread::yield();
        snapshot = StdioHostConsoleTransport::workerResourceSnapshot();
        CHECK(snapshot.activeWorkers == 0);
        CHECK(snapshot.quarantinedWorkers == 0);
        CHECK(snapshot.unresolvedIO == 0);
    }
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    CHECK(StdioHostConsoleTransport::configureWorkerLimits(
        16, 4, 4U * 1024U * 1024U));
}
#endif
} // namespace

int main() {
    std::puts("PPC64 Phase 17 host-console recovery acceptance");
    testStateModelAndBoundedEvents();
    testHealthyOutputAndCleanReconnect();
    testUncertaintyResolutionAndCheckpoint();
    testCancellationPendingAndPauseResume();
    testResetIsolationAndOwnership();
#ifdef _WIN32
    testRealPipeVMIsolation();
#endif
    testBoundedWorkerAdmission();
    testAuthenticGuestPAPROutput();
    testIntegratedGuestAfterCleanReconnect();
#ifdef _WIN32
    testRealPipeQuarantineBounds();
#endif
    std::printf("PPC64 Phase 17: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
