// PPC64 Phase 13 bounded pSeries XICS compatibility acceptance tests.

#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
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
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kCounter = 0x2000;
constexpr uint64_t kMessage = 0x3000;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
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

PPC64PlatformConfiguration xicsConfig(bool terminal = false,
                                      uint64_t ticksPerInstruction = 1,
                                      uint64_t initialTimebase = 0) {
    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.virtualTerminalEnabled = terminal;
    config.ticksPerInstruction = ticksPerInstruction;
    config.initialTimebase = initialTimebase;
    return config;
}

bool prepare(VirtualMachine& vm, std::vector<uint32_t> words) {
    for (size_t i = 0; i < 64; ++i) words.push_back(encodeAddi(10, 10, 1));
    if (!vm.init()) {
        std::printf("Phase 13 VM init failed\n");
        return false;
    }
    std::string error;
    if (!vm.preparePPC64Platform(image(words), kEntry, kEntry, &error)) {
        std::printf("Phase 13 platform preparation failed: %s\n", error.c_str());
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

void writeDoubleword(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint32_t readBE32(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
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

std::vector<uint32_t> basicXirrEoiHandler(bool extended = false,
                                         bool record = true) {
    std::vector<uint32_t> handler{
        encodeAddi(3, 0, static_cast<int16_t>(extended
            ? PPC64_H_XIRR_X : PPC64_H_XIRR)),
        kSc1};
    if (record) {
        handler.push_back(encodeAddi(8, 4, 0));
        handler.push_back(encodeAddi(7, 0, static_cast<int16_t>(kCounter)));
        handler.push_back(encodeD(36, 8, 7, 0));
    }
    handler.push_back(encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_EOI)));
    handler.push_back(kSc1);
    handler.push_back(kRfid);
    return handler;
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
            for (const auto& node : nodes) {
                if (!node.empty()) path += "/" + node;
            }
            if (path.empty()) path = "/";
            const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(stringsOffset + nameOffset),
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

void programA_InitializationAndFDT() {
    std::printf("[phase13-A-initialization-and-FDT]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, {
        encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)), kSc1,
        encodeAddi(4, 0, 0),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_IPOLL)), kSc1}));
    CHECK(vm.getPPC64PlatformConfiguration().interruptMode ==
          PPC64InterruptMode::XICS_COMPAT);
    CHECK(vm.getPPC64PlatformCapabilities().interruptController);
    CHECK(vm.getPPC64XICSSnapshot().enabled);
    CHECK(vm.getPPC64XICSSnapshot().presentations.size() == 1);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].ownerLPID == 0);
    std::string sourceError;
    CHECK(vm.configurePPC64InterruptSource(PPC64_XICS_FIRST_SOURCE,
          PPC64XICSSourceType::EDGE, 0x40, 0, 0, true, &sourceError));
    CHECK(vm.assertPPC64InterruptSource(PPC64_XICS_FIRST_SOURCE, 0,
                                        &sourceError));
    CHECK(steps(vm, 6));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(plugin->readGPR(4) == 0xff001100U);
    CHECK(plugin->readGPR(5) == 0xffU);
    CHECK(plugin->getNIP() == kEntry + 24U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);

    IndependentFDT fdt;
    CHECK(parseFDT(vm.getPPC64FDT(), fdt));
    const auto* type = fdtProperty(fdt, "/interrupt-controller", "device_type");
    const auto* compatible = fdtProperty(fdt, "/interrupt-controller", "compatible");
    const auto* cells = fdtProperty(fdt, "/interrupt-controller", "#interrupt-cells");
    const auto* servers = fdtProperty(fdt, "/interrupt-controller",
                                      "ibm,interrupt-server-ranges");
    const auto* phandle = fdtProperty(fdt, "/interrupt-controller", "phandle");
    const auto* linuxPhandle = fdtProperty(fdt, "/interrupt-controller", "linux,phandle");
    const auto* marker = fdtProperty(fdt, "/interrupt-controller", "interrupt-controller");
    CHECK(type != nullptr && std::string(type->begin(), type->end() - 1) ==
          "PowerPC-External-Interrupt-Presentation");
    CHECK(compatible != nullptr && std::string(compatible->begin(), compatible->end() - 1) ==
          "IBM,ppc-xicp");
    CHECK(cells != nullptr && cells->size() == 4 && be32(*cells, 0) == 2);
    CHECK(servers != nullptr && servers->size() == 8 && be32(*servers, 0) == 0 &&
          be32(*servers, 4) == 1);
    CHECK(phandle != nullptr && phandle->size() == 4 && be32(*phandle, 0) == 1);
    CHECK(linuxPhandle != nullptr && linuxPhandle->size() == 4 &&
          be32(*linuxPhandle, 0) == 1);
    CHECK(marker != nullptr && marker->empty());
    CHECK(fdtProperty(fdt, "/interrupt-controller", "ibm,power-ivpe") == nullptr);

    VirtualMachine disabled(kRamSize, 1, "PPC64", PPC64PlatformConfiguration{});
    CHECK(prepare(disabled, {encodeAddi(10, 10, 1)}));
    IndependentFDT disabledFdt;
    CHECK(parseFDT(disabled.getPPC64FDT(), disabledFdt));
    CHECK(fdtProperty(disabledFdt, "/interrupt-controller", "compatible") == nullptr);
    CHECK(!disabled.getPPC64PlatformCapabilities().interruptController);
}

