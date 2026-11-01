// PPC64 Phase 18 hosted direct-H_RTAS acceptance tests.

#include "PPC64ISAPlugin.h"
#include "PPC64RTAS.h"
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
constexpr uint64_t kParamBase = 0x3000;
constexpr uint64_t kResult = 0x2000;
constexpr uint32_t kSource = PPC64_XICS_FIRST_SOURCE + 1U;
constexpr uint32_t kLevelSource = PPC64_XICS_FIRST_SOURCE + 2U;
constexpr uint32_t kCrossOwnerSource = PPC64_XICS_FIRST_SOURCE + 3U;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint64_t kSprDEC = 22;
constexpr uint64_t kPartitionTable = 0x80000;
constexpr uint64_t kPartitionRoot = 0x100000;
constexpr uint64_t kPartitionL1 = 0x110000;
constexpr uint64_t kPartitionL2 = 0x111000;
constexpr uint64_t kPartitionL3 = 0x112000;
constexpr uint64_t kPartitionCodeGRA = 0x300000;
constexpr uint64_t kPartitionDataGRA = 0x310000;
constexpr uint64_t kCrossPartitionGRA = 0x320000;
constexpr uint64_t kPartitionCodeBacking = 0x400000;
constexpr uint64_t kPartitionDataBacking = 0x500000;
constexpr uint64_t kCrossPartitionBacking = 0x600000;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;

class TestMMIODevice final : public IMemoryMappedDevice {
public:
    uint64_t GetBaseAddress() const override { return 0x180000U; }
    size_t GetSize() const override { return 0x1000U; }
    bool Read(uint64_t, uint8_t*, size_t) const override { return false; }
    bool Write(uint64_t, const uint8_t*, size_t) override { return false; }
};

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

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra,
                  int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

std::vector<uint8_t> image(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (const uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

PPC64PlatformConfiguration platform(bool rtas = true, bool terminal = false) {
    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.virtualTerminalEnabled = terminal;
    config.rtasEnabled = rtas;
    return config;
}

bool prepare(VirtualMachine& vm, std::vector<uint32_t> words) {
    for (size_t i = 0; i < 64; ++i) {
        words.push_back(encodeAddi(10, 10, 1));
    }
    if (!vm.init()) return false;
    std::string error;
    if (!vm.preparePPC64Platform(image(words), kEntry, kEntry, &error)) {
        std::printf("Phase 18 platform preparation failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

void writeBE32(IMemory& memory, uint64_t address, uint32_t value) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    memory.Write(address, bytes.data(), bytes.size());
}

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (56U - index * 8U));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t partitionRadixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t partitionLeafPTE(uint64_t backing) {
    return kPteValid | kPteLeaf | kPteReference | kPteChange |
           (backing & 0x01fffffffffff000ULL) | 0x7ULL;
}

void mapPartitionGuestReal(IMemory& memory, uint64_t guestReal,
                           uint64_t backing) {
    const uint64_t pteAddress = kPartitionL3 +
        (((guestReal >> 12) & 0x1ffULL) * 8ULL);
    writeBE64(memory, pteAddress, partitionLeafPTE(backing));
}

uint32_t readBE32(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words) {
    const std::vector<uint8_t> bytes = image(words);
    if (!bytes.empty()) memory.Write(address, bytes.data(), bytes.size());
}

bool steps(VirtualMachine& vm, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (!vm.step()) {
            ++checks;
            ++failures;
            std::printf("FAIL %s:%d: guest instruction %zu/%zu executed\n",
                        __FILE__, __LINE__, index + 1U, count);
            return false;
        }
    }
    return true;
}

void emitLoad32(std::vector<uint32_t>& code, uint8_t reg, uint32_t value) {
    code.push_back(encodeAddis(reg, 0, static_cast<int16_t>(value >> 16)));
    code.push_back(encodeOri(reg, reg, static_cast<uint16_t>(value)));
}

void emitStoreCell(std::vector<uint32_t>& code, uint8_t base,
                   uint16_t offset, uint32_t value) {
    emitLoad32(code, 8, value);
    code.push_back(encodeD(36, 8, base, static_cast<int16_t>(offset)));
}

size_t emitRTASCall(std::vector<uint32_t>& code, uint32_t block,
                    uint32_t token, const std::vector<uint32_t>& arguments,
                    uint32_t returns) {
    const size_t start = code.size();
    emitLoad32(code, 7, block);
    emitStoreCell(code, 7, 0, token);
    emitStoreCell(code, 7, 4, static_cast<uint32_t>(arguments.size()));
    emitStoreCell(code, 7, 8, returns);
    uint16_t offset = 12;
    for (const uint32_t argument : arguments) {
        emitStoreCell(code, 7, offset, argument);
        offset = static_cast<uint16_t>(offset + 4U);
    }
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_RTAS));
    code.push_back(encodeOri(7, 4, 0));
    code.push_back(kSc1);
    return code.size() - start;
}

size_t emitRTASCallFromFDT(std::vector<uint32_t>& code, uint32_t block,
                           uint64_t tokenGuestRealAddress,
                           const std::vector<uint32_t>& arguments,
                           uint32_t returns) {
    const size_t start = code.size();
    emitLoad32(code, 7, block);
    emitLoad32(code, 6, static_cast<uint32_t>(tokenGuestRealAddress));
    code.push_back(encodeD(32, 8, 6, 0)); // lwz r8,0(r6): read FDT token
    code.push_back(encodeD(36, 8, 7, 0));
    emitStoreCell(code, 7, 4, static_cast<uint32_t>(arguments.size()));
    emitStoreCell(code, 7, 8, returns);
    uint16_t offset = 12;
    for (const uint32_t argument : arguments) {
        emitStoreCell(code, 7, offset, argument);
        offset = static_cast<uint16_t>(offset + 4U);
    }
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_RTAS));
    code.push_back(encodeOri(7, 4, 0));
    code.push_back(kSc1);
    return code.size() - start;
}

