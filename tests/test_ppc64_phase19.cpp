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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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
constexpr uint64_t kBlockBase = 0x3000;
constexpr uint64_t kCodeCapacity = 4096;
constexpr uint32_t kSource = PPC64_XICS_FIRST_SOURCE + 1U;
constexpr uint32_t kSc1 = ENCODING_SC_HCALL;
constexpr uint32_t kRfid = 0x4c000024U;
constexpr uint32_t kBctrl = (19U << 26) | (20U << 21) | (528U << 1) | 1U;
constexpr uint64_t kLPIDPartitionTable = 0x70000U;
constexpr uint64_t kLPIDAFirstTable = 0x80000U;
constexpr uint64_t kLPIDBFirstTable = 0x90000U;
constexpr uint64_t kLPIDPrivatePage = 0x100000U;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;
constexpr uint64_t kPteRWX = 0x7ULL;

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

uint32_t encodeMfspr(uint8_t rt, uint16_t spr) {
    return encodeXfx(rt, spr, 339U);
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

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t partitionRadixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t partitionLeafPte(uint64_t backing) {
    return kPteValid | kPteLeaf | kPteReference | kPteChange |
           (backing & 0x01fffffffffff000ULL) | kPteRWX;
}

bool installIdentityProcessRadix(VirtualMachine& vm,
                                 bool codeExecutable = true) {
    constexpr uint64_t processTable = 0x40000U;
    constexpr uint64_t root = 0x70000U;
    constexpr uint64_t level1 = 0x71000U;
    constexpr uint64_t level2 = 0x72000U;
    constexpr uint64_t level3 = 0x73000U;
    std::string reason;
    auto* cpu = vm.getPPC64ISAPlugin();
    if (cpu == nullptr || !cpu->configureRadixTranslation(
            PPC64RadixConfiguration{processTable, 5U, 0U, 0U, kRamSize},
            vm.getMemory(), reason)) {
        std::printf("Phase 19R3 process radix setup failed: %s\n",
                    reason.c_str());
        return false;
    }
    writeBE64(vm.getMemory(), processTable, partitionRadixRootWord(root));
    writeBE64(vm.getMemory(), root, kPteValid | level1 | 9ULL);
    writeBE64(vm.getMemory(), level1, kPteValid | level2 | 9ULL);
    writeBE64(vm.getMemory(), level2, kPteValid | level3 | 9ULL);
    for (uint64_t page = 0; page < kRamSize; page += 0x1000U) {
        uint64_t permissions = kPteRWX;
        if (!codeExecutable && page == kEntry) permissions &= ~1ULL;
        const uint64_t leaf = kPteValid | kPteLeaf | kPteReference |
            kPteChange | (page & 0x01fffffffffff000ULL) | permissions;
        writeBE64(vm.getMemory(), level3 + (((page >> 12) & 0x1ffULL) * 8U),
                  leaf);
    }
    return true;
}

void writeWords(IMemory& memory, uint64_t address,
                const std::vector<uint32_t>& words);

void writeRTASRequest(IMemory& memory, uint64_t address, uint32_t token,
                      uint32_t nargs, uint32_t nret,
                      const std::vector<uint32_t>& arguments) {
    std::vector<uint32_t> words{token, nargs, nret};
    words.insert(words.end(), arguments.begin(), arguments.end());
    writeWords(memory, address, words);
}

class CountingMMIODevice final : public IMemoryMappedDevice {
public:
    CountingMMIODevice(uint64_t base, size_t size) : base_(base), size_(size) {}
    uint64_t GetBaseAddress() const override { return base_; }
    size_t GetSize() const override { return size_; }
    bool Read(uint64_t, uint8_t* data, size_t size) const override {
        ++reads;
        std::fill(data, data + size, 0);
        return true;
    }
    bool Write(uint64_t, const uint8_t*, size_t) override {
        ++writes;
        return true;
    }
    mutable size_t reads = 0;
    size_t writes = 0;
private:
    uint64_t base_;
    size_t size_;
};

bool installIdentityPartition(
    VirtualMachine& vm, uint16_t lpid, uint64_t firstTable,
    std::shared_ptr<PPC64PartitionContext>& context, std::string& reason) {
    auto* manager = vm.getPPC64PartitionManager();
    if (manager == nullptr ||
        (context = manager->createPartition(lpid, reason)) == nullptr) {
        return false;
    }
    const uint64_t root = firstTable;
    const uint64_t l1 = firstTable + 0x1000U;
    const uint64_t l2 = firstTable + 0x2000U;
    const uint64_t l3 = firstTable + 0x3000U;
    writeBE64(vm.getMemory(), root, kPteValid | l1 | 9ULL);
    writeBE64(vm.getMemory(), l1, kPteValid | l2 | 9ULL);
    writeBE64(vm.getMemory(), l2, kPteValid | l3 | 9ULL);
    const uint64_t pateAddress = kLPIDPartitionTable +
                                 static_cast<uint64_t>(lpid) * 16ULL;
    writeBE64(vm.getMemory(), pateAddress,
        PPC64_PATE0_HR | partitionRadixRootWord(root));
    writeBE64(vm.getMemory(), pateAddress + 8U, 0);
    for (uint64_t guestReal = 0; guestReal < kRamSize; guestReal += 0x1000U) {
        const uint64_t pteAddress = l3 +
            (((guestReal >> 12) & 0x1ffULL) * 8ULL);
        writeBE64(vm.getMemory(), pteAddress, partitionLeafPte(guestReal));
    }
    reason.clear();
    return true;
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

PPC64PlatformConfiguration partitionPlatform() {
    auto result = platform();
    result.reservedRanges.push_back(
        {kLPIDPartitionTable, 0x10000U, "partition table", true});
    for (const auto& [base, name] : std::array<std::pair<uint64_t, const char*>, 8>{
             std::pair<uint64_t, const char*>{kLPIDAFirstTable, "LPID 7 radix root"},
             {kLPIDAFirstTable + 0x1000U, "LPID 7 radix l1"},
             {kLPIDAFirstTable + 0x2000U, "LPID 7 radix l2"},
             {kLPIDAFirstTable + 0x3000U, "LPID 7 radix l3"},
             {kLPIDBFirstTable, "LPID 8 radix root"},
             {kLPIDBFirstTable + 0x1000U, "LPID 8 radix l1"},
             {kLPIDBFirstTable + 0x2000U, "LPID 8 radix l2"},
             {kLPIDBFirstTable + 0x3000U, "LPID 8 radix l3"}}) {
        result.reservedRanges.push_back({base, 0x1000U, name, true});
    }
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

void appendTrampolinePointerCall(std::vector<uint32_t>& code,
                                 uint64_t trampoline,
                                 uint64_t blockAddress,
                                 GuestCall* site = nullptr) {
    emitLoad32(code, 3, static_cast<uint32_t>(blockAddress));
    emitLoad32(code, 10, static_cast<uint32_t>(trampoline));
    code.push_back(encodeMtspr(10, 9)); // mtctr r10
    const size_t branchIndex = code.size();
    code.push_back(kBctrl);
    if (site != nullptr) {
        site->branchIndex = branchIndex;
        site->returnAddress = kEntry + (branchIndex + 1U) * 4U;
        site->blockAddress = blockAddress;
    }
    code.push_back(encodeAddi(11, 11, 1)); // caller continuation
}

void appendCPPRCall(std::vector<uint32_t>& code, uint8_t cppr) {
    code.push_back(encodeAddi(4, 0, cppr));
    code.push_back(encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_CPPR)));
    code.push_back(kSc1);
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

void testLevelTriggeredXICS() {
    std::puts("[phase19R2-D-level-xics-through-guest-trampoline]");
    constexpr uint32_t levelSource = kSource;
    constexpr uint32_t prioritySource = kSource + 1U;
    constexpr uint32_t deassertedSource = kSource + 2U;
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));

    std::string error;
    CHECK(vm.configurePPC64InterruptSource(levelSource,
        PPC64XICSSourceType::LEVEL, 0x55, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(prioritySource,
        PPC64XICSSourceType::LEVEL, 0x44, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(deassertedSource,
        PPC64XICSSourceType::LEVEL, 0x24, PPC64_XICS_SERVER_ID, 0, true,
        &error));

    std::vector<uint32_t> code;
    std::vector<GuestCall> calls;
    appendCPPRCall(code, 0x20); // Higher numeric priority is eligible.
    const auto appendCall = [&](const std::string& token, uint64_t block,
                                const std::vector<uint32_t>& arguments,
                                uint32_t returns) {
        GuestCall call;
        appendTrampolineCall(code, fdt, layout.fdtBase, token, block,
                             arguments, returns, &call);
        calls.push_back(call);
    };
    appendCall("ibm,set-xive", 0x3300,
               {levelSource, PPC64_XICS_SERVER_ID, 0x30}, 1);
    appendCall("ibm,get-xive", 0x3340, {levelSource}, 3);
    appendCall("ibm,int-off", 0x3380, {levelSource}, 1);
    appendCall("ibm,int-on", 0x33c0, {levelSource}, 1);
    appendCPPRCall(code, 0x40);
    code.push_back(encodeMfmsr(6));
    code.push_back(encodeOri(6, 6, static_cast<uint16_t>(MSR_EE)));
    code.push_back(encodeMtmsr(6));
    const uint64_t firstContinuation = kEntry + code.size() * 4U;
    code.push_back(encodeAddi(13, 13, 1));

    appendCall("ibm,int-off", 0x3400, {levelSource}, 1);
    appendCall("ibm,int-off", 0x3440, {levelSource}, 1);
    appendCall("ibm,get-xive", 0x3480, {levelSource}, 3);
    appendCall("ibm,int-on", 0x34c0, {levelSource}, 1);
    appendCall("ibm,int-on", 0x3500, {levelSource}, 1);
    appendCall("ibm,get-xive", 0x3540, {levelSource}, 3);
    appendCall("ibm,int-off", 0x3580, {deassertedSource}, 1);
    appendCall("ibm,int-on", 0x35c0, {deassertedSource}, 1);
    appendCPPRCall(code, 0x20);
    appendCall("ibm,set-xive", 0x3600,
               {prioritySource, PPC64_XICS_SERVER_ID, 0x10}, 1);
    appendCPPRCall(code, 0x08);
    appendCall("ibm,set-xive", 0x3640,
               {PPC64_XICS_LAST_SOURCE + 1U, PPC64_XICS_SERVER_ID, 0x20}, 1);
    appendCall("ibm,set-xive", 0x3680,
               {prioritySource, 1U, 0x20}, 1);

    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);
    const auto sourceState = [&](uint32_t sourceId) {
        const auto snapshot = vm.getPPC64XICSSnapshot();
        const auto found = std::find_if(snapshot.sources.begin(),
            snapshot.sources.end(), [sourceId](const auto& source) {
                return source.sourceId == sourceId;
            });
        return found == snapshot.sources.end()
            ? PPC64XICSSourceSnapshot{} : *found;
    };
    const auto run = [&](size_t index) {
        return runCall(vm, calls[index], layout.rtasEntry, false);
    };

    CHECK(steps(vm, 3));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(run(0));
    CHECK(readBE32(vm.getMemory(), 0x3300U + 24U) == 0U);
    CHECK(run(1));
    CHECK(readBE32(vm.getMemory(), 0x3340U + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3340U + 20U) == PPC64_XICS_SERVER_ID);
    CHECK(readBE32(vm.getMemory(), 0x3340U + 24U) == 0x30U);
    CHECK(run(2));
    CHECK(readBE32(vm.getMemory(), 0x3380U + 16U) == 0U);
    CHECK(vm.assertPPC64InterruptSource(levelSource, 0, &error));
    auto level = sourceState(levelSource);
    CHECK(level.asserted && level.pending && !level.enabled && !level.presented);

    // C6: checkpoint the asserted but masked source, restore it, then continue
    // through the guest int-on call and interrupt acknowledgement path.
    const VMStateSnapshot pendingMaskedCheckpoint = vm.captureVMSnapshot();
    CHECK(vm.restoreVMSnapshot(pendingMaskedCheckpoint));
    cpu = vm.getPPC64ISAPlugin();
    level = sourceState(levelSource);
    CHECK(level.asserted && level.pending && !level.enabled && !level.presented);

    const auto c6Path = std::filesystem::temp_directory_path() /
        ("guidexos-p19r3-C6-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".gxcp");
    CHECK(!std::filesystem::exists(c6Path));
    CHECK(vm.pause());
    std::string c6Error;
    CHECK(VMCheckpointCodec::write(vm, c6Path.string(), "p19r3-C6", &c6Error));
    CHECK(vm.resume());

    VirtualMachine c6Restored(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(c6Restored));
    CHECK(VMCheckpointCodec::read(c6Restored, c6Path.string(),
                                  "p19r3-C6", &c6Error));
    auto* c6CPU = c6Restored.getPPC64ISAPlugin();
    const auto c6Source = [&]() {
        const auto snapshot = c6Restored.getPPC64XICSSnapshot();
        const auto found = std::find_if(snapshot.sources.begin(),
            snapshot.sources.end(), [levelSource](const auto& source) {
                return source.sourceId == levelSource;
            });
        return found == snapshot.sources.end()
            ? PPC64XICSSourceSnapshot{} : *found;
    };
    CHECK(c6Source().asserted && c6Source().pending &&
          !c6Source().enabled && !c6Source().presented);
    CHECK(c6Restored.validatePPC64RTASTrampoline());
    CHECK(c6Restored.resume());
    CHECK(runCall(c6Restored, calls[3], layout.rtasEntry, false));
    CHECK(readBE32(c6Restored.getMemory(), 0x33c0U + 16U) == 0U);
    CHECK(steps(c6Restored, 6U)); // CPPR, then guest enables EE.
    CHECK(c6CPU->getNIP() == firstContinuation);
    CHECK(c6Restored.step());
    CHECK(c6CPU->getNIP() == 0x500U);
    CHECK(c6CPU->getSRR0() == firstContinuation);
    const std::vector<uint32_t> c6Handler{
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_XIRR)), kSc1,
        encodeAddi(8, 4, 0), encodeAddi(4, 8, 0),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_EOI)), kSc1, kRfid};
    writeWords(c6Restored.getMemory(), 0x500U, c6Handler);
    CHECK(steps(c6Restored, 6U)); // H_XIRR acknowledges; H_EOI completes.
    CHECK(c6CPU->readGPR(4) == ((0x40U << 24) | levelSource));
    CHECK(c6Source().asserted && c6Source().pending &&
          c6Source().presented && !c6Source().acknowledged);
    CHECK(c6Restored.deassertPPC64InterruptSource(levelSource, 0, &error));
    CHECK(c6Restored.step()); // guest rfid returns with no source to reassert.
    CHECK(c6CPU->getNIP() == firstContinuation);
    std::error_code c6CleanupError;
    CHECK(std::filesystem::remove(c6Path, c6CleanupError));

    CHECK(run(3));
    level = sourceState(levelSource);
    CHECK(readBE32(vm.getMemory(), 0x33c0U + 16U) == 0U);
    CHECK(level.asserted && level.pending && level.enabled && !level.presented);
    CHECK(steps(vm, 3)); // Raise CPPR so priority 0x30 becomes eligible.
    level = sourceState(levelSource);
    CHECK(level.pending && level.presented);
    CHECK(steps(vm, 3)); // Guest enables EE; next boundary enters vector 0x500.
    CHECK(cpu->getNIP() == firstContinuation);

    const std::vector<uint32_t> levelHandler{
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_XIRR)), kSc1,
        encodeAddi(8, 4, 0), encodeAddi(4, 8, 0),
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_EOI)), kSc1, kRfid};
    writeWords(vm.getMemory(), 0x500, levelHandler);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == 0x500U);
    CHECK(cpu->getSRR0() == firstContinuation);
    CHECK(steps(vm, 2)); // H_XIRR acknowledges the first presentation.
    CHECK(cpu->readGPR(4) == ((0x40U << 24) | levelSource));
    level = sourceState(levelSource);
    CHECK(level.acknowledged && !level.pending && !level.presented);
    CHECK(steps(vm, 4)); // H_EOI completes it while the level remains asserted.
    level = sourceState(levelSource);
    CHECK(level.asserted && level.pending && level.presented && !level.acknowledged);
    CHECK(vm.step()); // rfid returns; the reasserted source enters the vector again.
    CHECK(cpu->getNIP() == firstContinuation);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == 0x500U);
    CHECK(cpu->getSRR0() == firstContinuation);
    CHECK(steps(vm, 2));
    CHECK(cpu->readGPR(4) == ((0x40U << 24) | levelSource));
    level = sourceState(levelSource);
    CHECK(level.acknowledged && !level.pending && !level.presented);
    CHECK(vm.deassertPPC64InterruptSource(levelSource, 0, &error));
    CHECK(!sourceState(levelSource).asserted);
    CHECK(steps(vm, 5)); // EOI after deassertion and return via rfid.
    level = sourceState(levelSource);
    CHECK(!level.asserted && !level.pending && !level.presented &&
          !level.acknowledged);
    CHECK(cpu->getNIP() == firstContinuation);
    CHECK(vm.step()); // The guest continuation runs; no spurious vector follows.
    CHECK(cpu->getNIP() == firstContinuation + 4U);
    CHECK(cpu->readGPR(13) == 1U);
    CHECK(!sourceState(levelSource).pending && !sourceState(levelSource).presented);

    // Exercise saved priority over repeated disable/enable while EE is clear.
    cpu->setMSR(MSR_ME | MSR_RI);
    CHECK(run(4));
    CHECK(run(5));
    CHECK(sourceState(levelSource).priority == 0xffU);
    CHECK(sourceState(levelSource).savedPriority == 0x30U);
    CHECK(run(6));
    CHECK(readBE32(vm.getMemory(), 0x3480U + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3480U + 24U) == 0xffU);
    CHECK(run(7));
    CHECK(run(8));
    CHECK(run(9));
    CHECK(readBE32(vm.getMemory(), 0x3540U + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x3540U + 24U) == 0x30U);
    CHECK(sourceState(levelSource).enabled);

    // A level removed before int-on must not become pending again.
    CHECK(run(10));
    CHECK(vm.assertPPC64InterruptSource(deassertedSource, 0, &error));
    CHECK(sourceState(deassertedSource).pending &&
          !sourceState(deassertedSource).presented);
    CHECK(vm.deassertPPC64InterruptSource(deassertedSource, 0, &error));
    CHECK(run(11));
    CHECK(!sourceState(deassertedSource).pending &&
          !sourceState(deassertedSource).presented);

    // A priority update while pending is immediately arbitrated against CPPR.
    CHECK(steps(vm, 3)); // CPPR 0x20 masks the original priority 0x44.
    CHECK(vm.assertPPC64InterruptSource(prioritySource, 0, &error));
    CHECK(sourceState(prioritySource).pending &&
          !sourceState(prioritySource).presented);
    CHECK(run(12)); // set-xive changes pending priority to 0x10.
    CHECK(readBE32(vm.getMemory(), 0x3600U + 24U) == 0U);
    CHECK(sourceState(prioritySource).priority == 0x10U);
    CHECK(sourceState(prioritySource).pending &&
          sourceState(prioritySource).presented);
    CHECK(steps(vm, 3)); // CPPR 0x08 masks it again.
    CHECK(sourceState(prioritySource).pending &&
          !sourceState(prioritySource).presented);
    CHECK(vm.deassertPPC64InterruptSource(prioritySource, 0, &error));
    CHECK(!sourceState(prioritySource).pending &&
          !sourceState(prioritySource).presented);
    const auto beforeInvalid = sourceState(prioritySource);

    // Invalid RTAS source/server requests return service errors without state changes.
    CHECK(run(13));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3640U + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(run(14));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3680U + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(sourceState(prioritySource).ownerLPID == beforeInvalid.ownerLPID);
    CHECK(sourceState(prioritySource).priority == beforeInvalid.priority);
    CHECK(sourceState(prioritySource).pending == beforeInvalid.pending);
}

