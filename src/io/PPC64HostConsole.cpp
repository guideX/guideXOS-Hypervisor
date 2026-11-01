#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PPC64HostConsole.h"

#include "VirtualMachine.h"
#include "PPC64VirtualTerminal.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace ia64::ppc64 {

namespace {
std::mutex gPipeSinkOwnersMutex;
std::vector<void*> gPipeSinkOwners;

struct WorkerResourceManager {
    std::mutex mutex;
    size_t activeWorkers = 0;
    size_t quarantinedWorkers = 0;
    size_t unresolvedIO = 0;
    size_t quarantinedBufferCapacityBytes = 0;
    size_t reservedQuarantineSlots = 0;
    size_t reservedBufferCapacityBytes = 0;
    uint64_t reclaimedWorkers = 0;
    size_t maximumActiveWorkers = 16;
    size_t maximumQuarantinedWorkers = 4;
    size_t maximumQuarantinedBufferCapacityBytes = 4U * 1024U * 1024U;

    bool reserve(size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeWorkers >= maximumActiveWorkers ||
            reservedQuarantineSlots >= maximumQuarantinedWorkers ||
            capacity > maximumQuarantinedBufferCapacityBytes -
                std::min(reservedBufferCapacityBytes,
                         maximumQuarantinedBufferCapacityBytes)) return false;
        ++activeWorkers;
        ++reservedQuarantineSlots;
        reservedBufferCapacityBytes += capacity;
        return true;
    }

    bool configure(size_t maximumActive, size_t maximumQuarantined,
                   size_t maximumCapacity) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeWorkers != 0 || quarantinedWorkers != 0 ||
            maximumActive == 0 || maximumQuarantined == 0 ||
            maximumQuarantined > 16 || maximumCapacity == 0) return false;
        maximumActiveWorkers = maximumActive;
        maximumQuarantinedWorkers = maximumQuarantined;
        maximumQuarantinedBufferCapacityBytes = maximumCapacity;
        return true;
    }

    void quarantine(size_t capacity, bool unresolved) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        ++quarantinedWorkers;
        quarantinedBufferCapacityBytes += capacity;
        if (unresolved) ++unresolvedIO;
    }

    void reaped(size_t capacity, bool unresolved) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (quarantinedWorkers != 0) --quarantinedWorkers;
        quarantinedBufferCapacityBytes -=
            std::min(quarantinedBufferCapacityBytes, capacity);
        if (unresolved && unresolvedIO != 0) --unresolvedIO;
        if (reclaimedWorkers != std::numeric_limits<uint64_t>::max())
            ++reclaimedWorkers;
    }

    void release(size_t capacity) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeWorkers != 0) --activeWorkers;
        if (reservedQuarantineSlots != 0) --reservedQuarantineSlots;
        reservedBufferCapacityBytes -=
            std::min(reservedBufferCapacityBytes, capacity);
    }

    StdioHostConsoleTransport::WorkerResourceSnapshot snapshot() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return {activeWorkers, quarantinedWorkers, unresolvedIO,
                quarantinedBufferCapacityBytes, reclaimedWorkers,
                maximumActiveWorkers, maximumQuarantinedWorkers,
                maximumQuarantinedBufferCapacityBytes};
    }
};

WorkerResourceManager& workerResources() {
    static WorkerResourceManager resources;
    return resources;
}

uint64_t incrementSaturated(uint64_t value, uint64_t amount) noexcept {
    return amount > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + amount;
}

size_t addSizeSaturated(size_t value, size_t amount) noexcept {
    return amount > std::numeric_limits<size_t>::max() - value
        ? std::numeric_limits<size_t>::max() : value + amount;
}

bool reservePipeSink(void* handle) {
    std::lock_guard<std::mutex> lock(gPipeSinkOwnersMutex);
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    if (module == nullptr) module = GetModuleHandleW(L"kernel32.dll");
    using CompareHandles = BOOL (WINAPI *)(HANDLE, HANDLE);
    const auto compareHandles = module == nullptr ? nullptr :
        reinterpret_cast<CompareHandles>(
            GetProcAddress(module, "CompareObjectHandles"));
    for (void* owner : gPipeSinkOwners) {
        if (owner == handle ||
            (compareHandles != nullptr &&
             compareHandles(static_cast<HANDLE>(owner),
                            static_cast<HANDLE>(handle)))) return false;
        // Before CompareObjectHandles is available, fail closed once another
        // pipe session exists instead of guessing that two handles differ.
        if (compareHandles == nullptr) return false;
    }
    gPipeSinkOwners.push_back(handle);
    return true;
}

void releasePipeSink(void* handle) noexcept {
    std::lock_guard<std::mutex> lock(gPipeSinkOwnersMutex);
    const auto found = std::find(gPipeSinkOwners.begin(), gPipeSinkOwners.end(),
                                 handle);
    if (found != gPipeSinkOwners.end()) gPipeSinkOwners.erase(found);
}

size_t terminalBytesNotHandedToWorker(
    ia64::VirtualMachine& vm, uint16_t lpid, size_t submittedBytes,
    bool submissionIsCurrentGeneration) {
    const size_t pending = vm.getPPC64TerminalPendingOutputBytes(lpid);
    if (submittedBytes == 0 || !submissionIsCurrentGeneration) return pending;
    return pending - std::min(pending, submittedBytes);
}

#ifdef _WIN32
HostConsoleIOStatus classifyPipeWriteError(DWORD error) {
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_NO_DATA) return HostConsoleIOStatus::DISCONNECTED;
    if (error == ERROR_OPERATION_ABORTED) return HostConsoleIOStatus::SHUTDOWN;
    return HostConsoleIOStatus::UNRECOVERABLE_ERROR;
}
#endif

HostConsoleFailureClass classifyOutputFailure(HostConsoleIOStatus status,
                                              int systemError) noexcept {
    if (status == HostConsoleIOStatus::WOULD_BLOCK)
        return HostConsoleFailureClass::TEMPORARY_BACKPRESSURE;
    if (status == HostConsoleIOStatus::DISCONNECTED)
        return HostConsoleFailureClass::BROKEN_PIPE;
    if (status == HostConsoleIOStatus::SHUTDOWN) {
        return systemError == 0 || systemError == 995
            ? HostConsoleFailureClass::CANCELLATION_COMPLETED
            : HostConsoleFailureClass::WORKER_TERMINATION;
    }
    if (systemError == 6)
        return HostConsoleFailureClass::INVALID_HANDLE;
    if (status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
        status == HostConsoleIOStatus::INVALID_CONFIGURATION)
        return HostConsoleFailureClass::PERMANENT_SINK_IO;
    return HostConsoleFailureClass::NONE;
}
} // namespace

bool PPC64OutputSessionStateMachine::isAllowed(
    PPC64OutputSessionState from, PPC64OutputSessionState to) noexcept {
    if (from == to) return true;
    using S = PPC64OutputSessionState;
    switch (from) {
    case S::DETACHED:
        return to == S::CONNECTED || to == S::CLEANLY_STOPPED;
    case S::CONNECTED:
        return to == S::OUTPUT_QUEUED || to == S::WRITE_IN_PROGRESS ||
               to == S::BACKPRESSURED || to == S::PERMANENT_FAILURE ||
               to == S::CANCELLATION_REQUESTED || to == S::CLEANLY_STOPPED;
    case S::OUTPUT_QUEUED:
        return to == S::WRITE_IN_PROGRESS || to == S::BACKPRESSURED ||
               to == S::CONNECTED || to == S::PERMANENT_FAILURE ||
               to == S::CANCELLATION_REQUESTED;
    case S::WRITE_IN_PROGRESS:
        return to == S::CONNECTED || to == S::OUTPUT_QUEUED ||
               to == S::BACKPRESSURED || to == S::PERMANENT_FAILURE ||
               to == S::CANCELLATION_REQUESTED ||
               to == S::DELIVERY_UNCERTAIN || to == S::QUARANTINED;
    case S::BACKPRESSURED:
        return to == S::OUTPUT_QUEUED || to == S::WRITE_IN_PROGRESS ||
               to == S::CONNECTED || to == S::PERMANENT_FAILURE ||
               to == S::CANCELLATION_REQUESTED || to == S::CLEANLY_STOPPED;
    case S::PERMANENT_FAILURE:
        return to == S::DELIVERY_UNCERTAIN ||
               to == S::RECONNECT_ELIGIBLE ||
               to == S::CANCELLATION_REQUESTED ||
               to == S::CANCELLATION_PENDING || to == S::QUARANTINED ||
               to == S::CLEANLY_STOPPED;
    case S::CANCELLATION_REQUESTED:
        return to == S::CANCELLATION_PENDING || to == S::CLEANLY_STOPPED ||
               to == S::PERMANENT_FAILURE ||
               to == S::DELIVERY_UNCERTAIN || to == S::QUARANTINED;
    case S::CANCELLATION_PENDING:
        return to == S::CLEANLY_STOPPED || to == S::PERMANENT_FAILURE ||
               to == S::DELIVERY_UNCERTAIN || to == S::QUARANTINED;
    case S::DELIVERY_UNCERTAIN:
        return to == S::RECONNECT_ELIGIBLE ||
               to == S::CANCELLATION_REQUESTED || to == S::QUARANTINED ||
               to == S::CLEANLY_STOPPED;
    case S::RECONNECT_ELIGIBLE:
        return to == S::RECONNECTING || to == S::PERMANENT_FAILURE ||
               to == S::CLEANLY_STOPPED;
    case S::RECONNECTING:
        return to == S::CONNECTED || to == S::PERMANENT_FAILURE ||
               to == S::CLEANLY_STOPPED;
    case S::QUARANTINED:
        return to == S::CLEANLY_STOPPED;
    case S::CLEANLY_STOPPED:
        return to == S::CONNECTED || to == S::CLEANLY_STOPPED;
    }
    return false;
}

bool PPC64OutputSessionStateMachine::transition(
    PPC64OutputSessionState next) noexcept {
    if (!isAllowed(state_, next)) return false;
    state_ = next;
    return true;
}