void programB_FirstAcknowledgedInterrupt() {
    std::printf("[phase13-B-first-acknowledged-interrupt]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, mainWithCPPR()));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler());
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(PPC64_XICS_FIRST_SOURCE,
          PPC64XICSSourceType::EDGE, 0x40, 0, 0, true, &error));
    CHECK(vm.assertPPC64InterruptSource(PPC64_XICS_FIRST_SOURCE, 0, &error));
    CHECK(!vm.assertPPC64ExternalInterrupt(0, &error));
    CHECK(steps(vm, 6));
    const uint64_t cyclesBeforeEntry = vm.getCyclesExecuted();
    CHECK(step(vm));
    auto* plugin = vm.getPPC64ISAPlugin();
    CHECK(plugin->getNIP() == 0x500U);
    CHECK(plugin->getSRR0() == kEntry + 24U);
    CHECK(plugin->getSRR1() == (MSR_SF | MSR_EE));
    CHECK(plugin->getMSR() == MSR_SF);
    CHECK(plugin->getInterruptSnapshot().lastDeliveryVector == 0x500U);
    CHECK(vm.getCyclesExecuted() == cyclesBeforeEntry);
    CHECK(steps(vm, 8));
    CHECK(readBE32(vm.getMemory(), kCounter) == 0xff001100U);
    CHECK(plugin->getNIP() == kEntry + 24U);
    CHECK(plugin->getMSR() == (MSR_SF | MSR_EE));
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(vm.deassertPPC64InterruptSource(PPC64_XICS_FIRST_SOURCE, 0, &error));
    CHECK(vm.removePPC64InterruptSource(PPC64_XICS_FIRST_SOURCE, 0, &error));
}