void testNonRootLPIDRTASIsolation() {
    std::puts("[phase19R2-I-nonroot-lpid-rtas-ownership]");
    constexpr uint16_t lpidA = 7;
    constexpr uint16_t lpidB = 8;
    constexpr uint32_t sourceA = kSource;
    constexpr uint32_t sourceB = kSource + 1U;
    constexpr uint64_t lpidBPrivateBacking = kLPIDPrivatePage + 0x10000U;
    VirtualMachine vm(kRamSize, 1, "PPC64", partitionPlatform());
    CHECK(prepareCapacity(vm));
    std::string error;
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(manager != nullptr);
    CHECK(manager->configurePartitionTable(vm.getMemory(),
        kLPIDPartitionTable, 4, error));
    std::shared_ptr<PPC64PartitionContext> contextA;
    std::shared_ptr<PPC64PartitionContext> contextB;
    CHECK(installIdentityPartition(vm, lpidA, kLPIDAFirstTable,
                                   contextA, error));
    CHECK(installIdentityPartition(vm, lpidB, kLPIDBFirstTable,
                                   contextB, error));
    CHECK(manager->registerPrivateBackingRange(contextA, kLPIDPrivatePage,
                                                0x1000U, error));
    const uint64_t bL3 = kLPIDBFirstTable + 0x3000U;
    const auto partitionPTE = [bL3](uint64_t guestReal) {
        return bL3 + (((guestReal >> 12) & 0x1ffULL) * 8U);
    };
    // The same guest-real block maps to distinct backing for LPID 8. A second
    // LPID-8 mapping deliberately aliases LPID 7's private backing and must be
    // rejected by the partition ownership check.
    writeBE64(vm.getMemory(), partitionPTE(kLPIDPrivatePage),
              partitionLeafPte(lpidBPrivateBacking));
    writeBE64(vm.getMemory(), partitionPTE(kLPIDPrivatePage + 0x1000U),
              partitionLeafPte(kLPIDPrivatePage));
    CHECK(manager->registerPrivateBackingRange(contextB,
        lpidBPrivateBacking, 0x1000U, error));

    CHECK(vm.configurePPC64InterruptSource(sourceA,
        PPC64XICSSourceType::LEVEL, 0x40, PPC64_XICS_SERVER_ID, lpidA, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(sourceB,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, lpidB, true,
        &error));
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    const uint64_t fdtBase = vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase;
    const uint64_t trampoline =
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry;

    std::vector<uint32_t> code;
    appendCPPRCall(code, 0x60);
    std::vector<GuestCall> calls(6);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3800,
        {sourceA, PPC64_XICS_SERVER_ID, 0x30}, 1, &calls[0]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3840,
        {sourceA, PPC64_XICS_SERVER_ID, 0x10}, 1, &calls[1]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,set-xive", 0x3880,
        {sourceB, PPC64_XICS_SERVER_ID, 0x22}, 1, &calls[2]);
    appendTrampolinePointerCall(code, trampoline, kLPIDPrivatePage, &calls[3]);
    appendTrampolinePointerCall(code, trampoline,
        kLPIDPrivatePage + 0x1000U, &calls[4]);
    appendTrampolineCall(code, fdt, fdtBase, "ibm,get-xive", 0x38c0,
        {sourceA}, 3, &calls[5]);
    CHECK(installProgram(vm, code));

    // The host stages a valid RTAS request in LPID A's private backing page.
    // LPID B's identical guest-real mapping must fail before reading it.
    writeWords(vm.getMemory(), kLPIDPrivatePage,
        {PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3, 1, sourceA,
         PPC64_XICS_SERVER_ID, 0x10, 0x12345678U});
    writeWords(vm.getMemory(), lpidBPrivateBacking,
        {PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3, 1, sourceB,
         PPC64_XICS_SERVER_ID, 0x12, 0x87654321U});
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->writeGPR(14, 0x1414141414141414ULL);
    cpu->setMSR(MSR_ME | MSR_RI);
    CHECK(cpu->activatePartitionContext(contextA, error));
    CHECK(cpu->getActiveLPID() == lpidA);
    CHECK(steps(vm, 3)); // LPID A raises CPPR using a genuine guest H_CPPR.
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(runCall(vm, calls[0], trampoline, false));
    CHECK(cpu->getActiveLPID() == lpidA);
    CHECK(readBE32(vm.getMemory(), 0x3800U + 24U) == 0U);

    auto stateA = vm.getPPC64XICSSnapshot();
    const auto findSource = [](const PPC64XICSSnapshot& snapshot,
                               uint32_t sourceId) {
        const auto found = std::find_if(snapshot.sources.begin(),
            snapshot.sources.end(), [sourceId](const auto& source) {
                return source.sourceId == sourceId;
            });
        return found == snapshot.sources.end()
            ? PPC64XICSSourceSnapshot{} : *found;
    };
    auto sourceStateA = findSource(stateA, sourceA);
    CHECK(sourceStateA.allocated && sourceStateA.ownerLPID == lpidA);
    CHECK(sourceStateA.priority == 0x30U && sourceStateA.savedPriority == 0x30U);
    CHECK(vm.assertPPC64InterruptSource(sourceA, lpidA, &error));
    sourceStateA = findSource(vm.getPPC64XICSSnapshot(), sourceA);
    CHECK(sourceStateA.pending && sourceStateA.presented && sourceStateA.asserted);

    const uint64_t nipBeforeB = cpu->getNIP();
    const uint64_t msrBeforeB = cpu->getMSR();
    const uint64_t lrBeforeB = cpu->getLR();
    const uint64_t gpr14BeforeB = cpu->readGPR(14);
    const uint32_t crBeforeB = cpu->getCR();
    const uint64_t xerBeforeB = cpu->getXER();
    CHECK(cpu->activatePartitionContext(contextB, error));
    CHECK(cpu->getActiveLPID() == lpidB);
    CHECK(cpu->getNIP() == nipBeforeB && cpu->getMSR() == msrBeforeB);
    CHECK(cpu->getLR() == lrBeforeB && cpu->readGPR(14) == gpr14BeforeB);
    CHECK(cpu->getCR() == crBeforeB && cpu->getXER() == xerBeforeB);
    const auto switchedSnapshot = vm.getPPC64XICSSnapshot();
    CHECK(switchedSnapshot.presentations.size() == 3U);
    CHECK(switchedSnapshot.presentations.back().ownerLPID == lpidB);
    CHECK(switchedSnapshot.presentations.back().presentedSourceId == 0U);
    CHECK(runCall(vm, calls[1], trampoline, false));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3840U + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    sourceStateA = findSource(vm.getPPC64XICSSnapshot(), sourceA);
    CHECK(sourceStateA.ownerLPID == lpidA && sourceStateA.priority == 0x30U);
    CHECK(sourceStateA.pending && sourceStateA.asserted);

    CHECK(runCall(vm, calls[2], trampoline, false));
    CHECK(readBE32(vm.getMemory(), 0x3880U + 24U) == 0U);
    const auto sourceStateB = findSource(vm.getPPC64XICSSnapshot(), sourceB);
    CHECK(sourceStateB.allocated && sourceStateB.ownerLPID == lpidB);
    CHECK(sourceStateB.priority == 0x22U && sourceStateB.enabled);

    CHECK(runCall(vm, calls[3], trampoline, false));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), lpidBPrivateBacking + 24U) == 0U);
    CHECK(findSource(vm.getPPC64XICSSnapshot(), sourceB).priority == 0x12U);

    const uint64_t nipBeforeDeniedPointer = cpu->getNIP();
    CHECK(runCall(vm, calls[4], trampoline, false));
    CHECK(cpu->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(cpu->getNIP() == calls[4].returnAddress + 4U);
    CHECK(readBE32(vm.getMemory(), kLPIDPrivatePage + 24U) == 0x12345678U);
    sourceStateA = findSource(vm.getPPC64XICSSnapshot(), sourceA);
    CHECK(sourceStateA.priority == 0x30U && sourceStateA.pending);
    CHECK(nipBeforeDeniedPointer < cpu->getNIP());

    const uint64_t nipBeforeA = cpu->getNIP();
    const uint64_t msrBeforeA = cpu->getMSR();
    const uint64_t lrBeforeA = cpu->getLR();
    const uint64_t gpr14BeforeA = cpu->readGPR(14);
    const uint32_t crBeforeA = cpu->getCR();
    const uint64_t xerBeforeA = cpu->getXER();
    CHECK(cpu->activatePartitionContext(contextA, error));
    CHECK(cpu->getActiveLPID() == lpidA);
    CHECK(cpu->getNIP() == nipBeforeA && cpu->getMSR() == msrBeforeA);
    CHECK(cpu->getLR() == lrBeforeA && cpu->readGPR(14) == gpr14BeforeA);
    CHECK(cpu->getCR() == crBeforeA && cpu->getXER() == xerBeforeA);
    CHECK(runCall(vm, calls[5], trampoline, false));
    CHECK(readBE32(vm.getMemory(), 0x38c0U + 16U) == 0U);
    CHECK(readBE32(vm.getMemory(), 0x38c0U + 24U) == 0x30U);
    CHECK(findSource(vm.getPPC64XICSSnapshot(), sourceB).priority == 0x12U);
    CHECK(vm.deassertPPC64InterruptSource(sourceA, lpidA, &error));
    sourceStateA = findSource(vm.getPPC64XICSSnapshot(), sourceA);
    CHECK(!sourceStateA.pending && !sourceStateA.presented &&
          !sourceStateA.asserted);
    // C8: restore a valid non-root LPID with its active binding and XICS
    // ownership intact; the VM continues to use a single explicit CPU state.
    const VMStateSnapshot lpidCheckpoint = vm.captureVMSnapshot();
    CHECK(vm.restoreVMSnapshot(lpidCheckpoint));
    auto* restoredCPU = vm.getPPC64ISAPlugin();
    CHECK(restoredCPU != nullptr);
    CHECK(restoredCPU != nullptr && restoredCPU->getActiveLPID() == lpidA);
    CHECK(restoredCPU != nullptr && restoredCPU->hasActivePartitionContext());
    CHECK(findSource(vm.getPPC64XICSSnapshot(), sourceA).ownerLPID == lpidA);
    CHECK(findSource(vm.getPPC64XICSSnapshot(), sourceB).ownerLPID == lpidB);
    CHECK(readBE32(vm.getMemory(), kLPIDPrivatePage + 24U) == 0x12345678U);
    CHECK(vm.validatePPC64RTASTrampoline());

    const auto c8Path = std::filesystem::temp_directory_path() /
        ("guidexos-p19r3-C8-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".gxcp");
    CHECK(!std::filesystem::exists(c8Path));
    CHECK(vm.pause());
    CHECK(VMCheckpointCodec::write(vm, c8Path.string(), "p19r3-C8", &error));
    CHECK(vm.resume());
    VirtualMachine c8Restored(kRamSize, 1, "PPC64", partitionPlatform());
    CHECK(prepareCapacity(c8Restored));
    CHECK(VMCheckpointCodec::read(c8Restored, c8Path.string(),
                                  "p19r3-C8", &error));
    auto* c8CPU = c8Restored.getPPC64ISAPlugin();
    CHECK(c8CPU->getActiveLPID() == lpidA);
    CHECK(c8CPU->hasActivePartitionContext());
    CHECK(c8Restored.validatePPC64RTASTrampoline());
    const auto c8Xics = c8Restored.getPPC64XICSSnapshot();
    const auto c8SourceA = std::find_if(c8Xics.sources.begin(),
        c8Xics.sources.end(), [sourceA](const auto& source) {
            return source.sourceId == sourceA;
        });
    const auto c8SourceB = std::find_if(c8Xics.sources.begin(),
        c8Xics.sources.end(), [sourceB](const auto& source) {
            return source.sourceId == sourceB;
        });
    CHECK(c8SourceA != c8Xics.sources.end() &&
          c8SourceA->ownerLPID == lpidA);
    CHECK(c8SourceB != c8Xics.sources.end() &&
          c8SourceB->ownerLPID == lpidB);
    CHECK(readBE32(c8Restored.getMemory(),
                   kLPIDPrivatePage + 24U) == 0x12345678U);
    std::error_code c8CleanupError;
    CHECK(std::filesystem::remove(c8Path, c8CleanupError));
}

void testR3ExplicitLPIDCPUContextSwitch() {
    std::puts("[phase19R3-I-host-mediated-A-B-A-register-restore]");
    constexpr uint16_t lpidA = 7;
    constexpr uint16_t lpidB = 8;
    constexpr uint64_t privateB = kLPIDPrivatePage + 0x10000U;
    VirtualMachine vm(kRamSize, 1, "PPC64", partitionPlatform());
    CHECK(prepareCapacity(vm));
    std::string error;
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(manager != nullptr);
    CHECK(manager->configurePartitionTable(vm.getMemory(),
        kLPIDPartitionTable, 4, error));
    std::shared_ptr<PPC64PartitionContext> contextA;
    std::shared_ptr<PPC64PartitionContext> contextB;
    CHECK(installIdentityPartition(vm, lpidA, kLPIDAFirstTable,
                                   contextA, error));
    CHECK(installIdentityPartition(vm, lpidB, kLPIDBFirstTable,
                                   contextB, error));
    const uint64_t bL3 = kLPIDBFirstTable + 0x3000U;
    const uint64_t privatePTE = bL3 +
        (((kLPIDPrivatePage >> 12) & 0x1ffULL) * 8U);
    writeBE64(vm.getMemory(), privatePTE, partitionLeafPte(privateB));
    CHECK(manager->registerPrivateBackingRange(contextA, kLPIDPrivatePage,
                                                0x1000U, error));
    CHECK(manager->registerPrivateBackingRange(contextB, privateB,
                                                0x1000U, error));
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, lpidA, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(kSource + 1U,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, lpidB, true,
        &error));

    const std::vector<uint32_t> code{
        encodeD(36, 3, 4, 0), // stw r3,0(r4) through the active LPID mapping
        encodeAddi(10, 10, 1),
        encodeAddi(11, 11, 1),
        encodeAddi(12, 12, 1),
        encodeAddi(13, 13, 1)};
    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);

    CHECK(cpu->activatePartitionContext(contextA, error));
    cpu->setPC(kEntry);
    cpu->writeGPR(3, 0x71U);
    cpu->writeGPR(4, kLPIDPrivatePage);
    cpu->writeGPR(10, 10U);
    cpu->writeGPR(15, 0xaaaaaaaaU);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 4U);
    CHECK(readBE32(vm.getMemory(), kLPIDPrivatePage) == 0x71U);
    const std::vector<uint8_t> savedA = cpu->serialize_state();

    CHECK(cpu->activatePartitionContext(contextB, error));
    cpu->setPC(kEntry);
    cpu->writeGPR(3, 0x82U);
    cpu->writeGPR(4, kLPIDPrivatePage);
    cpu->writeGPR(10, 20U);
    cpu->writeGPR(15, 0xbbbbbbbbU);
    const std::vector<uint8_t> initialB = cpu->serialize_state();
    CHECK(cpu->activatePartitionContext(contextA, error));
    CHECK(cpu->deserialize_state(savedA));
    CHECK(cpu->getActiveLPID() == lpidA);
    CHECK(cpu->getNIP() == kEntry + 4U);
    CHECK(cpu->readGPR(3) == 0x71U && cpu->readGPR(10) == 10U);
    CHECK(cpu->readGPR(15) == 0xaaaaaaaaU);
    CHECK(steps(vm, 4U));
    CHECK(cpu->getNIP() == kEntry + 20U);

    CHECK(cpu->activatePartitionContext(contextB, error));
    CHECK(cpu->deserialize_state(initialB));
    CHECK(cpu->getActiveLPID() == lpidB);
    CHECK(cpu->getNIP() == kEntry);
    CHECK(cpu->readGPR(3) == 0x82U && cpu->readGPR(10) == 20U);
    CHECK(cpu->readGPR(15) == 0xbbbbbbbbU);
    CHECK(steps(vm, code.size()));
    CHECK(cpu->getNIP() == kEntry + code.size() * 4U);
    CHECK(readBE32(vm.getMemory(), privateB) == 0x82U);
    CHECK(readBE32(vm.getMemory(), kLPIDPrivatePage) == 0x71U);
    const auto xics = vm.getPPC64XICSSnapshot();
    const auto stateA = std::find_if(xics.sources.begin(), xics.sources.end(),
        [](const auto& source) { return source.sourceId == kSource; });
    const auto stateB = std::find_if(xics.sources.begin(), xics.sources.end(),
        [](const auto& source) { return source.sourceId == kSource + 1U; });
    CHECK(stateA != xics.sources.end() && stateA->ownerLPID == lpidA);
    CHECK(stateB != xics.sources.end() && stateB->ownerLPID == lpidB);
    CHECK(vm.validatePPC64RTASTrampoline());
}

