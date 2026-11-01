#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ia64 {
class VirtualMachine;
}

namespace ia64::ppc64 {

enum class HostConsoleIOStatus : uint8_t {
    OK,
    WOULD_BLOCK,
    EOF_REACHED,
    DISCONNECTED,
    RECOVERABLE_ERROR,
    UNRECOVERABLE_ERROR,
    INVALID_CONFIGURATION,
    SHUTDOWN,
};

enum class HostConsoleFailureClass : uint8_t {
    NONE,
    TEMPORARY_BACKPRESSURE,
    BROKEN_PIPE,
    INVALID_HANDLE,
    PERMANENT_SINK_IO,
    OUTPUT_LEASE_CONFLICT,
    WORKER_TERMINATION,
    CANCELLATION_REQUESTED_UNRESOLVED,
    CANCELLATION_COMPLETED,
    UNCERTAIN_DELIVERY,
    INVALID_SESSION_GENERATION,
    INVALID_VM_LPID_OWNERSHIP,
    RECONNECT_INELIGIBLE,
};

enum class HostConsoleSinkType : uint8_t {
    UNKNOWN,
    MEMORY,
    WINDOWS_CONSOLE,
    CHARACTER_DEVICE,
    REDIRECTED_FILE,
    WINDOWS_PIPE,
};

struct HostConsoleIOResult {
    HostConsoleIOStatus status = HostConsoleIOStatus::WOULD_BLOCK;
    size_t bytes = 0;
    int systemError = 0;
};

enum class HostConsoleOutputWorkerState : uint8_t {
    IDLE,
    QUEUED,
    WRITING,
    COMPLETED,
    FAILED,
    STOPPING,
    STOPPED,
};

struct HostConsoleOutputSubmission {
    HostConsoleIOStatus status = HostConsoleIOStatus::WOULD_BLOCK;
    size_t queuedBytes = 0;
    int systemError = 0;
};

struct HostConsoleOutputCompletion {
    bool ready = false;
    size_t confirmedBytes = 0;
    size_t uncertainBytes = 0;
    size_t notAttemptedBytes = 0;
    HostConsoleIOStatus status = HostConsoleIOStatus::WOULD_BLOCK;
    int systemError = 0;
    bool partialWrite = false;
};

struct HostConsoleOutputWorkerSnapshot {
    HostConsoleOutputWorkerState state = HostConsoleOutputWorkerState::IDLE;
    size_t capacity = 0;
    size_t bufferedBytes = 0;
};

struct HostConsoleOutputStopResult {
    bool completed = false;
    bool cancellationRequested = false;
    int cancellationError = 0;
};

// Optional output path for transports whose sink may block. Submission only
// copies into the bounded handoff; the completion reports sink-confirmed,
// uncertain, and never-attempted prefixes separately.
class IAsyncHostConsoleOutput {
public:
    virtual ~IAsyncHostConsoleOutput() = default;
    virtual bool usesAsyncOutput() const noexcept = 0;
    virtual HostConsoleOutputSubmission submitOutput(
        const uint8_t* source, size_t maximumBytes) = 0;
    virtual HostConsoleOutputCompletion pollOutputCompletion() = 0;
    virtual HostConsoleOutputWorkerSnapshot outputWorkerSnapshot() const noexcept = 0;
    virtual HostConsoleOutputStopResult stopOutput(
        bool cancel, std::chrono::milliseconds timeout) noexcept = 0;
    virtual bool waitForOutputCompletion(std::chrono::milliseconds timeout) = 0;
};

// Host transports are nonblocking and own no VM or guest state.
class IHostConsoleTransport {
public:
    virtual ~IHostConsoleTransport() = default;
    virtual bool attach(std::string& error) = 0;
    virtual void detach() noexcept = 0;
    virtual void resetInput() noexcept = 0;
    virtual HostConsoleIOResult readInput(uint8_t* destination,
                                          size_t maximumBytes) = 0;
    virtual HostConsoleIOResult writeOutput(const uint8_t* source,
                                            size_t maximumBytes) = 0;
    virtual void shutdown() noexcept = 0;
    virtual bool isAttached() const noexcept = 0;
    virtual HostConsoleSinkType sinkType() const noexcept {
        return HostConsoleSinkType::UNKNOWN;
    }
};

// Deterministic bounded backend used by acceptance tests and embedders.
class MemoryHostConsoleTransport final : public IHostConsoleTransport {
public:
    explicit MemoryHostConsoleTransport(size_t inputCapacity = 16384,
                                        size_t outputCapacity = 16384);
    bool attach(std::string& error) override;
    void detach() noexcept override;
    void resetInput() noexcept override;
    HostConsoleIOResult readInput(uint8_t* destination,
                                  size_t maximumBytes) override;
    HostConsoleIOResult writeOutput(const uint8_t* source,
                                    size_t maximumBytes) override;
    void shutdown() noexcept override;
    bool isAttached() const noexcept override;