void programC_PriorityAndEqualPriorityOrdering() {
    std::printf("[phase13-C-priority-arbitration]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, mainWithCPPR()));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler());
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1101, PPC64XICSSourceType::EDGE,
                                           0x40, 0, 0, true, &error));
    CHECK(vm.configurePPC64InterruptSource(0x1102, PPC64XICSSourceType::EDGE,
                                           0x20, 0, 0, true, &error));
    CHECK(steps(vm, 6));
    CHECK(vm.assertPPC64InterruptSource(0x1101, 0, &error));
    CHECK(vm.assertPPC64InterruptSource(0x1102, 0, &error));
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(vm.getPPC64XICSSnapshot().sources[1].presented);
    CHECK(steps(vm, 8));
    CHECK(readBE32(vm.getMemory(), kCounter) == 0xff001102U);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, 8));
    CHECK(readBE32(vm.getMemory(), kCounter) == 0xff001101U);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(vm.deassertPPC64InterruptSource(0x1101, 0, &error));
    CHECK(vm.deassertPPC64InterruptSource(0x1102, 0, &error));

    VirtualMachine equal(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(equal, mainWithCPPR()));
    writeWords(equal.getMemory(), 0x500, basicXirrEoiHandler(false, false));
    CHECK(equal.configurePPC64InterruptSource(0x1104, PPC64XICSSourceType::EDGE,
                                               0x20, 0, 0, true, &error));
    CHECK(equal.configurePPC64InterruptSource(0x1103, PPC64XICSSourceType::EDGE,
                                               0x20, 0, 0, true, &error));
    CHECK(steps(equal, 6));
    CHECK(equal.assertPPC64InterruptSource(0x1104, 0, &error));
    CHECK(equal.assertPPC64InterruptSource(0x1103, 0, &error));
    CHECK(step(equal));
    CHECK(steps(equal, 2));
    CHECK(equal.getPPC64ISAPlugin()->readGPR(4) == 0xff001103U);

    VirtualMachine nested(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(nested, mainWithCPPR()));
    writeWords(nested.getMemory(), 0x500, basicXirrEoiHandler());
    CHECK(nested.configurePPC64InterruptSource(0x1109,
          PPC64XICSSourceType::EDGE, 0x40, 0, 0, true, &error));
    CHECK(nested.configurePPC64InterruptSource(0x110a,
          PPC64XICSSourceType::EDGE, 0x20, 0, 0, true, &error));
    CHECK(steps(nested, 6));
    CHECK(nested.assertPPC64InterruptSource(0x1109, 0, &error));
    CHECK(step(nested));
    CHECK(steps(nested, 5)); // Accept the first source but leave it active.
    CHECK(nested.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(nested.assertPPC64InterruptSource(0x110a, 0, &error));
    CHECK(nested.getPPC64XICSSnapshot().sources[1].presented);
    CHECK(nested.getPPC64XICSSnapshot().presentations[0].outstanding.size() == 1);
    CHECK(steps(nested, 3)); // EOI the first source and return through rfid.
    CHECK(step(nested));
    CHECK(nested.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(nested, 8));
    CHECK(readBE32(nested.getMemory(), kCounter) == 0xff00110aU);
    CHECK(nested.deassertPPC64InterruptSource(0x1109, 0, &error));
    CHECK(nested.deassertPPC64InterruptSource(0x110a, 0, &error));
}

void programD_CPPRMaskAndPolling() {
    std::printf("[phase13-D-cppr-mask-and-poll]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, {
        encodeAddi(4, 0, 0x30), encodeAddi(3, 0, PPC64_H_CPPR), kSc1,
        encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(6), encodeAddi(4, 0, 0),
        encodeAddi(3, 0, PPC64_H_IPOLL), kSc1,
        encodeAddi(4, 0, 0xff), encodeAddi(3, 0, PPC64_H_CPPR), kSc1}));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler(false, false));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1105, PPC64XICSSourceType::EDGE,
                                            0x40, 0, 0, true, &error));
    CHECK(vm.assertPPC64InterruptSource(0x1105, 0, &error));
    CHECK(steps(vm, 9));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 0x30000000U);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(5) == 0xffU);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].cppr == 0xff);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, 2));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 0xff001105U);

    VirtualMachine badServer(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(badServer, {encodeAddi(4, 0, 1),
        encodeAddi(5, 0, 0x55), encodeAddi(3, 0, PPC64_H_IPOLL), kSc1}));
    CHECK(steps(badServer, 4));
    CHECK(badServer.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(badServer.getPPC64ISAPlugin()->readGPR(4) == 1);
    CHECK(badServer.getPPC64ISAPlugin()->readGPR(5) == 0x55);
}

void programE_LevelReassertion() {
    std::printf("[phase13-E-level-reassertion]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, mainWithCPPR()));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler());
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1106, PPC64XICSSourceType::LEVEL,
                                            0x30, 0, 0, true, &error));
    CHECK(steps(vm, 6));
    CHECK(vm.assertPPC64InterruptSource(0x1106, 0, &error));
    CHECK(step(vm));
    CHECK(steps(vm, 8));
    CHECK(readBE32(vm.getMemory(), kCounter) == 0xff001106U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(vm.deassertPPC64InterruptSource(0x1106, 0, &error));
    CHECK(steps(vm, 8));
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 28U);
}

