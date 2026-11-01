#include "PPC64HostConsole.h"
#include "PPC64ISAPlugin.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64XICS.h"
#include "VirtualMachine.h"
#include "logger.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <atomic>
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
constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kHBusy = 1U;
constexpr size_t kEchoReadsPerInterrupt = PPC64_TERMINAL_QUEUE_CAPACITY / 16U;

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
uint32_t ld(uint8_t rt, uint8_t ra, int16_t displacement) {
    return (58U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
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

std::vector<uint32_t> guestEchoHandler() {
    std::vector<uint32_t> words{addi(3, 0, PPC64_H_XIRR), kSc1,
                                addi(8, 4, 0)};
    for (size_t i = 0; i < kEchoReadsPerInterrupt; ++i) {
        // H_GET_TERM_CHAR's count and two packed words are copied into the
        // corresponding H_PUT_TERM_CHAR arguments by guest instructions.
        words.insert(words.end(), {
            addis(4, 0, 0x3000), addi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            addi(9, 4, 0), addi(10, 5, 0), addi(11, 6, 0),
        });
        const size_t putStart = words.size();
        words.insert(words.end(), {
            addis(4, 0, 0x3000), addi(5, 9, 0), addi(6, 10, 0),
            addi(7, 11, 0), addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            // Keep the GET result live and retry it if the bounded terminal
            // FIFO reports H_BUSY. bridge.poll() runs between guest steps, so
            // a resumed host reader can make progress without losing bytes.
            (11U << 26) | (3U << 16) | kHBusy,
        });
        const int32_t branchDisplacement = -static_cast<int32_t>(
            (words.size() - putStart) * 4U);
        words.push_back((16U << 26) | (12U << 21) | (2U << 16) |
                        (static_cast<uint32_t>(branchDisplacement) & 0xfffcU));
    }
    words.insert(words.end(), {addi(4, 8, 0), addi(3, 0, PPC64_H_EOI),
                               kSc1, kRfid});
    return words;
}

bool writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        const std::array<uint8_t, 4> bytes{
            static_cast<uint8_t>(word >> 24),
            static_cast<uint8_t>(word >> 16),
            static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
        memory.Write(address + i * 4U, bytes.data(), bytes.size());
    }
    return true;
}

std::vector<uint32_t> guestPipeRecoveryProgram() {
    std::vector<uint32_t> words;
    auto append = [&](int16_t address, size_t count) {
        words.insert(words.end(), {
            addis(4, 0, 0x3000), addi(5, 0, static_cast<int16_t>(count)),
            addi(8, 0, address), ld(6, 8, 0), ld(7, 8, 8),
            addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
        });
    };
    append(0x4000, 8);
    append(0x4010, 8);
    return words;
}

bool parseOptions(int argc, char** argv, uint64_t& limit,
                  bool& recoveryDemo) {
    limit = 2'000'000;
    recoveryDemo = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--recover-pipe") {
            recoveryDemo = true;
            continue;
        }
        if (std::string(argv[i]) != "--max-steps" || i + 1 >= argc)
            return false;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || value == 0) return false;
        limit = static_cast<uint64_t>(value);
    }
    return true;
}

bool writeRecoveryPayload(IMemory& memory, uint64_t address,
                          const char (&text)[9]) {
    memory.Write(address, reinterpret_cast<const uint8_t*>(text), 8);
    return true;
}