size_t emitRawCall(std::vector<uint32_t>& code, uint32_t block) {
    const size_t start = code.size();
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_RTAS));
    emitLoad32(code, 4, block);
    code.push_back(kSc1);
    return code.size() - start;
}

void installGuestCode(VirtualMachine& vm, const std::vector<uint32_t>& code) {
    writeWords(vm.getMemory(), kEntry, code);
    vm.getPPC64ISAPlugin()->setPC(kEntry);
}

bool runCode(VirtualMachine& vm, const std::vector<uint32_t>& code) {
    installGuestCode(vm, code);
    return steps(vm, code.size());
}

void writeHostBlock(IMemory& memory, uint64_t address, uint32_t token,
                    const std::vector<uint32_t>& arguments, uint32_t returns) {
    writeBE32(memory, address, token);
    writeBE32(memory, address + 4, static_cast<uint32_t>(arguments.size()));
    writeBE32(memory, address + 8, returns);
    for (size_t index = 0; index < arguments.size(); ++index) {
        writeBE32(memory, address + 12U + index * 4U, arguments[index]);
    }
}

std::vector<uint32_t> xirrEoiHandler() {
    return {encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_XIRR)), kSc1,
            encodeAddi(8, 4, 0), encodeAddi(7, 0, static_cast<int16_t>(kResult)),
            encodeD(36, 8, 7, 0), encodeAddi(4, 8, 0),
            encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_EOI)), kSc1, kRfid};
}

std::vector<uint32_t> terminalEchoHandler() {
    return {encodeAddi(3, 0, PPC64_H_XIRR), kSc1,
            encodeAddi(8, 4, 0), encodeAddis(4, 0, 0x3000),
            encodeAddi(3, 0, PPC64_H_GET_TERM_CHAR), kSc1,
            encodeAddi(9, 4, 0), encodeAddi(10, 5, 0),
            encodeAddi(11, 6, 0), encodeAddis(4, 0, 0x3000),
            encodeAddi(5, 9, 0), encodeAddi(6, 10, 0),
            encodeAddi(7, 11, 0), encodeAddi(3, 0, PPC64_H_PUT_TERM_CHAR), kSc1,
            encodeAddi(4, 8, 0), encodeAddi(3, 0, PPC64_H_EOI), kSc1, kRfid};
}

uint32_t be32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

struct IndependentFDT {
    std::map<std::string, std::vector<uint8_t>> properties;
    std::map<std::string, size_t> valueOffsets;
};