void testR3LPIDCannotBeForgedThroughPIDR() {
    std::puts("[phase19R3-I-PIDR-does-not-forge-LPID]");
    constexpr uint16_t lpidA = 7;
    constexpr uint16_t lpidB = 8;
    VirtualMachine vm(kRamSize, 1, "PPC64", partitionPlatform());
    CHECK(prepareCapacity(vm));
    std::string error;
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(manager != nullptr);
    CHECK(manager->configurePartitionTable(vm.getMemory(),
        kLPIDPartitionTable, 4, error));
    std::shared_ptr<PPC64PartitionContext> contextA;
    std::shared_ptr<PPC64PartitionContext> contextB;
    CHECK(installIdentityPartition(vm, lpidA, kLPIDAFirstTable,
                                   contextA, error));
    CHECK(installIdentityPartition(vm, lpidB, kLPIDBFirstTable,
                                   contextB, error));
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, lpidA, true,
        &error));
    CHECK(vm.configurePPC64InterruptSource(kSource + 1U,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, lpidB, true,
        &error));

    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    std::vector<uint32_t> code;
    emitLoad32(code, 5, lpidA); // guest PIDR value, not the hypervisor LPID
    code.push_back(encodeMtspr(5, SPR_PIDR));
    GuestCall call;
    appendTrampolineCall(code, fdt,
        vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        "ibm,set-xive", 0x3980U,
        {kSource, PPC64_XICS_SERVER_ID, 0x22U}, 1U, &call);
    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);
    CHECK(cpu->activatePartitionContext(contextB, error));
    CHECK(cpu->getActiveLPID() == lpidB);
    CHECK(runCall(vm, call,
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
    CHECK(cpu->getPIDR() == lpidA);
    CHECK(cpu->getActiveLPID() == lpidB);
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), 0x3980U + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    const auto xics = vm.getPPC64XICSSnapshot();
    const auto sourceA = std::find_if(xics.sources.begin(), xics.sources.end(),
        [](const auto& source) { return source.sourceId == kSource; });
    const auto sourceB = std::find_if(xics.sources.begin(), xics.sources.end(),
        [](const auto& source) { return source.sourceId == kSource + 1U; });
    CHECK(sourceA != xics.sources.end() && sourceA->ownerLPID == lpidA);
    CHECK(sourceA != xics.sources.end() && sourceA->priority == 0x40U);
    CHECK(sourceB != xics.sources.end() && sourceB->ownerLPID == lpidB);
    CHECK(sourceB != xics.sources.end() && sourceB->priority == 0x44U);
}

