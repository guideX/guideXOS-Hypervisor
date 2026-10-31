#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PPC64HostConsole.h"

#include "VirtualMachine.h"
#include "PPC64VirtualTerminal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace ia64::ppc64 {

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

StdioHostConsoleTransport::StdioHostConsoleTransport() = default;

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
    const DWORD inputType = GetFileType(input);
    inputIsDisk_ = !inputIsConsole_ && inputType == FILE_TYPE_DISK;
    if (!inputIsConsole_ && !inputIsDisk_ && inputType != FILE_TYPE_PIPE) {
        error = "standard input must be a console, redirected pipe, or file";
        return false;
    }
    const DWORD outputType = GetFileType(output);
    if (outputType != FILE_TYPE_CHAR && outputType != FILE_TYPE_PIPE &&
        outputType != FILE_TYPE_DISK) {
        error = "standard output must be a console, redirected pipe, or file";
        return false;
    }
    if (outputType == FILE_TYPE_PIPE) {
        error = "redirected standard-output pipes are unsupported because inherited synchronous pipe handles can block; redirect output to a file or console";
        return false;
    }
    inputHandle_ = input;
    outputHandle_ = output;
    attached_ = true;
    capabilityNote_ = inputIsConsole_
        ? "interactive input uses the current Windows console code page; console modes are unchanged; redirected output to files is binary-safe"
        : "redirected input is polled without blocking; redirected output to files is binary-safe; inherited output pipes are unsupported";
    error.clear();
    return true;
#else
    error = "stdio transport is implemented for the Windows host environment";
    return false;
#endif
}

void StdioHostConsoleTransport::detach() noexcept { attached_ = false; }

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

void StdioHostConsoleTransport::shutdown() noexcept { attached_ = false; }

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
        attached_ = false;
        status_.state = PPC64ConsoleSessionState::VM_DESTROYED;
        if (error) *error = "PPC64 VM has been destroyed";
        return false;
    }
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
    if (vmAlive(vm)) {
        const size_t staged = vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
        status_.stagedInputBytes = staged;
        if (staged != 0)
            status_.diagnostic = "staged input remains VM-owned for a future bridge attachment";
        if (outputLease_ != 0) vm->releasePPC64ConsoleOutput(lpid_, outputLease_);
    }
    outputLease_ = 0;
    attached_ = false;
    if (transport_) transport_->shutdown();
    status_.state = vmAlive(vm)
        ? PPC64ConsoleSessionState::SHUTDOWN
        : PPC64ConsoleSessionState::VM_DESTROYED;
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
    vm.setPPC64ConsoleCheckpointReady(lpid_, outputLease_, staged == 0);
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
    if (attached_) {
        ia64::VirtualMachine* vm = nullptr;
        if (!vmAlive(vm)) result.state = PPC64ConsoleSessionState::VM_DESTROYED;
        else result.stagedInputBytes =
            vm->getPPC64ConsoleInputCount(lpid_, outputLease_);
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