// This reader only recognizes the public FDT token/string-table layout. It
// does not call the production builder's parser or validation routines.
bool parseIndependentFDT(const std::vector<uint8_t>& bytes,
                         IndependentFDT& parsed) {
    if (bytes.size() < 40U || be32(bytes, 0) != 0xd00dfeedU) return false;
    const size_t total = be32(bytes, 4);
    const size_t structureOffset = be32(bytes, 8);
    const size_t stringsOffset = be32(bytes, 12);
    const size_t structureSize = be32(bytes, 36);
    const size_t stringsSize = be32(bytes, 32);
    if (total > bytes.size() || structureOffset > total ||
        structureSize > total - structureOffset || stringsOffset > total ||
        stringsSize > total - stringsOffset) return false;
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
            for (const std::string& node : nodes) {
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

const std::vector<uint8_t>* property(const IndependentFDT& fdt,
                                    const std::string& path,
                                    const std::string& name) {
    const auto found = fdt.properties.find(path + "|" + name);
    return found == fdt.properties.end() ? nullptr : &found->second;
}

void testFDTRegistryAndDiscovery() {
    std::puts("[phase18-A-B-FDT-and-registry]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, {encodeAddi(10, 10, 1)}));
    CHECK(vm.getPPC64PlatformCapabilities().rtas);
    CHECK(vm.getPPC64RTASServiceRegistry() != nullptr);
    CHECK(vm.getPPC64RTASServiceRegistry()->services().size() == 4U);
    CHECK(vm.getPPC64RTASServiceRegistry()->find(
              PPC64_RTAS_TOKEN_IBM_SET_XIVE) != nullptr);
    CHECK(vm.getPPC64RTASServiceRegistry()->find(
              PPC64_RTAS_TOKEN_IBM_GET_XIVE) != nullptr);
    CHECK(vm.getPPC64RTASServiceRegistry()->find(
              PPC64_RTAS_TOKEN_IBM_INT_OFF) != nullptr);
    CHECK(vm.getPPC64RTASServiceRegistry()->find(
              PPC64_RTAS_TOKEN_IBM_INT_ON) != nullptr);

    PPC64RTASServiceDescriptor duplicate;
    duplicate.name = "duplicate-set-xive";
    duplicate.token = PPC64_RTAS_TOKEN_IBM_SET_XIVE;
    duplicate.requiredArguments = 3;
    duplicate.requiredReturns = 1;
    duplicate.handler = [](PPC64XICS&, uint16_t,
                           const std::vector<uint32_t>&) {
        return PPC64RTASServiceResult{};
    };
    std::string reason;
    CHECK(!vm.getPPC64RTASServiceRegistry()->registerService(duplicate, reason));
    CHECK(!reason.empty());

    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    const auto* compatible = property(fdt, "/rtas", "compatible");
    const auto* version = property(fdt, "/rtas", "rtas-version");
    CHECK(compatible != nullptr &&
          std::string(compatible->begin(), compatible->end() - 1) ==
              "guidexos,hosted-direct-hcall-rtas-v1");
    CHECK(version != nullptr && version->size() == 4U && be32(*version, 0) == 1U);
    const std::array<std::pair<const char*, uint32_t>, 4> tokens{{
        {"ibm,set-xive", PPC64_RTAS_TOKEN_IBM_SET_XIVE},
        {"ibm,get-xive", PPC64_RTAS_TOKEN_IBM_GET_XIVE},
        {"ibm,int-off", PPC64_RTAS_TOKEN_IBM_INT_OFF},
        {"ibm,int-on", PPC64_RTAS_TOKEN_IBM_INT_ON}}};
    for (const auto& [name, expected] : tokens) {
        const auto* value = property(fdt, "/rtas", name);
        CHECK(value != nullptr && value->size() == 4U &&
              be32(*value, 0) == expected);
    }
    CHECK(property(fdt, "/rtas", "linux,rtas-base") == nullptr);
    CHECK(property(fdt, "/rtas", "linux,rtas-entry") == nullptr);

    VirtualMachine disabled(kRamSize, 1, "PPC64", platform(false));
    CHECK(prepare(disabled, {encodeAddi(10, 10, 1)}));
    IndependentFDT disabledFdt;
    CHECK(parseIndependentFDT(disabled.getPPC64FDT(), disabledFdt));
    CHECK(property(disabledFdt, "/rtas", "ibm,set-xive") == nullptr);
    CHECK(disabled.getPPC64RTASServiceRegistry() == nullptr);
    CHECK(!disabled.getPPC64PlatformCapabilities().rtas);
}

void testAuthenticXiveAndPendingInterruptFlow() {
    std::puts("[phase18-C-D-E-F-guest-RTAS-and-XICS]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    std::vector<uint32_t> guest;
    size_t setCount = emitRTASCall(guest, 0x3000,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x30}, 1);
    const size_t getCount = emitRTASCall(guest, 0x3040,
        PPC64_RTAS_TOKEN_IBM_GET_XIVE, {kSource}, 3);
    const size_t offCount = emitRTASCall(guest, 0x3080,
        PPC64_RTAS_TOKEN_IBM_INT_OFF, {kSource}, 1);
    const size_t changeWhilePendingCount = emitRTASCall(guest, 0x30c0,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x28}, 1);
    const size_t offAgainCount = emitRTASCall(guest, 0x3100,
        PPC64_RTAS_TOKEN_IBM_INT_OFF, {kSource}, 1);
    const size_t onCount = emitRTASCall(guest, 0x3140,
        PPC64_RTAS_TOKEN_IBM_INT_ON, {kSource}, 1);
    const size_t cpprOffset = guest.size();
    guest.insert(guest.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1});
    const size_t enableOffset = guest.size();
    guest.insert(guest.end(), {encodeMfmsr(6),
        encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(6)});
    const size_t disabledOffCount = emitRTASCall(guest, 0x3180,
        PPC64_RTAS_TOKEN_IBM_INT_OFF, {kLevelSource}, 1);
    const size_t disabledOnCount = emitRTASCall(guest, 0x31c0,
        PPC64_RTAS_TOKEN_IBM_INT_ON, {kLevelSource}, 1);
    CHECK(prepare(vm, guest));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(kLevelSource,
        PPC64XICSSourceType::LEVEL, 0x38, PPC64_XICS_SERVER_ID, 0, false,
        &error));
    writeWords(vm.getMemory(), 0x500, xirrEoiHandler());
    const size_t firstThree = setCount + getCount + offCount;
    CHECK(steps(vm, firstThree));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64ISAPlugin()->readGPR(4) == 0x3080U);
    CHECK(readBE32(vm.getMemory(), 0x3000 + 24) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3040 + 16) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3040 + 20) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3040 + 24) == 0x30U);
    CHECK(readBE32(vm.getMemory(), 0x3080 + 16) == 0U);
    auto xics = vm.getPPC64XICSSnapshot();
    CHECK(xics.sources.size() == 2U);
    CHECK(xics.sources[0].sourceId == kSource);
    CHECK(xics.sources[0].priority == 0xffU);
    CHECK(xics.sources[0].savedPriority == 0x30U);
    CHECK(!xics.sources[0].enabled);
    CHECK(vm.assertPPC64InterruptSource(kSource, 0, &error));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(xics.sources[0].pending);
    CHECK(!xics.sources[0].presented);

    CHECK(steps(vm, changeWhilePendingCount + offAgainCount + onCount));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(xics.sources[0].pending);
    CHECK(xics.sources[0].priority == 0x28U);
    CHECK(xics.sources[0].savedPriority == 0x28U);
    CHECK(xics.sources[0].enabled);
    CHECK(readBE32(vm.getMemory(), 0x30c0 + 24) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3100 + 16) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3140 + 16) == 0U);

    CHECK(steps(vm, 3)); // Set ICP CPPR to allow the pending source.
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(steps(vm, 3)); // Enable guest external interrupts.
    const uint64_t returnPC = kEntry + (enableOffset + 3U) * 4U;
    CHECK(vm.step());
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(vm.getPPC64ISAPlugin()->getSRR0() == returnPC);
    CHECK(steps(vm, xirrEoiHandler().size()));
    CHECK(readBE32(vm.getMemory(), kResult) == (0xff000000U | kSource));
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == returnPC);
    xics = vm.getPPC64XICSSnapshot();
    CHECK(!xics.sources[0].pending);
    CHECK(!xics.sources[0].acknowledged);
    CHECK(xics.presentations[0].outstanding.empty());
    CHECK(returnPC == kEntry + (cpprOffset + 6U) * 4U);

    CHECK(steps(vm, disabledOffCount));
    CHECK(vm.assertPPC64InterruptSource(kLevelSource, 0, &error));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(!xics.sources[1].enabled);
    CHECK(xics.sources[1].pending);
    CHECK(xics.sources[1].priority == 0xffU);
    CHECK(xics.sources[1].savedPriority == 0x38U);
    CHECK(!xics.sources[1].savedEnabled);
    CHECK(steps(vm, disabledOnCount));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(!xics.sources[1].enabled);
    CHECK(xics.sources[1].pending);
    CHECK(xics.sources[1].priority == 0x38U);
    CHECK(!xics.sources[1].savedEnabled);
}

