// PPC64 Phase 14 interrupt-driven PAPR hvterm1 acceptance tests.

#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64XICS.h"
#include "VMCheckpoint.h"
#include "VMSnapshot.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
constexpr uint64_t kResult = 0x2000;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kTerminalId = 0x30000000U;
constexpr uint64_t kSprDEC = 22;

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeAddis(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (15U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeOri(uint8_t rs, uint8_t ra, uint16_t immediate) {
    return (24U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) | immediate;
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(reg) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, 467);
}

uint32_t encodeMfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}

uint32_t encodeMtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

std::vector<uint8_t> image(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

PPC64PlatformConfiguration platform(bool terminal = true,
                                     PPC64InterruptMode mode =
                                         PPC64InterruptMode::XICS_COMPAT) {
    PPC64PlatformConfiguration config;
    config.interruptMode = mode;
    config.virtualTerminalEnabled = terminal;
    return config;
}

bool prepare(VirtualMachine& vm, std::vector<uint32_t> words) {
    for (size_t i = 0; i < 64; ++i) words.push_back(encodeAddi(10, 10, 1));
    if (!vm.init()) {
        std::printf("Phase 14 VM init failed\n");
        return false;
    }
    std::string error;
    if (!vm.preparePPC64Platform(image(words), kEntry, kEntry, &error)) {
        std::printf("Phase 14 platform preparation failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        writeWord(memory, address + i * 4U, words[i]);
    }
}

uint32_t readBE32(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

uint64_t readBE64(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    uint64_t value = 0;
    for (uint8_t byte : bytes) value = (value << 8) | byte;
    return value;
}

bool step(VirtualMachine& vm) {
    const bool result = vm.step();
    CHECK(result);
    return result;
}

bool steps(VirtualMachine& vm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!step(vm)) return false;
    }
    return true;
}

std::vector<uint32_t> mainWithCPPR(uint8_t cppr = 0xff) {
    return {encodeAddi(4, 0, cppr),
            encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)), kSc1,
            encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
            encodeMtmsr(6), encodeAddi(10, 10, 1)};
}

std::vector<uint32_t> readAndAckHandler() {
    return {encodeAddi(3, 0, PPC64_H_XIRR), kSc1,
            encodeAddi(8, 4, 0),
            encodeAddis(4, 0, 0x3000),
            encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            encodeAddi(9, 4, 0), encodeAddi(10, 5, 0),
            encodeAddi(7, 0, static_cast<int16_t>(kResult)),
            encodeD(36, 8, 7, 0), encodeD(36, 9, 7, 4),
            encodeDS(62, 10, 7, 8),
            encodeAddi(4, 8, 0), encodeAddi(3, 0, PPC64_H_EOI), kSc1,
            kRfid};
}

std::vector<uint32_t> echoHandler() {
    return {encodeAddi(3, 0, PPC64_H_XIRR), kSc1,
            encodeAddi(8, 4, 0),
            encodeAddis(4, 0, 0x3000),
            encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            encodeAddi(9, 4, 0), encodeAddi(10, 5, 0),
            encodeAddi(11, 6, 0),
            encodeAddis(4, 0, 0x3000), encodeAddi(5, 9, 0),
            encodeAddi(6, 10, 0), encodeAddi(7, 11, 0),
            encodeAddi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            encodeAddi(4, 8, 0), encodeAddi(3, 0, PPC64_H_EOI), kSc1,
            kRfid};
}

uint32_t be32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

struct IndependentFDT {
    std::map<std::string, std::vector<uint8_t>> properties;
};

