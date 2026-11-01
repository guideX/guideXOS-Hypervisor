// PPC64 Phase 19 guest-side RTAS forwarding trampoline qualification.

#include "PPC64ISAPlugin.h"
#include "PPC64RTAS.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64XICS.h"
#include "VMCheckpoint.h"
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
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                            #condition);                                       \
            }                                                                  \
        } catch (const std::exception& exception) {                            \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s threw %s\n", __FILE__, __LINE__,     \
                        #condition, exception.what());                         \
        }                                                                      \
    } while (0)

constexpr size_t kRamSize = 2U << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kBlockBase = 0x3000;
constexpr uint64_t kCodeCapacity = 4096;
constexpr uint32_t kSource = PPC64_XICS_FIRST_SOURCE + 1U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kBctrl = (19U << 26) | (20U << 21) | (528U << 1) | 1U;

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

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra,
                 int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(displacement);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra,
                  int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

uint32_t encodeX(uint8_t rt, uint8_t ra, uint8_t rb,
                 uint16_t xo, bool record = false) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint32_t>(rb) << 11) |
           (static_cast<uint32_t>(xo) << 1) | (record ? 1U : 0U);
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(reg) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, 467U);
}

uint32_t encodeMfmsr(uint8_t rt) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) | (83U << 1);
}

uint32_t encodeMtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) | (146U << 1);
}

std::vector<uint8_t> encodeImage(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> result;
    result.reserve(words.size() * 4U);
    for (const uint32_t word : words) {
        result.push_back(static_cast<uint8_t>(word >> 24));
        result.push_back(static_cast<uint8_t>(word >> 16));
        result.push_back(static_cast<uint8_t>(word >> 8));
        result.push_back(static_cast<uint8_t>(word));
    }
    return result;
}

PPC64PlatformConfiguration platform(bool trampoline = true,
                                     bool terminal = false) {
    PPC64PlatformConfiguration result;
    result.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    result.rtasEnabled = true;
    result.rtasTrampolineEnabled = trampoline;
    result.virtualTerminalEnabled = terminal;
    return result;
}

bool prepareCapacity(VirtualMachine& vm, bool trampoline = true,
                     bool terminal = false) {
    if (!vm.init()) return false;
    std::vector<uint32_t> filler(
        static_cast<size_t>(kCodeCapacity / 4U), encodeAddi(9, 9, 0));
    std::string error;
    if (!vm.preparePPC64Platform(encodeImage(filler), kEntry, kEntry, &error)) {
        std::printf("Phase 19 platform preparation failed: %s\n", error.c_str());
        return false;
    }
    const auto config = vm.getPPC64PlatformConfiguration();
    return config.rtasTrampolineEnabled == trampoline &&
           config.virtualTerminalEnabled == terminal;
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    const auto bytes = encodeImage(words);
    if (!bytes.empty()) memory.Write(address, bytes.data(), bytes.size());
}

uint32_t be32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1U]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2U]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3U]);
}

uint32_t readBE32(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

struct IndependentFDT {
    std::map<std::string, std::vector<uint8_t>> properties;
    std::map<std::string, size_t> valueOffsets;
    std::vector<std::pair<uint64_t, uint64_t>> reservations;
};

bool parseIndependentFDT(const std::vector<uint8_t>& bytes,
                         IndependentFDT& parsed) {
    if (bytes.size() < 40U || be32(bytes, 0) != 0xd00dfeedU) return false;
    const size_t total = be32(bytes, 4);
    const size_t structureOffset = be32(bytes, 8);
    const size_t stringsOffset = be32(bytes, 12);
    const size_t reserveOffset = be32(bytes, 16);
    const size_t stringsSize = be32(bytes, 32);
    const size_t structureSize = be32(bytes, 36);
    if (total > bytes.size() || structureOffset > total ||
        structureSize > total - structureOffset || stringsOffset > total ||
        stringsSize > total - stringsOffset || reserveOffset > total) return false;
    size_t reserveCursor = reserveOffset;
    while (reserveCursor + 16U <= total) {
        uint64_t base = 0;
        uint64_t length = 0;
        for (size_t i = 0; i < 8U; ++i) {
            base = (base << 8) | bytes[reserveCursor + i];
            length = (length << 8) | bytes[reserveCursor + 8U + i];
        }
        reserveCursor += 16U;
        if (base == 0 && length == 0) break;
        parsed.reservations.emplace_back(base, length);
    }
    if (reserveCursor > total) return false;

    size_t cursor = structureOffset;
    const size_t end = structureOffset + structureSize;
    std::vector<std::string> nodes;
    while (cursor + 4U <= end) {
        const uint32_t token = be32(bytes, cursor);
        cursor += 4U;
        if (token == 1U) {
            const size_t begin = cursor;
            while (cursor < end && bytes[cursor] != 0) ++cursor;
            if (cursor == end) return false;
            nodes.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                               bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            cursor = (cursor + 4U) & ~size_t{3U};
        } else if (token == 2U) {
            if (nodes.empty()) return false;
            nodes.pop_back();
        } else if (token == 3U) {
            if (nodes.empty() || cursor + 8U > end) return false;
            const uint32_t length = be32(bytes, cursor);
            const uint32_t nameOffset = be32(bytes, cursor + 4U);
            cursor += 8U;
            if (length > end - cursor || nameOffset >= stringsSize) return false;
            const size_t nameStart = stringsOffset + nameOffset;
            size_t nameEnd = nameStart;
            while (nameEnd < stringsOffset + stringsSize && bytes[nameEnd] != 0) {
                ++nameEnd;
            }
            if (nameEnd == stringsOffset + stringsSize) return false;
            std::string path;
            for (const auto& node : nodes) {
                if (!node.empty()) path += "/" + node;
            }
            if (path.empty()) path = "/";
            const std::string name(
                bytes.begin() + static_cast<std::ptrdiff_t>(nameStart),
                bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd));
            parsed.valueOffsets[path + "|" + name] = cursor;
            parsed.properties[path + "|" + name] = std::vector<uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor = (cursor + length + 3U) & ~size_t{3U};
        } else if (token == 9U) {
            return nodes.empty();
        } else {
            return false;
        }
    }
    return false;
}