void testMalformedBlocksAndErrorLayers() {
    std::puts("[phase18-H-I-malformed-blocks-and-errors]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(vm, {encodeAddi(10, 10, 1)}));
    std::string error;
    auto* guestRAM = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(guestRAM != nullptr);
    CHECK(guestRAM != nullptr && vm.getPPC64PartitionManager()->
        configurePartitionTable(*guestRAM, 0x40000, 4, error));
    CHECK(vm.getPPC64PartitionManager()->createPartition(7, error) != nullptr);
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(kCrossOwnerSource,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 7, true,
        &error));

    const auto runPointer = [&vm](uint32_t pointer) {
        std::vector<uint32_t> code;
        emitRawCall(code, pointer);
        installGuestCode(vm, code);
        CHECK(steps(vm, code.size()));
        return vm.getPPC64ISAPlugin()->readGPR(3);
    };
    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_GET_XIVE,
                   {kSource}, 3);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), kParamBase + 20U) == 0U);
    CHECK(readBE32(vm.getMemory(), kParamBase + 24U) == 0x40U);

    const auto before = vm.getPPC64XICSSnapshot();
    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0, 0x100}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 1, 0x20}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {PPC64_XICS_LAST_SOURCE + 1U, 0, 0x20}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_GET_XIVE,
                   {kSource}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 16U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kParamBase + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0, 0x20}, 0);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_PARAMETER);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, 0xfeed1234U, {kSource}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_PARAMETER);
    CHECK(readBE32(vm.getMemory(), kParamBase + 16U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0, 0x20}, 1);
    writeBE32(vm.getMemory(), kParamBase + 4U, PPC64_RTAS_MAX_CELLS + 1U);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_PARAMETER);
    writeBE32(vm.getMemory(), kParamBase + 4U, 16U);
    writeBE32(vm.getMemory(), kParamBase + 8U, 1U);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase)) == PPC64_H_PARAMETER);

    writeHostBlock(vm.getMemory(), kParamBase, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0, 0x20}, 1);
    CHECK(runPointer(static_cast<uint32_t>(kParamBase + 1U)) == PPC64_H_PARAMETER);
    CHECK(runPointer(0x00200000U) == PPC64_H_PARAMETER);
    CHECK(runPointer(0xfffffff0U) == PPC64_H_PARAMETER);
    {
        std::vector<uint32_t> negativePointer;
        emitLoad32(negativePointer, 3, static_cast<uint32_t>(PPC64_H_RTAS));
        negativePointer.push_back(encodeAddi(4, 0, -16));
        negativePointer.push_back(kSc1);
        installGuestCode(vm, negativePointer);
        CHECK(steps(vm, negativePointer.size()));
        CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    }
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    TestMMIODevice mmio;
    if (guestRAM != nullptr) {
        guestRAM->RegisterDevice(&mmio);
        CHECK(runPointer(static_cast<uint32_t>(mmio.GetBaseAddress())) ==
              PPC64_H_PARAMETER);
        CHECK(guestRAM->UnregisterDevice(&mmio));
    }
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    // The guest request ends at the RAM limit; the response cell is outside
    // RAM. It must be rejected before set-xive changes the live source.
    const uint64_t endBlock = kRamSize - 24U;
    writeHostBlock(vm.getMemory(), endBlock, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                   {kSource, 0, 0x10}, 1);
    CHECK(runPointer(static_cast<uint32_t>(endBlock)) == PPC64_H_PARAMETER);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          before.sources[0].priority);

    // A complete request that crosses a page boundary is accepted because
    // both pages and the entire response range are validated.
    std::vector<uint32_t> crossPage;
    const size_t crossCount = emitRTASCall(crossPage, 0x3ff4,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x50}, 1);
    CHECK(crossCount != 0U);
    installGuestCode(vm, crossPage);
    CHECK(steps(vm, crossPage.size()));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3ff4 + 24U) == 0U);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x50U);

    // A source owned by LPID 7 cannot be read or changed by the root RTAS
    // caller, even though the token and source number are valid.
    std::vector<uint32_t> otherOwner;
    emitRTASCall(otherOwner, 0x3200, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                 {kCrossOwnerSource, 0, 0x10}, 1);
    CHECK(runCode(vm, otherOwner));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3200 + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    const auto afterOwnerCall = vm.getPPC64XICSSnapshot();
    const auto ownerSource = std::find_if(
        afterOwnerCall.sources.begin(), afterOwnerCall.sources.end(), [](const auto& source) {
            return source.sourceId == kCrossOwnerSource;
        });
    CHECK(ownerSource != afterOwnerCall.sources.end());
    CHECK(ownerSource->priority == 0x44U && ownerSource->ownerLPID == 7U);
}