struct StdioHostConsoleTransport::OutputWorker final
    : std::enable_shared_from_this<OutputWorker> {
#ifdef _WIN32
    OutputWorker(HANDLE sinkHandle, void* sinkKey, size_t queueCapacity)
        : sink(sinkHandle), key(sinkKey), capacity(queueCapacity),
          buffer(queueCapacity) {
        admitted = workerResources().reserve(queueCapacity);
        if (admitted) event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~OutputWorker() {
        if (thread.joinable()) std::terminate();
        if (ownsSinkKey) releasePipeSink(key);
        if (event != nullptr) CloseHandle(event);
        if (sink != nullptr && sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
        if (admitted) workerResources().release(capacity);
    }

    bool valid() const noexcept { return admitted && event != nullptr; }
    bool admittedCapacity() const noexcept { return admitted; }

    void start() {
        const std::shared_ptr<OutputWorker> self = shared_from_this();
        thread = std::thread([self] { self->run(); });
    }

    HostConsoleOutputSubmission submit(const uint8_t* source, size_t count) {
        std::lock_guard<std::mutex> lock(mutex);
        if (source == nullptr && count != 0)
            return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0,
                    ERROR_INVALID_PARAMETER};
        if (count == 0)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        if (failed || stopping || done)
            return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
        if (count > capacity || hasSubmission || completionReady)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        std::copy_n(source, count, buffer.data());
        requested = count;
        confirmed = 0;
        partialWrite = false;
        hasSubmission = true;
        state = HostConsoleOutputWorkerState::QUEUED;
        cv.notify_all();
        return {HostConsoleIOStatus::OK, count, 0};
    }

    HostConsoleOutputCompletion takeCompletion() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!completionReady) return {};
        HostConsoleOutputCompletion result = completion;
        completion = {};
        completionReady = false;
        hasSubmission = false;
        requested = 0;
        confirmed = 0;
        if (!done && !failed && !stopping)
            state = HostConsoleOutputWorkerState::IDLE;
        cv.notify_all();
        return result;
    }

    HostConsoleOutputWorkerSnapshot snapshot() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return {state, capacity, hasSubmission ? requested : 0};
    }

    bool waitCompletion(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] {
            return completionReady || done;
        }) && completionReady;
    }

    HostConsoleOutputStopResult stop(bool cancel,
                                     std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        HostConsoleOutputStopResult result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            if (state != HostConsoleOutputWorkerState::STOPPED &&
                !completionReady)
                state = HostConsoleOutputWorkerState::STOPPING;
            if (cancel) cancelRequested = true;
            result.cancellationRequested = cancel;
            cv.notify_all();
        }

        for (;;) {
            std::unique_lock<std::mutex> lock(mutex);
            if (done || !thread.joinable()) {
                result.completed = true;
                break;
            }
            if (cancel) {
                // Keep the OVERLAPPED object alive under the same lock used by
                // the worker to clear activeOverlapped after completion.
                if (activeOverlapped != nullptr &&
                    !CancelIoEx(sink, activeOverlapped)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_NOT_FOUND)
                        result.cancellationError = static_cast<int>(error);
                }
                const HANDLE workerThread = reinterpret_cast<HANDLE>(
                    thread.native_handle());
                if (!CancelSynchronousIo(workerThread)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_NOT_FOUND &&
                        result.cancellationError == 0)
                        result.cancellationError = static_cast<int>(error);
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            cv.wait_until(lock, std::min(deadline, now +
                std::chrono::milliseconds(10)), [this] { return done; });
        }

        if (result.completed && thread.joinable()) thread.join();
        if (result.completed) {
            std::lock_guard<std::mutex> lock(mutex);
            state = HostConsoleOutputWorkerState::STOPPED;
        }
        return result;
    }

    bool isDone() const noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return done;
    }

    void releaseSinkLease() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (ownsSinkKey) {
            ownsSinkKey = false;
            releasePipeSink(key);
        }
    }

    void preserveForReaping() {
        bool unresolved = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (reaperOwnsState) return;
            reaperOwnsState = true;
            unresolved = ioInProgress;
            quarantinedUnresolved = unresolved;
        }
        workerResources().quarantine(capacity, unresolved);
        struct Reaper {
            Reaper() : thread([this] { run(); }) {}

            bool keep(const std::shared_ptr<OutputWorker>& worker) {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& slot : workers) {
                    if (!slot) {
                        slot = worker;
                        cv.notify_all();
                        return true;
                    }
                }
                return false;
            }

            void run() {
                for (;;) {
                    std::array<std::shared_ptr<OutputWorker>, 16> ready{};
                    size_t readyCount = 0;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait_for(lock, std::chrono::milliseconds(25));
                        for (auto& slot : workers) {
                            if (slot && slot->isDone()) {
                                ready[readyCount++] = std::move(slot);
                                slot.reset();
                            }
                        }
                    }
                    for (size_t i = 0; i < readyCount; ++i) {
                        const auto& worker = ready[i];
                        if (worker->thread.joinable()) worker->thread.join();
                        worker->markReaped();
                        worker->releaseSinkLease();
                    }
                }
            }

            std::mutex mutex;
            std::condition_variable cv;
            std::array<std::shared_ptr<OutputWorker>, 16> workers{};
            std::thread thread;
        };
        // A timed-out synchronous pipe write must outlive its transport. The
        // process-lifetime reaper keeps the joinable worker, its OVERLAPPED
        // storage, buffer, and duplicated handle until I/O really completes.
        static Reaper* const reaper = new Reaper();
        if (!reaper->keep(shared_from_this())) {
            // The admission manager reserves a fixed reaper slot before each
            // worker starts, so this indicates an internal accounting fault.
            std::terminate();
        }
    }

    void markReaped() noexcept {
        bool unresolved = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!reaperOwnsState) return;
            reaperOwnsState = false;
            unresolved = quarantinedUnresolved;
            quarantinedUnresolved = false;
        }
        workerResources().reaped(capacity, unresolved);
    }

private:
    void complete(HostConsoleIOStatus status, int systemError,
                  size_t confirmedBytes, size_t uncertainBytes,
                  size_t notAttemptedBytes) {
        std::lock_guard<std::mutex> lock(mutex);
        completion.ready = true;
        completion.partialWrite = partialWrite;
        completion.confirmedBytes = confirmedBytes;
        completion.uncertainBytes = uncertainBytes;
        completion.notAttemptedBytes = notAttemptedBytes;
        completion.status = status;
        completion.systemError = systemError;
        completionReady = true;
        failed = status == HostConsoleIOStatus::DISCONNECTED ||
                 status == HostConsoleIOStatus::UNRECOVERABLE_ERROR;
        state = failed ? HostConsoleOutputWorkerState::FAILED
                       : HostConsoleOutputWorkerState::COMPLETED;
        if (stopping || failed) done = true;
        cv.notify_all();
    }

    void run() {
        for (;;) {
            size_t request = 0;
            size_t offset = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || hasSubmission; });
                if (stopping && (!hasSubmission || completionReady)) {
                    done = true;
                    state = HostConsoleOutputWorkerState::STOPPED;
                    cv.notify_all();
                    return;
                }
                if (completionReady) {
                    cv.wait(lock, [this] {
                        return !completionReady || stopping;
                    });
                    continue;
                }
                if (cancelRequested) {
                    const size_t pending = requested - confirmed;
                    lock.unlock();
                    complete(HostConsoleIOStatus::SHUTDOWN, 0, confirmed, 0,
                             pending);
                    return;
                }
                request = requested;
                offset = confirmed;
            }

            bool zeroProgress = false;
            while (offset < request) {
                bool cancelBeforeWrite = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (cancelRequested) {
                        cancelBeforeWrite = true;
                    } else {
                        state = HostConsoleOutputWorkerState::WRITING;
                    }
                }
                if (cancelBeforeWrite) {
                    complete(HostConsoleIOStatus::SHUTDOWN, 0, offset, 0,
                             request - offset);
                    return;
                }

                const DWORD callBytes = static_cast<DWORD>(request - offset);
                OVERLAPPED overlapped{};
                overlapped.hEvent = event;
                ResetEvent(event);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    activeOverlapped = &overlapped;
                    ioInProgress = true;
                    cv.notify_all();
                }

                DWORD transferred = 0;
                BOOL success = WriteFile(sink, buffer.data() + offset,
                                         callBytes, &transferred, &overlapped);
                DWORD error = success ? ERROR_SUCCESS : GetLastError();
                if (!success && error == ERROR_IO_PENDING) {
                    const DWORD waitResult = WaitForSingleObject(event, INFINITE);
                    if (waitResult == WAIT_OBJECT_0) {
                        success = GetOverlappedResult(sink, &overlapped,
                                                      &transferred, FALSE);
                        error = success ? ERROR_SUCCESS : GetLastError();
                    } else {
                        success = FALSE;
                        error = GetLastError();
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    activeOverlapped = nullptr;
                    ioInProgress = false;
                    cv.notify_all();
                }

                if (success) {
                    if (transferred == 0) {
                        complete(HostConsoleIOStatus::WOULD_BLOCK, 0, offset,
                                 0, request - offset);
                        zeroProgress = true;
                        break;
                    }
                    if (transferred > callBytes) {
                        complete(HostConsoleIOStatus::UNRECOVERABLE_ERROR,
                                 ERROR_INVALID_DATA, offset, callBytes, 0);
                        return;
                    }
                    offset += transferred;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        confirmed = offset;
                        if (transferred < callBytes) partialWrite = true;
                    }
                    continue;
                }

                const size_t known = std::min<size_t>(transferred, callBytes);
                const size_t confirmedTotal = offset + known;
                const size_t uncertain = callBytes - known;
                const HostConsoleIOStatus status =
                    classifyPipeWriteError(error);
                complete(status, static_cast<int>(error), confirmedTotal,
                         uncertain, 0);
                return;
            }

            if (zeroProgress) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return !completionReady || stopping; });
                if (stopping) {
                    done = true;
                    state = HostConsoleOutputWorkerState::STOPPED;
                    cv.notify_all();
                    return;
                }
                continue;
            }

            complete(HostConsoleIOStatus::OK, 0, request, 0, 0);
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping) {
                done = true;
                state = HostConsoleOutputWorkerState::STOPPED;
                cv.notify_all();
                return;
            }
            cv.wait(lock, [this] { return !completionReady || stopping; });
        }
    }

    HANDLE sink = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    void* key = nullptr;
    size_t capacity = 0;
    std::vector<uint8_t> buffer;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    OVERLAPPED* activeOverlapped = nullptr;
    HostConsoleOutputWorkerState state = HostConsoleOutputWorkerState::IDLE;
    HostConsoleOutputCompletion completion;
    size_t requested = 0;
    size_t confirmed = 0;
    bool hasSubmission = false;
    bool completionReady = false;
    bool stopping = false;
    bool cancelRequested = false;
    bool failed = false;
    bool done = false;
    bool ioInProgress = false;
    bool ownsSinkKey = true;
    bool reaperOwnsState = false;
    bool quarantinedUnresolved = false;
    bool admitted = false;
    bool partialWrite = false;
#endif
};

MemoryHostConsoleTransport::MemoryHostConsoleTransport(size_t inputCapacity,
                                                       size_t outputCapacity)
    : inputCapacity_(inputCapacity), outputCapacity_(outputCapacity) {}

bool MemoryHostConsoleTransport::attach(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) {
        error = "memory transport has been shut down";
        return false;
    }
    attached_ = true;
    error.clear();
    return true;
}

void MemoryHostConsoleTransport::detach() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    attached_ = false;
}

void MemoryHostConsoleTransport::resetInput() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    input_.clear();
}

HostConsoleIOResult MemoryHostConsoleTransport::readInput(
    uint8_t* destination, size_t maximumBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_ || shutdown_) return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
    if (maximumBytes != 0 && destination == nullptr)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
    if (inputDisconnected_)
        return {HostConsoleIOStatus::DISCONNECTED, 0, 0};
    if (readBlocked_ || maximumBytes == 0 || input_.empty()) {
        if (input_.empty() && inputClosed_)
            return {HostConsoleIOStatus::EOF_REACHED, 0, 0};
        return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    }
    const size_t count = std::min({maximumBytes, readLimit_, input_.size()});
    for (size_t i = 0; i < count; ++i) {
        destination[i] = input_.front();
        input_.pop_front();
    }
    return {HostConsoleIOStatus::OK, count, 0};
}