// Small independent FDT token/string-table reader used to validate the
// generated tree without relying on PPC64FDTBuilder's own validator.
bool parseFDT(const std::vector<uint8_t>& bytes, IndependentFDT& parsed) {
    if (bytes.size() < 40 || be32(bytes, 0) != 0xd00dfeedU) return false;
    const size_t total = be32(bytes, 4);
    const size_t structureOffset = be32(bytes, 8);
    const size_t stringsOffset = be32(bytes, 12);
    const size_t structureSize = be32(bytes, 36);
    if (total > bytes.size() || structureOffset + structureSize > total ||
        stringsOffset > total) return false;
    size_t cursor = structureOffset;
    const size_t end = structureOffset + structureSize;
    std::vector<std::string> nodes;
    while (cursor + 4 <= end) {
        const uint32_t token = be32(bytes, cursor);
        cursor += 4;
        if (token == 1) {
            const size_t begin = cursor;
            while (cursor < end && bytes[cursor] != 0) ++cursor;
            if (cursor == end) return false;
            nodes.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                               bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            cursor = (cursor + 4U) & ~size_t{3U};
        } else if (token == 2) {
            if (nodes.empty()) return false;
            nodes.pop_back();
        } else if (token == 3) {
            if (nodes.empty() || cursor + 8 > end) return false;
            const uint32_t length = be32(bytes, cursor);
            const uint32_t nameOffset = be32(bytes, cursor + 4);
            cursor += 8;
            if (cursor + length > end || stringsOffset + nameOffset >= total) return false;
            size_t nameEnd = stringsOffset + nameOffset;
            while (nameEnd < total && bytes[nameEnd] != 0) ++nameEnd;
            if (nameEnd == total) return false;
            std::string path;
            for (const auto& node : nodes) if (!node.empty()) path += "/" + node;
            if (path.empty()) path = "/";
            const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(
                                       stringsOffset + nameOffset),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd));
            parsed.properties[path + "|" + name] = std::vector<uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor = (cursor + length + 3U) & ~size_t{3U};
        } else if (token == 9) {
            return nodes.empty();
        } else {
            return false;
        }
    }
    return false;
}

const std::vector<uint8_t>* fdtProperty(const IndependentFDT& fdt,
                                        const std::string& path,
                                        const std::string& name) {
    const auto found = fdt.properties.find(path + "|" + name);
    return found == fdt.properties.end() ? nullptr : &found->second;
}

void programA_FirstInputInterrupt() {
    std::printf("[phase14-A-first-terminal-interrupt]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, mainWithCPPR()));
    const uint32_t source = vm.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    CHECK(PPC64XICS::validSourceId(source));
    CHECK(vm.getPPC64XICSSnapshot().sources.size() == 1);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].sourceId == source);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].type == PPC64XICSSourceType::EDGE);
    CHECK(vm.getPPC64TerminalSnapshot().attachments.size() == 1);
    CHECK(vm.getPPC64TerminalSnapshot().attachments[0].sourceId == source);
    writeWords(vm.getMemory(), 0x500, readAndAckHandler());
    CHECK(steps(vm, 6));
    const uint64_t cycles = vm.getCyclesExecuted();
    CHECK(vm.injectPPC64TerminalInputWithStatus({'A'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 1);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(step(vm));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin->getNIP() == 0x500U);
    CHECK(plugin->getSRR0() == kEntry + 24U);
    CHECK(plugin->getSRR1() == (MSR_SF | MSR_EE));
    CHECK(vm.getCyclesExecuted() == cycles);
    CHECK(plugin->getInterruptSnapshot().lastDeliveryVector == 0x500U);
    CHECK(steps(vm, readAndAckHandler().size()));
    CHECK(readBE32(vm.getMemory(), kResult) == (0xff000000U | source));
    CHECK(readBE32(vm.getMemory(), kResult + 4) == 1);
    CHECK(readBE64(vm.getMemory(), kResult + 8) == 0x4100000000000000ULL);
    CHECK(plugin->getNIP() == kEntry + 24U);
    CHECK(plugin->getMSR() == (MSR_SF | MSR_EE));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
}

void programB_ReadAcknowledgeResume() {
    std::printf("[phase14-B-read-acknowledge-resume]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, mainWithCPPR()));
    const auto handler = readAndAckHandler();
    writeWords(vm.getMemory(), 0x500, handler);
    CHECK(steps(vm, 6));
    CHECK(vm.injectPPC64TerminalInputWithStatus({'p', 'p', 'c'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, handler.size()));
    CHECK(readBE32(vm.getMemory(), kResult) ==
          (0xff000000U | vm.getPPC64PlatformConfiguration().
              virtualTerminalInterruptSourceId));
    CHECK(readBE32(vm.getMemory(), kResult + 4) == 3);
    CHECK(readBE64(vm.getMemory(), kResult + 8) == 0x7070630000000000ULL);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 24U);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);
}