const std::vector<uint8_t>* fdtProperty(const IndependentFDT& fdt,
                                        const std::string& name) {
    const auto found = fdt.properties.find("/rtas|" + name);
    return found == fdt.properties.end() ? nullptr : &found->second;
}

uint32_t fdtCell(const IndependentFDT& fdt, const std::string& name) {
    const auto* value = fdtProperty(fdt, name);
    return value != nullptr && value->size() == 4U ? be32(*value, 0) : 0U;
}

uint64_t fdtCellAddress(const IndependentFDT& fdt,
                        const std::string& name, uint64_t fdtBase) {
    const auto found = fdt.valueOffsets.find("/rtas|" + name);
    return found == fdt.valueOffsets.end() ? 0U : fdtBase + found->second;
}

void emitLoad32(std::vector<uint32_t>& code, uint8_t reg, uint32_t value) {
    code.push_back(encodeAddis(reg, 0, static_cast<int16_t>(value >> 16)));
    code.push_back(encodeOri(reg, reg, static_cast<uint16_t>(value)));
}

void emitStore32(std::vector<uint32_t>& code, uint8_t base,
                 uint16_t offset, uint32_t value) {
    emitLoad32(code, 8, value);
    code.push_back(encodeD(36, 8, base, static_cast<int16_t>(offset)));
}

struct GuestCall {
    size_t branchIndex = 0;
    uint64_t returnAddress = 0;
    uint64_t blockAddress = 0;
    std::string tokenName;
    std::vector<uint32_t> arguments;
    uint32_t returns = 0;
};

void appendTrampolineCall(std::vector<uint32_t>& code,
                          const IndependentFDT& fdt,
                          uint64_t fdtBase, const std::string& tokenName,
                          uint64_t blockAddress,
                          const std::vector<uint32_t>& arguments,
                          uint32_t returns, GuestCall* site = nullptr) {
    const uint64_t entryCell = fdtCellAddress(fdt, "linux,rtas-entry", fdtBase);
    const uint64_t tokenCell = fdtCellAddress(fdt, tokenName, fdtBase);
    emitLoad32(code, 7, static_cast<uint32_t>(blockAddress));
    emitLoad32(code, 6, static_cast<uint32_t>(tokenCell));
    code.push_back(encodeD(32, 8, 6, 0)); // lwz r8,token-cell(r6)
    code.push_back(encodeD(36, 8, 7, 0));
    emitStore32(code, 7, 4, static_cast<uint32_t>(arguments.size()));
    emitStore32(code, 7, 8, returns);
    uint16_t offset = 12;
    for (const uint32_t argument : arguments) {
        emitStore32(code, 7, offset, argument);
        offset = static_cast<uint16_t>(offset + 4U);
    }
    emitLoad32(code, 3, static_cast<uint32_t>(blockAddress));
    emitLoad32(code, 6, static_cast<uint32_t>(entryCell));
    code.push_back(encodeD(32, 10, 6, 0)); // lwz r10,linux,rtas-entry
    code.push_back(encodeMtspr(10, 9));   // mtctr r10
    const size_t branchIndex = code.size();
    code.push_back(kBctrl);
    if (site != nullptr) {
        site->branchIndex = branchIndex;
        site->returnAddress = kEntry + (branchIndex + 1U) * 4U;
        site->blockAddress = blockAddress;
        site->tokenName = tokenName;
        site->arguments = arguments;
        site->returns = returns;
    }
    code.push_back(encodeAddi(11, 11, 1)); // caller continuation
}

std::vector<uint32_t> buildRTASProgram(const IndependentFDT& fdt,
                                       uint64_t fdtBase,
                                       std::vector<GuestCall>& calls,
                                       uint32_t source,
                                       uint32_t priority = 0x33U) {
    std::vector<uint32_t> code;
    calls.resize(6);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3000,
                         {source, 0, priority}, 1, &calls[0]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,get-xive", 0x3040,
                         {source}, 3, &calls[1]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,int-off", 0x3080,
                         {source}, 1, &calls[2]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,int-on", 0x30c0,
                         {source}, 1, &calls[3]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3100,
                         {source, 1, 0x20}, 1, &calls[4]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3140,
                         {PPC64_XICS_LAST_SOURCE + 1U, 0, 0x20}, 1,
                         &calls[5]);
    code.push_back(encodeAddi(12, 12, 7)); // restore continuation sentinel
    return code;
}

bool steps(VirtualMachine& vm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!vm.step()) return false;
    }
    return true;
}

bool stepUntilNIP(VirtualMachine& vm, uint64_t target, size_t limit) {
    auto* cpu = vm.getPPC64ISAPlugin();
    for (size_t i = 0; i < limit && cpu->getNIP() != target; ++i) {
        if (!vm.step()) return false;
    }
    return cpu->getNIP() == target;
}