HostConsoleIOResult MemoryHostConsoleTransport::writeOutput(
    const uint8_t* source, size_t maximumBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_ || shutdown_) return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
    if (maximumBytes != 0 && source == nullptr)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
    if (outputDisconnected_)
        return {HostConsoleIOStatus::DISCONNECTED, 0, 0};
    if (writeBlocked_ || maximumBytes == 0)
        return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    const size_t room = outputCapacity_ - output_.size();
    const size_t count = std::min({maximumBytes, writeLimit_, room});
    if (count == 0) return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    for (size_t i = 0; i < count; ++i) output_.push_back(source[i]);
    return {HostConsoleIOStatus::OK, count, 0};
}

void MemoryHostConsoleTransport::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    attached_ = false;
    shutdown_ = true;
}

bool MemoryHostConsoleTransport::isAttached() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return attached_ && !shutdown_;
}

size_t MemoryHostConsoleTransport::offerInput(
    const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attached_ || shutdown_ || inputClosed_ || inputDisconnected_) return 0;
    const size_t count = std::min(bytes.size(), inputCapacity_ - input_.size());
    for (size_t i = 0; i < count; ++i) input_.push_back(bytes[i]);
    return count;
}

std::vector<uint8_t> MemoryHostConsoleTransport::takeOutput(size_t maximumBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t count = maximumBytes == 0
        ? output_.size() : std::min(maximumBytes, output_.size());
    std::vector<uint8_t> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(output_.front());
        output_.pop_front();
    }
    return result;
}

void MemoryHostConsoleTransport::closeInput() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    inputClosed_ = true;
}
void MemoryHostConsoleTransport::disconnectInput() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    inputDisconnected_ = true;
}
void MemoryHostConsoleTransport::disconnectOutput() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    outputDisconnected_ = true;
}
void MemoryHostConsoleTransport::setReadLimit(size_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    readLimit_ = value;
}
void MemoryHostConsoleTransport::setWriteLimit(size_t value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    writeLimit_ = value;
}
void MemoryHostConsoleTransport::setReadBlocked(bool value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    readBlocked_ = value;
}
void MemoryHostConsoleTransport::setWriteBlocked(bool value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    writeBlocked_ = value;
}
size_t MemoryHostConsoleTransport::pendingInput() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return input_.size();
}
size_t MemoryHostConsoleTransport::pendingOutput() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_.size();
}

StdioHostConsoleTransport::StdioHostConsoleTransport(
    size_t outputQueueCapacity)
    : outputQueueCapacity_(outputQueueCapacity) {}

StdioHostConsoleTransport::~StdioHostConsoleTransport() { shutdown(); }

bool StdioHostConsoleTransport::attach(std::string& error) {
#ifdef _WIN32
    return attachToHandles(GetStdHandle(STD_INPUT_HANDLE),
                           GetStdHandle(STD_OUTPUT_HANDLE), error);
#else
    error = "stdio transport is implemented for the Windows host environment";
    return false;
#endif
}

bool StdioHostConsoleTransport::attachToHandles(void* inputHandle,
                                                void* outputHandle,
                                                std::string& error) {
#ifdef _WIN32
    if (attached_) { error.clear(); return true; }
    HANDLE input = static_cast<HANDLE>(inputHandle);
    HANDLE output = static_cast<HANDLE>(outputHandle);
    if (input == nullptr || input == INVALID_HANDLE_VALUE ||
        output == nullptr || output == INVALID_HANDLE_VALUE) {
        error = "standard input or output handle is unavailable";
        return false;
    }
    DWORD mode = 0;
    inputIsConsole_ = GetConsoleMode(input, &mode) != 0;
    SetLastError(NO_ERROR);
    const DWORD inputType = GetFileType(input);
    const DWORD inputTypeError = GetLastError();
    if (inputType == FILE_TYPE_UNKNOWN && inputTypeError != NO_ERROR) {
        error = "standard input handle is invalid: " +
                std::to_string(inputTypeError);
        return false;
    }
    inputIsDisk_ = !inputIsConsole_ && inputType == FILE_TYPE_DISK;
    if (!inputIsConsole_ && !inputIsDisk_ && inputType != FILE_TYPE_PIPE &&
        inputType != FILE_TYPE_CHAR) {
        error = "standard input must be a console, redirected pipe, or file";
        return false;
    }
    SetLastError(NO_ERROR);
    const DWORD outputType = GetFileType(output);
    const DWORD outputTypeError = GetLastError();
    if (outputType == FILE_TYPE_UNKNOWN) {
        error = outputTypeError == NO_ERROR
            ? "standard output has an unsupported handle type"
            : "standard output handle is invalid: " +
                  std::to_string(outputTypeError);
        return false;
    }
    if (outputType != FILE_TYPE_CHAR && outputType != FILE_TYPE_PIPE &&
        outputType != FILE_TYPE_DISK) {
        error = "standard output must be a console, redirected pipe, or file";
        return false;
    }
    DWORD outputMode = 0;
    outputIsConsole_ = outputType == FILE_TYPE_CHAR &&
                       GetConsoleMode(output, &outputMode) != 0;

    std::shared_ptr<OutputWorker> pipeWorker;
    if (outputType == FILE_TYPE_PIPE) {
        if (outputQueueCapacity_ == 0 || outputQueueCapacity_ > 1024U * 1024U) {
            error = "redirected stdout worker capacity must be between 1 and 1048576 bytes";
            return false;
        }
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), output, GetCurrentProcess(),
                             &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            const DWORD duplicateError = GetLastError();
            error = "could not duplicate redirected stdout pipe handle: " +
                    std::to_string(duplicateError);
            return false;
        }
        if (!reservePipeSink(duplicate)) {
            CloseHandle(duplicate);
            error = "another host-console transport already owns this stdout pipe object";
            return false;
        }
        try {
            pipeWorker = std::make_shared<OutputWorker>(
                duplicate, duplicate, outputQueueCapacity_);
        } catch (...) {
            CloseHandle(duplicate);
            releasePipeSink(duplicate);
            error = "could not allocate the bounded redirected stdout worker";
            return false;
        }
        if (!pipeWorker->valid()) {
            const DWORD eventError = GetLastError();
            const bool admitted = pipeWorker->admittedCapacity();
            pipeWorker.reset(); // also releases the exclusive sink lease
            error = admitted
                ? "could not create redirected stdout worker event: " +
                      std::to_string(eventError)
                : "redirected stdout worker admission rejected by finite active or quarantine limits";
            return false;
        }
        try {
            pipeWorker->start();
        } catch (...) {
            pipeWorker.reset();
            error = "could not start redirected stdout worker thread";
            return false;
        }
    }
    inputHandle_ = input;
    outputHandle_ = output;
    outputWorker_ = std::move(pipeWorker);
    outputIsPipe_ = outputType == FILE_TYPE_PIPE;
    attached_ = true;
    const char* outputRoute = outputIsPipe_
        ? "stdout pipe writes use a bounded worker"
        : outputType == FILE_TYPE_DISK
            ? "redirected file output uses synchronous writes"
            : outputIsConsole_
                ? "console output uses synchronous writes"
                : "character-device output uses synchronous writes";
    if (outputIsPipe_) {
        capabilityNote_ = inputIsConsole_
            ? std::string("interactive input uses the current Windows console code page; ") + outputRoute + "; console modes are unchanged"
            : std::string("redirected input is polled without blocking; ") + outputRoute + "; output bytes are binary-safe";
    } else {
        capabilityNote_ = inputIsConsole_
            ? std::string("interactive input uses the current Windows console code page; ") + outputRoute + "; console modes are unchanged"
            : std::string("redirected input is polled without blocking; ") + outputRoute + "; output bytes are binary-safe";
    }
    error.clear();
    return true;
#else
    (void)inputHandle;
    (void)outputHandle;
    error = "stdio transport is implemented for the Windows host environment";
    return false;
#endif
}

HostConsoleSinkType StdioHostConsoleTransport::sinkType() const noexcept {
    if (!attached_) return HostConsoleSinkType::UNKNOWN;
    if (outputIsPipe_) return HostConsoleSinkType::WINDOWS_PIPE;
    if (outputIsConsole_) return HostConsoleSinkType::WINDOWS_CONSOLE;
    if (outputHandle_ == nullptr) return HostConsoleSinkType::UNKNOWN;
#ifdef _WIN32
    const DWORD type = GetFileType(static_cast<HANDLE>(outputHandle_));
    if (type == FILE_TYPE_DISK) return HostConsoleSinkType::REDIRECTED_FILE;
    if (type == FILE_TYPE_CHAR) return HostConsoleSinkType::CHARACTER_DEVICE;
#endif
    return HostConsoleSinkType::UNKNOWN;
}

bool StdioHostConsoleTransport::configureWorkerLimits(
    size_t maximumActiveWorkers, size_t maximumQuarantinedWorkers,
    size_t maximumQuarantinedBufferCapacityBytes) {
    return workerResources().configure(maximumActiveWorkers,
                                       maximumQuarantinedWorkers,
                                       maximumQuarantinedBufferCapacityBytes);
}

StdioHostConsoleTransport::WorkerResourceSnapshot
StdioHostConsoleTransport::workerResourceSnapshot() noexcept {
    return workerResources().snapshot();
}

void StdioHostConsoleTransport::detach() noexcept {
    if (outputWorker_) {
        const auto stopped = stopOutput(true, std::chrono::milliseconds(100));
        if (!stopped.completed) outputWorker_->preserveForReaping();
        else outputWorker_->releaseSinkLease();
        outputWorker_.reset();
    }
    attached_ = false;
    outputIsConsole_ = false;
    outputIsPipe_ = false;
    inputHandle_ = nullptr;
    outputHandle_ = nullptr;
}

void StdioHostConsoleTransport::resetInput() noexcept {
#ifdef _WIN32
    if (!attached_) return;
    inputResetError_ = 0;
    inputDiscardRemaining_ = 0;
    if (inputIsConsole_) {
        if (!FlushConsoleInputBuffer(static_cast<HANDLE>(inputHandle_))) {
            inputResetError_ = static_cast<int>(GetLastError());
        }
    } else if (inputIsDisk_) {
        LARGE_INTEGER end{};
        end.QuadPart = 0;
        if (!SetFilePointerEx(static_cast<HANDLE>(inputHandle_), end, nullptr,
                              FILE_END)) {
            inputResetError_ = static_cast<int>(GetLastError());
        }
    } else {
        DWORD available = 0;
        HANDLE input = static_cast<HANDLE>(inputHandle_);
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED)
                inputResetError_ = static_cast<int>(error);
            return;
        }
        inputDiscardRemaining_ = available;
        std::array<uint8_t, 4096> discard{};
        for (size_t i = 0; i < 256 && inputDiscardRemaining_ != 0; ++i) {
            const DWORD request = static_cast<DWORD>(std::min<uint64_t>(
                inputDiscardRemaining_, discard.size()));
            DWORD read = 0;
            if (!ReadFile(input, discard.data(), request, &read, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE ||
                    error == ERROR_PIPE_NOT_CONNECTED) {
                    inputDiscardRemaining_ = 0;
                } else {
                    inputResetError_ = static_cast<int>(error);
                }
                break;
            }
            if (read == 0) break;
            inputDiscardRemaining_ -= read;
        }
    }