void programF_DECAndXICSIndependence() {
    std::printf("[phase13-F-dec-and-xics-independence]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, {
        encodeAddi(3, 0, 0), encodeMtspr(3, static_cast<uint16_t>(kSprDEC)),
        encodeAddi(4, 0, 0xff), encodeAddi(3, 0, PPC64_H_CPPR), kSc1,
        encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(6)}));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler(false, false));
    writeWords(vm.getMemory(), 0x900, {
        encodeAddi(7, 0, 100), encodeMtspr(7, static_cast<uint16_t>(kSprDEC)), kRfid});
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1107, PPC64XICSSourceType::EDGE,
                                            0x20, 0, 0, true, &error));
    CHECK(vm.assertPPC64InterruptSource(0x1107, 0, &error));
    CHECK(steps(vm, 7));
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(step(vm));
    CHECK(step(vm)); // Enable EE, then deliver at the next instruction boundary.
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 0xff001107U);
    CHECK(vm.deassertPPC64InterruptSource(0x1107, 0, &error));
    CHECK(steps(vm, 2));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 32U);
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(step(vm));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x900U);
    CHECK(vm.getPPC64InterruptSnapshot().lastDeliveryVector == 0x900U);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry + 32U);
    CHECK(!vm.getPPC64InterruptSnapshot().decrementerPending);
}

void programG_ExtendedXirrAndVirtualTime() {
    std::printf("[phase13-G-xirr-x-timebase]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig(false, 7, 500));
    CHECK(prepare(vm, mainWithCPPR()));
    IndependentFDT fdt;
    CHECK(parseFDT(vm.getPPC64FDT(), fdt));
    const auto* compatible = fdtProperty(
        fdt, "/interrupt-controller", "compatible");
    const auto* servers = fdtProperty(
        fdt, "/interrupt-controller", "ibm,interrupt-server-ranges");
    CHECK(compatible != nullptr &&
          std::string(compatible->begin(), compatible->end() - 1) == "IBM,ppc-xicp");
    CHECK(servers != nullptr && servers->size() == 8 &&
          be32(*servers, 0) == 0 && be32(*servers, 4) == 1);
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler(true, false));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1108, PPC64XICSSourceType::EDGE,
                                            0x22, 0, 0, true, &error));
    CHECK(steps(vm, 6));
    CHECK(vm.assertPPC64InterruptSource(0x1108, 0, &error));
    CHECK(step(vm));
    const uint64_t presentedAt = vm.getPPC64Timebase();
    CHECK(steps(vm, 2));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 0xff001108U);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(5) == presentedAt + 7U);
    CHECK(vm.getPPC64Timebase() == presentedAt + 14U);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(vm.deassertPPC64InterruptSource(0x1108, 0, &error));
}