void testDirectHRTASCompatibility() {
    std::puts("[phase19-L-direct-h-rtas-compatibility]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    std::string error;
    std::shared_ptr<PPC64PartitionContext> unauthorizedContext;
    CHECK(vm.getPPC64PartitionManager()->configurePartitionTable(
        vm.getMemory(), kLPIDPartitionTable, 4, error));
    CHECK(installIdentityPartition(vm, 7U, kLPIDAFirstTable,
                                   unauthorizedContext, error));
    CHECK(vm.configurePPC64InterruptSource(
        kSource, PPC64XICSSourceType::EDGE, 0x44,
        PPC64_XICS_SERVER_ID, 0, true, &error));
    CHECK(vm.configurePPC64InterruptSource(
        kSource + 1U, PPC64XICSSourceType::EDGE, 0x55,
        PPC64_XICS_SERVER_ID, 7U, true, &error));
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

    constexpr uint32_t kUnauthorizedSourceBlock = 0x3240U;
    writeBlock(kUnauthorizedSourceBlock,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
        {kSource + 1U, PPC64_XICS_SERVER_ID, 0x11U});
    CHECK(directCall(kUnauthorizedSourceBlock));
    CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(vm.getMemory(), kUnauthorizedSourceBlock + 24U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          beforeBadToken.sources[0].priority);
    CHECK(vm.getPPC64XICSSnapshot().sources[1].priority == 0x55U);

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

    const uint64_t protectedResponseRequest = protectedEntry - 24U;
    writeBlock(static_cast<uint32_t>(protectedResponseRequest),
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
        {kSource, 0U, 0x11U});
    CHECK(directCall(static_cast<uint32_t>(protectedResponseRequest)));
    CHECK(cpu->readGPR(3) == PPC64_H_PARAMETER);
    CHECK(vm.getPPC64XICSSnapshot().sources[0].priority ==
          beforeBadToken.sources[0].priority);
    CHECK(vm.validatePPC64RTASTrampoline());
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

    VMStateSnapshot corruptOwner = vm.captureVMSnapshot();
    CHECK(!corruptOwner.ppc64Xics.sources.empty());
    if (!corruptOwner.ppc64Xics.sources.empty()) {
        corruptOwner.ppc64Xics.sources[0].ownerLPID = 7U;
    }
    CHECK(!restored.restoreVMSnapshot(corruptOwner));
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == restoredNIP);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].priority == 0x33U);

    VMStateSnapshot corruptRTASIdentity = vm.captureVMSnapshot();
    corruptRTASIdentity.ppc64Platform.rtasServiceRegistryVersion = 99U;
    CHECK(!restored.restoreVMSnapshot(corruptRTASIdentity));
    CHECK(restored.getPPC64ISAPlugin()->getNIP() == restoredNIP);

    VMStateSnapshot corruptRegisters = vm.captureVMSnapshot();
    CHECK(!corruptRegisters.cpus.empty());
    if (!corruptRegisters.cpus.empty() &&
        !corruptRegisters.cpus[0].isaState.empty()) {
        corruptRegisters.cpus[0].isaState[0] ^= 0xffU;
    }
    CHECK(!restored.restoreVMSnapshot(corruptRegisters));
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