#endif
}

HostConsoleIOResult StdioHostConsoleTransport::readInput(
    uint8_t* destination, size_t maximumBytes) {
#ifdef _WIN32
    if (!attached_) return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
    if (maximumBytes != 0 && destination == nullptr)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, ERROR_INVALID_PARAMETER};
    if (maximumBytes == 0) return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    if (inputResetError_ != 0)
        return {HostConsoleIOStatus::UNRECOVERABLE_ERROR, 0, inputResetError_};
    const DWORD limit = static_cast<DWORD>(std::min<size_t>(maximumBytes, 4096));
    if (inputIsConsole_) {
        size_t count = 0;
        while (count < limit && _kbhit() != 0) {
            const int value = _getch();
            if (value == 0 || value == 0xe0) {
                if (_kbhit() != 0) (void)_getch();
                continue;
            }
            destination[count++] = static_cast<uint8_t>(value);
        }
        return count == 0
            ? HostConsoleIOResult{HostConsoleIOStatus::WOULD_BLOCK, 0, 0}
            : HostConsoleIOResult{HostConsoleIOStatus::OK, count, 0};
    }
    DWORD available = 0;
    if (inputIsDisk_) {
        LARGE_INTEGER size{};
        LARGE_INTEGER position{};
        HANDLE input = static_cast<HANDLE>(inputHandle_);
        if (!GetFileSizeEx(input, &size))
            return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                    static_cast<int>(GetLastError())};
        LARGE_INTEGER move{};
        if (!SetFilePointerEx(input, move, &position, FILE_CURRENT))
            return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                    static_cast<int>(GetLastError())};
        if (position.QuadPart >= size.QuadPart)
            return {HostConsoleIOStatus::EOF_REACHED, 0, 0};
        available = static_cast<DWORD>(std::min<LONGLONG>(
            size.QuadPart - position.QuadPart, static_cast<LONGLONG>(limit)));
    } else {
        HANDLE input = static_cast<HANDLE>(inputHandle_);
        if (inputDiscardRemaining_ != 0) {
            DWORD discardAvailable = 0;
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &discardAvailable,
                               nullptr)) {
                const DWORD error = GetLastError();
                inputDiscardRemaining_ = 0;
                if (error == ERROR_BROKEN_PIPE ||
                    error == ERROR_PIPE_NOT_CONNECTED)
                    return {HostConsoleIOStatus::EOF_REACHED, 0,
                            static_cast<int>(error)};
                return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                        static_cast<int>(error)};
            }
            if (discardAvailable == 0)
                return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
            std::array<uint8_t, 4096> discard{};
            const DWORD discardCount = static_cast<DWORD>(std::min<uint64_t>(
                {static_cast<uint64_t>(discardAvailable),
                 static_cast<uint64_t>(discard.size()),
                 inputDiscardRemaining_}));
            DWORD discarded = 0;
            if (!ReadFile(input, discard.data(), discardCount, &discarded,
                          nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE ||
                    error == ERROR_PIPE_NOT_CONNECTED) {
                    inputDiscardRemaining_ = 0;
                    return {HostConsoleIOStatus::EOF_REACHED, 0,
                            static_cast<int>(error)};
                }
                return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                        static_cast<int>(error)};
            }
            if (discarded == 0)
                return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
            inputDiscardRemaining_ -= discarded;
            return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        }
        if (!PeekNamedPipe(input, nullptr, 0,
                           nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                return {HostConsoleIOStatus::EOF_REACHED, 0,
                        static_cast<int>(error)};
            return {HostConsoleIOStatus::DISCONNECTED, 0,
                    static_cast<int>(error)};
        }
        if (available == 0) return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
        available = std::min(available, limit);
    }
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(inputHandle_), destination, available,
                  &read, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
            return {HostConsoleIOStatus::EOF_REACHED, 0,
                    static_cast<int>(error)};
        if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0,
                    static_cast<int>(error)};
        return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                static_cast<int>(error)};
    }
    return read == 0
        ? HostConsoleIOResult{HostConsoleIOStatus::EOF_REACHED, 0, 0}
        : HostConsoleIOResult{HostConsoleIOStatus::OK, read, 0};
#else
    (void)destination; (void)maximumBytes;
    return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
#endif
}

HostConsoleIOResult StdioHostConsoleTransport::writeOutput(
    const uint8_t* source, size_t maximumBytes) {
#ifdef _WIN32
    if (!attached_) return {HostConsoleIOStatus::SHUTDOWN, 0, 0};
    if (outputIsPipe_)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0,
                ERROR_INVALID_FUNCTION};
    if (maximumBytes != 0 && source == nullptr)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, ERROR_INVALID_PARAMETER};
    if (maximumBytes == 0) return {HostConsoleIOStatus::WOULD_BLOCK, 0, 0};
    const DWORD request = static_cast<DWORD>(std::min<size_t>(maximumBytes, 4096));
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(outputHandle_), source, request,
                   &written, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY)
            return {HostConsoleIOStatus::WOULD_BLOCK, 0,
                    static_cast<int>(error)};
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
            return {HostConsoleIOStatus::DISCONNECTED, 0,
                    static_cast<int>(error)};
        return {HostConsoleIOStatus::RECOVERABLE_ERROR, 0,
                static_cast<int>(error)};
    }
    return written == 0
        ? HostConsoleIOResult{HostConsoleIOStatus::WOULD_BLOCK, 0, 0}
        : HostConsoleIOResult{HostConsoleIOStatus::OK, written, 0};
#else
    (void)source; (void)maximumBytes;
    return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
#endif
}

HostConsoleOutputSubmission StdioHostConsoleTransport::submitOutput(
    const uint8_t* source, size_t maximumBytes) {
#ifdef _WIN32
    if (!attached_ || !outputIsPipe_ || !outputWorker_)
        return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0,
                ERROR_INVALID_FUNCTION};
    return outputWorker_->submit(source,
        std::min(maximumBytes, outputQueueCapacity_));
#else
    (void)source;
    (void)maximumBytes;
    return {HostConsoleIOStatus::INVALID_CONFIGURATION, 0, 0};
#endif
}

HostConsoleOutputCompletion StdioHostConsoleTransport::pollOutputCompletion() {
#ifdef _WIN32
    return outputWorker_ ? outputWorker_->takeCompletion()
                         : HostConsoleOutputCompletion{};
#else
    return {};
#endif
}

HostConsoleOutputWorkerSnapshot
StdioHostConsoleTransport::outputWorkerSnapshot() const noexcept {
#ifdef _WIN32
    return outputWorker_ ? outputWorker_->snapshot()
                         : HostConsoleOutputWorkerSnapshot{};
#else
    return {};
#endif
}

HostConsoleOutputStopResult StdioHostConsoleTransport::stopOutput(
    bool cancel, std::chrono::milliseconds timeout) noexcept {
#ifdef _WIN32
    return outputWorker_
        ? outputWorker_->stop(cancel, timeout)
        : HostConsoleOutputStopResult{true, cancel, 0};
#else
    (void)cancel;
    (void)timeout;
    return {true, false, 0};
#endif
}

bool StdioHostConsoleTransport::waitForOutputCompletion(
    std::chrono::milliseconds timeout) {
#ifdef _WIN32
    return outputWorker_ && outputWorker_->waitCompletion(timeout);
#else
    (void)timeout;
    return false;
#endif
}

void StdioHostConsoleTransport::shutdown() noexcept { detach(); }

PPC64ConsoleBridge::PPC64ConsoleBridge(
    ia64::VirtualMachine& vm, std::shared_ptr<IHostConsoleTransport> transport,
    uint16_t lpid, size_t inputCapacity, size_t transferChunk)
    : vm_(&vm), lifetime_(vm.getPPC64ConsoleLifetimeToken()),
      transport_(std::move(transport)), lpid_(lpid),
      inputCapacity_(inputCapacity), transferChunk_(transferChunk) {
    status_.lpid = lpid;
    status_.inputCapacity = inputCapacity;
    status_.vmIdentity = vm.getPPC64ConsoleVMIdentity();
}

PPC64ConsoleBridge::~PPC64ConsoleBridge() { shutdown(); }

bool PPC64ConsoleBridge::vmAlive(ia64::VirtualMachine*& vm) const noexcept {
    auto lifetime = lifetime_.lock();
    if (!lifetime || vm_ == nullptr) return false;
    vm = vm_;
    return true;
}

void PPC64ConsoleBridge::recordEvent(PPC64ConsoleEventType type,
                                     size_t byteCount,
                                     HostConsoleFailureClass failure,
                                     int systemError) {
    if (nextEventSequence_ == 0) return;
    const size_t index = (eventRingStart_ + eventRingCount_) % eventRing_.size();
    PPC64ConsoleDiagnosticEvent event;
    event.sequence = nextEventSequence_;
    event.sessionGeneration = sessionGeneration_;
    event.type = type;
    event.state = outputStateMachine_.state();
    event.failure = failure;
    event.byteCount = byteCount;
    event.systemError = systemError;
    event.uncertainSegmentId = status_.uncertainSegmentId;
    if (eventRingCount_ < eventRing_.size()) {
        eventRing_[index] = event;
        ++eventRingCount_;
    } else {
        eventRing_[eventRingStart_] = event;
        eventRingStart_ = (eventRingStart_ + 1) % eventRing_.size();
    }
    if (nextEventSequence_ == std::numeric_limits<uint64_t>::max())
        nextEventSequence_ = 0;
    else
        ++nextEventSequence_;
}

bool PPC64ConsoleBridge::transitionOutput(
    PPC64OutputSessionState state, PPC64ConsoleEventType event,
    size_t byteCount, HostConsoleFailureClass failure, int systemError) {
    if (!outputStateMachine_.transition(state)) {
        status_.lastFailure = HostConsoleFailureClass::INVALID_SESSION_GENERATION;
        return false;
    }
    status_.outputState = state;
    recordEvent(event, byteCount, failure, systemError);
    return true;
}

bool PPC64ConsoleBridge::ownsOutputLease(
    const ia64::VirtualMachine& vm) const noexcept {
    const auto found = vm.ppc64ConsoleOutputLeases_.find(lpid_);
    return outputLease_ != 0 && found != vm.ppc64ConsoleOutputLeases_.end() &&
           found->second.first == outputLease_;
}

