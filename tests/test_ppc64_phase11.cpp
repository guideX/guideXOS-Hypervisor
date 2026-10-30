// PPC64 Phase 11 production PAPR hvterm1 acceptance tests.

#include "VirtualMachine.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64VirtualTerminal.h"
#include "VMSnapshot.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
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
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                      \
    } while (0)

constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kTableBase = 0x80000;
constexpr uint64_t kRoot = 0x100000;
constexpr uint64_t kL1 = 0x110000;
constexpr uint64_t kL2 = 0x111000;
constexpr uint64_t kL3 = 0x112000;
constexpr uint64_t kCodeGRA = 0x300000;
constexpr uint64_t kProcessGRA = 0x200000;
constexpr uint64_t kCodeBacking = 0x400000;
constexpr uint64_t kProcessBacking = 0x600000;
constexpr uint64_t kValid = 0x8000000000000000ULL;
constexpr uint64_t kLeaf = 0x4000000000000000ULL;
constexpr uint64_t kReference = 0x100ULL;
constexpr uint64_t kChange = 0x80ULL;
constexpr uint64_t kRWX = 7ULL;

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

uint32_t encodeMftb(uint8_t rt, uint16_t tbr) {
    const uint16_t spr = tbr;
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (371U << 1);
}

std::vector<uint8_t> program(const std::vector<uint32_t>& words) {
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

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void writeBE(IMemory& memory, uint64_t address, uint64_t value, size_t width) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - i - 1U)));
    }
    memory.Write(address, bytes.data(), width);
}

uint64_t radixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t leafPte(uint64_t backing) {
    return kValid | kLeaf | kReference | kChange |
           (backing & 0x01fffffffffff000ULL) | kRWX;
}

uint64_t l3Address(uint64_t gra) {
    return kL3 + (((gra >> 12) & 0x1ffULL) * 8ULL);
}

// Independent reader for the public FDT v17 structure/property encoding.
struct ParsedFDT {
    std::map<std::string, std::map<std::string, std::vector<uint8_t>>> nodes;
};

bool parseFDT(const std::vector<uint8_t>& bytes, ParsedFDT& parsed) {
    if (bytes.size() < 40U || readBe32(bytes.data()) != 0xd00dfeedU) return false;
    const uint32_t total = readBe32(bytes.data() + 4);
    const uint32_t structOffset = readBe32(bytes.data() + 8);
    const uint32_t stringsOffset = readBe32(bytes.data() + 12);
    const uint32_t structSize = readBe32(bytes.data() + 36);
    const uint32_t stringsSize = readBe32(bytes.data() + 32);
    if (total != bytes.size() || readBe32(bytes.data() + 20) != 17U ||
        structOffset > total || structSize > total - structOffset ||
        stringsOffset > total || stringsSize > total - stringsOffset) return false;
    const size_t end = static_cast<size_t>(structOffset) + structSize;
    size_t cursor = structOffset;
    std::vector<std::string> stack;
    while (cursor + 4U <= end) {
        const uint32_t token = readBe32(bytes.data() + cursor);
        cursor += 4U;
        if (token == 1U) {
            const size_t begin = cursor;
            while (cursor < end && bytes[cursor] != 0) ++cursor;
            if (cursor == end) return false;
            const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            ++cursor;
            cursor = (cursor + 3U) & ~size_t{3U};
            const std::string path = stack.empty() ? "/" :
                (stack.back() == "/" ? "/" + name : stack.back() + "/" + name);
            stack.push_back(path);
            parsed.nodes.emplace(path,
                std::map<std::string, std::vector<uint8_t>>{});
        } else if (token == 2U) {
            if (stack.empty()) return false;
            stack.pop_back();
        } else if (token == 3U) {
            if (stack.empty() || cursor + 8U > end) return false;
            const uint32_t length = readBe32(bytes.data() + cursor);
            const uint32_t nameOffset = readBe32(bytes.data() + cursor + 4U);
            cursor += 8U;
            if (length > end - cursor || nameOffset >= stringsSize) return false;
            const size_t nameBegin = stringsOffset + nameOffset;
            size_t nameEnd = nameBegin;
            const size_t stringsEnd = stringsOffset + stringsSize;
            while (nameEnd < stringsEnd && bytes[nameEnd] != 0) ++nameEnd;
            if (nameEnd == stringsEnd) return false;
            const std::string propertyName(
                bytes.begin() + static_cast<std::ptrdiff_t>(nameBegin),
                bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd));
            std::vector<uint8_t> value(
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            if (!parsed.nodes[stack.back()].emplace(propertyName,
                                                     std::move(value)).second) return false;
            cursor = (cursor + length + 3U) & ~size_t{3U};
        } else if (token == 9U) {
            return stack.empty() && cursor == end;
        } else {
            return false;
        }
        if (cursor > end) return false;
    }
    return false;
}

