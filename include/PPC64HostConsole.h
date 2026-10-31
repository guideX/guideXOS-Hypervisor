#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
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
// pipes and regular files use bounded Win32 reads. It does not change console
// modes and never starts a worker thread.
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
    const std::string& capabilityNote() const noexcept { return capabilityNote_; }
    bool usesAsyncOutput() const noexcept override { return outputIsPipe_; }
    HostConsoleOutputSubmission submitOutput(
        const uint8_t* source, size_t maximumBytes) override;
    HostConsoleOutputCompletion pollOutputCompletion() override;
    HostConsoleOutputWorkerSnapshot outputWorkerSnapshot() const noexcept override;
    HostConsoleOutputStopResult stopOutput(
        bool cancel, std::chrono::milliseconds timeout) noexcept override;
    bool waitForOutputCompletion(std::chrono::milliseconds timeout) override;

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

struct PPC64ConsoleBridgeStatus {
    PPC64ConsoleSessionState state = PPC64ConsoleSessionState::DETACHED;
    uint16_t lpid = 0;
    uint32_t terminalId = 0;
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
    size_t terminalOutputBytesPending = 0;
    size_t outputHandoffCapacity = 0;
    size_t outputHandoffBytesPending = 0;
    HostConsoleOutputWorkerState outputWorkerState =
        HostConsoleOutputWorkerState::IDLE;
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
    bool detach(bool discardStagedInput = false, std::string* error = nullptr);
    void shutdown() noexcept;
    void poll(size_t maximumInputRead = 256,
              size_t maximumOutputWrite = 256);
    bool writeCheckpoint(const std::string& path,
                         const std::string& guestIdentity,
                         std::string* error = nullptr);
    PPC64ConsoleBridgeStatus status() const;
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

    ia64::VirtualMachine* vm_ = nullptr;
    std::weak_ptr<const uint8_t> lifetime_;
    std::shared_ptr<IHostConsoleTransport> transport_;
    uint16_t lpid_ = 0;
    uint32_t terminalId_ = 0;
    size_t inputCapacity_ = 0;
    size_t transferChunk_ = 0;
    uint64_t outputLease_ = 0;
    uint64_t observedGeneration_ = 0;
    PPC64ConsoleBridgeStatus status_;
    bool attached_ = false;
    bool inputEOF_ = false;
    size_t outputSubmissionBytes_ = 0;
    uint64_t outputSubmissionGeneration_ = 0;
    bool outputStopRequested_ = false;
    std::chrono::steady_clock::time_point outputRetryAfter_{};
};

} // namespace ia64::ppc64