int runPipeRecoveryDemo(VirtualMachine& vm) {
#ifdef _WIN32
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE oldReader = INVALID_HANDLE_VALUE;
    HANDLE oldWriter = INVALID_HANDLE_VALUE;
    if (input == INVALID_HANDLE_VALUE ||
        !CreatePipe(&oldReader, &oldWriter, nullptr, 4096)) {
        std::cerr << "could not create the initial demonstration pipe\n";
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return 1;
    }

    auto failedTransport = std::make_shared<StdioHostConsoleTransport>(4096);
    std::string error;
    if (!failedTransport->attachToHandles(input, oldWriter, error)) {
        std::cerr << "could not attach the initial pipe: " << error << '\n';
        CloseHandle(oldReader);
        CloseHandle(oldWriter);
        CloseHandle(input);
        return 1;
    }
    CloseHandle(oldReader); // Force the guest output operation to see a broken pipe.
    oldReader = INVALID_HANDLE_VALUE;
    CloseHandle(oldWriter);
    oldWriter = INVALID_HANDLE_VALUE;

    PPC64ConsoleBridge bridge(vm, failedTransport);
    if (!bridge.attach(error)) {
        std::cerr << "could not attach the initial console bridge: " << error << '\n';
        CloseHandle(input);
        return 1;
    }
    for (size_t i = 0; i < 7; ++i) {
        if (!vm.step()) {
            std::cerr << "guest stopped before its first hvterm1 write\n";
            (void)bridge.detach(false, &error);
            CloseHandle(input);
            return 1;
        }
        bridge.poll(0, 4096);
    }

    const auto failureDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(3);
    PPC64ConsoleBridgeStatus failedStatus;
    while (std::chrono::steady_clock::now() < failureDeadline) {
        bridge.poll(0, 4096);
        failedStatus = bridge.status();
        if (failedStatus.outputState ==
            PPC64OutputSessionState::DELIVERY_UNCERTAIN) break;
        std::this_thread::yield();
    }
    if (failedStatus.outputState != PPC64OutputSessionState::DELIVERY_UNCERTAIN) {
        std::cerr << "initial pipe did not reach an observable uncertain failure; state="
                  << static_cast<unsigned>(failedStatus.outputState)
                  << " error=" << failedStatus.lastSystemError << '\n';
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }
    std::cerr << "initial Windows pipe failed: state=delivery-uncertain, bytes="
              << failedStatus.unresolvedUncertainBytes << ", error="
              << failedStatus.lastSystemError << "; automatic reconnect refused\n";

    for (size_t i = 0; i < 7; ++i) {
        if (!vm.step()) {
            std::cerr << "guest stopped before its later hvterm1 write\n";
            (void)bridge.detach(false, &error);
            CloseHandle(input);
            return 1;
        }
        bridge.poll(0, 4096);
    }
    if (vm.getPPC64TerminalPendingOutputBytes() != 8) {
        std::cerr << "later guest output did not remain queued at the failed boundary\n";
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }
    if (!bridge.resolveOutputUncertainty(
            PPC64OutputUncertaintyResolution::DISCARD_UNCERTAIN_SEGMENT,
            failedStatus.uncertainSegmentId, error)) {
        std::cerr << "could not explicitly resolve uncertain segment: " << error << '\n';
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }

    HANDLE newReader = INVALID_HANDLE_VALUE;
    HANDLE newWriter = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&newReader, &newWriter, nullptr, 4096)) {
        std::cerr << "could not create a genuinely new replacement pipe\n";
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }
    std::array<uint8_t, 8> received{};
    size_t receivedBytes = 0;
    std::mutex captureMutex;
    std::condition_variable captureCV;
    bool captureFinished = false;
    std::thread reader([&] {
        while (receivedBytes < received.size()) {
            DWORD count = 0;
            if (!ReadFile(newReader, received.data() + receivedBytes,
                          static_cast<DWORD>(received.size() - receivedBytes),
                          &count, nullptr) || count == 0) break;
            receivedBytes += count;
        }
        {
            std::lock_guard<std::mutex> lock(captureMutex);
            captureFinished = true;
        }
        captureCV.notify_all();
    });

    auto replacement = std::make_shared<StdioHostConsoleTransport>(4096);
    if (!replacement->attachToHandles(input, newWriter, error) ||
        !bridge.reconnect(replacement, error)) {
        std::cerr << "replacement attach/reconnect failed: " << error << '\n';
        CloseHandle(newWriter);
        CloseHandle(newReader);
        reader.join();
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }
    CloseHandle(newWriter);

    const auto outputDeadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < outputDeadline) {
        bridge.poll(0, 4096);
        if (bridge.status().guestOutputBytesAcceptedByHost >= 8) break;
        std::this_thread::yield();
    }
    {
        std::unique_lock<std::mutex> lock(captureMutex);
        (void)captureCV.wait_until(lock, outputDeadline,
                                  [&] { return captureFinished; });
    }
    reader.join();
    CloseHandle(newReader);
    const std::string captured(received.begin(), received.end());
    if (receivedBytes != received.size() || captured != "IJKLMNOP" ||
        vm.getPPC64TerminalPendingOutputBytes() != 0) {
        std::cerr << "replacement pipe output did not match the later guest bytes; captured="
                  << captured << " count=" << receivedBytes << '\n';
        (void)bridge.detach(false, &error);
        CloseHandle(input);
        return 1;
    }
    std::cerr << "replacement Windows pipe received exact later guest bytes: "
              << captured << "; first uncertain segment was not replayed\n";
    if (!bridge.detach(false, &error)) {
        std::cerr << "console bridge shutdown remains pending: " << error << '\n';
        CloseHandle(input);
        return 1;
    }
    CloseHandle(input);
    return 0;
#else
    (void)vm;
    std::cerr << "--recover-pipe requires Windows anonymous pipes\n";
    return 2;