const std::vector<uint8_t>* fdtProperty(const ParsedFDT& fdt,
                                       const std::string& path,
                                       const std::string& name) {
    auto node = fdt.nodes.find(path);
    if (node == fdt.nodes.end()) return nullptr;
    auto property = node->second.find(name);
    return property == node->second.end() ? nullptr : &property->second;
}

std::string fdtString(const ParsedFDT& fdt, const std::string& path,
                      const std::string& name) {
    const auto* bytes = fdtProperty(fdt, path, name);
    if (bytes == nullptr || bytes->empty() || bytes->back() != 0) return {};
    return std::string(bytes->begin(), bytes->end() - 1);
}

bool startVM(VirtualMachine& vm, const std::vector<uint8_t>& code,
             uint64_t address = kEntry) {
    return vm.init() && vm.loadProgram(code.data(), code.size(), address);
}

uint64_t callPut(VirtualMachine& vm, uint64_t id, uint64_t length,
                 uint64_t first, uint64_t second, uint64_t nip = kEntry) {
    auto* plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_PUT_TERM_CHAR);
    plugin->writeGPR(4, id);
    plugin->writeGPR(5, length);
    plugin->writeGPR(6, first);
    plugin->writeGPR(7, second);
    vm.setIP(0, nip);
    const bool stepped = vm.step();
    CHECK(stepped);
    CHECK(vm.getIP() == nip + 4U);
    return vm.readGR(3);
}

uint64_t callGet(VirtualMachine& vm, uint64_t id, uint64_t nip = kEntry) {
    auto* plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id);
    plugin->writeGPR(5, 0x1111111111111111ULL);
    plugin->writeGPR(6, 0x2222222222222222ULL);
    vm.setIP(0, nip);
    const bool stepped = vm.step();
    CHECK(stepped);
    CHECK(vm.getIP() == nip + 4U);
    return vm.readGR(3);
}

bool installPartition(VirtualMachine& vm,
                      std::shared_ptr<PPC64PartitionContext>& context) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (memory == nullptr) return false;
    std::string reason;
    auto* manager = vm.getPPC64PartitionManager();
    if (!manager->isConfigured() &&
        !manager->configurePartitionTable(*memory, kTableBase, 4, reason)) {
        return false;
    }
    context = manager->createPartition(1, reason);
    if (!context) return false;
    writeBE(*memory, kRoot, kValid | kL1 | 9ULL, 8);
    writeBE(*memory, kL1, kValid | kL2 | 9ULL, 8);
    writeBE(*memory, kL2 + 8, kValid | kL3 | 9ULL, 8);
    writeBE(*memory, kTableBase + 16U,
            PPC64_PATE0_HR | radixRootWord(kRoot), 8);
    writeBE(*memory, kTableBase + 24U, 0, 8);
    writeBE(*memory, l3Address(kCodeGRA), leafPte(kCodeBacking), 8);
    for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
        writeBE(*memory, l3Address(kProcessGRA + offset),
                leafPte(kProcessBacking + offset), 8);
    }
    return manager->registerPrivateBackingRange(context, kCodeBacking, 0x1000,
                                                 reason) &&
           manager->registerPrivateBackingRange(context, kProcessBacking,
                                                 0x20000, reason) &&
           vm.getPPC64ISAPlugin()->configureHypervisorMemory(*memory, 0, 0x1000,
                                                              reason) &&
           vm.getPPC64ISAPlugin()->activatePartitionContext(context, reason);
}

