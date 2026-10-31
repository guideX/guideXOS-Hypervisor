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
} // namespace

struct StdioHostConsoleTransport::OutputWorker final
    : std::enable_shared_from_this<OutputWorker> {
#ifdef _WIN32
    OutputWorker(HANDLE sinkHandle, void* sinkKey, size_t queueCapacity)
        : sink(sinkHandle), key(sinkKey), capacity(queueCapacity),
          buffer(queueCapacity) {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~OutputWorker() {
        if (thread.joinable()) std::terminate();
        if (ownsSinkKey) releasePipeSink(key);
        if (event != nullptr) CloseHandle(event);
        if (sink != nullptr && sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    }

    bool valid() const noexcept { return event != nullptr; }

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
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (reaperOwnsState) return;
            reaperOwnsState = true;
        }
        struct Reaper {
            Reaper() : thread([this] { run(); }) {}

            void keep(const std::shared_ptr<OutputWorker>& worker) {
                std::lock_guard<std::mutex> lock(mutex);
                workers.push_back(worker);
                cv.notify_all();
            }

            void run() {
                for (;;) {
                    std::vector<std::shared_ptr<OutputWorker>> ready;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait_for(lock, std::chrono::milliseconds(25));
                        auto current = workers.begin();
                        while (current != workers.end()) {
                            if ((*current)->isDone()) {
                                ready.push_back(std::move(*current));
                                current = workers.erase(current);
                            } else {
                                ++current;
                            }
                        }
                    }
                    for (const auto& worker : ready) {
                        if (worker->thread.joinable()) worker->thread.join();
                        worker->releaseSinkLease();
                    }
                }
            }

            std::mutex mutex;
            std::condition_variable cv;
            std::vector<std::shared_ptr<OutputWorker>> workers;
            std::thread thread;
        };
        // A timed-out synchronous pipe write must outlive its transport. The
        // process-lifetime reaper keeps the joinable worker, its OVERLAPPED
        // storage, buffer, and duplicated handle until I/O really completes.
        static Reaper* const reaper = new Reaper();
        reaper->keep(shared_from_this());
    }

private:
    void complete(HostConsoleIOStatus status, int systemError,
                  size_t confirmedBytes, size_t uncertainBytes,
                  size_t notAttemptedBytes) {
        std::lock_guard<std::mutex> lock(mutex);
        completion.ready = true;
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
    if (attached_) { error.clear(); return true; }
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
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
            pipeWorker.reset(); // also releases the exclusive sink lease
            error = "could not create redirected stdout worker event: " +
                    std::to_string(eventError);
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
    error = "stdio transport is implemented for the Windows host environment";
    return false;
#endif
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
}

PPC64ConsoleBridge::~PPC64ConsoleBridge() { shutdown(); }

bool PPC64ConsoleBridge::vmAlive(ia64::VirtualMachine*& vm) const noexcept {
    auto lifetime = lifetime_.lock();
    if (!lifetime || vm_ == nullptr) return false;
    vm = vm_;
    return true;
}

bool PPC64ConsoleBridge::attach(std::string& error) {
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
        error = status_.diagnostic;
        return false;
    }
    if (!transport_->attach(error)) {
        vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
        outputLease_ = 0;
        status_.state = PPC64ConsoleSessionState::FAILED;
        status_.diagnostic = error;
        return false;
    }
    observedGeneration_ = vm->getPPC64ConsoleGeneration();
    attached_ = true;
    status_.state = PPC64ConsoleSessionState::ACTIVE;
    status_.terminalId = terminalId_;
    status_.diagnostic.clear();
    updateCheckpointReady(*vm);
    error.clear();
    return true;
}

bool PPC64ConsoleBridge::detach(bool discardStagedInput, std::string* error) {
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
    attached_ = false;
    status_.state = PPC64ConsoleSessionState::DETACHED;
    if (error) error->clear();
    return true;
}

void PPC64ConsoleBridge::shutdown() noexcept {
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
        status_.state = vmAlive(vm)
            ? PPC64ConsoleSessionState::SHUTDOWN
            : PPC64ConsoleSessionState::VM_DESTROYED;
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
    vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_,
                                      staged == 0 && outputSubmissionBytes_ == 0);
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
        status_.guestInputBytesAccepted += count;
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
            status_.outputBytesHandedToWorker += submitted.queuedBytes;
            outputRetryAfter_ = {};
            updateCheckpointReady(vm);
            const auto worker = async->outputWorkerSnapshot();
            status_.outputHandoffCapacity = worker.capacity;
            status_.outputHandoffBytesPending = worker.bufferedBytes;
            status_.outputWorkerState = worker.state;
            return;
        }
        if (submitted.status == HostConsoleIOStatus::DISCONNECTED) {
            fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
                 "redirected stdout pipe disconnected before accepting output");
        } else if (submitted.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
                   submitted.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "redirected stdout worker rejected output permanently");
        }
        const auto worker = async->outputWorkerSnapshot();
        status_.outputHandoffCapacity = worker.capacity;
        status_.outputHandoffBytesPending = worker.bufferedBytes;
        status_.outputWorkerState = worker.state;
        updateCheckpointReady(vm);
        return;
    }

    const auto peeked = vm.readPPC64ConsoleOutput(lpid_, outputLease_,
                                                  maximumBytes);
    if (peeked.empty()) return;
    const HostConsoleIOResult result =
        transport_->writeOutput(peeked.data(), peeked.size());
    status_.lastOutputStatus = result.status;
    status_.lastSystemError = result.systemError;
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
        status_.guestOutputBytesAcceptedByHost += result.bytes;
    }
    if (result.status == HostConsoleIOStatus::DISCONNECTED) {
        fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
             "host output sink disconnected; undelivered guest bytes remain queued");
    } else if (result.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
               result.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
        fail(PPC64ConsoleSessionState::FAILED,
             "host output failed; undelivered guest bytes remain queued");
    }
}