void programC_GuestEcho() {
    std::printf("[phase14-C-guest-echo]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, mainWithCPPR()));
    const auto handler = echoHandler();
    writeWords(vm.getMemory(), 0x500, handler);
    CHECK(steps(vm, 6));
    const std::vector<uint8_t> message{'h', 'v', 't', 'e', 'r', 'm', '1', '!', 0, '\n'};
    CHECK(vm.injectPPC64TerminalInputWithStatus(message) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.pause());
    CHECK(vm.resume());
    CHECK(step(vm));
    CHECK(steps(vm, handler.size()));
    CHECK(vm.readPPC64TerminalOutput(0) == message);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending == false);
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 24U);
}

void programD_EmptyToNonemptyEdgesAndCapacity() {
    std::printf("[phase14-D-input-edge-and-FIFO]\n");
    PPC64VirtualTerminal backend;
    unsigned pulses = 0;
    const std::array<uint8_t, 2> batch{'a', 'b'};
    CHECK(backend.injectInputWithStatus(0, batch.data(), batch.size(),
        [&pulses] { ++pulses; return true; }) == PPC64TerminalResult::SUCCESS);
    CHECK(pulses == 1 && backend.pendingInputBytes(0) == 2);
    CHECK(backend.injectInputWithStatus(0, batch.data(), batch.size(),
        [&pulses] { ++pulses; return true; }) == PPC64TerminalResult::SUCCESS);
    CHECK(pulses == 1 && backend.pendingInputBytes(0) == 4);
    std::array<uint8_t, 16> drained{};
    size_t drainedCount = 0;
    CHECK(backend.get(backend.terminalId(), 0, drained, drainedCount) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(drainedCount == 4 && drained[0] == 'a' && drained[1] == 'b' &&
          drained[2] == 'a' && drained[3] == 'b');
    CHECK(backend.injectInputWithStatus(0, batch.data(), 1,
        [] { return false; }) == PPC64TerminalResult::RESOURCE);
    CHECK(backend.pendingInputBytes(0) == 0); // Failed signal rolls bytes back.
    CHECK(backend.injectInputWithStatus(0, batch.data(), 1,
        [&pulses] { ++pulses; return true; }) == PPC64TerminalResult::SUCCESS);
    CHECK(pulses == 2);

    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    auto main = mainWithCPPR();
    main.insert(main.end(), {encodeAddis(4, 0, 0x3000),
        encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1});
    CHECK(prepare(vm, main));
    const auto handler = readAndAckHandler();
    writeWords(vm.getMemory(), 0x500, handler);
    CHECK(steps(vm, 6));
    std::vector<uint8_t> first(20);
    for (size_t i = 0; i < first.size(); ++i) first[i] = static_cast<uint8_t>(i);
    CHECK(vm.injectPPC64TerminalInputWithStatus(first) == PPC64TerminalResult::SUCCESS);
    CHECK(vm.injectPPC64TerminalInputWithStatus({}) == PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(step(vm));
    CHECK(steps(vm, handler.size()));
    CHECK(readBE32(vm.getMemory(), kResult + 4) == 16);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 4);
    CHECK(vm.injectPPC64TerminalInputWithStatus({'x'}) == PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 5);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(steps(vm, 4)); // Resume, then guest drains the remaining bytes and x.
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 5);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(5) == 0x1011121378000000ULL);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(vm.injectPPC64TerminalInputWithStatus({'z'}) == PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);

    vm.clearPPC64TerminalState();
    std::vector<uint8_t> full(vm.getPPC64TerminalQueueCapacity(), 0x5a);
    CHECK(vm.injectPPC64TerminalInputWithStatus(full) == PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == full.size());
    CHECK(vm.injectPPC64TerminalInputWithStatus({'!'}) == PPC64TerminalResult::BUSY);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == full.size());
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
}