void programAOutput() {
    std::printf("[phase11-A-output]\n");
    VirtualMachine vm(1U << 20, 1, "PPC64");
    const auto code = program({ENCODING_SC_HCALL});
    CHECK(startVM(vm, code));
    vm.setEntryPoint(kEntry);
    const std::string expected = "Hello from guideXOS PPC64!";
    size_t offset = 0;
    while (offset < expected.size()) {
        const size_t count = std::min<size_t>(16U, expected.size() - offset);
        uint64_t first = 0;
        uint64_t second = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t value = static_cast<uint8_t>(expected[offset + i]);
            if (i < 8U) first |= value << (56U - 8U * i);
            else second |= value << (56U - 8U * (i - 8U));
        }
        CHECK(callPut(vm, vm.getPPC64VirtualTerminalId(), count, first, second) ==
              PPC64_H_SUCCESS);
        offset += count;
    }
    CHECK(vm.getPPC64TerminalQueueCapacity() ==
          PPC64_TERMINAL_QUEUE_CAPACITY);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == expected.size());
    const auto peeked = vm.readPPC64TerminalOutput();
    CHECK(std::string(peeked.begin(), peeked.end()) == expected);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == expected.size());
    const auto output = vm.drainPPC64TerminalOutput();
    CHECK(std::string(output.begin(), output.end()) == expected);
    CHECK(vm.getPPC64TerminalPendingOutputBytes() == 0U);
    CHECK(vm.getCyclesExecuted() == 2U);
}

void programBInputEcho() {
    std::printf("[phase11-B-input-echo]\n");
    VirtualMachine vm(1U << 20, 1, "PPC64");
    const uint32_t id = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID;
    const auto code = program({
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_GET_TERM_CHAR)),
        encodeAddis(4, 0, 0x3000), encodeOri(4, 4, 0), ENCODING_SC_HCALL,
        encodeAddi(10, 4, 0), encodeAddi(11, 5, 0), encodeAddi(12, 6, 0),
        encodeAddis(8, 0, 0x3000), encodeOri(8, 8, 0),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_PUT_TERM_CHAR)),
        encodeAddi(4, 8, 0), encodeAddi(5, 10, 0),
        encodeAddi(6, 11, 0), encodeAddi(7, 12, 0), ENCODING_SC_HCALL,
    });
    CHECK(startVM(vm, code));
    vm.setEntryPoint(kEntry);
    const std::vector<uint8_t> input{0x41, 0x00, 0xc3, 0xa9, 0x80, 0x0d, 0xff};
    CHECK(vm.injectPPC64TerminalInput(input));
    for (size_t i = 0; i < 15U; ++i) CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS);
    CHECK(vm.readGR(4) == id);
    CHECK(vm.readGR(5) == input.size());
    CHECK(vm.drainPPC64TerminalOutput() == input);
    CHECK(vm.getPPC64TerminalSnapshot().channels.empty() == false);
}