bool PPC64ConsoleBridge::attach(std::string& error) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    if (!vmAlive(vm)) { error = "PPC64 VM has been destroyed"; return false; }
    if (attached_) { error.clear(); return true; }
    if (!transport_ || inputCapacity_ == 0 || transferChunk_ == 0 ||
        inputCapacity_ > 1024U * 1024U) {
        status_.state = PPC64ConsoleSessionState::INVALID_BINDING;
        status_.diagnostic = "transport or bounded queue configuration is invalid";
        error = status_.diagnostic;
        return false;
    }
    if (vm->injectPPC64TerminalInputWithStatus({}, lpid_) !=
        PPC64TerminalResult::SUCCESS) {
        status_.state = PPC64ConsoleSessionState::INVALID_BINDING;
        status_.diagnostic = "selected VM, hvterm1 terminal, or LPID is unavailable";
        error = status_.diagnostic;
        return false;
    }
    terminalId_ = vm->getPPC64VirtualTerminalId();
    outputLease_ = vm->claimPPC64ConsoleOutput(lpid_, inputCapacity_);
    if (outputLease_ == 0) {
        status_.state = PPC64ConsoleSessionState::INVALID_BINDING;
        status_.diagnostic = "another host consumer owns this terminal output channel";
        status_.lastFailure = HostConsoleFailureClass::OUTPUT_LEASE_CONFLICT;
        error = status_.diagnostic;
        return false;
    }
    status_.outputLeaseHeld = true;
    if (!transport_->attach(error)) {
        vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
        outputLease_ = 0;
        status_.outputLeaseHeld = false;
        status_.state = PPC64ConsoleSessionState::FAILED;
        status_.diagnostic = error;
        status_.lastFailure = HostConsoleFailureClass::PERMANENT_SINK_IO;
        return false;
    }
    if (sessionGeneration_ != std::numeric_limits<uint64_t>::max())
        ++sessionGeneration_;
    else {
        transport_->detach();
        vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
        outputLease_ = 0;
        status_.outputLeaseHeld = false;
        error = "console session generation exhausted";
        status_.lastFailure = HostConsoleFailureClass::INVALID_SESSION_GENERATION;
        return false;
    }
    observedGeneration_ = vm->getPPC64ConsoleGeneration();
    attached_ = true;
    status_.state = PPC64ConsoleSessionState::ACTIVE;
    status_.terminalId = terminalId_;
    status_.sessionGeneration = sessionGeneration_;
    status_.vmResetGeneration = observedGeneration_;
    status_.sinkType = transport_->sinkType();
    status_.diagnostic.clear();
    status_.lastFailure = HostConsoleFailureClass::NONE;
    if (!transitionOutput(PPC64OutputSessionState::CONNECTED)) {
        transport_->detach();
        vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
        outputLease_ = 0;
        status_.outputLeaseHeld = false;
        attached_ = false;
        error = "invalid output-session transition during attachment";
        return false;
    }
    recordEvent(PPC64ConsoleEventType::SESSION_ATTACHED);
    if (status_.sinkType == HostConsoleSinkType::WINDOWS_PIPE)
        recordEvent(PPC64ConsoleEventType::WORKER_STARTED);
    updateCheckpointReady(*vm);
    error.clear();
    return true;
}

bool PPC64ConsoleBridge::reconnect(
    std::shared_ptr<IHostConsoleTransport> replacement, std::string& error) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    recordEvent(PPC64ConsoleEventType::RECONNECT_REQUESTED);
    auto reject = [&](const std::string& reason,
                      HostConsoleFailureClass failure) {
        status_.lastFailure = failure;
        status_.diagnostic = reason;
        recordEvent(PPC64ConsoleEventType::RECONNECT_REJECTED, 0, failure,
                    status_.lastSystemError);
        error = reason;
        return false;
    };

    ia64::VirtualMachine* vm = nullptr;
    if (!vmAlive(vm))
        return reject("reconnect rejected: owning PPC64 VM was destroyed",
                      HostConsoleFailureClass::INVALID_VM_LPID_OWNERSHIP);
    if (!attached_ || !replacement || replacement.get() == transport_.get() ||
        (outputStateMachine_.state() !=
             PPC64OutputSessionState::RECONNECT_ELIGIBLE &&
         outputStateMachine_.state() !=
             PPC64OutputSessionState::PERMANENT_FAILURE &&
         outputStateMachine_.state() !=
             PPC64OutputSessionState::DELIVERY_UNCERTAIN))
        return reject("reconnect rejected: output session is not eligible for replacement",
                      HostConsoleFailureClass::RECONNECT_INELIGIBLE);
    if (outputSubmissionBytes_ != 0 && !reconcileAsyncOutput(*vm))
        return reject("reconnect rejected: prior worker completion is unresolved",
                      HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED);
    if (outputSubmissionBytes_ != 0 || status_.unresolvedUncertainBytes != 0)
        return reject("reconnect rejected: output boundary still contains unresolved or in-flight bytes",
                      status_.unresolvedUncertainBytes != 0
                          ? HostConsoleFailureClass::UNCERTAIN_DELIVERY
                          : HostConsoleFailureClass::RECONNECT_INELIGIBLE);
    if (!ownsOutputLease(*vm) ||
        vm->injectPPC64TerminalInputWithStatus({}, lpid_) !=
            PPC64TerminalResult::SUCCESS ||
        vm->getPPC64VirtualTerminalId() != terminalId_) {
        return reject("reconnect rejected: VM, LPID, terminal, or output lease ownership changed",
                      HostConsoleFailureClass::INVALID_VM_LPID_OWNERSHIP);
    }
    if (vm->cpus_.empty() ||
        vm->cpus_[0].state == ia64::CPUExecutionState::HALTED ||
        vm->cpus_[0].state == ia64::CPUExecutionState::ERROR) {
        return reject("reconnect rejected: VM lifecycle state is not recoverable",
                      HostConsoleFailureClass::RECONNECT_INELIGIBLE);
    }
    if (sessionGeneration_ == std::numeric_limits<uint64_t>::max())
        return reject("reconnect rejected: session generation exhausted",
                      HostConsoleFailureClass::INVALID_SESSION_GENERATION);

    if (outputStateMachine_.state() ==
        PPC64OutputSessionState::PERMANENT_FAILURE &&
        !transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE))
        return reject("reconnect rejected: failed output boundary is invalid",
                      HostConsoleFailureClass::RECONNECT_INELIGIBLE);

    if (auto* oldAsync = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
        oldAsync != nullptr && oldAsync->usesAsyncOutput()) {
        const auto stopped = oldAsync->stopOutput(
            false, std::chrono::milliseconds(500));
        if (!stopped.completed)
            return reject("reconnect rejected: old worker can still access its output buffer",
                          HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED);
        status_.outputWorkerState = HostConsoleOutputWorkerState::STOPPED;
        recordEvent(PPC64ConsoleEventType::WORKER_STOPPED);
    }

    if (!transitionOutput(PPC64OutputSessionState::RECONNECTING))
        return reject("reconnect rejected: invalid output-session state transition",
                      HostConsoleFailureClass::INVALID_SESSION_GENERATION);

    std::string attachError;
    if (!replacement->attach(attachError)) {
        (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                               PPC64ConsoleEventType::RECONNECT_REJECTED,
                               0, HostConsoleFailureClass::PERMANENT_SINK_IO);
        status_.lastFailure = HostConsoleFailureClass::PERMANENT_SINK_IO;
        return reject("replacement sink could not be attached: " + attachError,
                      HostConsoleFailureClass::PERMANENT_SINK_IO);
    }

    if (transport_) transport_->detach();
    transport_ = std::move(replacement);
    ++sessionGeneration_;
    outputSubmissionSessionGeneration_ = 0;
    observedGeneration_ = vm->getPPC64ConsoleGeneration();
    status_.sessionGeneration = sessionGeneration_;
    status_.vmResetGeneration = observedGeneration_;
    status_.sinkType = transport_->sinkType();
    status_.outputLeaseHeld = ownsOutputLease(*vm);
    status_.lastFailure = HostConsoleFailureClass::NONE;
    status_.lastOutputStatus = HostConsoleIOStatus::WOULD_BLOCK;
    status_.lastSystemError = 0;
    status_.state = PPC64ConsoleSessionState::ACTIVE;
    status_.diagnostic = "reconnected at a reconciled output boundary; queued never-attempted bytes remain in FIFO order";
    outputStopRequested_ = false;
    outputRetryAfter_ = {};
    if (!transitionOutput(PPC64OutputSessionState::CONNECTED,
                          PPC64ConsoleEventType::RECONNECT_COMPLETED)) {
        transport_->detach();
        status_.state = PPC64ConsoleSessionState::FAILED;
        return reject("replacement attached but output-session transition failed",
                      HostConsoleFailureClass::INVALID_SESSION_GENERATION);
    }
    if (status_.sinkType == HostConsoleSinkType::WINDOWS_PIPE)
        recordEvent(PPC64ConsoleEventType::WORKER_STARTED);
    updateCheckpointReady(*vm);
    error.clear();
    return true;
}

bool PPC64ConsoleBridge::resolveOutputUncertainty(
    PPC64OutputUncertaintyResolution resolution, uint64_t uncertainSegmentId,
    std::string& error) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    if (resolution == PPC64OutputUncertaintyResolution::KEEP_UNRESOLVED) {
        status_.diagnostic = "delivery uncertainty was explicitly kept unresolved; later output remains blocked";
        recordEvent(PPC64ConsoleEventType::UNCERTAINTY_KEPT_UNRESOLVED,
                    status_.unresolvedUncertainBytes,
                    HostConsoleFailureClass::UNCERTAIN_DELIVERY);
        error.clear();
        return true;
    }
    if (status_.unresolvedUncertainBytes == 0 ||
        outputSubmissionBytes_ != 0) {
        error = "there is no settled uncertain output segment to resolve";
        status_.lastFailure = HostConsoleFailureClass::RECONNECT_INELIGIBLE;
        return false;
    }
    const bool abandon = resolution ==
        PPC64OutputUncertaintyResolution::ABANDON_FAILED_SESSION;
    if (!abandon && uncertainSegmentId != status_.uncertainSegmentId) {
        error = "uncertainty resolution rejected: segment identifier does not match the pending segment";
        status_.lastFailure = HostConsoleFailureClass::UNCERTAIN_DELIVERY;
        return false;
    }
    if (abandon && outputStateMachine_.state() !=
                       PPC64OutputSessionState::DELIVERY_UNCERTAIN &&
                   outputStateMachine_.state() !=
                       PPC64OutputSessionState::PERMANENT_FAILURE) {
        error = "session abandonment rejected: no failed output session is pending";
        status_.lastFailure = HostConsoleFailureClass::RECONNECT_INELIGIBLE;
        return false;
    }
    const size_t resolvedBytes = status_.unresolvedUncertainBytes;
    status_.unresolvedUncertainBytes = 0;
    uncertainSegmentBytes_ = 0;
    const char* action = abandon
        ? "failed output session abandoned with an explicit output discontinuity"
        : "identified uncertain output segment explicitly discarded; recovery is not lossless";
    status_.diagnostic = action;
    const PPC64ConsoleEventType resolutionEvent = abandon
        ? PPC64ConsoleEventType::FAILED_SESSION_ABANDONED
        : PPC64ConsoleEventType::UNCERTAINTY_SEGMENT_DISCARDED;
    if (outputStateMachine_.state() ==
        PPC64OutputSessionState::DELIVERY_UNCERTAIN) {
        (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE,
                               resolutionEvent,
                               resolvedBytes,
                               HostConsoleFailureClass::UNCERTAIN_DELIVERY);
    } else if (outputStateMachine_.state() ==
               PPC64OutputSessionState::PERMANENT_FAILURE) {
        (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE,
                               resolutionEvent,
                               resolvedBytes,
                               HostConsoleFailureClass::UNCERTAIN_DELIVERY);
    }
    error.clear();
    return true;
}