void testProtectedMemoryAndCrossVMIsolation() {
    std::puts("[phase18-J-memory-protection-and-two-VM-isolation]");
    VirtualMachine protectedVM(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(protectedVM, {encodeAddi(10, 10, 1)}));
    std::string error;
    CHECK(protectedVM.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    writeHostBlock(protectedVM.getMemory(), 0x500,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x10}, 1);
    CHECK(protectedVM.getPPC64ISAPlugin()->configureHypervisorMemory(
        protectedVM.getMemory(), 0, 0x1000, error));
    std::vector<uint32_t> protectedCall;
    emitRawCall(protectedCall, 0x500);
    CHECK(runCode(protectedVM, protectedCall));
    CHECK(protectedVM.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(protectedVM.getPPC64XICSSnapshot().sources[0].priority == 0x40U);

    VirtualMachine first(kRamSize, 1, "PPC64", platform(true, true));
    VirtualMachine second(kRamSize, 1, "PPC64", platform(true, true));
    std::vector<uint32_t> setFirst;
    std::vector<uint32_t> setSecond;
    emitRTASCall(setFirst, 0x3000, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                 {kSource, 0, 0x20}, 1);
    emitRTASCall(setSecond, 0x3000, PPC64_RTAS_TOKEN_IBM_SET_XIVE,
                 {kSource, 0, 0x60}, 1);
    setFirst.insert(setFirst.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1, encodeMfmsr(6),
        encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(6)});
    setSecond.insert(setSecond.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1, encodeMfmsr(6),
        encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(6)});
    CHECK(prepare(first, setFirst));
    CHECK(prepare(second, setSecond));
    CHECK(first.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(second.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(first.getPPC64RTASServiceRegistry() !=
          second.getPPC64RTASServiceRegistry());
    writeWords(first.getMemory(), 0x500, terminalEchoHandler());
    writeWords(second.getMemory(), 0x500, terminalEchoHandler());
    CHECK(steps(first, setFirst.size()));
    CHECK(steps(second, setSecond.size()));
    CHECK(first.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(second.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(first.getPPC64XICSSnapshot().sources[1].priority == 0x20U);
    CHECK(second.getPPC64XICSSnapshot().sources[1].priority == 0x60U);
    CHECK(readBE32(first.getMemory(), 0x3000 + 24U) == 0U);
    CHECK(readBE32(second.getMemory(), 0x3000 + 24U) == 0U);
    CHECK(readBE32(first.getMemory(), 0x3000 + 20U) == 0x20U);
    CHECK(readBE32(second.getMemory(), 0x3000 + 20U) == 0x60U);
    CHECK(first.getPPC64XICSSnapshot().sources[1].ownerLPID == 0U);
    CHECK(second.getPPC64XICSSnapshot().sources[1].ownerLPID == 0U);
    CHECK(first.injectPPC64TerminalInputWithStatus({'1'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(second.injectPPC64TerminalInputWithStatus({'2'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(first.getPPC64TerminalPendingInputBytes() == 1U);
    CHECK(second.getPPC64TerminalPendingInputBytes() == 1U);
    CHECK(first.step());
    CHECK(second.step());
    CHECK(first.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(second.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(first, terminalEchoHandler().size()));
    CHECK(steps(second, terminalEchoHandler().size()));
    CHECK(first.readPPC64TerminalOutput() == std::vector<uint8_t>{'1'});
    CHECK(second.readPPC64TerminalOutput() == std::vector<uint8_t>{'2'});
}

void testPartitionTranslationAndLPIDOwnership() {
    std::puts("[phase18-J-partition-translation-and-LPID-ownership]");
    VirtualMachine vm(8U << 20, 1, "PPC64", platform());
    CHECK(prepare(vm, {encodeAddi(10, 10, 1)}));
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    std::string error;
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(memory != nullptr && manager->configurePartitionTable(
        *memory, kPartitionTable, PPC64_POWER9_PARTITION_TABLE_PATS, error));
    const auto partition7 = manager->createPartition(7, error);
    const auto partition8 = manager->createPartition(8, error);
    CHECK(partition7 != nullptr && partition8 != nullptr);
    CHECK(manager->registerPrivateBackingRange(
        partition7, kPartitionCodeBacking, 0x1000, error));
    CHECK(manager->registerPrivateBackingRange(
        partition7, kPartitionDataBacking, 0x1000, error));
    CHECK(manager->registerPrivateBackingRange(
        partition8, kCrossPartitionBacking, 0x1000, error));

    CHECK(memory != nullptr);
    if (memory == nullptr || partition7 == nullptr || partition8 == nullptr) return;
    writeBE64(*memory, kPartitionRoot, kPteValid | kPartitionL1 | 9ULL);
    writeBE64(*memory, kPartitionL1, kPteValid | kPartitionL2 | 9ULL);
    writeBE64(*memory, kPartitionL2 + 8U, kPteValid | kPartitionL3 | 9ULL);
    const uint64_t partitionPATE = kPartitionTable + 7U * 16U;
    writeBE64(*memory, partitionPATE,
              PPC64_PATE0_HR | partitionRadixRootWord(kPartitionRoot));
    writeBE64(*memory, partitionPATE + 8U, 0);
    mapPartitionGuestReal(*memory, kPartitionCodeGRA, kPartitionCodeBacking);
    mapPartitionGuestReal(*memory, kPartitionDataGRA, kPartitionDataBacking);
    mapPartitionGuestReal(*memory, kCrossPartitionGRA, kCrossPartitionBacking);

    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 7, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(kCrossOwnerSource,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 8, true,
        &error));
    CHECK(vm.getPPC64ISAPlugin()->activatePartitionContext(partition7, error));
    CHECK(vm.getPPC64XICSSnapshot().activeLPID == 7U);

    std::vector<uint32_t> guestSet;
    emitRTASCall(guestSet, static_cast<uint32_t>(kPartitionDataGRA),
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x33}, 1);
    writeWords(*memory, kPartitionCodeBacking, guestSet);
    vm.getPPC64ISAPlugin()->setPC(kPartitionCodeGRA);
    CHECK(steps(vm, guestSet.size()));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(*memory, kPartitionDataBacking + 24U) == 0U);
    auto snapshot = vm.getPPC64XICSSnapshot();
    const auto ownedBy7 = std::find_if(snapshot.sources.begin(),
        snapshot.sources.end(), [](const auto& source) {
            return source.sourceId == kSource;
        });
    CHECK(ownedBy7 != snapshot.sources.end());
    CHECK(ownedBy7 != snapshot.sources.end() &&
          ownedBy7->priority == 0x33U && ownedBy7->ownerLPID == 7U);

    std::vector<uint32_t> forgedOwner;
    emitRTASCall(forgedOwner, static_cast<uint32_t>(kPartitionDataGRA),
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kCrossOwnerSource, 0, 0x10}, 1);
    writeWords(*memory, kPartitionCodeBacking, forgedOwner);
    vm.getPPC64ISAPlugin()->setPC(kPartitionCodeGRA);
    CHECK(steps(vm, forgedOwner.size()));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(*memory, kPartitionDataBacking + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    snapshot = vm.getPPC64XICSSnapshot();
    const auto ownedBy8 = std::find_if(snapshot.sources.begin(),
        snapshot.sources.end(), [](const auto& source) {
            return source.sourceId == kCrossOwnerSource;
        });
    CHECK(ownedBy8 != snapshot.sources.end());
    CHECK(ownedBy8 != snapshot.sources.end() &&
          ownedBy8->priority == 0x44U && ownedBy8->ownerLPID == 8U);

    writeHostBlock(*memory, kCrossPartitionBacking,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kCrossOwnerSource, 0, 0x10}, 1);
    std::vector<uint32_t> crossPartition;
    emitRawCall(crossPartition, static_cast<uint32_t>(kCrossPartitionGRA));
    writeWords(*memory, kPartitionCodeBacking, crossPartition);
    vm.getPPC64ISAPlugin()->setPC(kPartitionCodeGRA);
    CHECK(steps(vm, crossPartition.size()));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    snapshot = vm.getPPC64XICSSnapshot();
    const auto afterCrossPartition = std::find_if(snapshot.sources.begin(),
        snapshot.sources.end(), [](const auto& source) {
            return source.sourceId == kCrossOwnerSource;
        });
    CHECK(afterCrossPartition != snapshot.sources.end());
    CHECK(afterCrossPartition != snapshot.sources.end() &&
          afterCrossPartition->priority == 0x44U &&
          afterCrossPartition->ownerLPID == 8U);

    std::vector<uint32_t> inaccessibleCrossPage;
    emitRawCall(inaccessibleCrossPage, 0x310ff8U);
    writeWords(*memory, kPartitionCodeBacking, inaccessibleCrossPage);
    vm.getPPC64ISAPlugin()->setPC(kPartitionCodeGRA);
    CHECK(steps(vm, inaccessibleCrossPage.size()));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_PARAMETER);
    snapshot = vm.getPPC64XICSSnapshot();
    const auto afterUnmappedPage = std::find_if(snapshot.sources.begin(),
        snapshot.sources.end(), [](const auto& source) {
            return source.sourceId == kSource;
        });
    CHECK(afterUnmappedPage != snapshot.sources.end());
    CHECK(afterUnmappedPage != snapshot.sources.end() &&
          afterUnmappedPage->priority == 0x33U &&
          afterUnmappedPage->ownerLPID == 7U);
    vm.getPPC64ISAPlugin()->deactivatePartitionContext();
    CHECK(vm.getPPC64XICSSnapshot().activeLPID == 0U);
}

void testTerminalGuestProgramAndLifecycle() {
    std::puts("[phase18-G-L-M-terminal-output-and-lifecycle]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform(true, true));
    CHECK(prepare(vm, {encodeAddi(10, 10, 1)}));
    const uint32_t source = vm.getPPC64PlatformConfiguration().
        virtualTerminalInterruptSourceId;
    const auto platformSnapshot = vm.getPPC64PlatformSnapshot();
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    const auto tokenAddress = fdt.valueOffsets.find("/rtas|ibm,set-xive");
    CHECK(tokenAddress != fdt.valueOffsets.end());
    const uint64_t fdtTokenAddress = platformSnapshot.memoryLayout.fdtBase +
        (tokenAddress == fdt.valueOffsets.end() ? 0U : tokenAddress->second);
    std::vector<uint32_t> code;
    emitRTASCallFromFDT(code, 0x3000, fdtTokenAddress,
                        {source, 0, 0x10}, 1);
    code.insert(code.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1,
        encodeMfmsr(6), encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)),
        encodeMtmsr(6)});
    const size_t interruptReadySteps = code.size();
    emitLoad32(code, 4, vm.getPPC64VirtualTerminalId());
    code.push_back(encodeAddi(5, 0, 4));
    emitLoad32(code, 7, 0x3200);
    code.push_back(encodeDS(58, 6, 7, 0)); // ld r6,0(r7)
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_PUT_TERM_CHAR));
    code.push_back(kSc1);
    installGuestCode(vm, code);
    CHECK(vm.getPPC64PlatformCapabilities().rtas);
    writeBE64(vm.getMemory(), 0x3200, 0x444f4e4500000000ULL);
    writeWords(vm.getMemory(), 0x500, terminalEchoHandler());
    CHECK(steps(vm, interruptReadySteps));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3000 + 24U) == 0U);
    auto snapshot = vm.getPPC64XICSSnapshot();
    CHECK(snapshot.sources.size() == 1U);
    CHECK(snapshot.sources[0].sourceId == source);
    CHECK(snapshot.sources[0].priority == 0x10U);
    CHECK(vm.getPPC64TerminalSnapshot().attachments.size() == 1U);

    CHECK(vm.injectPPC64TerminalInputWithStatus({'Q'}) ==
          PPC64TerminalResult::SUCCESS);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(vm.step());
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, terminalEchoHandler().size()));
    CHECK(vm.getPPC64TerminalPendingInputBytes() == 0U);
    CHECK(vm.getPPC64XICSSnapshot().presentations[0].outstanding.empty());
    const auto echo = vm.readPPC64TerminalOutput();
    CHECK(echo == std::vector<uint8_t>{'Q'});
    CHECK(vm.getPPC64ISAPlugin()->getNIP() ==
          kEntry + interruptReadySteps * 4U);
    const size_t outputCount = code.size() - interruptReadySteps;
    CHECK(steps(vm, outputCount));
    const auto output = vm.readPPC64TerminalOutput();
    CHECK(output == std::vector<uint8_t>({'Q', 'D', 'O', 'N', 'E'}));

    CHECK(vm.pause());
    CHECK(!vm.step());
    CHECK(vm.resume());
    CHECK(vm.reset());
    CHECK(vm.getPPC64RTASServiceRegistry() != nullptr);
    CHECK(vm.getPPC64PlatformCapabilities().rtas);
    snapshot = vm.getPPC64XICSSnapshot();
    CHECK(snapshot.sources.size() == 1U);
    CHECK(snapshot.sources[0].sourceId == source);
    CHECK(snapshot.sources[0].priority == PPC64_TERMINAL_XICS_PRIORITY);
    CHECK(snapshot.sources[0].enabled);
}