void testR3ExecutionModesAndPrivilege() {
    std::puts("[phase19R3-G-modes-and-privilege]");
    const auto runMode = [](const char* name, uint64_t initialMSR,
                            uint64_t entryTranslation, bool transition) {
        std::printf("[phase19R3-mode-%s]\n", name);
        VirtualMachine vm(kRamSize, 1, "PPC64", platform());
        CHECK(prepareCapacity(vm));
        auto* cpu = vm.getPPC64ISAPlugin();
        if (entryTranslation != 0) CHECK(installIdentityProcessRadix(vm));
        std::string error;
        CHECK(vm.configurePPC64InterruptSource(kSource,
            PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 0, true,
            &error));
        IndependentFDT fdt;
        CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
        std::vector<uint32_t> code;
        if (transition) {
            code.push_back(encodeMfmsr(6));
            code.push_back(encodeD(28, 6, 6,
                                   static_cast<int16_t>(0xffcfU)));
            code.push_back(encodeMtmsr(6));
            code.push_back(ENCODING_ISYNC);
        }
        GuestCall call;
        appendTrampolineCall(code, fdt,
            vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
            "ibm,set-xive", 0x3900U, {kSource, 0U, 0x33U}, 1U, &call);
        CHECK(installProgram(vm, code));
        cpu->setGuestDeliveryEnabled(true);
        cpu->setMSR(initialMSR | entryTranslation);
        CHECK(runCall(vm, call,
            vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
        CHECK(cpu->getNIP() == call.returnAddress + 4U);
        CHECK(cpu->getLR() == call.returnAddress);
        CHECK(cpu->readGPR(3) == PPC64_H_SUCCESS);
        CHECK(readBE32(vm.getMemory(), 0x3900U + 24U) == 0U);
        CHECK(vm.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
        CHECK(cpu->getMSR() == (initialMSR & ~entryTranslation));
        CHECK(vm.validatePPC64RTASTrampoline());
    };

    // SF=1 is exercised only for a low 32-bit real-address parameter block;
    // this is not a qualification of a general 64-bit RTAS ABI.
    runMode("sf1-low-real-address", MSR_SF | MSR_ME | MSR_RI, 0, false);
    // A translated supervisor caller clears IR and/or DR with privileged
    // mtmsr plus isync before entering the real-address RTAS stub.
    runMode("ir-to-real", MSR_ME | MSR_RI, MSR_IR, true);
    runMode("dr-to-real", MSR_ME | MSR_RI, MSR_DR, true);
    runMode("ir-dr-to-real", MSR_ME | MSR_RI,
            MSR_IR | MSR_DR, true);
    runMode("me-clear", MSR_RI, 0, false);
    runMode("ri-clear", MSR_ME, 0, false);

    VirtualMachine problem(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(problem));
    std::string error;
    CHECK(problem.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(problem.getPPC64FDT(), fdt));
    std::vector<uint32_t> code;
    GuestCall call;
    appendTrampolineCall(code, fdt,
        problem.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        "ibm,set-xive", 0x3940U, {kSource, 0U, 0x22U}, 1U, &call);
    writeWords(problem.getMemory(), call.blockAddress + 24U, {0x5a5a5a5aU});
    CHECK(installProgram(problem, code));
    auto* problemCPU = problem.getPPC64ISAPlugin();
    problemCPU->setMSR(MSR_PR | MSR_ME | MSR_RI);
    CHECK(runCall(problem, call,
        problem.getPPC64PlatformSnapshot().memoryLayout.rtasEntry, false));
    CHECK(problemCPU->getNIP() == call.returnAddress + 4U);
    CHECK(problemCPU->readGPR(3) == PPC64_H_PRIVILEGE);
    CHECK(problemCPU->getMSR() == (MSR_PR | MSR_ME | MSR_RI));
    CHECK(readBE32(problem.getMemory(), call.blockAddress + 24U) ==
          0x5a5a5a5aU);
    CHECK(problem.getPPC64XICSSnapshot().sources[0].priority == 0x44U);

    // A real guest mtmsr attempt to set HV is rejected without changing MSR.
    VirtualMachine noHV(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(noHV));
    const uint64_t forgedMSR = MSR_HV | MSR_ME | MSR_RI;
    writeBE64(noHV.getMemory(), 0x3200U, forgedMSR);
    std::vector<uint32_t> noHVCode{
        encodeDS(58, 3, 4, 0), // ld r3,0(r4)
        encodeMtmsr(3)};
    CHECK(installProgram(noHV, noHVCode));
    auto* noHVCPU = noHV.getPPC64ISAPlugin();
    noHVCPU->setGuestDeliveryEnabled(true);
    noHVCPU->setMSR(MSR_ME | MSR_RI);
    noHVCPU->writeGPR(4, 0x3200U);
    CHECK(noHV.step());
    CHECK(!noHV.step());
    CHECK(noHVCPU->getMSR() == (MSR_ME | MSR_RI));
    CHECK((noHVCPU->getMSR() & MSR_HV) == 0);
    CHECK(noHV.getLastExecutionDiagnostic().size() != 0U);

    // Hypervisor-only SPR reads from guest state take Program/privilege.
    VirtualMachine hypervisorSPR(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(hypervisorSPR));
    CHECK(installProgram(hypervisorSPR,
        {encodeMfspr(3, SPR_HSRR0)}));
    auto* hvCPU = hypervisorSPR.getPPC64ISAPlugin();
    hvCPU->setGuestDeliveryEnabled(true);
    hvCPU->setMSR(MSR_ME | MSR_RI);
    CHECK(hypervisorSPR.step());
    CHECK(hvCPU->getNIP() == VEC_PROG);
    CHECK(hvCPU->getSRR0() == kEntry);
    CHECK((hvCPU->getSRR1() & SRR1_PROGPRIV) == SRR1_PROGPRIV);
    CHECK((hvCPU->getMSR() & MSR_HV) == 0);
}

void testR3InstructionFaultMatrix() {
    std::puts("[phase19R3-H-instruction-fault-matrix]");

    VirtualMachine unaligned(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(unaligned));
    auto* cpu = unaligned.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    const uint64_t unalignedNIP =
        unaligned.getPPC64PlatformSnapshot().memoryLayout.rtasEntry + 1U;
    cpu->setPC(unalignedNIP);
    CHECK(unaligned.step());
    CHECK(cpu->getNIP() == VEC_ALIGN);
    CHECK(cpu->getSRR0() == unalignedNIP);
    CHECK(cpu->getMSR() == (MSR_ME | MSR_SF));

    VirtualMachine unmapped(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(unmapped));
    cpu = unmapped.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    const uint64_t unmappedNIP = kRamSize + 0x100U;
    cpu->setPC(unmappedNIP);
    CHECK(unmapped.step());
    CHECK(cpu->getNIP() == VEC_ISI);
    CHECK(cpu->getSRR0() == unmappedNIP);
    CHECK((cpu->getSRR1() & SRR1_ISI_NOPT) == SRR1_ISI_NOPT);

    VirtualMachine invalid(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(invalid));
    writeWords(invalid.getMemory(), kEntry, {0xffffffffU});
    cpu = invalid.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    cpu->setPC(kEntry);
    CHECK(invalid.step());
    CHECK(cpu->getNIP() == VEC_PROG);
    CHECK(cpu->getSRR0() == kEntry);
    CHECK((cpu->getSRR1() & SRR1_PROGILL) == SRR1_PROGILL);

    VirtualMachine nonExecutable(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(nonExecutable));
    CHECK(installIdentityProcessRadix(nonExecutable, false));
    cpu = nonExecutable.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI | MSR_IR);
    cpu->setPC(kEntry);
    CHECK(nonExecutable.step());
    CHECK(cpu->getNIP() == VEC_ISI);
    CHECK(cpu->getSRR0() == kEntry);
    CHECK((cpu->getSRR1() & SRR1_ISI_PROT) == SRR1_ISI_PROT);

    VirtualMachine invalidReturn(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(invalidReturn));
    writeWords(invalidReturn.getMemory(), kEntry,
               {encodeMtspr(5, SPR_LR), 0x4e800020U}); // mtlr r5; blr
    cpu = invalidReturn.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    const uint64_t invalidLR = kRamSize + 0x1000U;
    cpu->writeGPR(5, invalidLR);
    cpu->setPC(kEntry);
    CHECK(invalidReturn.step());
    CHECK(cpu->getLR() == invalidLR);
    CHECK(invalidReturn.step());
    CHECK(cpu->getNIP() == invalidLR);
    CHECK(invalidReturn.step());
    CHECK(cpu->getNIP() == VEC_ISI);
    CHECK(cpu->getSRR0() == invalidLR);
    CHECK((cpu->getSRR1() & SRR1_ISI_NOPT) == SRR1_ISI_NOPT);
}

void testR3MalformedRTASInputs() {
    std::puts("[phase19R3-H-malformed-rtas-inputs]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::vector<uint32_t> code;
    emitLoad32(code, 3, static_cast<uint32_t>(PPC64_H_RTAS));
    code.push_back(kSc1);
    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);
    const auto issue = [&](uint64_t pointer) {
        cpu->setMSR(MSR_ME | MSR_RI);
        cpu->setPC(kEntry);
        cpu->writeGPR(4, pointer);
        if (!steps(vm, 3U)) return uint64_t{0xffffffffffffffffULL};
        CHECK(cpu->getNIP() == kEntry + 12U);
        return cpu->readGPR(3);
    };
    const auto unchangedSource = [&]() {
        const auto snapshot = vm.getPPC64XICSSnapshot();
        const auto found = std::find_if(snapshot.sources.begin(),
            snapshot.sources.end(), [](const auto& source) {
                return source.sourceId == kSource;
            });
        CHECK(found != snapshot.sources.end());
        if (found != snapshot.sources.end()) {
            CHECK(found->priority == 0x44U);
            CHECK(found->enabled);
            CHECK(!found->pending);
        }
    };

    CHECK(issue(0) == PPC64_H_PARAMETER);
    unchangedSource();
    CHECK(issue(0x3201U) == PPC64_H_PARAMETER); // unaligned block
    unchangedSource();
    CHECK(issue(kRamSize) == PPC64_H_PARAMETER); // inaccessible guest RAM
    unchangedSource();
    CHECK(issue(std::numeric_limits<uint64_t>::max() - 3U) ==
          PPC64_H_PARAMETER); // aligned request whose range overflows
    unchangedSource();

    constexpr uint64_t kBadCounts = 0x3300U;
    writeRTASRequest(vm.getMemory(), kBadCounts,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, PPC64_RTAS_MAX_CELLS, 1U, {});
    writeWords(vm.getMemory(), kBadCounts + 76U, {0x5a5a5a5aU});
    CHECK(issue(kBadCounts) == PPC64_H_PARAMETER);
    CHECK(readBE32(vm.getMemory(), kBadCounts + 76U) == 0x5a5a5a5aU);
    unchangedSource();

    constexpr uint64_t kNoReturn = 0x3340U;
    writeRTASRequest(vm.getMemory(), kNoReturn,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 0U,
        {kSource, PPC64_XICS_SERVER_ID, 0x22U});
    CHECK(issue(kNoReturn) == PPC64_H_PARAMETER);
    unchangedSource();

    constexpr uint64_t kRAMCrossing = kRamSize - 8U;
    CHECK(issue(kRAMCrossing) == PPC64_H_PARAMETER); // 12-byte header crosses RAM
    unchangedSource();

    CountingMMIODevice mmio(0x7000U, 0x1000U);
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory != nullptr) memory->RegisterDevice(&mmio);
    constexpr uint64_t kMMIOCrossing = 0x6ff8U;
    writeWords(vm.getMemory(), kMMIOCrossing,
               {PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U});
    CHECK(issue(kMMIOCrossing) == PPC64_H_PARAMETER);
    CHECK(mmio.reads == 0U && mmio.writes == 0U);
    unchangedSource();
    CHECK(issue(0x7000U) == PPC64_H_PARAMETER); // MMIO-backed request
    CHECK(mmio.reads == 0U && mmio.writes == 0U);
    unchangedSource();
    CHECK(memory != nullptr && memory->UnregisterDevice(&mmio));

    constexpr uint64_t kNoWritableResponse = kRamSize - 24U;
    writeRTASRequest(vm.getMemory(), kNoWritableResponse,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
        {kSource, PPC64_XICS_SERVER_ID, 0x22U});
    CHECK(issue(kNoWritableResponse) == PPC64_H_PARAMETER);
    unchangedSource();

    const uint64_t protectedResponse = layout.rtasProtectionBase;
    const uint64_t protectedResponseRequest = protectedResponse - 24U;
    writeRTASRequest(vm.getMemory(), protectedResponseRequest,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
        {kSource, PPC64_XICS_SERVER_ID, 0x22U});
    CHECK(issue(protectedResponseRequest) == PPC64_H_PARAMETER);
    unchangedSource();
    CHECK(vm.validatePPC64RTASTrampoline());

    std::shared_ptr<PPC64PartitionContext> hypervisorContext;
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(manager != nullptr);
    CHECK(manager->configurePartitionTable(vm.getMemory(),
        kLPIDPartitionTable, 4U, error));
    CHECK(installIdentityPartition(vm, 7U, kLPIDAFirstTable,
                                   hypervisorContext, error));
    const uint64_t l3 = kLPIDAFirstTable + 0x3000U;
    writeBE64(vm.getMemory(), l3 + 3U * 8U, partitionLeafPte(0));
    constexpr uint32_t protectedSource = kSource + 2U;
    CHECK(vm.configurePPC64InterruptSource(protectedSource,
        PPC64XICSSourceType::EDGE, 0x44U, PPC64_XICS_SERVER_ID, 7U, true,
        &error));
    CHECK(cpu->activatePartitionContext(hypervisorContext, error));
    constexpr uint64_t hypervisorBacking = 0;
    CHECK(cpu->configureHypervisorMemory(vm.getMemory(), hypervisorBacking,
                                          0x1000U, error));
    constexpr uint64_t hypervisorResponseRequest = 0x2fe8U;
    writeWords(vm.getMemory(), hypervisorBacking, {0x5a5a5a5aU});
    writeRTASRequest(vm.getMemory(), hypervisorResponseRequest,
        PPC64_RTAS_TOKEN_IBM_SET_XIVE, 3U, 1U,
        {protectedSource, PPC64_XICS_SERVER_ID, 0x22U});
    CHECK(issue(hypervisorResponseRequest) == PPC64_H_PARAMETER);
    CHECK(readBE32(vm.getMemory(), hypervisorBacking) == 0x5a5a5a5aU);
    unchangedSource();
    const auto protectedSourceState = vm.getPPC64XICSSnapshot().sources;
    const auto protectedSourceEntry = std::find_if(
        protectedSourceState.begin(), protectedSourceState.end(),
        [protectedSource](const auto& source) {
            return source.sourceId == protectedSource;
        });
    CHECK(protectedSourceEntry != protectedSourceState.end());
    if (protectedSourceEntry != protectedSourceState.end()) {
        CHECK(protectedSourceEntry->ownerLPID == 7U);
        CHECK(protectedSourceEntry->priority == 0x44U);
    }

    constexpr uint64_t kUnsupportedToken = 0x4380U;
    writeRTASRequest(vm.getMemory(), kUnsupportedToken, 0xfeed1234U, 1U, 1U,
                     {kSource});
    writeWords(vm.getMemory(), kUnsupportedToken + 16U,
               {0x5a5a5a5aU});
    CHECK(issue(kUnsupportedToken) == PPC64_H_PARAMETER);
    CHECK(readBE32(vm.getMemory(), kUnsupportedToken + 16U) ==
          static_cast<uint32_t>(PPC64_RTAS_OUT_PARAM_ERROR));
    unchangedSource();
}

void testR3GuestFaultRecovery() {
    std::puts("[phase19R3-H-guest-dsi-recovery-then-rtas]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x44, PPC64_XICS_SERVER_ID, 0, true,
        &error));

    const std::vector<uint32_t> dsiHandler{
        encodeMfspr(6, SPR_DAR),
        encodeMfspr(7, SPR_DSISR),
        encodeD(36, 6, 0, 0x2000),
        encodeD(36, 7, 0, 0x2004),
        encodeAddis(5, 0, 1),
        encodeAddi(5, 5, 4),
        encodeMtspr(5, SPR_SRR0),
        kRfid};
    writeWords(vm.getMemory(), VEC_DSI, dsiHandler);

    std::vector<uint32_t> code{encodeD(36, 4, 3, 0)}; // stw to protected page
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    GuestCall call;
    appendTrampolineCall(code, fdt, layout.fdtBase, "ibm,set-xive",
        0x3900U, {kSource, PPC64_XICS_SERVER_ID, 0x33U}, 1U, &call);
    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setGuestDeliveryEnabled(true);
    cpu->setMSR(MSR_ME | MSR_RI);
    cpu->writeGPR(3, layout.rtasProtectionBase);
    cpu->writeGPR(4, 0xaabbccddU);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == VEC_DSI);
    CHECK(cpu->getSRR0() == kEntry);
    CHECK(cpu->getDAR() == layout.rtasProtectionBase);
    CHECK(cpu->getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));

    // Save and restore the actual DSI vector through writer v9, then let
    // guest code inspect DAR/DSISR, advance SRR0, and return with rfid.
    const auto c7Path = std::filesystem::temp_directory_path() /
        ("guidexos-p19r3-C7-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".gxcp");
    CHECK(!std::filesystem::exists(c7Path));
    CHECK(vm.pause());
    std::string c7Error;
    CHECK(VMCheckpointCodec::write(vm, c7Path.string(), "p19r3-C7", &c7Error));
    CHECK(vm.resume());
    VirtualMachine restored(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(restored));
    CHECK(VMCheckpointCodec::read(restored, c7Path.string(),
                                  "p19r3-C7", &c7Error));
    auto* restoredCPU = restored.getPPC64ISAPlugin();
    CHECK(restoredCPU->getNIP() == VEC_DSI);
    CHECK(restoredCPU->getSRR0() == kEntry);
    CHECK(restoredCPU->getDAR() == layout.rtasProtectionBase);
    CHECK(restoredCPU->getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
    CHECK(restored.validatePPC64RTASTrampoline());
    bool protectedWriteDenied = false;
    try {
        const uint8_t corrupt = 0;
        restored.getMemory().Write(layout.rtasProtectionBase, &corrupt, 1U);
    } catch (const std::exception&) {
        protectedWriteDenied = true;
    }
    CHECK(protectedWriteDenied);
    CHECK(restored.resume());
    CHECK(steps(restored, dsiHandler.size()));
    CHECK(restoredCPU->getNIP() == kEntry + 4U);
    CHECK(restoredCPU->getMSR() == (MSR_ME | MSR_RI));
    CHECK(readBE32(restored.getMemory(), 0x2000U) == layout.rtasProtectionBase);
    CHECK(readBE32(restored.getMemory(), 0x2004U) ==
          (DSISR_ISSTORE | DSISR_PROTFAULT));
    CHECK(restored.getPPC64XICSSnapshot().sources[0].priority == 0x44U);
    CHECK(restored.validatePPC64RTASTrampoline());

    CHECK(runCall(restored, call, layout.rtasEntry, false));
    CHECK(restoredCPU->getNIP() == call.returnAddress + 4U);
    CHECK(restoredCPU->getLR() == call.returnAddress);
    CHECK(restoredCPU->readGPR(3) == PPC64_H_SUCCESS);
    CHECK(readBE32(restored.getMemory(), 0x3900U + 24U) == 0U);
    CHECK(restored.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
    CHECK(restored.validatePPC64RTASTrampoline());
    std::error_code c7CleanupError;
    CHECK(std::filesystem::remove(c7Path, c7CleanupError));
}

void testR3SerializedCheckpointBoundaries() {
    std::puts("[phase19R3-J-checkpoint-C1-C5]");
    VirtualMachine vm(kRamSize, 1, "PPC64", platform());
    CHECK(prepareCapacity(vm));
    IndependentFDT fdt;
    CHECK(parseIndependentFDT(vm.getPPC64FDT(), fdt));
    std::string error;
    CHECK(vm.configurePPC64InterruptSource(kSource,
        PPC64XICSSourceType::EDGE, 0x40, PPC64_XICS_SERVER_ID, 0, true,
        &error));
    std::vector<uint32_t> code;
    GuestCall call;
    appendTrampolineCall(code, fdt,
        vm.getPPC64PlatformSnapshot().memoryLayout.fdtBase,
        "ibm,set-xive", 0x3900U, {kSource, 0U, 0x33U}, 1U, &call);
    CHECK(installProgram(vm, code));
    auto* cpu = vm.getPPC64ISAPlugin();
    cpu->setMSR(MSR_ME | MSR_RI);

    struct SavedBoundary {
        std::string label;
        std::filesystem::path path;
        uint64_t nip;
    };
    std::vector<SavedBoundary> saved;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto saveBoundary = [&](const char* label, uint64_t nip) {
        CHECK(cpu->getNIP() == nip);
        const auto path = std::filesystem::temp_directory_path() /
            ("guidexos-p19r3-" + std::to_string(nonce) + "-" + label + ".gxcp");
        if (std::filesystem::exists(path)) {
            CHECK(false); // Never replace an existing checkpoint artifact.
            return;
        }
        CHECK(vm.pause());
        std::string checkpointError;
        const bool wrote = VMCheckpointCodec::write(
            vm, path.string(), std::string("p19r3-") + label,
            &checkpointError);
        CHECK(wrote);
        if (!wrote) {
            std::printf("R3 %s checkpoint write failed: %s\n", label,
                        checkpointError.c_str());
        } else {
            saved.push_back({label, path, nip});
        }
        CHECK(vm.resume());
    };

    const uint64_t branchNIP = kEntry + call.branchIndex * 4U;
    const uint64_t trampoline =
        vm.getPPC64PlatformSnapshot().memoryLayout.rtasEntry;
    CHECK(stepUntilNIP(vm, branchNIP, 512U));
    saveBoundary("C1-before-entry", branchNIP);
    CHECK(vm.step()); // bctrl enters the authentic trampoline.
    CHECK(cpu->getNIP() == trampoline);
    CHECK(vm.step()); // mr r4,r3
    saveBoundary("C2-after-pointer-transfer", trampoline + 4U);
    CHECK(steps(vm, 2U)); // lis/ori, now immediately before sc 1.
    saveBoundary("C3-before-sc1", trampoline + 12U);
    CHECK(vm.step()); // H_RTAS completes exactly at the next instruction boundary.
    saveBoundary("C4-after-sc1", trampoline + 16U);
    CHECK(vm.step()); // blr returns to the caller.
    saveBoundary("C5-after-return", call.returnAddress);

    for (const auto& boundary : saved) {
        VirtualMachine restored(kRamSize, 1, "PPC64", platform());
        CHECK(prepareCapacity(restored));
        std::string checkpointError;
        CHECK(VMCheckpointCodec::read(restored, boundary.path.string(),
            std::string("p19r3-") + boundary.label, &checkpointError));
        CHECK(restored.validatePPC64RTASTrampoline());
        auto* restoredCPU = restored.getPPC64ISAPlugin();
        CHECK(restoredCPU->getNIP() == boundary.nip);
        if (boundary.label == "C2-after-pointer-transfer") {
            CHECK(restoredCPU->readGPR(4) == call.blockAddress);
            CHECK(restoredCPU->getLR() == call.returnAddress);
        } else if (boundary.label == "C3-before-sc1") {
            CHECK(restoredCPU->readGPR(3) == PPC64_H_RTAS);
            CHECK(restoredCPU->readGPR(4) == call.blockAddress);
        } else if (boundary.label == "C4-after-sc1") {
            CHECK(restoredCPU->readGPR(3) == PPC64_H_SUCCESS);
            CHECK(restored.getPPC64XICSSnapshot().sources[0].priority ==
                  0x33U);
            // If restore incorrectly rewinds to sc 1, this malformed request
            // changes the outer status. Correct restore executes only blr.
            writeWords(restored.getMemory(), call.blockAddress + 8U, {0U});
        }
        CHECK(restored.resume());
        CHECK(stepUntilNIP(restored, call.returnAddress + 4U, 32U));
        CHECK(restoredCPU->getNIP() == call.returnAddress + 4U);
        CHECK(restoredCPU->getLR() == call.returnAddress);
        CHECK(restoredCPU->readGPR(3) == PPC64_H_SUCCESS);
        CHECK(restored.getPPC64XICSSnapshot().sources[0].priority == 0x33U);
        CHECK(restored.validatePPC64RTASTrampoline());
        if (boundary.label == "C5-after-return") {
            CHECK(restoredCPU->readGPR(11) == 1U);
        }
    }

    for (const auto& boundary : saved) {
        std::error_code cleanupError;
        CHECK(std::filesystem::remove(boundary.path, cleanupError));
    }
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
    testLevelTriggeredXICS();
    testNonRootLPIDRTASIsolation();
    testR3ExplicitLPIDCPUContextSwitch();
    testR3LPIDCannotBeForgedThroughPIDR();
    testDirectHRTASCompatibility();
    testTerminalInterruptIntegration();
    testCheckpointAndFaultBoundaries();
    testR3ExecutionModesAndPrivilege();
    testR3InstructionFaultMatrix();
    testR3MalformedRTASInputs();
    testR3GuestFaultRecovery();
    testR3SerializedCheckpointBoundaries();
    testHistoricalV6Fixture();
    std::printf("PPC64 Phase 19: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