void programCtoEValidationsAndBackpressure() {
    std::printf("[phase11-C-binary-D-errors-E-backpressure]\n");
    VirtualMachine vm(1U << 20, 1, "PPC64");
    CHECK(startVM(vm, program({ENCODING_SC_HCALL})));
    vm.setEntryPoint(kEntry);
    const uint64_t id = vm.getPPC64VirtualTerminalId();
    auto* plugin = vm.getPPC64ISAPlugin();
    const uint64_t binary = callPut(vm, id, 5, 0x000a0d80ff000000ULL, 0);
    CHECK(binary == PPC64_H_SUCCESS);
    CHECK(vm.drainPPC64TerminalOutput() ==
          std::vector<uint8_t>({0x00, 0x0a, 0x0d, 0x80, 0xff}));
    CHECK(callPut(vm, id, 0, 0xffffffffffffffffULL, 0xffffffffffffffffULL) ==
          PPC64_H_SUCCESS);
    CHECK(callPut(vm, id, 1, 0x4100000000000000ULL, 0) == PPC64_H_SUCCESS);
    CHECK(callPut(vm, id, 8, 0x0102030405060708ULL, 0) == PPC64_H_SUCCESS);
    CHECK(callPut(vm, id, 9, 0x0102030405060708ULL,
                  0x0900000000000000ULL) == PPC64_H_SUCCESS);
    CHECK(callPut(vm, id, 16, 0x0102030405060708ULL,
                  0x090a0b0c0d0e0f10ULL) == PPC64_H_SUCCESS);
    const auto lengthChecks = vm.drainPPC64TerminalOutput();
    CHECK(lengthChecks.size() == 34U);
    CHECK(callPut(vm, id, 17, 0x1234, 0x5678) == PPC64_H_PARAMETER);
    CHECK(vm.readGR(4) == id && vm.readGR(5) == 17U);
    CHECK(callPut(vm, id + 0x1000U, 1, 0x4100000000000000ULL, 0) ==
          PPC64_H_PARAMETER);

    std::vector<uint8_t> fullInput(PPC64_TERMINAL_QUEUE_CAPACITY);
    for (size_t i = 0; i < fullInput.size(); ++i) {
        fullInput[i] = static_cast<uint8_t>(i);
    }
    CHECK(vm.injectPPC64TerminalInput(fullInput));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == fullInput.size());
    CHECK(vm.injectPPC64TerminalInputWithStatus({0xaa}) ==
          ppc64::PPC64TerminalResult::BUSY);
    CHECK(!vm.injectPPC64TerminalInput({0xaa}));
    CHECK(callGet(vm, id) == PPC64_H_SUCCESS);
    CHECK(vm.readGR(4) == 16U);
    CHECK(vm.readGR(5) == 0x0001020304050607ULL);
    CHECK(vm.readGR(6) == 0x08090a0b0c0d0e0fULL);
    CHECK(callGet(vm, id) == PPC64_H_SUCCESS);
    CHECK(vm.readGR(4) == 16U);
    CHECK(vm.readGR(5) == 0x1011121314151617ULL);
    CHECK(vm.readGR(6) == 0x18191a1b1c1d1e1fULL);
    CHECK(vm.getPPC64TerminalSnapshot().channels[0].input.size() ==
          PPC64_TERMINAL_QUEUE_CAPACITY - 32U);
    CHECK(vm.getPPC64TerminalPendingInputBytes() ==
          PPC64_TERMINAL_QUEUE_CAPACITY - 32U);
    CHECK(vm.injectPPC64TerminalInput({0xaa}));

    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id + 0x1000U);
    plugin->writeGPR(5, 0x1111111111111111ULL);
    plugin->writeGPR(6, 0x2222222222222222ULL);
    const auto invalidGetBefore = vm.getPPC64RegisterView();
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_PARAMETER);
    CHECK(vm.readGR(4) == invalidGetBefore.gpr[4]);
    CHECK(vm.readGR(5) == invalidGetBefore.gpr[5]);
    CHECK(vm.readGR(6) == invalidGetBefore.gpr[6]);

    CHECK(vm.reset());
    vm.setIP(0, kEntry);
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id);
    plugin->writeGPR(5, 0x1111111111111111ULL);
    plugin->writeGPR(6, 0x2222222222222222ULL);
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS && vm.readGR(4) == 0U);
    CHECK(vm.readGR(5) == 0U && vm.readGR(6) == 0U);
    plugin->writeGPR(3, 0xdeadbeefU);
    plugin->writeGPR(4, 1);
    const auto before = vm.getPPC64RegisterView();
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_FUNCTION);
    CHECK(vm.readGR(4) == before.gpr[4]);
    CHECK(vm.readGR(5) == before.gpr[5] && vm.readGR(6) == before.gpr[6]);

    for (size_t i = 0; i < PPC64_TERMINAL_QUEUE_CAPACITY / 16U; ++i) {
        CHECK(callPut(vm, id, 16, 0x5a5a5a5a5a5a5a5aULL,
                      0x5a5a5a5a5a5a5a5aULL) == PPC64_H_SUCCESS);
    }
    plugin->writeGPR(3, PPC64_H_PUT_TERM_CHAR);
    plugin->writeGPR(4, id);
    plugin->writeGPR(5, 1);
    plugin->writeGPR(6, 0x4100000000000000ULL);
    plugin->writeGPR(7, 0);
    const auto rejectedArgs = vm.getPPC64RegisterView();
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_BUSY);
    CHECK(vm.readGR(4) == rejectedArgs.gpr[4] && vm.readGR(5) == 1U);
    CHECK(vm.drainPPC64TerminalOutput(0, 32).size() == 32U);
    CHECK(callPut(vm, id, 1, 0x4100000000000000ULL, 0) == PPC64_H_SUCCESS);
    auto remaining = vm.drainPPC64TerminalOutput();
    CHECK(remaining.size() == PPC64_TERMINAL_QUEUE_CAPACITY - 31U);
    CHECK(remaining.back() == 0x41);
}