bool runCall(VirtualMachine& vm, const GuestCall& call,
             uint64_t trampoline, bool inspectState) {
    auto* cpu = vm.getPPC64ISAPlugin();
    const uint64_t branchAddress = kEntry + call.branchIndex * 4U;
    size_t guard = 0;
    while (cpu->getNIP() != branchAddress && guard++ < 512U) {
        if (!vm.step()) return false;
    }
    if (cpu->getNIP() != branchAddress) return false;
    if (!vm.step()) return false; // bctrl entered guest-resident trampoline
    if (inspectState) {
        CHECK(cpu->getNIP() == trampoline);
        CHECK(cpu->getLR() == call.returnAddress);
        CHECK(cpu->readGPR(3) == call.blockAddress);
        CHECK((cpu->getMSR() & (MSR_IR | MSR_DR | MSR_EE | MSR_PR | MSR_HV)) == 0);
    }
    if (!steps(vm, 3U)) return false; // mr, lis, ori
    if (inspectState) {
        CHECK(cpu->getNIP() == trampoline + 12U);
        CHECK(cpu->readGPR(3) == PPC64_H_RTAS);
        CHECK(cpu->readGPR(4) == call.blockAddress);
        CHECK(cpu->getLR() == call.returnAddress);
    }
    if (!vm.step()) return false; // authentic sc 1 / H_RTAS
    if (inspectState) {
        CHECK(cpu->getNIP() == trampoline + 16U);
        CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
        CHECK(cpu->readGPR(4) == call.blockAddress);
        CHECK(cpu->getLR() == call.returnAddress);
    }
    if (!vm.step()) return false; // blr returns through caller LR
    if (inspectState) {
        CHECK(cpu->getNIP() == call.returnAddress);
        CHECK(cpu->getLR() == call.returnAddress);
        CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    }
    if (!vm.step()) return false; // guest continuation
    return true;
}

bool installProgram(VirtualMachine& vm, const std::vector<uint32_t>& code) {
    if (code.size() * 4U > kCodeCapacity) return false;
    writeWords(vm.getMemory(), kEntry, code);
    vm.getPPC64ISAPlugin()->setPC(kEntry);
    return true;
}