bool PPC64ConsoleBridge::detach(bool discardStagedInput, std::string* error) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    if (!vmAlive(vm)) {
        if (transport_) transport_->shutdown();
        attached_ = false;
        status_.state = PPC64ConsoleSessionState::VM_DESTROYED;
        if (error) *error = "PPC64 VM has been destroyed";
        return false;
    }
    if (!stopAsyncOutput(*vm, error)) return false;
    const size_t staged = vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
    if (!discardStagedInput && staged != 0)
        status_.diagnostic = "staged input remains VM-owned and will resume on a later bridge attachment";
    if (discardStagedInput && staged != 0) {
        vm->discardPPC64ConsoleInput(lpid_, outputLease_);
        status_.diagnostic = "staged host input was explicitly discarded during detach";
    }
    status_.stagedInputBytes = discardStagedInput ? 0 : staged;
    if (transport_) transport_->detach();
    if (outputLease_ != 0) vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
    outputLease_ = 0;
    status_.outputLeaseHeld = false;
    attached_ = false;
    status_.state = PPC64ConsoleSessionState::DETACHED;
    status_.outputWorkerState = HostConsoleOutputWorkerState::STOPPED;
    if (outputStateMachine_.state() != PPC64OutputSessionState::CLEANLY_STOPPED)
        (void)transitionOutput(PPC64OutputSessionState::CLEANLY_STOPPED,
                               PPC64ConsoleEventType::WORKER_STOPPED);
    recordEvent(PPC64ConsoleEventType::SESSION_DETACHED);
    if (error) error->clear();
    return true;
}

void PPC64ConsoleBridge::shutdown() noexcept {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    bool safeToReleaseLease = true;
    if (vmAlive(vm)) {
        std::string ignored;
        safeToReleaseLease = stopAsyncOutput(*vm, &ignored);
        const size_t staged = vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
        status_.stagedInputBytes = staged;
        if (staged != 0)
            status_.diagnostic = "staged input remains VM-owned for a future bridge attachment";
        if (!safeToReleaseLease) {
            vm->setPPC64ConsoleCheckpointReady(lpid_, outputLease_, false);
            status_.state = PPC64ConsoleSessionState::OUTPUT_STOP_PENDING;
        } else if (outputLease_ != 0) {
            vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
        }
    }
    if (safeToReleaseLease) outputLease_ = 0;
    attached_ = false;
    if (transport_) transport_->shutdown();
    if (safeToReleaseLease) {
        status_.outputLeaseHeld = false;
        if (outputStateMachine_.state() !=
            PPC64OutputSessionState::CLEANLY_STOPPED)
            (void)transitionOutput(PPC64OutputSessionState::CLEANLY_STOPPED,
                                   PPC64ConsoleEventType::WORKER_STOPPED);
        status_.state = vmAlive(vm)
            ? PPC64ConsoleSessionState::SHUTDOWN
            : PPC64ConsoleSessionState::VM_DESTROYED;
    } else {
        (void)transitionOutput(PPC64OutputSessionState::QUARANTINED,
                               PPC64ConsoleEventType::WORKER_QUARANTINED,
                               status_.outputHandoffBytesPending,
                               HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED,
                               status_.lastSystemError);
        status_.workerQuarantined = true;
    }
}

void PPC64ConsoleBridge::fail(PPC64ConsoleSessionState state,
                              const std::string& diagnostic) {
    status_.state = state;
    status_.diagnostic = diagnostic;
}

void PPC64ConsoleBridge::noticeGenerationChange(ia64::VirtualMachine& vm) {
    const uint64_t current = vm.getPPC64ConsoleGeneration();
    if (current == observedGeneration_) return;
    observedGeneration_ = current;
    status_.vmResetGeneration = current;
    transport_->resetInput();
    inputEOF_ = false;
    status_.diagnostic = "staged and unread host input cleared at VM reset or restore boundary";
    updateCheckpointReady(vm);
}

void PPC64ConsoleBridge::updateCheckpointReady(ia64::VirtualMachine& vm) {
    const size_t staged = vm.getPPC64ConsoleInputCount(lpid_, outputLease_);
    status_.stagedInputBytes = staged;
    status_.terminalOutputBytesPending = terminalBytesNotHandedToWorker(
        vm, lpid_, outputSubmissionBytes_,
        outputSubmissionGeneration_ == vm.getPPC64ConsoleGeneration());
    status_.guestOutputBytesPending =
        vm.getPPC64TerminalPendingOutputBytes(lpid_);
    status_.knownNotAttemptedBytes = status_.terminalOutputBytesPending;
    vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_,
        staged == 0 && outputSubmissionBytes_ == 0 &&
        status_.unresolvedUncertainBytes == 0 &&
        outputStateMachine_.state() != PPC64OutputSessionState::QUARANTINED);
}

void PPC64ConsoleBridge::transferInput(ia64::VirtualMachine& vm) {
    const auto staged = vm.peekPPC64ConsoleInput(lpid_, outputLease_, transferChunk_);
    if (staged.empty()) { updateCheckpointReady(vm); return; }
    const size_t capacity = vm.getPPC64TerminalQueueCapacity();
    const size_t pending = vm.getPPC64TerminalPendingInputBytes(lpid_);
    if (pending >= capacity) { updateCheckpointReady(vm); return; }
    const size_t count = std::min(staged.size(), capacity - pending);
    std::vector<uint8_t> bytes(staged.begin(), staged.begin() +
        static_cast<std::ptrdiff_t>(count));
    const PPC64TerminalResult result =
        vm.injectPPC64TerminalInputWithStatus(bytes, lpid_);
    if (result == PPC64TerminalResult::SUCCESS) {
        if (!vm.consumePPC64ConsoleInput(lpid_, outputLease_, count)) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "VM staged-input ownership changed during transfer");
            return;
        }
        status_.guestInputBytesAccepted = incrementSaturated(
            status_.guestInputBytesAccepted, count);
    } else if (result == PPC64TerminalResult::PARAMETER ||
               result == PPC64TerminalResult::RESOURCE) {
        fail(PPC64ConsoleSessionState::INVALID_BINDING,
             "VM rejected input for the bound terminal channel");
    }
    updateCheckpointReady(vm);
}

void PPC64ConsoleBridge::transferOutput(ia64::VirtualMachine& vm,
                                       size_t maximumBytes) {
    if (status_.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED ||
        status_.state == PPC64ConsoleSessionState::FAILED) return;

    if (auto* async = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
        async != nullptr && async->usesAsyncOutput()) {
        if (!reconcileAsyncOutput(vm)) return;
        const auto currentWorker = async->outputWorkerSnapshot();
        if (currentWorker.state == HostConsoleOutputWorkerState::WRITING &&
            outputSubmissionBytes_ != 0 &&
            outputStateMachine_.state() == PPC64OutputSessionState::OUTPUT_QUEUED)
            (void)transitionOutput(PPC64OutputSessionState::WRITE_IN_PROGRESS);
        if (outputSubmissionBytes_ != 0 || outputStopRequested_) {
            updateCheckpointReady(vm);
            return;
        }
        if (outputRetryAfter_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() < outputRetryAfter_) return;
        const size_t request = std::min(maximumBytes,
                                       async->outputWorkerSnapshot().capacity);
        const auto peeked = vm.readPPC64ConsoleOutput(lpid_, outputLease_,
                                                      request);
        if (peeked.empty()) {
            updateCheckpointReady(vm);
            return;
        }
        const HostConsoleOutputSubmission submitted =
            async->submitOutput(peeked.data(), peeked.size());
        status_.lastOutputStatus = submitted.status;
        if (submitted.systemError != 0)
            status_.lastSystemError = submitted.systemError;
        if (submitted.queuedBytes > peeked.size()) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "host worker accepted more output than was offered");
            return;
        }
        if (submitted.queuedBytes != 0) {
            if (submitted.status != HostConsoleIOStatus::OK) {
                fail(PPC64ConsoleSessionState::FAILED,
                     "host worker reported queued bytes with a non-success status");
                return;
            }
            outputSubmissionBytes_ = submitted.queuedBytes;
            outputSubmissionGeneration_ = vm.getPPC64ConsoleGeneration();
            outputSubmissionSessionGeneration_ = sessionGeneration_;
            status_.outputBytesHandedToWorker = incrementSaturated(
                status_.outputBytesHandedToWorker, submitted.queuedBytes);
            status_.writeAttempts = incrementSaturated(status_.writeAttempts, 1);
            status_.lastFailure = HostConsoleFailureClass::NONE;
            (void)transitionOutput(PPC64OutputSessionState::OUTPUT_QUEUED,
                                   PPC64ConsoleEventType::WRITE_SUBMITTED,
                                   submitted.queuedBytes);
            outputRetryAfter_ = {};
            updateCheckpointReady(vm);
            const auto worker = async->outputWorkerSnapshot();
            status_.outputHandoffCapacity = worker.capacity;
            status_.outputHandoffBytesPending = worker.bufferedBytes;
            status_.workerBytesPending = worker.bufferedBytes;
            status_.outputWorkerState = worker.state;
            return;
        }
        if (submitted.status == HostConsoleIOStatus::DISCONNECTED) {
            fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
                 "redirected stdout pipe disconnected before accepting output");
            status_.lastFailure = HostConsoleFailureClass::BROKEN_PIPE;
            (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                                   PPC64ConsoleEventType::BROKEN_PIPE,
                                   0, HostConsoleFailureClass::BROKEN_PIPE,
                                   submitted.systemError);
            (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
        } else if (submitted.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
                   submitted.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "redirected stdout worker rejected output permanently");
            status_.lastFailure = classifyOutputFailure(submitted.status,
                                                        submitted.systemError);
            (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                                   PPC64ConsoleEventType::STATE_TRANSITION,
                                   0, status_.lastFailure,
                                   submitted.systemError);
            (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
        } else if (submitted.status == HostConsoleIOStatus::WOULD_BLOCK) {
            status_.backpressureEvents = incrementSaturated(
                status_.backpressureEvents, 1);
            status_.lastFailure = HostConsoleFailureClass::TEMPORARY_BACKPRESSURE;
            (void)transitionOutput(PPC64OutputSessionState::BACKPRESSURED,
                                   PPC64ConsoleEventType::BACKPRESSURE_ENTERED,
                                   0, HostConsoleFailureClass::TEMPORARY_BACKPRESSURE);
        }
        const auto worker = async->outputWorkerSnapshot();
        status_.outputHandoffCapacity = worker.capacity;
        status_.outputHandoffBytesPending = worker.bufferedBytes;
        status_.workerBytesPending = worker.bufferedBytes;
        status_.outputWorkerState = worker.state;
        updateCheckpointReady(vm);
        return;
    }

    const auto peeked = vm.readPPC64ConsoleOutput(lpid_, outputLease_,
                                                  maximumBytes);
    if (peeked.empty()) return;
    (void)transitionOutput(PPC64OutputSessionState::WRITE_IN_PROGRESS);
    status_.writeAttempts = incrementSaturated(status_.writeAttempts, 1);
    const HostConsoleIOResult result =
        transport_->writeOutput(peeked.data(), peeked.size());
    status_.writeCompletions = incrementSaturated(status_.writeCompletions, 1);
    recordEvent(PPC64ConsoleEventType::WRITE_COMPLETED, result.bytes,
                classifyOutputFailure(result.status, result.systemError),
                result.systemError);
    status_.lastOutputStatus = result.status;
    if (result.systemError != 0) status_.lastSystemError = result.systemError;
    if (result.bytes > peeked.size()) {
        fail(PPC64ConsoleSessionState::FAILED,
             "host transport reported an output count larger than requested");
        return;
    }
    if (result.bytes != 0) {
        const auto drained = vm.drainPPC64ConsoleOutput(lpid_, outputLease_,
                                                        result.bytes);
        if (drained.size() != result.bytes) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "terminal output changed while its host lease was active");
            return;
        }
        status_.guestOutputBytesAcceptedByHost = incrementSaturated(
            status_.guestOutputBytesAcceptedByHost, result.bytes);
    }
    if (result.status == HostConsoleIOStatus::DISCONNECTED) {
        fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
             "host output sink disconnected; undelivered guest bytes remain queued");
        status_.lastFailure = HostConsoleFailureClass::BROKEN_PIPE;
        (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                               PPC64ConsoleEventType::BROKEN_PIPE,
                               result.bytes, HostConsoleFailureClass::BROKEN_PIPE,
                               result.systemError);
        (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
    } else if (result.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
               result.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
        fail(PPC64ConsoleSessionState::FAILED,
             "host output failed; undelivered guest bytes remain queued");
        status_.lastFailure = classifyOutputFailure(result.status,
                                                    result.systemError);
        (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                               PPC64ConsoleEventType::STATE_TRANSITION,
                               result.bytes, status_.lastFailure,
                               result.systemError);
        (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
    } else if (result.status == HostConsoleIOStatus::WOULD_BLOCK) {
        status_.backpressureEvents = incrementSaturated(status_.backpressureEvents, 1);
        status_.lastFailure = HostConsoleFailureClass::TEMPORARY_BACKPRESSURE;
        (void)transitionOutput(PPC64OutputSessionState::BACKPRESSURED,
                               PPC64ConsoleEventType::BACKPRESSURE_ENTERED,
                               peeked.size() - result.bytes,
                               HostConsoleFailureClass::TEMPORARY_BACKPRESSURE);
    } else {
        if (result.bytes != 0 && result.bytes < peeked.size()) {
            status_.partialWrites = incrementSaturated(status_.partialWrites, 1);
            recordEvent(PPC64ConsoleEventType::PARTIAL_WRITE, result.bytes);
        }
        status_.lastFailure = HostConsoleFailureClass::NONE;
        (void)transitionOutput(PPC64OutputSessionState::CONNECTED,
                               result.bytes != 0 && result.bytes < peeked.size()
                                   ? PPC64ConsoleEventType::PARTIAL_WRITE
                                   : PPC64ConsoleEventType::STATE_TRANSITION,
                               result.bytes);
    }
}