void programFVMAndLPIDIsolation() {
    std::printf("[phase11-F-vm-and-lpid-isolation]\n");
    auto first = std::make_unique<VirtualMachine>(1U << 20, 1, "PPC64");
    VirtualMachine second(1U << 20, 1, "PPC64");
    const auto code = program({ENCODING_SC_HCALL});
    CHECK(startVM(*first, code));
    CHECK(startVM(second, code));
    first->setEntryPoint(kEntry);
    second.setEntryPoint(kEntry);
    CHECK(callPut(*first, first->getPPC64VirtualTerminalId(), 3,
                  0x4141410000000000ULL, 0) == PPC64_H_SUCCESS);
    CHECK(callPut(second, second.getPPC64VirtualTerminalId(), 2,
                  0x4242000000000000ULL, 0) == PPC64_H_SUCCESS);
    CHECK(first->drainPPC64TerminalOutput() ==
          std::vector<uint8_t>({'A', 'A', 'A'}));
    CHECK(second.drainPPC64TerminalOutput() ==
          std::vector<uint8_t>({'B', 'B'}));
    CHECK(first->injectPPC64TerminalInput({0x31}));
    first.reset();
    CHECK(second.injectPPC64TerminalInput({0x32}));

    VirtualMachine partitioned(8U << 20, 1, "PPC64");
    CHECK(startVM(partitioned, code));
    std::shared_ptr<PPC64PartitionContext> context;
    CHECK(installPartition(partitioned, context));
    CHECK(partitioned.injectPPC64TerminalInput({0x51}, 1));
    CHECK(!partitioned.injectPPC64TerminalInput({0x52}, 2));
    CHECK(partitioned.injectPPC64TerminalInput({0x50}, 0));
    CHECK(partitioned.getPPC64VirtualTerminalId() ==
          PPC64_DEFAULT_VIRTUAL_TERMINAL_ID);
    // The host API distinguishes root and LPID 1 queues under the same vty ID.
    CHECK(partitioned.getPPC64TerminalSnapshot().channels.size() == 2U);
    CHECK(partitioned.getPPC64TerminalSnapshot().channels[0].lpid == 0U);
    CHECK(partitioned.getPPC64TerminalSnapshot().channels[1].lpid == 1U);
    CHECK(context->lpid() == 1U);
}

void programGDeviceTreeDiscovery() {
    std::printf("[phase11-G-device-tree-discovery]\n");
    PPC64PlatformConfiguration config;
    config.virtualTerminalEnabled = true;
    config.virtualTerminalId = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID + 0x1000U;
    VirtualMachine vm(8U << 20, 1, "PPC64", config);
    CHECK(vm.init());
    const auto code = program({ENCODING_SC_HCALL});
    std::string error;
    CHECK(vm.preparePPC64Platform(code, kEntry, kEntry, &error));
    ParsedFDT fdt;
    CHECK(parseFDT(vm.getPPC64FDT(), fdt));
    const std::string node = "/vdevice/vty@30001000";
    CHECK(fdtString(fdt, node, "compatible") == "hvterm1");
    CHECK(fdtString(fdt, node, "device_type") == "serial");
    const auto* reg = fdtProperty(fdt, node, "reg");
    CHECK(reg != nullptr && reg->size() == 4U);
    CHECK(reg != nullptr && readBe32(reg->data()) == config.virtualTerminalId);
    CHECK(fdtString(fdt, "/chosen", "stdout-path") == node);
    CHECK(vm.getPPC64VirtualTerminalId() == config.virtualTerminalId);
    CHECK(vm.getPPC64PlatformCapabilities().virtualTerminal);
    VMStateSnapshot wrongId = vm.captureVMSnapshot();
    wrongId.ppc64VirtualTerminal.terminalId += 0x1000U;
    CHECK(!vm.restoreVMSnapshot(wrongId));
    vm.writeGR(3, PPC64_H_PUT_TERM_CHAR);
    vm.writeGR(4, config.virtualTerminalId);
    vm.writeGR(5, 1);
    vm.writeGR(6, 0x5800000000000000ULL);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS);
    CHECK(vm.drainPPC64TerminalOutput() == std::vector<uint8_t>({'X'}));

    config.virtualTerminalEnabled = false;
    VirtualMachine disabled(8U << 20, 1, "PPC64", config);
    CHECK(disabled.init());
    CHECK(disabled.preparePPC64Platform(code, kEntry, kEntry, &error));
    ParsedFDT disabledFdt;
    CHECK(parseFDT(disabled.getPPC64FDT(), disabledFdt));
    CHECK(disabledFdt.nodes.find(node) == disabledFdt.nodes.end());
    CHECK(fdtProperty(disabledFdt, "/chosen", "stdout-path") == nullptr);
    CHECK(!disabled.injectPPC64TerminalInput({0x41}));
}