    size_t offerInput(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> takeOutput(size_t maximumBytes = 0);
    void closeInput() noexcept;
    void disconnectInput() noexcept;
    void disconnectOutput() noexcept;
    void setReadLimit(size_t maximumBytes) noexcept;
    void setWriteLimit(size_t maximumBytes) noexcept;
    void setReadBlocked(bool blocked) noexcept;
    void setWriteBlocked(bool blocked) noexcept;
    size_t pendingInput() const noexcept;
    size_t pendingOutput() const noexcept;
    HostConsoleSinkType sinkType() const noexcept override {
        return HostConsoleSinkType::MEMORY;
    }

private:
    mutable std::mutex mutex_;
    std::deque<uint8_t> input_;
    std::deque<uint8_t> output_;
    size_t inputCapacity_;
    size_t outputCapacity_;
    size_t readLimit_ = 256;
    size_t writeLimit_ = 256;
    bool attached_ = false;
    bool shutdown_ = false;
    bool inputClosed_ = false;
    bool inputDisconnected_ = false;
    bool outputDisconnected_ = false;
    bool readBlocked_ = false;
    bool writeBlocked_ = false;
};

// Windows console/stdio adapter. Console reads are key-polled; redirected
// pipes and regular files use bounded Win32 reads. Redirected pipe output uses
// a bounded worker so a synchronous pipe write can block only that worker.
// The adapter does not change console modes.
class StdioHostConsoleTransport final : public IHostConsoleTransport,
                                        public IAsyncHostConsoleOutput {
public:
    explicit StdioHostConsoleTransport(size_t outputQueueCapacity = 4096);
    ~StdioHostConsoleTransport() override;
    bool attach(std::string& error) override;
    void detach() noexcept override;
    void resetInput() noexcept override;
    HostConsoleIOResult readInput(uint8_t* destination,
                                  size_t maximumBytes) override;
    HostConsoleIOResult writeOutput(const uint8_t* source,
                                    size_t maximumBytes) override;
    void shutdown() noexcept override;
    bool isAttached() const noexcept override { return attached_; }
    HostConsoleSinkType sinkType() const noexcept override;
    const std::string& capabilityNote() const noexcept { return capabilityNote_; }
    // Attach with an explicitly supplied destination. Handles are borrowed;
    // redirected pipe workers duplicate and own the output handle.
    bool attachToHandles(void* inputHandle, void* outputHandle,
                         std::string& error);
    bool usesAsyncOutput() const noexcept override { return outputIsPipe_; }
    HostConsoleOutputSubmission submitOutput(
        const uint8_t* source, size_t maximumBytes) override;
    HostConsoleOutputCompletion pollOutputCompletion() override;
    HostConsoleOutputWorkerSnapshot outputWorkerSnapshot() const noexcept override;
    HostConsoleOutputStopResult stopOutput(
        bool cancel, std::chrono::milliseconds timeout) noexcept override;
    bool waitForOutputCompletion(std::chrono::milliseconds timeout) override;

    struct WorkerResourceSnapshot {
        size_t activeWorkers = 0;
        size_t quarantinedWorkers = 0;
        size_t unresolvedIO = 0;
        size_t quarantinedBufferCapacityBytes = 0;
        uint64_t reclaimedWorkers = 0;
        size_t maximumActiveWorkers = 0;
        size_t maximumQuarantinedWorkers = 0;
        size_t maximumQuarantinedBufferCapacityBytes = 0;
    };
    static bool configureWorkerLimits(size_t maximumActiveWorkers,
                                      size_t maximumQuarantinedWorkers,
                                      size_t maximumQuarantinedBufferCapacityBytes);
    static WorkerResourceSnapshot workerResourceSnapshot() noexcept;

private:
    struct OutputWorker;
    void* inputHandle_ = nullptr;
    void* outputHandle_ = nullptr;
    std::shared_ptr<OutputWorker> outputWorker_;
    size_t outputQueueCapacity_ = 0;
    bool attached_ = false;
    bool inputIsConsole_ = false;
    bool inputIsDisk_ = false;
    bool outputIsConsole_ = false;
    bool outputIsPipe_ = false;
    uint64_t inputDiscardRemaining_ = 0;
    int inputResetError_ = 0;
    std::string capabilityNote_;
};

enum class PPC64ConsoleSessionState : uint8_t {
    DETACHED,
    ACTIVE,
    INPUT_EOF,
    INPUT_DISCONNECTED,
    OUTPUT_DISCONNECTED,
    VM_HALTED,
    VM_DESTROYED,
    INVALID_BINDING,
    FAILED,
    OUTPUT_STOP_PENDING,
    SHUTDOWN,
};

enum class PPC64OutputSessionState : uint8_t {
    DETACHED,
    CONNECTED,
    OUTPUT_QUEUED,
    WRITE_IN_PROGRESS,
    BACKPRESSURED,
    PERMANENT_FAILURE,
    CANCELLATION_REQUESTED,
    CANCELLATION_PENDING,
    DELIVERY_UNCERTAIN,
    RECONNECT_ELIGIBLE,
    RECONNECTING,
    QUARANTINED,
    CLEANLY_STOPPED,
};

// Small public model used by the bridge and transition acceptance tests.
class PPC64OutputSessionStateMachine final {
public:
    PPC64OutputSessionState state() const noexcept { return state_; }
    bool transition(PPC64OutputSessionState next) noexcept;
    static bool isAllowed(PPC64OutputSessionState from,
                          PPC64OutputSessionState to) noexcept;
private:
    PPC64OutputSessionState state_ = PPC64OutputSessionState::DETACHED;
};

enum class PPC64ConsoleEventType : uint8_t {
    SESSION_ATTACHED,
    SESSION_DETACHED,
    WORKER_STARTED,
    WORKER_STOPPED,
    WRITE_SUBMITTED,
    WRITE_COMPLETED,
    PARTIAL_WRITE,
    BACKPRESSURE_ENTERED,
    BACKPRESSURE_CLEARED,
    BROKEN_PIPE,
    CANCELLATION_REQUESTED,
    CANCELLATION_OBSERVED,
    WORKER_QUARANTINED,
    RECONNECT_REQUESTED,
    RECONNECT_REJECTED,
    RECONNECT_COMPLETED,
    UNCERTAINTY_KEPT_UNRESOLVED,
    UNCERTAINTY_SEGMENT_DISCARDED,
    FAILED_SESSION_ABANDONED,
    UNCERTAINTY_RESOLVED,
    STATE_TRANSITION,
};

struct PPC64ConsoleDiagnosticEvent {
    uint64_t sequence = 0;
    uint64_t sessionGeneration = 0;
    PPC64ConsoleEventType type = PPC64ConsoleEventType::STATE_TRANSITION;
    PPC64OutputSessionState state = PPC64OutputSessionState::DETACHED;
    HostConsoleFailureClass failure = HostConsoleFailureClass::NONE;
    size_t byteCount = 0;
    int systemError = 0;
    uint64_t uncertainSegmentId = 0;
};

enum class PPC64OutputUncertaintyResolution : uint8_t {
    KEEP_UNRESOLVED,
    DISCARD_UNCERTAIN_SEGMENT,
    ABANDON_FAILED_SESSION,
};

struct PPC64ConsoleBridgeStatus {
    PPC64ConsoleSessionState state = PPC64ConsoleSessionState::DETACHED;
    PPC64OutputSessionState outputState = PPC64OutputSessionState::DETACHED;
    uint64_t vmIdentity = 0;
    uint16_t lpid = 0;
    uint32_t terminalId = 0;
    uint64_t sessionGeneration = 0;
    uint64_t vmResetGeneration = 0;
    HostConsoleSinkType sinkType = HostConsoleSinkType::UNKNOWN;
    size_t stagedInputBytes = 0;
    size_t inputCapacity = 0;
    uint64_t hostInputBytesRead = 0;
    uint64_t guestInputBytesAccepted = 0;
    // Output byte totals are cumulative. Terminal and handoff fields are
    // pending counts and exclude the in-flight prefix from the terminal FIFO.
    uint64_t guestOutputBytesAcceptedByHost = 0;
    uint64_t outputBytesHandedToWorker = 0;
    uint64_t outputBytesWithUncertainDelivery = 0;
    // Sum of worker-reported not-attempted bytes across completions.
    uint64_t outputBytesReportedNotAttempted = 0;
    uint64_t staleGenerationOutputCompletions = 0;
    uint64_t writeAttempts = 0;
    uint64_t writeCompletions = 0;
    uint64_t partialWrites = 0;
    uint64_t backpressureEvents = 0;
    uint64_t cancellationRequests = 0;
    uint64_t cancellationCompletions = 0;
    size_t unresolvedUncertainBytes = 0;
    uint64_t uncertainSegmentId = 0;
    size_t terminalOutputBytesPending = 0;
    size_t guestOutputBytesPending = 0;
    size_t knownNotAttemptedBytes = 0;
    size_t outputHandoffCapacity = 0;
    size_t outputHandoffBytesPending = 0;
    size_t workerBytesPending = 0;
    size_t inFlightBytes = 0;
    HostConsoleOutputWorkerState outputWorkerState =
        HostConsoleOutputWorkerState::IDLE;
    HostConsoleFailureClass lastFailure = HostConsoleFailureClass::NONE;
    bool outputLeaseHeld = false;
    bool workerRunning = false;
    bool workerStopped = false;
    bool workerQuarantined = false;
    HostConsoleIOStatus lastInputStatus = HostConsoleIOStatus::WOULD_BLOCK;
    HostConsoleIOStatus lastOutputStatus = HostConsoleIOStatus::WOULD_BLOCK;
    int lastSystemError = 0;
    std::string diagnostic;
};

// Pumps host I/O outside VirtualMachine::step(). A bridge is bound to one VM,
// terminal and LPID. VM lifetime is tracked through a weak token; pump calls
// and VM lifecycle operations remain on the VM's owning host thread.
class PPC64ConsoleBridge final {
public:
    PPC64ConsoleBridge(ia64::VirtualMachine& vm,
                       std::shared_ptr<IHostConsoleTransport> transport,
                       uint16_t lpid = 0,
                       size_t inputCapacity = 4096,
                       size_t transferChunk = 256);
    ~PPC64ConsoleBridge();
    PPC64ConsoleBridge(const PPC64ConsoleBridge&) = delete;
    PPC64ConsoleBridge& operator=(const PPC64ConsoleBridge&) = delete;

