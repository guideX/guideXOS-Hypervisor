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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {
constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;
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
            addis(4, 0, 0x3000), addi(5, 9, 0), addi(6, 10, 0),
            addi(7, 11, 0), addi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
        });
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

bool parseLimit(int argc, char** argv, uint64_t& limit) {
    limit = 2'000'000;
    if (argc == 1) return true;
    if (argc != 3 || std::string(argv[1]) != "--max-steps") return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || value == 0) return false;
    limit = static_cast<uint64_t>(value);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    uint64_t maxSteps = 0;
    if (!parseLimit(argc, argv, maxSteps)) {
        std::cerr << "usage: ppc64_console_demo [--max-steps N]\n";
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

    std::vector<uint32_t> mainCode{
        addi(4, 0, 0xff), addi(3, 0, PPC64_H_CPPR), kSc1,
        mfmsr(6), ori(6, 6, static_cast<uint16_t>(MSR_EE)), mtmsr(6),
    };
    for (size_t i = 0; i < 64; ++i) mainCode.push_back(addi(10, 10, 1));
    mainCode.push_back(0x4bfffffcU); // bounded host loop; poll remains outside the guest
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