void programH_TwoVMIsolation() {
    std::printf("[phase13-H-two-vm-isolation]\n");
    auto first = std::make_unique<VirtualMachine>(kRamSize, 1, "PPC64", xicsConfig());
    VirtualMachine second(kRamSize, 1, "PPC64", xicsConfig());
    std::vector<uint32_t> rootProbe = mainWithCPPR();
    rootProbe.insert(rootProbe.end(), {
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_XIRR)), kSc1});
    CHECK(prepare(*first, rootProbe));
    CHECK(prepare(second, rootProbe));
    std::string error;
    auto createPartition = [&error](VirtualMachine& vm) {
        auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
        if (memory == nullptr ||
            !vm.getPPC64PartitionManager()->configurePartitionTable(
                *memory, 0x40000, 4, error)) {
            return std::shared_ptr<PPC64PartitionContext>{};
        }
        return vm.getPPC64PartitionManager()->createPartition(7, error);
    };
    const auto firstPartition = createPartition(*first);
    const auto secondPartition = createPartition(second);
    CHECK(firstPartition != nullptr && secondPartition != nullptr);
    CHECK(first->configurePPC64InterruptSource(0x1110,
          PPC64XICSSourceType::LEVEL, 0x20, 0, 7, true, &error));
    CHECK(second.configurePPC64InterruptSource(0x1110,
          PPC64XICSSourceType::LEVEL, 0x60, 0, 7, true, &error));
    CHECK(steps(*first, 6));
    CHECK(steps(second, 6));
    CHECK(first->assertPPC64InterruptSource(0x1110, 7, &error));
    CHECK(first->getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!second.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!second.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(!first->assertPPC64InterruptSource(0x1110, 0, &error));
    CHECK(!first->deassertPPC64InterruptSource(0x1110, 0, &error));
    CHECK(steps(*first, 3)); // Root H_XIRR cannot acknowledge LPID 7's source.
    CHECK(first->getPPC64ISAPlugin()->readGPR(4) == 0xff000000U);
    CHECK(first->getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!first->getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(first->getPPC64ISAPlugin()->activatePartitionContext(firstPartition,
                                                               error));
    CHECK(first->getPPC64XICSSnapshot().activeLPID == 7);
    CHECK(second.getPPC64ISAPlugin()->activatePartitionContext(secondPartition,
                                                                error));
    CHECK(second.getPPC64XICSSnapshot().activeLPID == 7);
    CHECK(second.assertPPC64InterruptSource(0x1110, 7, &error));
    CHECK(second.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(first->getPPC64XICSSnapshot().sources[0].priority == 0x20);
    CHECK(second.getPPC64XICSSnapshot().sources[0].priority == 0x60);
    CHECK(first->getPPC64XICSSnapshot().presentations[1].ownerLPID == 7);
    CHECK(second.getPPC64XICSSnapshot().presentations[1].ownerLPID == 7);
    first->getPPC64ISAPlugin()->deactivatePartitionContext();
    second.getPPC64ISAPlugin()->deactivatePartitionContext();
    CHECK(first->getPPC64XICSSnapshot().activeLPID == 0);
    CHECK(second.getPPC64XICSSnapshot().activeLPID == 0);
    CHECK(steps(second, 3));
    CHECK(second.getPPC64ISAPlugin()->readGPR(4) == 0xff000000U);
    CHECK(second.getPPC64XICSSnapshot().sources[0].pending);
    first.reset();
    CHECK(second.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(second.deassertPPC64InterruptSource(0x1110, 7, &error));
    CHECK(second.removePPC64InterruptSource(0x1110, 7, &error));
    CHECK(second.getPPC64XICSSnapshot().sources.empty());
}

void programI_CheckpointContinuationAndValidation() {
    std::printf("[phase13-I-checkpoint-continuation]\n");
    VirtualMachine masked(kRamSize, 1, "PPC64", xicsConfig());
    std::vector<uint32_t> maskedProgram = mainWithCPPR(0x20);
    maskedProgram.insert(maskedProgram.end(), {
        encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)), kSc1});
    CHECK(prepare(masked, maskedProgram));
    std::string error;
    CHECK(masked.configurePPC64InterruptSource(0x1115,
          PPC64XICSSourceType::LEVEL, 0x30, 0, 0, true, &error));
    CHECK(steps(masked, 6));
    CHECK(masked.assertPPC64InterruptSource(0x1115, 0, &error));
    const VMStateSnapshot maskedPending = masked.captureVMSnapshot();
    CHECK(maskedPending.ppc64Xics.sources[0].pending);
    CHECK(!maskedPending.ppc64Xics.sources[0].presented);
    CHECK(masked.restoreVMSnapshot(maskedPending));
    CHECK(masked.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!masked.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(steps(masked, 4)); // Lower CPPR through guest H_CPPR.
    CHECK(step(masked));
    CHECK(masked.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(masked.deassertPPC64InterruptSource(0x1115, 0, &error));
    CHECK(masked.removePPC64InterruptSource(0x1115, 0, &error));

    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, mainWithCPPR()));
    writeWords(vm.getMemory(), 0x500, basicXirrEoiHandler());
    CHECK(vm.configurePPC64InterruptSource(0x1111, PPC64XICSSourceType::LEVEL,
                                            0x30, 0, 0, true, &error));
    CHECK(steps(vm, 6));
    const VMStateSnapshot noInterrupt = vm.captureVMSnapshot();
    CHECK(noInterrupt.ppc64Xics.sources.size() == 1);
    CHECK(vm.restoreVMSnapshot(noInterrupt));
    CHECK(vm.assertPPC64InterruptSource(0x1111, 0, &error));
    const VMStateSnapshot presented = vm.captureVMSnapshot();
    CHECK(presented.ppc64Xics.sources[0].pending);
    CHECK(presented.ppc64Xics.sources[0].presented);
    CHECK(vm.restoreVMSnapshot(presented));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(steps(vm, 3)); // Enter vector, load H_XIRR, then acknowledge it.
    const VMStateSnapshot acknowledged = vm.captureVMSnapshot();
    CHECK(acknowledged.ppc64Xics.sources[0].acknowledged);
    CHECK(acknowledged.ppc64Xics.presentations[0].outstanding.size() == 1);
    CHECK(vm.restoreVMSnapshot(acknowledged));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(steps(vm, 7));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);

    const VMStateSnapshot levelStillAsserted = vm.captureVMSnapshot();
    CHECK(vm.restoreVMSnapshot(levelStillAsserted));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].asserted);

    VMStateSnapshot corrupt = vm.captureVMSnapshot();
    corrupt.ppc64Xics.sources.push_back(corrupt.ppc64Xics.sources[0]);
    CHECK(!vm.restoreVMSnapshot(corrupt));
    CHECK(vm.getPPC64XICSSnapshot().sources.size() == 1);
    corrupt = vm.captureVMSnapshot();
    corrupt.ppc64Xics.activeLPID = 1;
    CHECK(!vm.restoreVMSnapshot(corrupt));
    CHECK(vm.getPPC64XICSSnapshot().activeLPID == 0);

    const auto path = std::filesystem::temp_directory_path() /
                      "guidexos-phase13-xics.gxcp";
    const auto corruptPath = std::filesystem::temp_directory_path() /
                             "guidexos-phase13-xics-corrupt.gxcp";
    CHECK(vm.pause());
    CHECK(VMCheckpointCodec::write(vm, path.string(), "phase13-xics", &error));
    VirtualMachine restored(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(restored, mainWithCPPR()));
    writeWords(restored.getMemory(), 0x500, basicXirrEoiHandler());
    CHECK(VMCheckpointCodec::read(restored, path.string(), "phase13-xics", &error));
    CHECK(restored.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].presented);
    std::ifstream input(path, std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    CHECK(file.size() > 40);
    if (!file.empty()) file.back() ^= 1;
    {
        std::ofstream output(corruptPath, std::ios::binary | std::ios::trunc);
        output.write(file.data(), static_cast<std::streamsize>(file.size()));
    }
    CHECK(!VMCheckpointCodec::read(restored, corruptPath.string(),
                                   "phase13-xics", &error));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(corruptPath, ignored);
}