#endif
}
} // namespace

int main(int argc, char** argv) {
    uint64_t maxSteps = 0;
    bool recoveryDemo = false;
    if (!parseOptions(argc, argv, maxSteps, recoveryDemo)) {
        std::cerr << "usage: ppc64_console_demo [--max-steps N] [--recover-pipe]\n";
        return 2;
    }
    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    // Emulator diagnostics use C++ streams; keep guest bytes written through
    // the host transport as the only content on redirected stdout.
    std::cout.rdbuf(std::cerr.rdbuf());

    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.virtualTerminalEnabled = true;
    VirtualMachine vm(kRamSize, 1, "PPC64", config);
    if (!vm.init()) {
        std::cerr << "could not initialize the PPC64 VM\n";
        return 1;
    }

    std::vector<uint32_t> mainCode;
    if (recoveryDemo) {
        mainCode = guestPipeRecoveryProgram();
    } else {
        mainCode = {
            addi(4, 0, 0xff), addi(3, 0, PPC64_H_CPPR), kSc1,
            mfmsr(6), ori(6, 6, static_cast<uint16_t>(MSR_EE)), mtmsr(6),
        };
        for (size_t i = 0; i < 64; ++i) mainCode.push_back(addi(10, 10, 1));
        mainCode.push_back(0x4bfffffcU); // bounded host loop; poll remains outside the guest
    }
    const auto program = encode(mainCode);
    std::string error;
    if (!vm.preparePPC64Platform(program, kEntry, kEntry, &error)) {
        std::cerr << "could not prepare pSeries PPC64 platform: " << error << '\n';
        return 1;
    }
    const auto handler = guestEchoHandler();
    if (!writeWords(vm.getMemory(), 0x500, handler)) {
        std::cerr << "could not install the encoded guest interrupt handler\n";
        return 1;
    }

    if (recoveryDemo) {
        writeRecoveryPayload(vm.getMemory(), 0x4000, "ABCDEFGH");
        writeRecoveryPayload(vm.getMemory(), 0x4010, "IJKLMNOP");
        return runPipeRecoveryDemo(vm);
    }

    auto transport = std::make_shared<StdioHostConsoleTransport>();
    PPC64ConsoleBridge bridge(vm, transport);
    if (!bridge.attach(error)) {
        std::cerr << "could not attach host stdio: " << error << '\n';
        return 1;
    }
    std::cerr << "PPC64 hvterm1 console ready; " << transport->capabilityNote()
              << ". Input is guest data. EOF or --max-steps stops the VM.\n";

    uint64_t steps = 0;
    uint64_t quietAfterEOF = 0;
    uint64_t lastOutputBytes = 0;
    while (steps < maxSteps) {
        bridge.poll();
        if (!vm.step()) break;
        ++steps;
        bridge.poll();

        const PPC64ConsoleBridgeStatus status = bridge.status();
        if (status.state == PPC64ConsoleSessionState::FAILED ||
            status.state == PPC64ConsoleSessionState::OUTPUT_DISCONNECTED ||
            status.state == PPC64ConsoleSessionState::VM_DESTROYED) {
            std::cerr << "console session stopped: " << status.diagnostic << '\n';
            break;
        }
        const auto xics = vm.getPPC64XICSSnapshot();
        const bool outstanding = !xics.presentations.empty() &&
            !xics.presentations[0].outstanding.empty();
        const bool handlerActive = vm.getPPC64ISAPlugin()->getNIP() >= 0x500U &&
            vm.getPPC64ISAPlugin()->getNIP() < 0x500U + handler.size() * 4U;
        const bool quiescent = status.state == PPC64ConsoleSessionState::INPUT_EOF &&
            status.stagedInputBytes == 0 &&
            vm.getPPC64TerminalPendingInputBytes() == 0 &&
            vm.getPPC64TerminalPendingOutputBytes() == 0 && !outstanding &&
            !handlerActive && !xics.sources.empty() && !xics.sources[0].pending;
        if (quiescent && status.guestOutputBytesAcceptedByHost == lastOutputBytes) {
            ++quietAfterEOF;
            if (quietAfterEOF >= 128) break;
        } else {
            quietAfterEOF = 0;
        }
        lastOutputBytes = status.guestOutputBytesAcceptedByHost;
        if ((steps & 0xfffU) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto finalStatus = bridge.status();
    if (finalStatus.state != PPC64ConsoleSessionState::INPUT_EOF && steps >= maxSteps) {
        std::cerr << "execution limit reached after " << steps << " guest instructions\n";
        return 3;
    }
    std::string detachError;
    (void)bridge.detach(false, &detachError);
    return 0;
}