void programE_CPPRAndEEAreIndependentMasks() {
    std::printf("[phase14-E-masked-input]\n");
    VirtualMachine cppr(kRamSize, 1, "PPC64", platform());
    auto cpprProgram = mainWithCPPR(0x10);
    cpprProgram.insert(cpprProgram.end(), {
        encodeAddi(4, 0, 0xff), encodeAddi(3, 0, PPC64_H_CPPR), kSc1});
    CHECK(prepare(cppr, cpprProgram));
    const auto handler = readAndAckHandler();
    writeWords(cppr.getMemory(), 0x500, handler);
    CHECK(steps(cppr, 6));
    CHECK(cppr.injectPPC64TerminalInputWithStatus({'c'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(cppr.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!cppr.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(steps(cppr, 4)); // Resume at the idle instruction, then lower CPPR.
    CHECK(step(cppr));
    CHECK(cppr.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(cppr, handler.size()));
    CHECK(readBE64(cppr.getMemory(), kResult + 8) == 0x6300000000000000ULL);

    VirtualMachine ee(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(ee, {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1,
        encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(6)}));
    writeWords(ee.getMemory(), 0x500, readAndAckHandler());
    CHECK(steps(ee, 3));
    CHECK((ee.getPPC64ISAPlugin()->getMSR() & MSR_EE) == 0);
    CHECK(ee.injectPPC64TerminalInputWithStatus({'e'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(ee.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!ee.getPPC64ISAPlugin()->getInterruptSnapshot().externalPending);
    CHECK(steps(ee, 3));
    CHECK(step(ee));
    CHECK(ee.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(ee, readAndAckHandler().size()));
    CHECK(readBE64(ee.getMemory(), kResult + 8) == 0x6500000000000000ULL);
}

void programF_CompetingSourcesAndDEC() {
    std::printf("[phase14-F-competing-sources-and-DEC]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    const auto source = vm.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    CHECK(prepare(vm, {encodeAddi(3, 0, 0), encodeMtspr(3, kSprDEC),
        encodeAddi(4, 0, 0xff), encodeAddi(3, 0, PPC64_H_CPPR), kSc1,
        encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(6)}));
    writeWords(vm.getMemory(), 0x500, {
        encodeAddi(3, 0, PPC64_H_XIRR), kSc1,
        encodeAddi(8, 4, 0), encodeAddi(7, 0, static_cast<int16_t>(kResult)),
        encodeD(36, 8, 7, 0), encodeAddi(4, 8, 0),
        encodeAddi(3, 0, PPC64_H_EOI), kSc1, kRfid});
    writeWords(vm.getMemory(), 0x900, {
        encodeAddi(7, 0, 100), encodeMtspr(7, kSprDEC), kRfid});
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(source + 1U,
        PPC64XICSSourceType::EDGE, 0x10, 0, 0, true, &error));
    CHECK(vm.assertPPC64InterruptSource(source + 1U, 0, &error));
    CHECK(steps(vm, 7));
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(vm.injectPPC64TerminalInputWithStatus({'t'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(vm)); // Enable EE; delivery occurs at the next boundary.
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, 2));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == (0xff000000U | (source + 1U)));
    CHECK(steps(vm, 7)); // Complete the first XIRR/EOI/rfid sequence.
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, 2));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == (0xff000000U | source));
    CHECK(steps(vm, 7));
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(vm.getPPC64InterruptSnapshot().lastDeliveryVector == 0x900U);
    CHECK(steps(vm, 3));
    CHECK(!vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(vm.deassertPPC64InterruptSource(source + 1U, 0, &error));
    CHECK(vm.removePPC64InterruptSource(source + 1U, 0, &error));
}

void programG_PollingCompatibility() {
    std::printf("[phase14-G-polling-compatibility]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64",
        platform(true, PPC64InterruptMode::LEGACY_EXTERNAL));
    CHECK(prepare(vm, {encodeAddis(4, 0, 0x3000),
        encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1}));
    CHECK(!vm.getPPC64PlatformCapabilities().interruptController);
    CHECK(vm.getPPC64TerminalSnapshot().attachments.empty());
    CHECK(vm.getPPC64PlatformConfiguration().virtualTerminalInterruptSourceId == 0);
    CHECK(vm.injectPPC64TerminalInputWithStatus({'p', 0, 'l'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 3);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(5) == 0x70006c0000000000ULL);
    CHECK(!vm.getPPC64PlatformCapabilities().interruptController);
}

void programH_FDTDiscovery() {
    std::printf("[phase14-H-independent-FDT-validation]\n");
    VirtualMachine irq(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(irq, mainWithCPPR()));
    IndependentFDT fdt;
    CHECK(parseFDT(irq.getPPC64FDT(), fdt));
    const std::string vty = "/vdevice/vty@30000000";
    const auto* type = fdtProperty(fdt, vty, "device_type");
    const auto* compatible = fdtProperty(fdt, vty, "compatible");
    const auto* reg = fdtProperty(fdt, vty, "reg");
    const auto* parent = fdtProperty(fdt, vty, "interrupt-parent");
    const auto* interrupts = fdtProperty(fdt, vty, "interrupts");
    const auto* stdoutPath = fdtProperty(fdt, "/chosen", "stdout-path");
    const auto* xicsPhandle = fdtProperty(fdt, "/interrupt-controller", "phandle");
    CHECK(type != nullptr && std::string(type->begin(), type->end() - 1) == "serial");
    CHECK(compatible != nullptr &&
          std::string(compatible->begin(), compatible->end() - 1) == "hvterm1");
    CHECK(reg != nullptr && reg->size() == 4 && be32(*reg, 0) == kTerminalId);
    CHECK(parent != nullptr && parent->size() == 4 && be32(*parent, 0) == 1);
    CHECK(interrupts != nullptr && interrupts->size() == 8);
    CHECK(interrupts != nullptr && be32(*interrupts, 0) ==
          irq.getPPC64PlatformConfiguration().virtualTerminalInterruptSourceId);
    CHECK(interrupts != nullptr && be32(*interrupts, 4) == 0);
    CHECK(xicsPhandle != nullptr && be32(*xicsPhandle, 0) == 1);
    CHECK(stdoutPath != nullptr &&
          std::string(stdoutPath->begin(), stdoutPath->end() - 1) == vty);

    VirtualMachine polling(kRamSize, 1, "PPC64",
        platform(true, PPC64InterruptMode::LEGACY_EXTERNAL));
    CHECK(prepare(polling, mainWithCPPR()));
    IndependentFDT pollingFdt;
    CHECK(parseFDT(polling.getPPC64FDT(), pollingFdt));
    CHECK(fdtProperty(pollingFdt, vty, "reg") != nullptr);
    CHECK(fdtProperty(pollingFdt, vty, "interrupts") == nullptr);
    CHECK(fdtProperty(pollingFdt, vty, "interrupt-parent") == nullptr);
    CHECK(fdtProperty(pollingFdt, "/chosen", "stdout-path") != nullptr);

    auto noTerminalConfig = platform(false);
    VirtualMachine noTerminal(kRamSize, 1, "PPC64", noTerminalConfig);
    CHECK(prepare(noTerminal, mainWithCPPR()));
    IndependentFDT noTerminalFdt;
    CHECK(parseFDT(noTerminal.getPPC64FDT(), noTerminalFdt));
    CHECK(fdtProperty(noTerminalFdt, vty, "reg") == nullptr);
    CHECK(fdtProperty(noTerminalFdt, "/chosen", "stdout-path") == nullptr);
    CHECK(noTerminal.getPPC64XICSSnapshot().sources.empty());
}

void programI_CheckpointAndRestore() {
    std::printf("[phase14-I-checkpoint-validation]\n");
    auto fixtureDirectory = std::filesystem::current_path() / "tests" / "fixtures";
    if (!std::filesystem::exists(fixtureDirectory / "ppc64-v1-empty.gxcp")) {
        fixtureDirectory = std::filesystem::current_path().parent_path() /
                           "tests" / "fixtures";
    }
    for (uint32_t version = 1; version <= 5; ++version) {
        const auto fixture = fixtureDirectory /
            ("ppc64-v" + std::to_string(version) + "-empty.gxcp");
        CHECK(std::filesystem::exists(fixture));
        std::ifstream headerInput(fixture, std::ios::binary);
        std::array<uint8_t, 64> header{};
        headerInput.read(reinterpret_cast<char*>(header.data()), header.size());
        CHECK(headerInput.good());
        const uint32_t headerVersion = static_cast<uint32_t>(header[8]) |
            (static_cast<uint32_t>(header[9]) << 8) |
            (static_cast<uint32_t>(header[10]) << 16) |
            (static_cast<uint32_t>(header[11]) << 24);
        CHECK(headerVersion == version);
        uint64_t ramSize = 0;
        for (unsigned byte = 0; byte < 8; ++byte) {
            ramSize |= static_cast<uint64_t>(header[56 + byte]) << (byte * 8U);
        }
        CHECK(ramSize != 0);
        std::unique_ptr<VirtualMachine> legacy;
        if (version == 2) {
            legacy = std::make_unique<VirtualMachine>(
                static_cast<size_t>(ramSize), 1, "PPC64",
                PPC64PlatformConfiguration{});
        } else {
            legacy = std::make_unique<VirtualMachine>(
                static_cast<size_t>(ramSize), 1, "PPC64");
        }
        CHECK(legacy->init());
        std::string legacyError;
        const bool restored = VMCheckpointCodec::read(
            *legacy, fixture.string(), "phase14-v" + std::to_string(version),
            &legacyError);
        if (!restored) {
            std::printf("legacy checkpoint v%u rejected: %s\n", version,
                        legacyError.c_str());
        }
        CHECK(restored);
    }

    VirtualMachine masked(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(masked, mainWithCPPR(0x10)));
    const auto handler = readAndAckHandler();
    writeWords(masked.getMemory(), 0x500, handler);
    CHECK(steps(masked, 6));
    CHECK(masked.injectPPC64TerminalInputWithStatus({'s', 'a', 'v', 'e'}) ==
          PPC64TerminalResult::SUCCESS);
    const VMStateSnapshot pending = masked.captureVMSnapshot();
    CHECK(pending.ppc64VirtualTerminal.version == PPC64_TERMINAL_STATE_VERSION);
    CHECK(pending.ppc64VirtualTerminal.attachments.size() == 1);
    CHECK(pending.ppc64Xics.sources[0].pending);
    CHECK(!pending.ppc64Xics.sources[0].presented);
    VMStateSnapshot corrupt = pending;
    corrupt.ppc64VirtualTerminal.attachments[0].sourceId = 0x100f;
    CHECK(!masked.restoreVMSnapshot(corrupt));
    corrupt = pending;
    corrupt.ppc64VirtualTerminal.attachments[0].ownerLPID = 1;
    CHECK(!masked.restoreVMSnapshot(corrupt));
    CHECK(masked.getPPC64TerminalPendingInputBytes() == 4);
    CHECK(masked.getPPC64XICSSnapshot().sources[0].pending);

    const auto file = std::filesystem::temp_directory_path() /
                      "guidexos-phase14-terminal.gxcp";
    const auto corruptFile = std::filesystem::temp_directory_path() /
                             "guidexos-phase14-terminal-corrupt.gxcp";
    std::string error;
    CHECK(masked.pause());
    CHECK(VMCheckpointCodec::write(masked, file.string(), "phase14-terminal", &error));
    VirtualMachine restored(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(restored, mainWithCPPR(0x10)));
    writeWords(restored.getMemory(), 0x500, handler);
    CHECK(VMCheckpointCodec::read(restored, file.string(), "phase14-terminal", &error));
    CHECK(restored.getPPC64TerminalPendingInputBytes() == 4);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(restored.getPPC64TerminalSnapshot().attachments[0].sourceId ==
          restored.getPPC64PlatformConfiguration().virtualTerminalInterruptSourceId);
    std::ifstream input(file, std::ios::binary);
    std::vector<char> contents((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
    CHECK(contents.size() > 48);
    if (!contents.empty()) contents.back() ^= 1;
    {
        std::ofstream output(corruptFile, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    CHECK(!VMCheckpointCodec::read(restored, corruptFile.string(),
                                   "phase14-terminal", &error));
    CHECK(restored.getPPC64TerminalPendingInputBytes() == 4);

    // Capture after guest H_XIRR but before H_EOI: restore must retain the
    // acknowledged outstanding token instead of replaying a fresh edge.
    VirtualMachine acknowledged(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(acknowledged, mainWithCPPR()));
    writeWords(acknowledged.getMemory(), 0x500, handler);
    CHECK(steps(acknowledged, 6));
    CHECK(acknowledged.injectPPC64TerminalInputWithStatus({'a'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(acknowledged));
    CHECK(steps(acknowledged, 2));
    CHECK(acknowledged.getPPC64XICSSnapshot().sources[0].acknowledged);
    const VMStateSnapshot accepted = acknowledged.captureVMSnapshot();
    CHECK(acknowledged.restoreVMSnapshot(accepted));
    CHECK(acknowledged.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(steps(acknowledged, handler.size() - 2U));
    CHECK(acknowledged.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(acknowledged.getPPC64XICSSnapshot().presentations[0].outstanding.empty());

    VirtualMachine presented(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(presented, mainWithCPPR()));
    writeWords(presented.getMemory(), 0x500, handler);
    CHECK(steps(presented, 6));
    CHECK(presented.injectPPC64TerminalInputWithStatus({'v'}) ==
          PPC64TerminalResult::SUCCESS);
    const VMStateSnapshot inService = presented.captureVMSnapshot();
    CHECK(inService.ppc64Xics.sources[0].presented);
    CHECK(presented.restoreVMSnapshot(inService));
    CHECK(presented.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(step(presented));
    CHECK(steps(presented, handler.size()));
    CHECK(readBE64(presented.getMemory(), kResult + 8) == 0x7600000000000000ULL);

    VirtualMachine partlyRead(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(partlyRead, mainWithCPPR()));
    writeWords(partlyRead.getMemory(), 0x500, handler);
    CHECK(steps(partlyRead, 6));
    std::vector<uint8_t> partialInput(20, 0x31);
    CHECK(partlyRead.injectPPC64TerminalInputWithStatus(partialInput) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(partlyRead));
    CHECK(steps(partlyRead, handler.size()));
    CHECK(partlyRead.getPPC64TerminalPendingInputBytes() == 4);
    const VMStateSnapshot queuedRemainder = partlyRead.captureVMSnapshot();
    CHECK(partlyRead.restoreVMSnapshot(queuedRemainder));
    CHECK(partlyRead.getPPC64TerminalPendingInputBytes() == 4);
    CHECK(!partlyRead.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(steps(partlyRead, 1)); // Restored execution resumes at the saved main PC.
    CHECK(partlyRead.getPPC64TerminalPendingInputBytes() == 4);

    VirtualMachine completed(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(completed, mainWithCPPR()));
    writeWords(completed.getMemory(), 0x500, handler);
    CHECK(steps(completed, 6));
    CHECK(completed.injectPPC64TerminalInputWithStatus({'d'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(completed));
    CHECK(steps(completed, handler.size()));
    const VMStateSnapshot drained = completed.captureVMSnapshot();
    CHECK(completed.restoreVMSnapshot(drained));
    CHECK(completed.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(!completed.getPPC64XICSSnapshot().sources[0].pending);
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(corruptFile, ignored);
}

void programJ_TwoVMAndLPIDIsolation() {
    std::printf("[phase14-J-two-VM-and-LPID-isolation]\n");
    auto first = std::make_unique<VirtualMachine>(kRamSize, 1, "PPC64", platform());
    VirtualMachine second(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(*first, mainWithCPPR()));
    auto secondMain = mainWithCPPR();
    secondMain.insert(secondMain.end(), {encodeAddis(4, 0, 0x3000),
        encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1});
    CHECK(prepare(second, std::move(secondMain)));
    const uint32_t firstSource = first->getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    const uint32_t secondSource = second.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    CHECK(firstSource == secondSource); // Same guest IRQ namespace, VM-owned state.
    const auto handler = readAndAckHandler();
    writeWords(first->getMemory(), 0x500, handler);
    writeWords(second.getMemory(), 0x500, handler);
    CHECK(steps(*first, 6));
    CHECK(steps(second, 6));
    CHECK(first->injectPPC64TerminalInputWithStatus({'1'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(second.injectPPC64TerminalInputWithStatus({'2'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(first->getPPC64XICSSnapshot().sources[0].pending);
    CHECK(second.getPPC64XICSSnapshot().sources[0].pending);
    first.reset();
    CHECK(second.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(step(second));
    CHECK(steps(second, handler.size()));
    CHECK(readBE64(second.getMemory(), kResult + 8) == 0x3200000000000000ULL);

    auto* memory = dynamic_cast<Memory*>(&second.getMemory());
    std::string error;
    CHECK(memory != nullptr && second.getPPC64PartitionManager()->
        configurePartitionTable(*memory, 0x40000, 4, error));
    auto partition = second.getPPC64PartitionManager()->createPartition(7, error);
    CHECK(partition != nullptr);
    CHECK(second.attachPPC64TerminalInterrupt(7, 0x30, &error));
    const auto attachments = second.getPPC64TerminalSnapshot().attachments;
    CHECK(attachments.size() == 2);
    CHECK(attachments[1].ownerLPID == 7);
    CHECK(attachments[1].sourceId != secondSource);
    CHECK(second.injectPPC64TerminalInputWithStatus({'L'}, 7) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(second.getPPC64XICSSnapshot().sources[1].ownerLPID == 7);
    CHECK(second.getPPC64XICSSnapshot().sources[1].pending);
    CHECK(second.getPPC64TerminalPendingInputBytes(0) == 0);
    CHECK(second.getPPC64TerminalPendingInputBytes(7) == 1);
    CHECK(second.getPPC64XICSSnapshot().activeLPID == 0);
    CHECK(steps(second, 4)); // Root H_GET cannot consume LPID 7's queued byte.
    CHECK(second.getPPC64ISAPlugin()->readGPR(4) == 0);
    CHECK(second.getPPC64TerminalPendingInputBytes(7) == 1);
    CHECK(second.getPPC64PartitionManager()->destroyPartition(7));
    CHECK(second.getPPC64TerminalSnapshot().attachments.size() == 1);
    CHECK(second.getPPC64XICSSnapshot().sources.size() == 1);
    CHECK(second.getPPC64TerminalPendingInputBytes(7) == 0);
    CHECK(second.injectPPC64TerminalInputWithStatus({'?'}, 7) ==
          PPC64TerminalResult::PARAMETER);
}

void programK_ErrorResetAndBoundedFaults() {
    std::printf("[phase14-K-errors-reset-and-faults]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, mainWithCPPR()));
    CHECK(steps(vm, 6));
    std::string error;
    const uint32_t source = vm.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    CHECK(!vm.assertPPC64InterruptSource(source, 0, &error));
    CHECK(!vm.assertPPC64InterruptSource(0x100f, 0, &error));
    CHECK(vm.injectPPC64TerminalInputWithStatus({'r'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.reset());
    CHECK(vm.getPPC64XICSSnapshot().sources.size() == 1);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].sourceId == source);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0);
    CHECK(vm.getPPC64TerminalSnapshot().attachments.size() == 1);

    VirtualMachine badEOI(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(badEOI, {encodeAddi(4, 0, 0x1101),
        encodeAddi(3, 0, PPC64_H_EOI), kSc1,
        encodeAddis(4, 0, 0x3000), encodeAddi(4, 4, 0x1000),
        encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1}));
    CHECK(steps(badEOI, 3));
    CHECK(badEOI.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(steps(badEOI, 4));
    CHECK(badEOI.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(badEOI.injectPPC64TerminalInputWithStatus({}, 7) ==
          PPC64TerminalResult::PARAMETER);

    VirtualMachine fault(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(fault, mainWithCPPR()));
    CHECK(steps(fault, 6));
    CHECK(fault.injectPPC64TerminalInputWithStatus({'f'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(step(fault)); // Entry itself succeeds; the uninstalled vector faults boundedly.
    CHECK(!fault.step());
    CHECK(fault.getState() == VMState::ERROR || fault.getState() == VMState::CRASHED);
}

} // namespace

int main() {
    std::printf("PPC64 Phase 14 interrupt-driven PAPR hvterm1 acceptance\n");
    programA_FirstInputInterrupt();
    programB_ReadAcknowledgeResume();
    programC_GuestEcho();
    programD_EmptyToNonemptyEdgesAndCapacity();
    programE_CPPRAndEEAreIndependentMasks();
    programF_CompetingSourcesAndDEC();
    programG_PollingCompatibility();
    programH_FDTDiscovery();
    programI_CheckpointAndRestore();
    programJ_TwoVMAndLPIDIsolation();
    programK_ErrorResetAndBoundedFaults();
    std::printf("PPC64 Phase 14: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