bool PPC64ConsoleBridge::reconcileAsyncOutput(ia64::VirtualMachine& vm) {
    auto* async = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
    if (async == nullptr || !async->usesAsyncOutput()) return true;
    const HostConsoleOutputCompletion completion =
        async->pollOutputCompletion();
    if (completion.ready) {
        const bool countsFit = completion.confirmedBytes <= outputSubmissionBytes_ &&
            completion.uncertainBytes <= outputSubmissionBytes_ -
                std::min(outputSubmissionBytes_, completion.confirmedBytes) &&
            completion.notAttemptedBytes <= outputSubmissionBytes_ -
                std::min(outputSubmissionBytes_, completion.confirmedBytes) -
                std::min(outputSubmissionBytes_ -
                             std::min(outputSubmissionBytes_, completion.confirmedBytes),
                         completion.uncertainBytes);
        const size_t settled = countsFit
            ? completion.confirmedBytes + completion.uncertainBytes +
                  completion.notAttemptedBytes
            : 0;
        if (outputSubmissionBytes_ == 0 || !countsFit ||
            settled != outputSubmissionBytes_) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "stdout worker completion did not reconcile with its submitted prefix");
            status_.lastFailure = HostConsoleFailureClass::WORKER_TERMINATION;
            (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                                   PPC64ConsoleEventType::STATE_TRANSITION,
                                   0, status_.lastFailure,
                                   completion.systemError);
            return false;
        }
        status_.writeCompletions = incrementSaturated(status_.writeCompletions, 1);
        status_.guestOutputBytesAcceptedByHost = incrementSaturated(
            status_.guestOutputBytesAcceptedByHost, completion.confirmedBytes);
        status_.outputBytesWithUncertainDelivery = incrementSaturated(
            status_.outputBytesWithUncertainDelivery, completion.uncertainBytes);
        status_.outputBytesReportedNotAttempted = incrementSaturated(
            status_.outputBytesReportedNotAttempted,
            completion.notAttemptedBytes);
        recordEvent(PPC64ConsoleEventType::WRITE_COMPLETED,
                    completion.confirmedBytes,
                    classifyOutputFailure(completion.status,
                                          completion.systemError),
                    completion.systemError);
        if (completion.partialWrite) {
            status_.partialWrites = incrementSaturated(status_.partialWrites, 1);
            recordEvent(PPC64ConsoleEventType::PARTIAL_WRITE,
                        completion.confirmedBytes);
        }
        const bool currentSession =
            outputSubmissionSessionGeneration_ == sessionGeneration_;
        if (outputSubmissionGeneration_ == vm.getPPC64ConsoleGeneration() &&
            currentSession) {
            const size_t committed = completion.confirmedBytes +
                                     completion.uncertainBytes;
            if (committed != 0) {
                const auto drained = vm.drainPPC64ConsoleOutput(
                    lpid_, outputLease_, committed);
                if (drained.size() != committed) {
                    fail(PPC64ConsoleSessionState::FAILED,
                         "terminal output changed before worker completion was committed");
                    return false;
                }
            }
        } else {
            status_.staleGenerationOutputCompletions = incrementSaturated(
                status_.staleGenerationOutputCompletions, 1);
            status_.lastFailure = HostConsoleFailureClass::INVALID_SESSION_GENERATION;
        }
        if (completion.uncertainBytes != 0) {
            uncertainSegmentBytes_ = completion.uncertainBytes;
            status_.unresolvedUncertainBytes = addSizeSaturated(
                status_.unresolvedUncertainBytes, completion.uncertainBytes);
            status_.uncertainSegmentId = nextUncertainSegmentId_;
            nextUncertainSegmentId_ = incrementSaturated(
                nextUncertainSegmentId_, 1);
            status_.lastFailure = HostConsoleFailureClass::UNCERTAIN_DELIVERY;
        }
        outputSubmissionBytes_ = 0;
        outputSubmissionGeneration_ = 0;
        outputSubmissionSessionGeneration_ = 0;
        status_.lastOutputStatus = completion.status;
        if (completion.systemError != 0)
            status_.lastSystemError = completion.systemError;
        if (completion.status == HostConsoleIOStatus::DISCONNECTED) {
            fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
                 "stdout pipe disconnected; confirmed and uncertain prefixes were reconciled; never-attempted guest bytes remain queued");
            status_.lastFailure = completion.uncertainBytes != 0
                ? HostConsoleFailureClass::UNCERTAIN_DELIVERY
                : HostConsoleFailureClass::BROKEN_PIPE;
            (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                                   PPC64ConsoleEventType::BROKEN_PIPE,
                                   completion.uncertainBytes,
                                   HostConsoleFailureClass::BROKEN_PIPE,
                                   completion.systemError);
            if (completion.uncertainBytes != 0) {
                (void)transitionOutput(PPC64OutputSessionState::DELIVERY_UNCERTAIN,
                                       PPC64ConsoleEventType::STATE_TRANSITION,
                                       completion.uncertainBytes,
                                       HostConsoleFailureClass::UNCERTAIN_DELIVERY);
            } else {
                (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
            }
        } else if (completion.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
                   completion.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "stdout worker failed permanently; never-attempted guest bytes remain queued");
            status_.lastFailure = completion.uncertainBytes != 0
                ? HostConsoleFailureClass::UNCERTAIN_DELIVERY
                : classifyOutputFailure(completion.status,
                                        completion.systemError);
            (void)transitionOutput(PPC64OutputSessionState::PERMANENT_FAILURE,
                                   PPC64ConsoleEventType::STATE_TRANSITION,
                                   completion.uncertainBytes,
                                   classifyOutputFailure(completion.status,
                                                         completion.systemError),
                                   completion.systemError);
            if (completion.uncertainBytes != 0) {
                (void)transitionOutput(PPC64OutputSessionState::DELIVERY_UNCERTAIN,
                                       PPC64ConsoleEventType::STATE_TRANSITION,
                                       completion.uncertainBytes,
                                       HostConsoleFailureClass::UNCERTAIN_DELIVERY);
            } else {
                (void)transitionOutput(PPC64OutputSessionState::RECONNECT_ELIGIBLE);
            }
        } else if (completion.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
                   completion.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
            // Kept for clarity if additional permanent statuses are added.
        } else if (completion.status == HostConsoleIOStatus::WOULD_BLOCK) {
            outputRetryAfter_ = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(10);
            status_.backpressureEvents = incrementSaturated(
                status_.backpressureEvents, 1);
            status_.lastFailure = HostConsoleFailureClass::TEMPORARY_BACKPRESSURE;
            (void)transitionOutput(PPC64OutputSessionState::BACKPRESSURED,
                                   PPC64ConsoleEventType::BACKPRESSURE_ENTERED,
                                   completion.notAttemptedBytes,
                                   HostConsoleFailureClass::TEMPORARY_BACKPRESSURE);
        } else if (completion.status == HostConsoleIOStatus::SHUTDOWN) {
            status_.lastFailure = HostConsoleFailureClass::CANCELLATION_COMPLETED;
        } else {
            if (outputStateMachine_.state() ==
                PPC64OutputSessionState::BACKPRESSURED)
                recordEvent(PPC64ConsoleEventType::BACKPRESSURE_CLEARED);
            status_.lastFailure = HostConsoleFailureClass::NONE;
            (void)transitionOutput(PPC64OutputSessionState::CONNECTED);
        }
    }
    const auto worker = async->outputWorkerSnapshot();
    status_.outputHandoffCapacity = worker.capacity;
    status_.outputHandoffBytesPending = worker.bufferedBytes;
    status_.workerBytesPending = worker.bufferedBytes;
    status_.outputWorkerState = worker.state;
    status_.inFlightBytes = worker.state == HostConsoleOutputWorkerState::WRITING
        ? worker.bufferedBytes : 0;
    status_.workerRunning = worker.state == HostConsoleOutputWorkerState::QUEUED ||
        worker.state == HostConsoleOutputWorkerState::WRITING ||
        worker.state == HostConsoleOutputWorkerState::STOPPING;
    status_.workerStopped = worker.state == HostConsoleOutputWorkerState::STOPPED;
    if (worker.state == HostConsoleOutputWorkerState::WRITING &&
        outputSubmissionBytes_ != 0 &&
        outputStateMachine_.state() == PPC64OutputSessionState::OUTPUT_QUEUED)
        (void)transitionOutput(PPC64OutputSessionState::WRITE_IN_PROGRESS);
    updateCheckpointReady(vm);
    return status_.state != PPC64ConsoleSessionState::FAILED &&
           status_.state != PPC64ConsoleSessionState::OUTPUT_DISCONNECTED;
}