    bool attach(std::string& error);
    bool reconnect(std::shared_ptr<IHostConsoleTransport> replacement,
                   std::string& error);
    bool resolveOutputUncertainty(PPC64OutputUncertaintyResolution resolution,
                                  uint64_t uncertainSegmentId,
                                  std::string& error);
    bool detach(bool discardStagedInput = false, std::string* error = nullptr);
    void shutdown() noexcept;
    void poll(size_t maximumInputRead = 256,
              size_t maximumOutputWrite = 256);
    bool writeCheckpoint(const std::string& path,
                         const std::string& guestIdentity,
                         std::string* error = nullptr);
    PPC64ConsoleBridgeStatus status() const;
    std::vector<PPC64ConsoleDiagnosticEvent> diagnosticEvents() const;
    size_t stagedInputBytes() const noexcept;

private:
    bool vmAlive(ia64::VirtualMachine*& vm) const noexcept;
    void noticeGenerationChange(ia64::VirtualMachine& vm);
    void transferInput(ia64::VirtualMachine& vm);
    void transferOutput(ia64::VirtualMachine& vm, size_t maximumBytes);
    bool reconcileAsyncOutput(ia64::VirtualMachine& vm);
    bool stopAsyncOutput(ia64::VirtualMachine& vm, std::string* error);
    void updateCheckpointReady(ia64::VirtualMachine& vm);
    void fail(PPC64ConsoleSessionState state, const std::string& diagnostic);
    bool transitionOutput(PPC64OutputSessionState state,
                          PPC64ConsoleEventType event = PPC64ConsoleEventType::STATE_TRANSITION,
                          size_t byteCount = 0,
                          HostConsoleFailureClass failure = HostConsoleFailureClass::NONE,
                          int systemError = 0);
    void recordEvent(PPC64ConsoleEventType event, size_t byteCount = 0,
                     HostConsoleFailureClass failure = HostConsoleFailureClass::NONE,
                     int systemError = 0);
    bool ownsOutputLease(const ia64::VirtualMachine& vm) const noexcept;