void testInstalledEntryAndFDT() {
    std::puts("[phase19-A-C-M-installed-stub-fdt-profile]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    const auto snapshot = vm.getPPC64PlatformSnapshot();
    const auto& layout = snapshot.memoryLayout;
    CHECK(layout.rtasBase == layout.rtasEntry);
    CHECK((layout.rtasEntry & (PPC64_RTAS_PROTECTED_REGION_SIZE - 1U)) == 0U);
    CHECK(layout.rtasSize == PPC64_RTAS_FORWARDING_CODE_SIZE);
    CHECK(layout.rtasProtectionBase == layout.rtasBase);
    CHECK(layout.rtasProtectionSize == PPC64_RTAS_PROTECTED_REGION_SIZE);
    CHECK(layout.rtasBase + layout.rtasProtectionSize <= layout.fdtBase ||
          layout.fdtBase < layout.rtasBase);
    CHECK(layout.rtasBase + layout.rtasSize <= layout.imageBase ||
          layout.imageBase + layout.imageSize <= layout.rtasBase);
    CHECK(vm.validatePPC64RTASTrampoline());

    std::array<uint8_t, PPC64_RTAS_FORWARDING_CODE_SIZE> installed{};
    vm.getMemory().Read(layout.rtasBase, installed.data(), installed.size());
    const std::array<uint8_t, PPC64_RTAS_FORWARDING_CODE_SIZE> expected{{
        0x7c, 0x64, 0x1b, 0x78, 0x3c, 0x60, 0x00, 0x00,
        0x60, 0x63, 0xf0, 0x00, 0x44, 0x00, 0x00, 0x22,
        0x4e, 0x80, 0x00, 0x20}};
    CHECK(installed == expected);
    CHECK(be32(std::vector<uint8_t>(installed.begin(), installed.end()), 0) ==
          0x7c641b78U);
    CHECK(be32(std::vector<uint8_t>(installed.begin(), installed.end()), 12) ==
          ENCODING_SC_HCALL);

    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    CHECK(fdtCell(fdt, "linux,rtas-base") == layout.rtasBase);
    CHECK(fdtCell(fdt, "linux,rtas-entry") == layout.rtasEntry);
    CHECK(fdtCell(fdt, "rtas-size") == layout.rtasSize);
    CHECK(fdtCell(fdt, "rtas-version") == 1U);
    CHECK(fdtCell(fdt, "ibm,set-xive") == PPC64_RTAS_TOKEN_IBM_SET_XIVE);
    CHECK(fdtCell(fdt, "ibm,get-xive") == PPC64_RTAS_TOKEN_IBM_GET_XIVE);
    CHECK(fdtCell(fdt, "ibm,int-off") == PPC64_RTAS_TOKEN_IBM_INT_OFF);
    CHECK(fdtCell(fdt, "ibm,int-on") == PPC64_RTAS_TOKEN_IBM_INT_ON);
    CHECK(std::find(fdt.reservations.begin(), fdt.reservations.end(),
          std::make_pair(layout.rtasProtectionBase,
                         layout.rtasProtectionSize)) != fdt.reservations.end());
    CHECK(vm.validatePPC64RTASTrampoline());

    std::string error;
    const uint8_t corrupt = 0;
    bool rawWriteDenied = false;
    try {
        vm.getMemory().Write(layout.rtasBase, &corrupt, 1U);
    } catch (const std::exception&) {
        rawWriteDenied = true;
    }
    CHECK(rawWriteDenied);
    bool typedWriteDenied = false;
    try {
        dynamic_cast<Memory&>(vm.getMemory()).write<uint32_t>(layout.rtasBase, 0U);
    } catch (const std::exception&) {
        typedWriteDenied = true;
    }
    CHECK(typedWriteDenied);
    bool bulkWriteDenied = false;
    try {
        vm.getMemory().loadBuffer(layout.rtasBase, &corrupt, 1U);
    } catch (const std::exception&) {
        bulkWriteDenied = true;
    }
    CHECK(bulkWriteDenied);
    bool transactionDenied = false;
    try {
        GuestRealWriteSegment write{layout.rtasBase, {corrupt}};
        dynamic_cast<Memory&>(vm.getMemory()).WriteGuestRealTransaction({write});
    } catch (const std::exception&) {
        transactionDenied = true;
    }
    CHECK(transactionDenied);
    CHECK(vm.validatePPC64RTASTrampoline(&error));
    CHECK(vm.reset());
    CHECK(vm.validatePPC64RTASTrampoline());

    VirtualMachine legacy(kRamSize, 1, "PPC64", platform(false));
    CHECK(prepareCapacity(legacy, false));
    IndependentFDT legacyFDT;
    CHECK(parseIndependentFDT(legacy.getPPC64FDT(), legacyFDT));
    CHECK(fdtProperty(legacyFDT, "linux,rtas-base") == nullptr);
    CHECK(fdtProperty(legacyFDT, "linux,rtas-entry") == nullptr);
    CHECK(fdtProperty(legacyFDT, "rtas-size") == nullptr);
    CHECK(fdtProperty(legacyFDT, "ibm,set-xive") != nullptr);

    const uint64_t defaultFdt = (kRamSize - 64U * 1024U) & ~7ULL;
    const uint64_t trampoline =
        (defaultFdt & ~(static_cast<uint64_t>(PPC64_RTAS_PROTECTED_REGION_SIZE) - 1ULL)) -
        PPC64_RTAS_PROTECTED_REGION_SIZE;
    auto conflictConfig = platform();
    conflictConfig.reservedRanges.push_back(
        {trampoline, PPC64_RTAS_PROTECTED_REGION_SIZE, "test conflict", true});
    VirtualMachine conflict(kRamSize, 1, "PPC64", conflictConfig);
    CHECK(conflict.init());
    std::string conflictError;
    CHECK(!conflict.preparePPC64Platform(
        encodeImage({encodeAddi(9, 9, 0)}), kEntry, kEntry, &conflictError));
    CHECK(!conflictError.empty());
}

void testGuestWriteProtectionAndFaults() {
    std::puts("[phase19R1-A-H-protected-page-guest-faults]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    const uint64_t protectedBase = layout.rtasProtectionBase;
    const uint8_t dsiHandler[] = {0x4c, 0x00, 0x00, 0x24}; // rfid
    vm.getMemory().Write(VEC_DSI, dsiHandler, sizeof(dsiHandler));
    const std::vector<uint32_t> program{
        encodeD(38, 4, 3, 0), // stb
        encodeD(44, 4, 3, 0), // sth
        encodeD(36, 4, 3, 0), // stw
        encodeDS(62, 4, 3, 0), // std
        encodeD(36, 4, 3, 0), // cross-boundary stw (r3 adjusted below)
        encodeX(6, 0, 3, XO_LWARX),
        encodeX(4, 0, 3, XO_STWCX, true),
        encodeD(32, 5, 3, 0), // lwz reads protected code
    };
    CHECK(installProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    cpu->writeGPR(3, protectedBase);
    cpu->writeGPR(4, 0xaabbccddU);
    cpu->setPC(kEntry);

    for (size_t index = 0; index < 4U; ++index) {
        CHECK(vm.step());
        CHECK(cpu->getNIP() == VEC_DSI);
        CHECK(cpu->getSRR0() == kEntry + index * 4U);
        CHECK(cpu->getDAR() == protectedBase);
        CHECK(cpu->getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
        cpu->setSRR0(kEntry + (index + 1U) * 4U);
        CHECK(vm.step()); // recover through the guest DSI vector's rfid
    }

    std::string radixReason;
    CHECK(cpu->configureRadixTranslation(
        PPC64RadixConfiguration{0x40000U, 5U, 0, 0, kRamSize},
        vm.getMemory(), radixReason));

    const std::array<uint8_t, 4> leftMarker{{0x11, 0x22, 0x33, 0x44}};
    vm.getMemory().Write(protectedBase - leftMarker.size(),
                         leftMarker.data(), leftMarker.size());
    cpu->writeGPR(3, protectedBase - 2U);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == VEC_DSI);
    CHECK(cpu->getSRR0() == kEntry + 16U);
    CHECK(cpu->getDAR() == protectedBase - 2U);
    CHECK(cpu->getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
    std::array<uint8_t, 4> leftAfter{};
    vm.getMemory().Read(protectedBase - leftAfter.size(),
                        leftAfter.data(), leftAfter.size());
    CHECK(leftAfter == leftMarker);
    cpu->setSRR0(kEntry + 20U);
    CHECK(vm.step());

    cpu->writeGPR(3, protectedBase);
    CHECK(vm.step()); // lwarx can read and reserve protected executable memory
    CHECK(cpu->hasReservation());
    CHECK(vm.step()); // stwcx. faults architecturally instead of silently failing
    CHECK(cpu->getNIP() == VEC_DSI);
    CHECK(cpu->getSRR0() == kEntry + 24U);
    CHECK(cpu->getDAR() == protectedBase);
    CHECK(cpu->getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
    CHECK(!cpu->hasReservation());
    cpu->setSRR0(kEntry + 28U);
    CHECK(vm.step());
    CHECK(vm.step()); // execute load from protected guest memory
    CHECK(cpu->readGPR(5) == PPC64_RTAS_FORWARDING_WORDS[0]);

    cpu->setPC(layout.rtasEntry);
    cpu->writeGPR(3, kBlockBase);
    CHECK(vm.step()); // authentic fetch and execution from protected page
    CHECK(cpu->getNIP() == layout.rtasEntry + 4U);
    CHECK(cpu->readGPR(4) == kBlockBase);
    CHECK(vm.validatePPC64RTASTrampoline());
}

void testGuestForwardingAndXICS() {
    std::puts("[phase19-B-D-G-guest-forwarding-xics-real-mode]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    const auto snapshot = vm.getPPC64PlatformSnapshot();
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    std::vector<GuestCall> calls;
    const auto code = buildRTASProgram(
        fdt, snapshot.memoryLayout.fdtBase, calls, kSource);
    CHECK(installProgram(vm, code));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(
        kSource, PPC64XICSSourceType::EDGE, 0x55,
        PPC64_XICS_SERVER_ID, 0, true, &error));

    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->writeGPR(14, 0x1414141414141414ULL);
    cpu->writeGPR(15, 0x1515151515151515ULL);
    cpu->setMSR(MSR_ME | MSR_RI); // Linux PPC64 32-bit RTAS real-mode profile.
    CHECK((cpu->getMSR() & (MSR_SF | MSR_IR | MSR_DR | MSR_EE | MSR_PR | MSR_HV)) == 0);
    const uint64_t instructionsBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    CHECK(runCall(vm, calls[0], snapshot.memoryLayout.rtasEntry, true));
    CHECK(readBE32(vm.getMemory(), calls[0].blockAddress + 24U) == 0U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].savedPriority == 0x33U);
    CHECK(cpu->getMSR() == (MSR_ME | MSR_RI));
    CHECK(cpu->getCR() == 0U);
    CHECK(cpu->getXER() == 0U);
    CHECK(cpu->readGPR(14) == 0x1414141414141414ULL);
    CHECK(cpu->readGPR(15) == 0x1515151515151515ULL);
    CHECK(vm.getCyclesExecuted() > instructionsBefore);
    CHECK(vm.getPPC64Timebase() - timebaseBefore ==
          vm.getCyclesExecuted() - instructionsBefore);

    CHECK(runCall(vm, calls[1], snapshot.memoryLayout.rtasEntry, false));
    CHECK(readBE32(vm.getMemory(), calls[1].blockAddress + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), calls[1].blockAddress + 20U) == 0U);
    CHECK(readBE32(vm.getMemory(), calls[1].blockAddress + 24U) == 0x33U);

    CHECK(runCall(vm, calls[2], snapshot.memoryLayout.rtasEntry, false));
    CHECK(readBE32(vm.getMemory(), calls[2].blockAddress + 16U) == 0U);
    auto xics = vm.getPPC64XICSSnapshot();
    CHECK(xics.sources[0].priority == 0xffU);
    CHECK(xics.sources[0].savedPriority == 0x33U);
    CHECK(!xics.sources[0].enabled);
    CHECK(vm.assertPPC64InterruptSource(kSource, 0, &error));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].presented);

    CHECK(runCall(vm, calls[3], snapshot.memoryLayout.rtasEntry, false));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(readBE32(vm.getMemory(), calls[3].blockAddress + 16U) == 0U);
    CHECK(xics.sources[0].priority == 0x33U);
    CHECK(xics.sources[0].enabled);
    CHECK(xics.sources[0].pending);

    CHECK(runCall(vm, calls[4], snapshot.memoryLayout.rtasEntry, false));
    CHECK(readBE32(vm.getMemory(), calls[4].blockAddress + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x33U);

    CHECK(runCall(vm, calls[5], snapshot.memoryLayout.rtasEntry, false));
    CHECK(readBE32(vm.getMemory(), calls[5].blockAddress + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
    CHECK(vm.step());
    CHECK(cpu->readGPR(12) == 7U);

    // The same FDT-discovered code address and source ID are private to a VM.
    VirtualMachine peer(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(peer));
    IndependentFDT peerFDT;
    CHECK(parseIndependentFDT(peer.getPPC64FDT(), peerFDT));
    std::vector<GuestCall> peerCalls;
    const auto peerCode = buildRTASProgram(
        peerFDT, peer.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        peerCalls, kSource, 0x66U);
    CHECK(installProgram(peer, peerCode));
    CHECK(peer.configurePPC64InterruptSource(
        kSource, PPC64XICSSourceType::EDGE, 0x66,
        PPC64_XICS_SERVER_ID, 0, true, &error));
    peer.getPPC64ISAPlugin()->setMSR(MSR_ME | MSR_RI);
    CHECK(runCall(peer, peerCalls[0],
                  peer.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
    CHECK(peer.getPPC64XICSSnapshot().sources[0].priority == 0x66U);
    CHECK(peer.getMemory().GetRawData() != vm.getMemory().GetRawData());
}

void testDirectHRTASCompatibility() {
    std::puts("[phase19-L-direct-h-rtas-compatibility]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(
        kSource, PPC64XICSSourceType::EDGE, 0x44,
        PPC64_XICS_SERVER_ID, 0, true, &error));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);
    auto* const registry = vm.getPPC64RTASServiceRegistry();
    CHECK(registry != nullptr);
    CHECK(registry != nullptr && registry->services().size() == 4U);

    const auto writeBlock = [&vm](uint64_t address, uint32_t token,
                                  uint32_t nargs, uint32_t nret,
                                  const std::vector<uint32_t>& args) {
        std::array<uint8_t, 12> header{{
            static_cast<uint8_t>(token >> 24),
            static_cast<uint8_t>(token >> 16),
            static_cast<uint8_t>(token >> 8),
            static_cast<uint8_t>(token),
            static_cast<uint8_t>(nargs >> 24),
            static_cast<uint8_t>(nargs >> 16),
            static_cast<uint8_t>(nargs >> 8),
            static_cast<uint8_t>(nargs),
            static_cast<uint8_t>(nret >> 24),
            static_cast<uint8_t>(nret >> 16),
            static_cast<uint8_t>(nret >> 8),
            static_cast<uint8_t>(nret)}};
        vm.getMemory().Write(address, header.data(), header.size());
        for (size_t i = 0; i < args.size(); ++i) {
            const uint32_t value = args[i];
            const std::array<uint8_t, 4> cell{{
                static_cast<uint8_t>(value >> 24),
                static_cast<uint8_t>(value >> 16),
                static_cast<uint8_t>(value >> 8),
                static_cast<uint8_t>(value)}};
            vm.getMemory().Write(address + 12U + i * 4U,
                                 cell.data(), cell.size());
        }
    };
    const auto directCall = [&vm, cpu](uint32_t block) {
        std::vector<uint32_t> code;
        emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_RTAS));
        emitLoad32(code, 4, block);
        code.push_back(kSc1);
        code.push_back(encodeAddi(12, 12, 1));
        CHECK(installProgram(vm, code));
        cpu->setMSR(MSR_ME | MSR_RI);
        return steps(vm, 5U);
    };

    constexpr uint32_t kDirectBlock = 0x3180U;
    writeBlock(kDirectBlock, PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
               {kSource, 0U, 0x22U});
    CHECK(directCall(kDirectBlock));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kDirectBlock + 24U) == 0U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x22U);
    CHECK(vm.getPPC64RTASServiceRegistry() == registry);

    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    std::vector<uint32_t> trampolineCode;
    GuestCall trampolineCall;
    appendTrampolineCall(trampolineCode, fdt,
        vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        "ibm,set-xive", 0x31c0U, {kSource, 0U, 0x26U}, 1U,
        &trampolineCall);
    CHECK(installProgram(vm, trampolineCode));
    CHECK(runCall(vm, trampolineCall,
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), trampolineCall.blockAddress + 24U) == 0U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x26U);
    CHECK(vm.getPPC64RTASServiceRegistry() == registry);

    constexpr uint32_t kBadTokenBlock = 0x3200U;
    const auto beforeBadToken = vm.getPPC64XICSSnapshot();
    writeBlock(kBadTokenBlock, 0xfeed1234U, 1U, 1U, {kSource});
    CHECK(directCall(kBadTokenBlock));
    CHECK(cpu->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(readBE32(vm.getMemory(), kBadTokenBlock + 16U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          beforeBadToken.sources[0].priority);
    CHECK(vm.getPPC64RTASServiceRegistry() == registry);

    CHECK(directCall(static_cast<uint32_t>(kRamSize)));
    CHECK(cpu->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          beforeBadToken.sources[0].priority);

    const uint64_t protectedEntry =
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry;
    CHECK(directCall(static_cast<uint32_t>(protectedEntry)));
    CHECK(cpu->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(vm.validatePPC64RTASTrampoline());
    CHECK(vm.getPPC64RTASServiceRegistry() == registry);
}

std::vector<uint32_t> terminalHandler() {
    return {encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_XIRR)), kSc1,
            encodeAddi(8, 4, 0), encodeAddis(4, 0, 0x3000),
            encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_GET_TERM_CHAR)), kSc1,
            encodeAddi(9, 4, 0), encodeAddi(10, 5, 0),
            encodeAddi(11, 6, 0), encodeAddis(4, 0, 0x3000),
            encodeAddi(5, 9, 0), encodeAddi(6, 10, 0),
            encodeAddi(7, 11, 0), encodeAddi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            encodeAddi(4, 8, 0), encodeAddi(3, 0, PPC64_H_EOI), kSc1, kRfid};
}