void programJ_GuestConsoleIntegration() {
    std::printf("[phase13-J-guest-console-integration]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig(true));
    CHECK(prepare(vm, mainWithCPPR()));
    writeDoubleword(vm.getMemory(), kMessage, 0x5849435300000000ULL); // XICS
    writeWords(vm.getMemory(), 0x500, {
        encodeAddi(3, 0, PPC64_H_XIRR), kSc1,
        encodeAddi(8, 4, 0), encodeAddis(4, 0, 0x3000),
        encodeAddi(5, 0, 4), encodeAddi(7, 0, static_cast<int16_t>(kMessage)),
        encodeDS(58, 6, 7, 0), encodeAddi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
        encodeAddi(4, 8, 0), encodeAddi(3, 0, PPC64_H_EOI), kSc1, kRfid});
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(0x1112, PPC64XICSSourceType::EDGE,
                                            0x20, 0, 0, true, &error));
    CHECK(steps(vm, 6));
    CHECK(vm.assertPPC64InterruptSource(0x1112, 0, &error));
    CHECK(step(vm));
    CHECK(steps(vm, 13));
    const auto output = vm.readPPC64TerminalOutput(0, 8);
    CHECK(output.size() == 4);
    CHECK(std::string(output.begin(), output.end()) == "XICS");
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(vm.deassertPPC64InterruptSource(0x1112, 0, &error));
}