    ia64::VirtualMachine* vm_ = nullptr;
    std::weak_ptr<const uint8_t> lifetime_;
    std::shared_ptr<IHostConsoleTransport> transport_;
    uint16_t lpid_ = 0;
    uint32_t terminalId_ = 0;
    size_t inputCapacity_ = 0;
    size_t transferChunk_ = 0;
    uint64_t outputLease_ = 0;
    uint64_t observedGeneration_ = 0;
    uint64_t sessionGeneration_ = 0;
    uint64_t outputSubmissionSessionGeneration_ = 0;
    uint64_t nextUncertainSegmentId_ = 1;
    PPC64OutputSessionStateMachine outputStateMachine_;
    PPC64ConsoleBridgeStatus status_;
    mutable std::mutex statusMutex_;
    std::array<PPC64ConsoleDiagnosticEvent, 128> eventRing_{};
    size_t eventRingStart_ = 0;
    size_t eventRingCount_ = 0;
    uint64_t nextEventSequence_ = 1;
    size_t uncertainSegmentBytes_ = 0;
    bool attached_ = false;
    bool inputEOF_ = false;
    size_t outputSubmissionBytes_ = 0;
    uint64_t outputSubmissionGeneration_ = 0;
    bool outputStopRequested_ = false;
    std::chrono::steady_clock::time_point outputRetryAfter_{};
};

} // namespace ia64::ppc64