void testTerminalInterruptIntegration() {
    std::puts("[phase19-E-F-N-trampoline-terminal-interrupt-integration]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform(true, true));
    CHECK(prepareCapacity(vm, true, true));
    const auto platformSnapshot = vm.getPPC64PlatformSnapshot();
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    const uint32_t source = vm.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    std::vector<uint32_t> code;
    GuestCall setXive;
    appendTrampolineCall(code, fdt, platformSnapshot.memoryLayout.fdtBase,
                         "ibm,set-xive", 0x3000, {source, 0, 0x10}, 1,
                         &setXive);
    code.insert(code.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)), kSc1,
        encodeMfmsr(6)});
    code.push_back(encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)));
    code.push_back(encodeMtmsr(6));
    emitLoad32(code, 7, 0x3200);
    emitLoad32(code, 6, 0x5031394fU); // guest creates "P19O"
    code.push_back(encodeD(36, 6, 7, 0));
    code.push_back(encodeDS(58, 6, 7, 0)); // ld packed bytes into r6
    emitLoad32(code, 4, vm.getPPC64VirtualTerminalId());
    code.push_back(encodeAddi(5, 0, 4));
    code.push_back(encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_PUT_TERM_CHAR)));
    code.push_back(kSc1);
    const size_t outputReadyPC = kEntry + code.size() * 4U;
    code.push_back(encodeAddi(9, 9, 1));
    CHECK(installProgram(vm, code));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == kEntry);
    vm.getPPC64ISAPlugin()->setMSR(MSR_ME | MSR_RI);
    writeWords(vm.getMemory(), 0x500, terminalHandler());
    CHECK(vm.getPPC64RTASServiceRegistry() != nullptr);
    CHECK(stepUntilNIP(vm, outputReadyPC, 512U));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == outputReadyPC);
    CHECK(vm.readPPC64TerminalOutput() == std::vector<uint8_t>({'P', '1', '9', 'O'}));
    CHECK(readBE32(vm.getMemory(), 0x3018) == 0U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x10U);

    CHECK(vm.injectPPC64TerminalInputWithStatus({'Q'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.step());
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, terminalHandler().size()));
    CHECK(vm.readPPC64TerminalOutput() == std::vector<uint8_t>({'P', '1', '9', 'O', 'Q'}));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == outputReadyPC);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].acknowledged);
}