void testLevelEventsCheckpointAndLegacyProfile() {
    std::puts("[phase18-E-K-level-events-and-checkpoint]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    std::vector<uint32_t> code;
    const size_t setCount = emitRTASCall(code, 0x3000,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kLevelSource, 0, 0x35}, 1);
    const size_t offCount = emitRTASCall(code, 0x3040,
        PPC64_RTAS_TOKEN_IBM_INT_OFF, {kLevelSource}, 1);
    const size_t onCount = emitRTASCall(code, 0x3080,
        PPC64_RTAS_TOKEN_IBM_INT_ON, {kLevelSource}, 1);
    code.insert(code.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1, encodeMfmsr(6),
        encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(6)});
    CHECK(prepare(vm, code));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(kLevelSource,
        PPC64XICSSourceType::LEVEL, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    writeWords(vm.getMemory(), 0x500, xirrEoiHandler());
    CHECK(steps(vm, setCount + offCount));
    CHECK(vm.assertPPC64InterruptSource(kLevelSource, 0, &error));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(steps(vm, onCount + 6U));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(vm.step());
    CHECK(vm.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(vm, xirrEoiHandler().size()));
    auto xics = vm.getPPC64XICSSnapshot();
    CHECK(!xics.sources[0].acknowledged);
    CHECK(xics.sources[0].asserted);
    CHECK(xics.sources[0].pending);
    CHECK(xics.sources[0].presented);
    CHECK(vm.deassertPPC64InterruptSource(kLevelSource, 0, &error));
    xics = vm.getPPC64XICSSnapshot();
    CHECK(!xics.sources[0].asserted);
    CHECK(!xics.sources[0].pending);
    CHECK(!xics.sources[0].presented);

    // A separate edge source is captured masked with a saved priority and a
    // pending event, then restored into another VM with the same RTAS profile.
    VirtualMachine checkpointed(kRamSize, 1, "PPC64", platform());
    std::vector<uint32_t> checkpointCode;
    const size_t checkpointSet = emitRTASCall(checkpointCode, 0x3000,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {kSource, 0, 0x22}, 1);
    const size_t checkpointOff = emitRTASCall(checkpointCode, 0x3040,
        PPC64_RTAS_TOKEN_IBM_INT_OFF, {kSource}, 1);
    const size_t checkpointOn = emitRTASCall(checkpointCode, 0x3080,
        PPC64_RTAS_TOKEN_IBM_INT_ON, {kSource}, 1);
    checkpointCode.insert(checkpointCode.end(), {encodeAddi(4, 0, 0xff),
        encodeAddi(3, 0, PPC64_H_CPPR), kSc1, encodeMfmsr(6),
        encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)), encodeMtmsr(6)});
    CHECK(prepare(checkpointed, checkpointCode));
    CHECK(checkpointed.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true, &error));
    writeWords(checkpointed.getMemory(), 0x500, xirrEoiHandler());
    CHECK(steps(checkpointed, checkpointSet + checkpointOff));
    CHECK(checkpointed.assertPPC64InterruptSource(kSource, 0, &error));
    const VMStateSnapshot maskedPending = checkpointed.captureVMSnapshot();
    CHECK(maskedPending.ppc64Xics.sources[0].priority == 0xffU);
    CHECK(maskedPending.ppc64Xics.sources[0].savedPriority == 0x22U);
    CHECK(maskedPending.ppc64Xics.sources[0].savedEnabled);
    CHECK(maskedPending.ppc64Xics.sources[0].pending);
    CHECK(checkpointed.restoreVMSnapshot(maskedPending));
    CHECK(checkpointed.getPPC64XICSSnapshot().sources[0].priority == 0xffU);
    CHECK(checkpointed.getPPC64XICSSnapshot().sources[0].savedPriority == 0x22U);
    CHECK(checkpointed.getPPC64XICSSnapshot().sources[0].savedEnabled);

    VirtualMachine restored(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(restored, checkpointCode));
    writeWords(restored.getMemory(), 0x500, xirrEoiHandler());
    CHECK(restored.restoreVMSnapshot(maskedPending));
    CHECK(restored.getPPC64RTASServiceRegistry() != nullptr);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].savedPriority == 0x22U);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].savedEnabled);
    CHECK(steps(restored, checkpointOn + 6U));
    CHECK(restored.getPPC64XICSSnapshot().sources[0].presented);
    CHECK(restored.step());
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == 0x500U);
    CHECK(steps(restored, xirrEoiHandler().size()));
    CHECK(!restored.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!restored.getPPC64XICSSnapshot().sources[0].acknowledged);
    const VMStateSnapshot postEOI = restored.captureVMSnapshot();
    const uint64_t postEOINIP = restored.getPPC64ISAPlugin()->getNIP();
    CHECK(restored.restoreVMSnapshot(postEOI));
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == postEOINIP);
    CHECK(!restored.getPPC64XICSSnapshot().sources[0].pending);
    CHECK(!restored.getPPC64XICSSnapshot().sources[0].acknowledged);

    const auto checkpointPath = std::filesystem::temp_directory_path() /
        "guidexos-phase18-rtas.gxcp";
    std::string checkpointError;
    CHECK(checkpointed.pause());
    CHECK(VMCheckpointCodec::write(checkpointed, checkpointPath.string(),
                                   "phase18-rtas", &checkpointError));
    std::ifstream file(checkpointPath, std::ios::binary);
    std::array<uint8_t, 16> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    CHECK(file.gcount() == static_cast<std::streamsize>(header.size()));
    const uint32_t fileVersion = static_cast<uint32_t>(header[8]) |
        (static_cast<uint32_t>(header[9]) << 8) |
        (static_cast<uint32_t>(header[10]) << 16) |
        (static_cast<uint32_t>(header[11]) << 24);
    CHECK(fileVersion == 9U);
    file.close();
    VirtualMachine fileRestored(kRamSize, 1, "PPC64", platform());
    CHECK(prepare(fileRestored, checkpointCode));
    writeWords(fileRestored.getMemory(), 0x500, xirrEoiHandler());
    CHECK(VMCheckpointCodec::read(fileRestored, checkpointPath.string(),
                                  "phase18-rtas", &checkpointError));
    CHECK(fileRestored.getPPC64PlatformConfiguration().rtasEnabled);
    CHECK(fileRestored.getPPC64RTASServiceRegistry() != nullptr);
    CHECK(fileRestored.getPPC64XICSSnapshot().sources[0].priority == 0xffU);
    CHECK(fileRestored.getPPC64XICSSnapshot().sources[0].savedPriority == 0x22U);
    CHECK(fileRestored.getPPC64XICSSnapshot().sources[0].savedEnabled);
    std::error_code removeError;
    std::filesystem::remove(checkpointPath, removeError);

    VirtualMachine disabled(kRamSize, 1, "PPC64", platform(false));
    CHECK(prepare(disabled, {encodeAddi(10, 10, 1)}));
    CHECK(!disabled.isPPC64PlatformConfigured() ||
          !disabled.getPPC64PlatformCapabilities().rtas);
}