bool PPC64ConsoleBridge::stopAsyncOutput(ia64::VirtualMachine& vm,
                                         std::string* error) {
    auto* async = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
    if (async == nullptr || !async->usesAsyncOutput()) return true;
    const bool alreadyStopping = outputStopRequested_;
    outputStopRequested_ = true;

    // Allow one bounded normal drain attempt, then request cancellation against
    // this worker's exact I/O and wait a second bounded interval.
    HostConsoleOutputStopResult stopped =
        async->stopOutput(false, std::chrono::milliseconds(50));
    if (!stopped.completed) {
        if (!alreadyStopping) {
            status_.cancellationRequests = incrementSaturated(
                status_.cancellationRequests, 1);
            status_.lastFailure = HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED;
            (void)transitionOutput(PPC64OutputSessionState::CANCELLATION_REQUESTED,
                                   PPC64ConsoleEventType::CANCELLATION_REQUESTED,
                                   outputSubmissionBytes_,
                                   HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED);
        }
        const auto canceled =
            async->stopOutput(true, std::chrono::milliseconds(150));
        if (canceled.cancellationError != 0)
            status_.lastSystemError = canceled.cancellationError;
        stopped = canceled;
    }
    if (!stopped.completed) {
        status_.state = PPC64ConsoleSessionState::OUTPUT_STOP_PENDING;
        status_.lastFailure = HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED;
        status_.diagnostic =
            "stdout worker completion is still pending after a bounded cancellation request; output lease and VM buffers remain owned";
        (void)transitionOutput(PPC64OutputSessionState::CANCELLATION_PENDING,
                               PPC64ConsoleEventType::CANCELLATION_OBSERVED,
                               outputSubmissionBytes_,
                               HostConsoleFailureClass::CANCELLATION_REQUESTED_UNRESOLVED,
                               stopped.cancellationError);
        vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_, false);
        if (error) *error = status_.diagnostic;
        const auto worker = async->outputWorkerSnapshot();
        status_.outputHandoffCapacity = worker.capacity;
        status_.outputHandoffBytesPending = worker.bufferedBytes;
        status_.outputWorkerState = worker.state;
        return false;
    }
    if (stopped.cancellationRequested) {
        status_.cancellationCompletions = incrementSaturated(
            status_.cancellationCompletions, 1);
        recordEvent(PPC64ConsoleEventType::CANCELLATION_OBSERVED, 0,
                    HostConsoleFailureClass::CANCELLATION_COMPLETED);
    }
    const bool outputStateHealthy = reconcileAsyncOutput(vm);
    if (outputSubmissionBytes_ != 0) {
        status_.state = PPC64ConsoleSessionState::OUTPUT_STOP_PENDING;
        status_.lastFailure = HostConsoleFailureClass::WORKER_TERMINATION;
        status_.diagnostic =
            "stdout worker stopped without a completion record; output lease remains held";
        vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_, false);
        if (error) *error = status_.diagnostic;
        return false;
    }
    (void)outputStateHealthy; // A settled permanent sink failure is safe to detach.
    if (error) error->clear();
    return true;
}

void PPC64ConsoleBridge::poll(size_t maximumInputRead,
                              size_t maximumOutputWrite) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    if (!attached_) return;
    if (!vmAlive(vm)) {
        attached_ = false;
        status_.state = PPC64ConsoleSessionState::VM_DESTROYED;
        status_.diagnostic = "bound PPC64 VM has been destroyed";
        if (transport_) transport_->shutdown();
        return;
    }
    noticeGenerationChange(*vm);
    if (outputStopRequested_) {
        (void)reconcileAsyncOutput(*vm);
        return;
    }
    if (!transport_ || !transport_->isAttached()) {
        fail(PPC64ConsoleSessionState::SHUTDOWN,
             "host transport is detached or shut down");
        return;
    }
    transferOutput(*vm, maximumOutputWrite);
    if (vm->cpus_.empty() || vm->cpus_[0].state == ia64::CPUExecutionState::HALTED) {
        status_.state = PPC64ConsoleSessionState::VM_HALTED;
        status_.diagnostic = "VM is halted; staged input remains buffered until reset or explicit discard";
        updateCheckpointReady(*vm);
        return;
    }
    if (vm->cpus_[0].state == ia64::CPUExecutionState::ERROR) {
        fail(PPC64ConsoleSessionState::FAILED,
             "VM CPU is in an error state; staged input remains buffered");
        updateCheckpointReady(*vm);
        return;
    }
    transferInput(*vm);

    const size_t staged = vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
    const size_t free = staged >= inputCapacity_ ? 0 : inputCapacity_ - staged;
    const size_t request = std::min({maximumInputRead, free, size_t{4096}});
    if (request != 0 && !inputEOF_ &&
        status_.state != PPC64ConsoleSessionState::INPUT_DISCONNECTED &&
        status_.state != PPC64ConsoleSessionState::FAILED) {
        std::array<uint8_t, 4096> buffer{};
        const HostConsoleIOResult result =
            transport_->readInput(buffer.data(), request);
        status_.lastInputStatus = result.status;
        if (result.systemError != 0) status_.lastSystemError = result.systemError;
        if (result.bytes > request) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "host transport reported an input count larger than requested");
        } else if (result.bytes != 0) {
            const size_t stagedBytes = vm->stagePPC64ConsoleInput(
                lpid_, outputLease_, buffer.data(), result.bytes);
            status_.hostInputBytesRead = incrementSaturated(
                status_.hostInputBytesRead, result.bytes);
            if (stagedBytes != result.bytes) {
                fail(PPC64ConsoleSessionState::FAILED,
                     "VM bridge staging queue could not retain all host-read bytes");
            }
        }
        if (result.status == HostConsoleIOStatus::EOF_REACHED) {
            inputEOF_ = true;
            if (status_.state == PPC64ConsoleSessionState::ACTIVE)
                status_.state = PPC64ConsoleSessionState::INPUT_EOF;
        } else if (result.status == HostConsoleIOStatus::DISCONNECTED) {
            if (status_.state == PPC64ConsoleSessionState::ACTIVE)
                status_.state = PPC64ConsoleSessionState::INPUT_DISCONNECTED;
            status_.diagnostic = "host input disconnected; staged bytes will still be delivered";
        } else if (result.status == HostConsoleIOStatus::INVALID_CONFIGURATION ||
                   result.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "host input transport failed");
        }
    }
    transferInput(*vm);
    transferOutput(*vm, maximumOutputWrite);
    updateCheckpointReady(*vm);
}

bool PPC64ConsoleBridge::writeCheckpoint(const std::string& path,
                                        const std::string& guestIdentity,
                                        std::string* error) {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    if (!attached_ || !vmAlive(vm)) {
        if (error) *error = "console bridge is not attached to a live VM";
        return false;
    }
    noticeGenerationChange(*vm);
    if (outputSubmissionBytes_ != 0) {
        if (error) {
            *error = "checkpoint rejected: redirected stdout has a queued or in-flight write whose external completion is unresolved";
        }
        return false;
    }
    if (status_.unresolvedUncertainBytes != 0) {
        if (error) {
            *error = "checkpoint rejected: output delivery uncertainty must be explicitly resolved";
        }
        return false;
    }
    const size_t staged = vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
    if (staged != 0) {
        const size_t capacity = vm->getPPC64TerminalQueueCapacity();
        const size_t pending = vm->getPPC64TerminalPendingInputBytes(lpid_);
        if (staged > capacity - std::min(capacity, pending)) {
            if (error) *error = "staged input does not fit in the guest terminal; checkpoint rejected without changing VM state";
            return false;
        }
        std::vector<uint8_t> bytes =
            vm->peekPPC64ConsoleInput(lpid_, outputLease_, staged);
        if (bytes.size() != staged) {
            if (error) *error = "bridge staging ownership changed before checkpoint";
            return false;
        }
        const PPC64TerminalResult result =
            vm->injectPPC64TerminalInputWithStatus(bytes, lpid_);
        if (result != PPC64TerminalResult::SUCCESS) {
            if (error) *error = "staged input could not be quiesced into the guest terminal";
            return false;
        }
        status_.guestInputBytesAccepted = incrementSaturated(
            status_.guestInputBytesAccepted, bytes.size());
        if (!vm->consumePPC64ConsoleInput(lpid_, outputLease_, staged)) {
            if (error) *error = "could not consume quiesced bridge input";
            return false;
        }
        updateCheckpointReady(*vm);
    }
    return vm->writeDiagnosticCheckpoint(path, guestIdentity, error);
}

PPC64ConsoleBridgeStatus PPC64ConsoleBridge::status() const {
    std::lock_guard<std::mutex> guard(statusMutex_);
    PPC64ConsoleBridgeStatus result = status_;
    if (const auto* async = dynamic_cast<const IAsyncHostConsoleOutput*>(
            transport_.get()); async != nullptr && async->usesAsyncOutput()) {
        const auto worker = async->outputWorkerSnapshot();
        result.outputWorkerState = worker.state;
        result.outputHandoffCapacity = worker.capacity;
        result.outputHandoffBytesPending = worker.bufferedBytes;
        result.inFlightBytes = worker.state == HostConsoleOutputWorkerState::WRITING
            ? worker.bufferedBytes : 0;
        result.workerRunning = worker.state == HostConsoleOutputWorkerState::QUEUED ||
            worker.state == HostConsoleOutputWorkerState::WRITING ||
            worker.state == HostConsoleOutputWorkerState::STOPPING;
    }
    if (attached_) {
        ia64::VirtualMachine* vm = nullptr;
        if (!vmAlive(vm)) result.state = PPC64ConsoleSessionState::VM_DESTROYED;
        else {
            result.stagedInputBytes =
                vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
            result.terminalOutputBytesPending = terminalBytesNotHandedToWorker(
                *vm, lpid_, outputSubmissionBytes_,
                outputSubmissionGeneration_ == vm->getPPC64ConsoleGeneration());
            result.knownNotAttemptedBytes = result.terminalOutputBytesPending;
            result.outputLeaseHeld = ownsOutputLease(*vm);
            result.vmIdentity = vm->getPPC64ConsoleVMIdentity();
            result.sessionGeneration = sessionGeneration_;
            result.vmResetGeneration = vm->getPPC64ConsoleGeneration();
            result.sinkType = transport_ ? transport_->sinkType()
                                         : HostConsoleSinkType::UNKNOWN;
        }
    }
    return result;
}

std::vector<PPC64ConsoleDiagnosticEvent>
PPC64ConsoleBridge::diagnosticEvents() const {
    std::lock_guard<std::mutex> guard(statusMutex_);
    std::vector<PPC64ConsoleDiagnosticEvent> result;
    result.reserve(eventRingCount_);
    for (size_t i = 0; i < eventRingCount_; ++i)
        result.push_back(eventRing_[(eventRingStart_ + i) % eventRing_.size()]);
    return result;
}

size_t PPC64ConsoleBridge::stagedInputBytes() const noexcept {
    std::lock_guard<std::mutex> guard(statusMutex_);
    ia64::VirtualMachine* vm = nullptr;
    return vmAlive(vm)
        ? vm->getPPC64ConsoleInputCount(lpid_, outputLease_)
        : status_.stagedInputBytes;
}

} // namespace ia64::ppc64