void testCheckpointAndFaultBoundaries() {
    std::puts("[phase19-H-J-checkpoint-fault-boundaries]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    std::vector<GuestCall> calls;
    const auto code = buildRTASProgram(
        fdt, vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        calls, kSource);
    CHECK(installProgram(vm, code));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(
        kSource, PPC64XICSSourceType::EDGE, 0x40,
        PPC64_XICS_SERVER_ID, 0, true, &error));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);
    CHECK(runCall(vm, calls[0],
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
    CHECK(vm.pause());

    const auto checkpointPath = std::filesystem::temp_directory_path() /
        "guidexos-phase19r1-rtas-v9.gxcp";
    std::error_code removeError;
    std::filesystem::remove(checkpointPath, removeError);
    std::string checkpointError;
    CHECK(VMCheckpointCodec::write(vm, checkpointPath.string(),
                                   "phase19-trampoline", &checkpointError));
    std::ifstream input(checkpointPath, std::ios::binary);
    std::array<uint8_t, 16> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    CHECK(input.gcount() == static_cast<std::streamsize>(header.size()));
    const uint32_t fileVersion = static_cast<uint32_t>(header[8]) |
        (static_cast<uint32_t>(header[9]) << 8) |
        (static_cast<uint32_t>(header[10]) << 16) |
        (static_cast<uint32_t>(header[11]) << 24);
    CHECK(fileVersion == 9U);

    VirtualMachine restored(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(restored));
    CHECK(VMCheckpointCodec::read(restored, checkpointPath.string(),
        "phase19-trampoline", &checkpointError));
    CHECK(restored.validatePPC64RTASTrampoline());
    CHECK(restored.getPPC64PlatformSnapshot().memoryLayout.rtasEntry ==
          vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry);
    CHECK(restored.getPPC64PlatformSnapshot().rtasServiceRegistryVersion ==
          PPC64_RTAS_SERVICE_REGISTRY_VERSION);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
    CHECK(restored.getPPC64ISAPlugin()->getLR() == cpu->getLR());
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == cpu->getNIP());
    bool restoredWriteDenied = false;
    try {
        const uint8_t corrupt = 0;
        restored.getMemory().Write(
            restored.getPPC64PlatformSnapshot().memoryLayout.rtasBase,
            &corrupt, 1U);
    } catch (const std::exception&) {
        restoredWriteDenied = true;
    }
    CHECK(restoredWriteDenied);
    CHECK(restored.resume());
    const uint64_t continuation = restored.getPPC64ISAPlugin()->getNIP();
    CHECK(restored.step());
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == continuation + 4U);

    const uint64_t restoredNIP = restored.getPPC64ISAPlugin()->getNIP();
    const auto restoredLayout =
        restored.getPPC64PlatformSnapshot().memoryLayout;
    VMStateSnapshot corruptPage = vm.captureVMSnapshot();
    corruptPage.memoryState.data[static_cast<size_t>(
        restoredLayout.rtasProtectionBase +
        PPC64_RTAS_FORWARDING_CODE_SIZE)] = 0x5a;
    CHECK(!restored.restoreVMSnapshot(corruptPage));
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == restoredNIP);
    CHECK(restored.validatePPC64RTASTrampoline());

    VMStateSnapshot corruptPermissionMetadata = vm.captureVMSnapshot();
    corruptPermissionMetadata.ppc64Platform.memoryLayout.rtasProtectionSize =
        PPC64_RTAS_PROTECTED_REGION_SIZE / 2U;
    CHECK(!restored.restoreVMSnapshot(corruptPermissionMetadata));
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == restoredNIP);
    std::filesystem::remove(checkpointPath, removeError);

    VirtualMachine badEntry(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(badEntry));
    badEntry.getPPC64ISAPlugin()->setMSR(MSR_ME | MSR_RI);
    badEntry.getPPC64ISAPlugin()->setPC(
        badEntry.getPPC64PlatformSnapshot().memoryLayout.rtasEntry + 1U);
    CHECK(!badEntry.step());
    CHECK(badEntry.getLastExecutionDiagnostic().size() != 0U);

    VirtualMachine unmapped(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(unmapped));
    unmapped.getPPC64ISAPlugin()->setMSR(MSR_ME | MSR_RI);
    unmapped.getPPC64ISAPlugin()->setPC(kRamSize + 0x100U);
    CHECK(!unmapped.step());
    CHECK(unmapped.getLastExecutionDiagnostic().size() != 0U);

    VirtualMachine badLR(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(badLR));
    badLR.getPPC64ISAPlugin()->setMSR(MSR_ME | MSR_RI);
    badLR.getPPC64ISAPlugin()->writeGPR(5, kRamSize + 0x1000U);
    writeWords(badLR.getMemory(), kEntry, {encodeMtspr(5, 8)}); // mtlr r5
    CHECK(badLR.step());
    CHECK(badLR.getPPC64ISAPlugin()->getLR() == kRamSize + 0x1000U);
    badLR.getPPC64ISAPlugin()->setPC(
        badLR.getPPC64PlatformSnapshot().memoryLayout.rtasEntry + 16U);
    CHECK(badLR.step()); // blr consumes the deliberately invalid LR
    CHECK(badLR.getPPC64ISAPlugin()->getNIP() == kRamSize + 0x1000U);
    CHECK(!badLR.step());

    VirtualMachine relocated(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(relocated));
    relocated.getPPC64ISAPlugin()->setMSR(MSR_SF | MSR_IR | MSR_DR);
    relocated.getPPC64ISAPlugin()->setPC(
        relocated.getPPC64PlatformSnapshot().memoryLayout.rtasEntry);
    CHECK(!relocated.step()); // no process/radix translation profile is configured
    CHECK(relocated.getLastExecutionDiagnostic().size() != 0U);
}