void testTerminalAndTimerCompatibility() {
    std::puts("[phase18-L-terminal-and-DEC-compatibility]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform(true, true));
    std::vector<uint32_t> code;
    const uint32_t source = PPC64_XICS_FIRST_SOURCE;
    const size_t setCount = emitRTASCall(code, 0x3000,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, {source, 0, 0x20}, 1);
    code.push_back(encodeAddi(7, 0, 0));
    code.push_back(encodeMtspr(7, static_cast<uint16_t>(kSprDEC)));
    emitLoad32(code, 4, vm.getPPC64VirtualTerminalId());
    code.push_back(encodeAddi(5, 0, 2));
    emitLoad32(code, 7, 0x3200);
    code.push_back(encodeDS(58, 6, 7, 0));
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_PUT_TERM_CHAR));
    code.push_back(kSc1);
    CHECK(prepare(vm, code));
    writeBE64(vm.getMemory(), 0x3200, 0x4f4b000000000000ULL);
    CHECK(steps(vm, setCount + 2U));
    CHECK(vm.getPPC64ISAPlugin()->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(vm.getPPC64InterruptSnapshot().decrementerPending);
    CHECK(steps(vm, code.size() - setCount - 2U));
    CHECK(vm.readPPC64TerminalOutput() ==
          std::vector<uint8_t>({'O', 'K'}));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x20U);
}

} // namespace

int main() {
    testFDTRegistryAndDiscovery();
    testAuthenticXiveAndPendingInterruptFlow();
    testMalformedBlocksAndErrorLayers();
    testProtectedMemoryAndCrossVMIsolation();
    testPartitionTranslationAndLPIDOwnership();
    testTerminalGuestProgramAndLifecycle();
    testLevelEventsCheckpointAndLegacyProfile();
    testTerminalAndTimerCompatibility();
    std::printf("PPC64 Phase 18: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