void programHCheckpointAndLifecycle() {
    std::printf("[phase11-H-checkpoint-and-lifecycle]\n");
    VirtualMachine vm(2U << 20, 1, "PPC64");
    CHECK(startVM(vm, program({ENCODING_SC_HCALL})));
    vm.setEntryPoint(kEntry);
    const uint64_t id = vm.getPPC64VirtualTerminalId();
    const std::vector<uint8_t> input{0x00, 0x7f, 0x80, 0xff};
    CHECK(vm.injectPPC64TerminalInput(input));
    CHECK(callPut(vm, id, 4, 0x102030400000000ULL, 0) == PPC64_H_SUCCESS);
    const VMStateSnapshot saved = vm.captureVMSnapshot();
    CHECK(vm.drainPPC64TerminalOutput() ==
          std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}));
    auto* plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id);
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(4) == input.size());
    CHECK(vm.restoreVMSnapshot(saved));
    CHECK(vm.drainPPC64TerminalOutput() ==
          std::vector<uint8_t>({0x01, 0x02, 0x03, 0x04}));
    plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id);
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(4) == input.size());
    CHECK(vm.readGR(5) == 0x007f80ff00000000ULL);
    CHECK(vm.readGR(6) == 0U);
    CHECK(vm.drainPPC64TerminalOutput().empty());

    VMStateSnapshot invalid = saved;
    invalid.ppc64VirtualTerminal.channels[0].input.resize(
        PPC64_TERMINAL_QUEUE_CAPACITY + 1U);
    const uint64_t oldIP = vm.getIP();
    CHECK(!vm.restoreVMSnapshot(invalid));
    CHECK(vm.getIP() == oldIP);
    CHECK(vm.injectPPC64TerminalInput({0x33}));
    vm.clearPPC64TerminalState();
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0U);
    CHECK(vm.getPPC64TerminalSnapshot().channels.empty());
    CHECK(vm.reset());
    CHECK(vm.getPPC64TerminalSnapshot().channels.empty());
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.pause());
    CHECK(vm.resume());
    CHECK(vm.injectPPC64TerminalInput({0x44}));

    const auto path = std::filesystem::temp_directory_path() /
                      "guidexos-phase11-terminal.gxcp";
    std::string error;
    CHECK(vm.writeDiagnosticCheckpoint(path.string(), "phase11-terminal", &error));
    CHECK(vm.injectPPC64TerminalInput({0x99}));
    const bool restored = vm.readDiagnosticCheckpoint(
        path.string(), "phase11-terminal", &error);
    if (!restored) std::printf("checkpoint restore diagnostic: %s\n", error.c_str());
    CHECK(restored);
    plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, id);
    vm.setIP(0, kEntry);
    CHECK(vm.step());
    CHECK(vm.readGR(4) == 1U && vm.readGR(5) == 0x4400000000000000ULL);
    std::filesystem::remove(path);
}