void testHistoricalV6Fixture() {
    std::puts("[phase19-K-historical-v6-fixture]");
    const std::filesystem::path fixture(PPC64_PHASE19_V6_FIXTURE);
    std::ifstream input(fixture, std::ios::binary);
    std::array<uint8_t, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    CHECK(input.gcount() == static_cast<std::streamsize>(header.size()));
    CHECK(std::vector<uint8_t>(header.begin(), header.begin() + 8) ==
          std::vector<uint8_t>({'G','X','P','P','C','6','4','V'}));
    const uint32_t version = static_cast<uint32_t>(header[8]) |
        (static_cast<uint32_t>(header[9]) << 8) |
        (static_cast<uint32_t>(header[10]) << 16) |
        (static_cast<uint32_t>(header[11]) << 24);
    CHECK(version == 6U);

    auto config = platform(false, true);
    config.rtasEnabled = false;
    config.reservedRanges.push_back(
        {0x40000U, 0x10000U, "partition table", true});
    config.reservedRanges.push_back(
        {0x50000U, 0x1000U, "partition radix root", true});
    config.reservedRanges.push_back(
        {0x51000U, 0x1000U, "partition radix l1", true});
    config.reservedRanges.push_back(
        {0x52000U, 0x1000U, "partition radix l2", true});
    config.reservedRanges.push_back(
        {0x53000U, 0x1000U, "partition radix l3", true});
    VirtualMachine restored(kRamSize, 1, "PPC64", config);
    CHECK(restored.init());
    const std::vector<uint32_t> fixtureProgram{
        encodeAddi(7, 0, 0x80), encodeMtspr(7, 22),
        encodeAddi(10, 10, 1), kSc1, encodeAddi(10, 10, 1)};
    std::string error;
    CHECK(restored.preparePPC64Platform(
        encodeImage(fixtureProgram), kEntry, kEntry, &error));
    CHECK(VMCheckpointCodec::read(restored, fixture.string(),
        "ppc64-phase17-v6-fixture", &error));
    auto* cpu = restored.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    CHECK(cpu->getNIP() == kEntry + 16U);
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(cpu->readGPR(10) == 1U);
    CHECK(cpu->getDEC() == 125U);
    CHECK(cpu->getActiveLPID() == 0U);

    std::array<uint8_t, 8> ram{};
    restored.getMemory().Read(0x2000U, ram.data(), ram.size());
    CHECK((ram == std::array<uint8_t, 8>{{'V','6','R','A','M',0x12,0x34,0x56}}));
    CHECK(restored.readPPC64TerminalOutput() ==
          std::vector<uint8_t>({'V','6','O','K'}));
    const auto xics = restored.getPPC64XICSSnapshot();
    CHECK(xics.sources.size() == 2U);
    const auto source = std::find_if(xics.sources.begin(), xics.sources.end(),
        [](const auto& item) { return item.sourceId == 0x1111U; });
    CHECK(source != xics.sources.end());
    CHECK(source != xics.sources.end() && source->priority == 0x33U);
    CHECK(source != xics.sources.end() && source->enabled);
    CHECK(restored.getPPC64PartitionManager()->partitionCount() == 1U);
    CHECK(restored.getPPC64PartitionManager()->findPartition(7U) != nullptr);
    CHECK(restored.step());
    CHECK(cpu->getNIP() == kEntry + 20U);
    CHECK(cpu->readGPR(10) == 2U);
}

} // namespace

int main() {
    testInstalledEntryAndFDT();
    testGuestWriteProtectionAndFaults();
    testGuestForwardingAndXICS();
    testDirectHRTASCompatibility();
    testTerminalInterruptIntegration();
    testCheckpointAndFaultBoundaries();
    testHistoricalV6Fixture();
    std::printf("PPC64 Phase 19: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