void programK_ErrorAndSourceCleanup() {
    std::printf("[phase13-K-error-and-cleanup]\n");
    VirtualMachine vm(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(vm, {
        encodeAddi(4, 0, 0), encodeAddi(5, 0, 0),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_IPI)), kSc1,
        encodeAddi(4, 0, 1), encodeAddi(5, 0, 0x20),
        encodeAddi(3, 0, PPC64_H_IPI), kSc1,
        encodeAddi(4, 0, 0xff), encodeAddi(3, 0, PPC64_H_EOI), kSc1}));
    std::string error;
    CHECK(!vm.configurePPC64InterruptSource(0x100f, PPC64XICSSourceType::EDGE,
                                            0x20, 0, 0, true, &error));
    CHECK(!vm.configurePPC64InterruptSource(0x1113, PPC64XICSSourceType::EDGE,
                                            0xff, 0, 0, true, &error));
    CHECK(!vm.configurePPC64InterruptSource(0x1113, PPC64XICSSourceType::EDGE,
                                            0x20, 1, 0, true, &error));
    CHECK(vm.configurePPC64InterruptSource(0x1113, PPC64XICSSourceType::EDGE,
                                           0x20, 0, 0, true, &error));
    CHECK(steps(vm, 4));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_FUNCTION);
    CHECK(steps(vm, 4));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(steps(vm, 3));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);
    CHECK(vm.assertPPC64InterruptSource(0x1113, 0, &error));
    CHECK(!vm.removePPC64InterruptSource(0x1113, 0, &error));
    CHECK(vm.deassertPPC64InterruptSource(0x1113, 0, &error));
    CHECK(!vm.removePPC64InterruptSource(0x1113, 0, &error));

    VirtualMachine resetVM(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(resetVM, mainWithCPPR()));
    CHECK(resetVM.configurePPC64InterruptSource(0x1116,
          PPC64XICSSourceType::LEVEL, 0x20, 0, 0, true, &error));
    CHECK(steps(resetVM, 6));
    CHECK(resetVM.assertPPC64InterruptSource(0x1116, 0, &error));
    CHECK(resetVM.setPPC64InterruptSourceEnabled(0x1116, 0, false, &error));
    CHECK(resetVM.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!resetVM.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(resetVM.setPPC64InterruptSourceEnabled(0x1116, 0, true, &error));
    CHECK(resetVM.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(resetVM.reset());
    const auto resetState = resetVM.getPPC64XICSSnapshot();
    CHECK(resetState.sources.empty());
    CHECK(resetState.presentations.size() == 1);
    CHECK(resetState.presentations[0].ownerLPID == 0);
    CHECK(resetState.presentations[0].cppr == 0);
    CHECK(resetState.presentations[0].mfrr == 0xff);
    CHECK(resetVM.configurePPC64InterruptSource(0x1116,
          PPC64XICSSourceType::LEVEL, 0x20, 0, 0, true, &error));

    VirtualMachine inaccessible(kRamSize, 1, "PPC64", xicsConfig());
    CHECK(prepare(inaccessible, mainWithCPPR()));
    CHECK(inaccessible.configurePPC64InterruptSource(0x1114,
          PPC64XICSSourceType::EDGE, 0x20, 0, 0, true, &error));
    CHECK(steps(inaccessible, 6));
    CHECK(inaccessible.assertPPC64InterruptSource(0x1114, 0, &error));
    CHECK(step(inaccessible));
    CHECK(!inaccessible.step());
    CHECK(inaccessible.getState() == VMState::ERROR);
}

} // namespace

int main() {
    std::printf("PPC64 Phase 13 POWER9 pSeries XICS compatibility acceptance\n");
    programA_InitializationAndFDT();
    programB_FirstAcknowledgedInterrupt();
    programC_PriorityAndEqualPriorityOrdering();
    programD_CPPRMaskAndPolling();
    programE_LevelReassertion();
    programF_DECAndXICSIndependence();
    programG_ExtendedXirrAndVirtualTime();
    programH_TwoVMIsolation();
    programI_CheckpointContinuationAndValidation();
    programJ_GuestConsoleIntegration();
    programK_ErrorAndSourceCleanup();
    std::printf("PPC64 Phase 13: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