void programIPlatformRegression() {
    std::printf("[phase11-I-platform-integration]\n");
    PPC64PlatformConfiguration config;
    config.virtualTerminalEnabled = true;
    config.timebaseFrequency = 512'000'000ULL;
    config.initialTimebase = 10;
    const auto rootCode = program({
        encodeMftb(10, SPR_TBL), 0x44000002U, encodeAddi(11, 0, 2),
        ENCODING_SC_HCALL,
    });
    VirtualMachine vm(8U << 20, 1, "PPC64", config);
    CHECK(vm.init());
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    std::string partitionReason;
    CHECK(memory != nullptr && vm.getPPC64PartitionManager()->configurePartitionTable(
        *memory, kTableBase, 4, partitionReason));
    std::string error;
    CHECK(vm.preparePPC64Platform(rootCode, kEntry, kEntry, &error));
    auto* plugin = vm.getPPC64ISAPlugin();
    plugin->setGuestDeliveryEnabled(true);
    if (memory != nullptr) {
        const auto handler = program({encodeAddi(11, 0, 1), ENCODING_RFID});
        memory->Write(VEC_SC, handler.data(), handler.size());
    }
    CHECK(vm.step()); // mftb through the platform time base
    CHECK(vm.readGR(10) == 10U);
    CHECK(vm.step()); // sc 0, delivered to the guest syscall vector
    CHECK(vm.getIP() == VEC_SC);
    CHECK(vm.step());
    CHECK(vm.step()); // rfid resumes at the instruction after sc
    CHECK(vm.getIP() == kEntry + 8U);
    CHECK(vm.step());
    CHECK(vm.readGR(11) == 2U);
    CHECK(vm.injectPPC64TerminalInput({0x52}));

    std::shared_ptr<PPC64PartitionContext> context;
    CHECK(installPartition(vm, context));
    if (memory != nullptr) {
        writeBE(*memory, kCodeBacking, ENCODING_SC_HCALL, 4);
        writeBE(*memory, kCodeBacking + 4U, ENCODING_SC_HCALL, 4);
        writeBE(*memory, kCodeBacking + 8U, ENCODING_SC_HCALL, 4);
    }
    plugin->writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    plugin->writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                           PPC64_PROC_TBL_RADIX);
    plugin->writeGPR(5, kProcessGRA);
    plugin->writeGPR(6, 0);
    plugin->writeGPR(7, 5);
    vm.setIP(0, kCodeGRA);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS);
    CHECK(context && context->lpid() == 1U);
    plugin = vm.getPPC64ISAPlugin();
    plugin->writeGPR(3, PPC64_H_PUT_TERM_CHAR);
    plugin->writeGPR(4, config.virtualTerminalId);
    plugin->writeGPR(5, 4);
    plugin->writeGPR(6, 0x5041505200000000ULL);
    plugin->writeGPR(7, 0);
    vm.setIP(0, kCodeGRA + 4U);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS);
    CHECK(vm.drainPPC64TerminalOutput(1) ==
          std::vector<uint8_t>({'P', 'A', 'P', 'R'}));
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, config.virtualTerminalId);
    vm.setIP(0, kCodeGRA + 8U);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS && vm.readGR(4) == 0U);
    CHECK(vm.readGR(5) == 0U && vm.readGR(6) == 0U);
    plugin = vm.getPPC64ISAPlugin();
    plugin->deactivatePartitionContext();
    plugin->writeGPR(3, PPC64_H_GET_TERM_CHAR);
    plugin->writeGPR(4, config.virtualTerminalId);
    vm.setIP(0, kEntry + 12U);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == PPC64_H_SUCCESS && vm.readGR(4) == 1U);
    CHECK(vm.readGR(5) == 0x5200000000000000ULL);
    CHECK(vm.getPPC64Timebase() >= 18U);
}

} // namespace

int main() {
    std::printf("PPC64 Phase 11 PAPR virtual terminal acceptance\n");
    programAOutput();
    programBInputEcho();
    programCtoEValidationsAndBackpressure();
    programFVMAndLPIDIsolation();
    programGDeviceTreeDiscovery();
    programHCheckpointAndLifecycle();
    programIPlatformRegression();
    std::printf("PPC64 Phase 11: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