bool PPC64ConsoleBridge::reconcileAsyncOutput(ia64::VirtualMachine& vm) {
    auto* async = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
    if (async == nullptr || !async->usesAsyncOutput()) return true;
    const HostConsoleOutputCompletion completion =
        async->pollOutputCompletion();
    if (completion.ready) {
        const size_t settled = completion.confirmedBytes +
                               completion.uncertainBytes +
                               completion.notAttemptedBytes;
        if (outputSubmissionBytes_ == 0 || settled != outputSubmissionBytes_) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "stdout worker completion did not reconcile with its submitted prefix");
            return false;
        }
        status_.guestOutputBytesAcceptedByHost += completion.confirmedBytes;
        status_.outputBytesWithUncertainDelivery += completion.uncertainBytes;
        status_.outputBytesReportedNotAttempted +=
            completion.notAttemptedBytes;
        if (outputSubmissionGeneration_ == vm.getPPC64ConsoleGeneration()) {
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
            ++status_.staleGenerationOutputCompletions;
        }
        outputSubmissionBytes_ = 0;
        outputSubmissionGeneration_ = 0;
        status_.lastOutputStatus = completion.status;
        status_.lastSystemError = completion.systemError;
        if (completion.status == HostConsoleIOStatus::DISCONNECTED) {
            fail(PPC64ConsoleSessionState::OUTPUT_DISCONNECTED,
                 "stdout pipe disconnected; confirmed and uncertain prefixes were reconciled; never-attempted guest bytes remain queued");
        } else if (completion.status == HostConsoleIOStatus::UNRECOVERABLE_ERROR ||
                   completion.status == HostConsoleIOStatus::INVALID_CONFIGURATION) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "stdout worker failed permanently; never-attempted guest bytes remain queued");
        } else if (completion.status == HostConsoleIOStatus::WOULD_BLOCK) {
            outputRetryAfter_ = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(10);
        }
    }
    const auto worker = async->outputWorkerSnapshot();
    status_.outputHandoffCapacity = worker.capacity;
    status_.outputHandoffBytesPending = worker.bufferedBytes;
    status_.outputWorkerState = worker.state;
    updateCheckpointReady(vm);
    return status_.state != PPC64ConsoleSessionState::FAILED &&
           status_.state != PPC64ConsoleSessionState::OUTPUT_DISCONNECTED;
}

bool PPC64ConsoleBridge::stopAsyncOutput(ia64::VirtualMachine& vm,
                                         std::string* error) {
    auto* async = dynamic_cast<IAsyncHostConsoleOutput*>(transport_.get());
    if (async == nullptr || !async->usesAsyncOutput()) return true;
    outputStopRequested_ = true;

    // Allow one bounded normal drain attempt, then request cancellation against
    // this worker's exact I/O and wait a second bounded interval.
    HostConsoleOutputStopResult stopped =
        async->stopOutput(false, std::chrono::milliseconds(50));
    if (!stopped.completed) {
        const auto canceled =
            async->stopOutput(true, std::chrono::milliseconds(150));
        if (canceled.cancellationError != 0)
            status_.lastSystemError = canceled.cancellationError;
        stopped = canceled;
    }
    if (!stopped.completed) {
        status_.state = PPC64ConsoleSessionState::OUTPUT_STOP_PENDING;
        status_.diagnostic =
            "stdout worker completion is still pending after a bounded cancellation request; output lease and VM buffers remain owned";
        vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_, false);
        if (error) *error = status_.diagnostic;
        const auto worker = async->outputWorkerSnapshot();
        status_.outputHandoffCapacity = worker.capacity;
        status_.outputHandoffBytesPending = worker.bufferedBytes;
        status_.outputWorkerState = worker.state;
        return false;
    }
    const bool outputStateHealthy = reconcileAsyncOutput(vm);
    if (outputSubmissionBytes_ != 0) {
        status_.state = PPC64ConsoleSessionState::OUTPUT_STOP_PENDING;
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
        status_.lastSystemError = result.systemError;
        if (result.bytes > request) {
            fail(PPC64ConsoleSessionState::FAILED,
                 "host transport reported an input count larger than requested");
        } else if (result.bytes != 0) {
            const size_t stagedBytes = vm->stagePPC64ConsoleInput(
                lpid_, outputLease_, buffer.data(), result.bytes);
            status_.hostInputBytesRead += result.bytes;
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
        status_.guestInputBytesAccepted += bytes.size();
        if (!vm->consumePPC64ConsoleInput(lpid_, outputLease_, staged)) {
            if (error) *error = "could not consume quiesced bridge input";
            return false;
        }
        updateCheckpointReady(*vm);
    }
    return vm->writeDiagnosticCheckpoint(path, guestIdentity, error);
}

PPC64ConsoleBridgeStatus PPC64ConsoleBridge::status() const {
    PPC64ConsoleBridgeStatus result = status_;
    if (const auto* async = dynamic_cast<const IAsyncHostConsoleOutput*>(
            transport_.get()); async != nullptr && async->usesAsyncOutput()) {
        const auto worker = async->outputWorkerSnapshot();
        result.outputWorkerState = worker.state;
        result.outputHandoffCapacity = worker.capacity;
        result.outputHandoffBytesPending = worker.bufferedBytes;
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
        }
    }
    return result;
}

size_t PPC64ConsoleBridge::stagedInputBytes() const noexcept {
    ia64::VirtualMachine* vm = nullptr;
    return vmAlive(vm)
        ? vm->getPPC64ConsoleInputCount(lpid_, outputLease_)
        : status_.stagedInputBytes;
}

} // namespace ia64::ppc64
