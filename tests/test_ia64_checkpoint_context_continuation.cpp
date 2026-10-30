#include "IA64ISAPlugin.h"
#include "VMCheckpoint.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ia64 {
struct IA64ISAPluginCheckpointTestAccess {
    static void allowDiagnosticWrite(IA64ISAPlugin& plugin) {
        plugin.efiHandoffCheckpointConsumed_ = true;
    }
};
} // namespace ia64

namespace {

using namespace ia64;

constexpr uint64_t kAMain = 0x10000;
constexpr uint64_t kBMain = 0x11000;
constexpr uint64_t kF1 = 0x12000;
constexpr uint64_t kF2 = 0x13000;
constexpr uint64_t kSwitcher = 0x14000;
constexpr uint64_t kRecordA = 0x6000;
constexpr uint64_t kRecordB = 0x6100;
constexpr uint64_t kBackingA = 0x8000;
constexpr uint64_t kBackingB = 0x9000;
constexpr uint64_t kVerifyA = 0x7000;
constexpr uint64_t kVerifyB = 0x7200;
constexpr uint64_t kGuestRamSize = 1U << 20;
constexpr uint64_t kRootCFM = 64ULL | (64ULL << 7);
constexpr uint64_t kF1Continuation = kF1 + 0x40;
constexpr uint64_t kF2Continuation = kF2 + 0x40;
constexpr uint8_t kMiiStop = 0x01;
constexpr uint8_t kMibStop = 0x11;
constexpr uint64_t kSlotMask = (1ULL << 41) - 1;
constexpr uint64_t kSentinel = 0xABABABABABABABABULL;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

void putBits(std::array<uint8_t, 16>& bundle,
             unsigned firstBit,
             unsigned bitCount,
             uint64_t value) {
    for (unsigned i = 0; i < bitCount; ++i) {
        if (((value >> i) & 1ULL) != 0) {
            const unsigned bit = firstBit + i;
            bundle[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
        }
    }
}

void emitBundle(IMemory& memory, uint64_t ip, uint8_t templ,
                uint64_t slot0 = 0, uint64_t slot1 = 0, uint64_t slot2 = 0) {
    std::array<uint8_t, 16> bundle{};
    putBits(bundle, 0, 5, templ);
    putBits(bundle, 5, 41, slot0 & kSlotMask);
    putBits(bundle, 46, 41, slot1 & kSlotMask);
    putBits(bundle, 87, 41, slot2 & kSlotMask);
    memory.Write(ip, bundle.data(), bundle.size());
}

void emitM(IMemory& memory, uint64_t ip, uint64_t instruction) {
    emitBundle(memory, ip, kMiiStop, instruction, 0, 0);
}

void emitI(IMemory& memory, uint64_t ip, uint64_t instruction) {
    emitBundle(memory, ip, kMiiStop, 0, instruction, 0);
}

void emitB(IMemory& memory, uint64_t ip, uint64_t instruction) {
    emitBundle(memory, ip, kMibStop, 0, 0, instruction);
}

void emitNop(IMemory& memory, uint64_t ip) {
    emitBundle(memory, ip, kMiiStop);
}

uint64_t movToAR(uint8_t ar, uint8_t source) {
    return (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(ar) << 20) | (0x2AULL << 27);
}

uint64_t movFromAR(uint8_t destination, uint8_t ar) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(ar) << 20) | (0x32ULL << 27);
}

uint64_t alloc(uint8_t destination, uint8_t sof, uint8_t sol) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(sof) << 13) |
           (static_cast<uint64_t>(sol) << 20) |
           (0x6ULL << 33) | (0x1ULL << 37);
}

uint64_t load8(uint8_t destination, uint8_t base, uint16_t offset = 0) {
    require(offset < 512, "synthetic ld8 offset is too large");
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(offset & 0x7FU) << 13) |
           (static_cast<uint64_t>(base) << 20) |
           (static_cast<uint64_t>((offset >> 7) & 1U) << 27) |
           (static_cast<uint64_t>(0x03) << 30) |
           (static_cast<uint64_t>((offset >> 8) & 1U) << 36) | (0x5ULL << 37);
}

uint64_t store8(uint8_t base, uint8_t source, uint16_t offset = 0) {
    require(offset < 512, "synthetic st8 offset is too large");
    return (static_cast<uint64_t>(offset & 0x7FU) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(base) << 20) |
           (static_cast<uint64_t>((offset >> 7) & 1U) << 27) |
           (static_cast<uint64_t>(0x33) << 30) |
           (static_cast<uint64_t>((offset >> 8) & 1U) << 36) | (0x5ULL << 37);
}

uint64_t movGR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) | (0x2ULL << 27) |
           (0x3ULL << 29) | (0x8ULL << 37);
}

uint64_t andImmediate(uint8_t destination, uint8_t source, int8_t immediate) {
    const uint8_t encoded = static_cast<uint8_t>(immediate);
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(encoded & 0x7FU) << 13) |
           (static_cast<uint64_t>(source) << 20) | (0xBULL << 29) |
           (0x8ULL << 37) |
           (static_cast<uint64_t>((encoded >> 7) & 1U) << 36);
}

uint64_t movFromBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) | (0x31ULL << 27);
}

uint64_t movToBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) | (0x7ULL << 33);
}

uint64_t flushrs() { return 0xCULL << 27; }
uint64_t invala() { return 1ULL << 31; }

uint64_t directBranch(uint8_t br, uint64_t ip, uint64_t target) {
    require((ip & 0xFULL) == 0 && (target & 0xFULL) == 0,
            "synthetic branch endpoints must be bundle aligned");
    const int64_t displacement =
        (static_cast<int64_t>(target) - static_cast<int64_t>(ip)) / 16;
    require(displacement >= -(1LL << 20) && displacement < (1LL << 20),
            "synthetic branch displacement is out of range");
    const uint64_t encoded = static_cast<uint64_t>(displacement) & 0x1FFFFFULL;
    return (static_cast<uint64_t>(br & 0x7U) << 6) |
           ((encoded & 0xFFFFFULL) << 13) |
           (((encoded >> 20) & 1ULL) << 36) | (5ULL << 37);
}

uint64_t indirectReturn(uint8_t source) {
    return (0x4ULL << 6) | (static_cast<uint64_t>(source & 0x7U) << 13) |
           (0x4ULL << 27) | (0x1ULL << 37);
}

void writeU64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t readU64(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

uint64_t backingRegisterAddress(uint64_t base, size_t index) {
    uint64_t address = base;
    for (size_t i = 0; i < index; ++i) {
        if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
        address += 8;
    }
    if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
    return address;
}

uint64_t emitSwitcher(IMemory& memory) {
    uint64_t ip = kSwitcher;
    auto m = [&](uint64_t instruction) {
        emitM(memory, ip, instruction);
        ip += 0x10;
    };
    auto i = [&](uint64_t instruction) {
        emitI(memory, ip, instruction);
        ip += 0x10;
    };

    m(alloc(42, 40, 32));
    m(movGR(57, 30));
    m(movFromAR(41, 16));
    m(movFromAR(43, 17));
    m(store8(57, 41, 8));
    m(store8(57, 43, 8));
    m(store8(57, 42, 8));
    m(flushrs());
    i(andImmediate(44, 41, -4));
    m(movToAR(16, 44));
    m(movFromAR(45, 19));
    m(movFromAR(46, 18));
    m(store8(57, 45, 8));
    m(store8(57, 46, 8));
    i(movFromBR(47, 0));
    i(movFromBR(48, 1));
    i(movFromBR(49, 2));
    m(store8(57, 47, 8));
    m(store8(57, 48, 8));
    m(store8(57, 49, 8));
    m(store8(57, 1, 8));
    m(store8(57, 12, 8));
    m(store8(57, 13, 8));
    m(store8(57, 28, 8));
    m(store8(57, 26, 8));
    m(store8(57, 27, 8));
    m(invala());

    m(movGR(58, 31));
    m(load8(51, 58, 8));
    m(load8(52, 58, 8));
    m(load8(49, 58, 8));
    m(load8(50, 58, 8));
    m(load8(48, 58, 8));
    m(movToAR(16, 0));
    m(movToAR(18, 48));
    m(movToAR(17, 52));
    m(movToAR(64, 49));
    m(movToAR(19, 50));
    m(movToAR(16, 51));
    m(load8(47, 58, 8));
    i(movToBR(0, 47));
    m(load8(48, 58, 8));
    i(movToBR(1, 48));
    m(load8(49, 58, 8));
    i(movToBR(2, 49));
    m(load8(54, 58, 8));
    i(movGR(1, 54));
    m(load8(55, 58, 8));
    i(movGR(12, 55));
    m(load8(56, 58, 8));
    i(movGR(13, 56));
    m(load8(59, 58, 8));
    i(movGR(28, 59));
    m(load8(59, 58, 8));
    i(movGR(26, 59));
    m(load8(59, 58, 8));
    i(movGR(27, 59));
    i(movGR(29, 30));
    i(movGR(30, 31));
    i(movGR(31, 29));
    const uint64_t returnIP = ip;
    emitB(memory, returnIP, indirectReturn(2));
    return returnIP;
}

void emitRoot(IMemory& memory, uint64_t ip) {
    emitB(memory, ip, directBranch(0, ip, kF1));
    emitM(memory, ip + 0x10, store8(28, 32, 8));
    emitM(memory, ip + 0x20, store8(28, 95, 8));
    emitB(memory, ip + 0x30, directBranch(0, ip + 0x30, kF1));
    emitM(memory, ip + 0x40, store8(28, 32, 8));
    emitM(memory, ip + 0x50, store8(28, 95, 8));
    emitB(memory, ip + 0x60, directBranch(0, ip + 0x60, kF1));
    emitM(memory, ip + 0x70, store8(28, 32, 8));
    emitM(memory, ip + 0x80, store8(28, 95, 8));
    emitNop(memory, ip + 0x90);
}

uint64_t emitFramesAndContexts(IMemory& memory) {
    emitRoot(memory, kAMain);
    emitRoot(memory, kBMain);

    emitM(memory, kF1, alloc(40, 64, 64));
    emitI(memory, kF1 + 0x10, movGR(34, 26));
    emitI(memory, kF1 + 0x20, movGR(35, 27));
    emitB(memory, kF1 + 0x30, directBranch(1, kF1 + 0x30, kF2));
    emitM(memory, kF1Continuation, store8(28, 34, 8));
    emitM(memory, kF1 + 0x50, store8(28, 35, 8));
    emitM(memory, kF1 + 0x60, movToAR(64, 40));
    emitB(memory, kF1 + 0x70, indirectReturn(0));

    emitM(memory, kF2, alloc(41, 64, 64));
    emitI(memory, kF2 + 0x10, movGR(32, 26));
    emitI(memory, kF2 + 0x20, movGR(95, 27));
    emitB(memory, kF2 + 0x30, directBranch(2, kF2 + 0x30, kSwitcher));
    emitM(memory, kF2Continuation, store8(28, 32, 8));
    emitM(memory, kF2 + 0x50, store8(28, 95, 8));
    emitM(memory, kF2 + 0x60, movToAR(64, 41));
    emitB(memory, kF2 + 0x70, indirectReturn(1));
    return emitSwitcher(memory);
}

void seedContextB(IMemory& memory) {
    uint64_t firstRNAT = 0;
    for (size_t i = 0; i < 64; ++i) {
        const uint64_t address = backingRegisterAddress(kBackingB, i);
        writeU64(memory, address, 0xB000000000000000ULL + i);
        if (i == 3 || i == 62) firstRNAT |= 1ULL << (i % 63);
    }
    writeU64(memory, kBackingB + 0x1F8, firstRNAT);
    const uint64_t bsp = backingRegisterAddress(kBackingB, 64);
    writeU64(memory, kRecordB + 0, 0);
    writeU64(memory, kRecordB + 8, bsp);
    writeU64(memory, kRecordB + 16, kRootCFM);
    writeU64(memory, kRecordB + 24, 1); // Live collection contains r95's NaT bit.
    writeU64(memory, kRecordB + 32, bsp);
    writeU64(memory, kRecordB + 40, 0);
    writeU64(memory, kRecordB + 48, 0);
    writeU64(memory, kRecordB + 56, kBMain);
    writeU64(memory, kRecordB + 64, 0xB100000000000001ULL);
    writeU64(memory, kRecordB + 72, 0xB200000000000002ULL);
    writeU64(memory, kRecordB + 80, 0xB300000000000003ULL);
    writeU64(memory, kRecordB + 88, kVerifyB);
    writeU64(memory, kRecordB + 96, 0xB4B4B4B4B4B4B4B4ULL);
    writeU64(memory, kRecordB + 104, 0xB5B5B5B5B5B5B5B5ULL);
}

uint64_t initializeGuest(VirtualMachine& vm) {
    require(vm.init(), "VM initialization failed");
    CPU& cpu = vm.getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "VM did not select the production IA-64 plugin");
    plugin->reset();
    IMemory& memory = vm.getMemory();
    const uint64_t switchReturnIP = emitFramesAndContexts(memory);
    for (uint64_t address = kBackingA; address < kBackingA + 0x1000; address += 8) {
        writeU64(memory, address, kSentinel);
    }
    for (uint64_t address = kVerifyA; address < kVerifyA + 0x200; address += 8) {
        writeU64(memory, address, 0);
        writeU64(memory, kVerifyB + (address - kVerifyA), 0);
    }
    seedContextB(memory);

    CPUState& state = cpu.getState();
    state.SetCFM(kRootCFM);
    state.SetRSC(0);
    state.SetBSP(kBackingA);
    state.SetBSPSTORE(kBackingA);
    state.SetRNAT((1ULL << 3) | (1ULL << 62));
    state.SetRSEBOFForCheckpoint(0);
    state.SetGR(30, kRecordA);
    state.SetGR(31, kRecordB);
    state.SetGR(28, kVerifyA);
    state.SetGR(26, 0xA4A4A4A4A4A4A4A4ULL);
    state.SetGR(27, 0xA5A5A5A5A5A5A5A5ULL);
    state.SetGR(1, 0xA100000000000001ULL);
    state.SetGR(12, 0xA200000000000002ULL);
    state.SetGR(13, 0xA300000000000003ULL);
    for (size_t i = 0; i < 64; ++i) {
        state.SetGR(32 + i, 0xA000000000000000ULL + i);
    }
    state.SetGRNaT(35, true);
    state.SetGRNaT(94, true);
    state.SetGRNaT(95, true);
    cpu.setIP(kAMain);
    return switchReturnIP;
}

struct ArchSnapshot {
    std::vector<uint64_t> words;
    std::vector<uint8_t> bytes;

    bool operator==(const ArchSnapshot& other) const {
        return words == other.words && bytes == other.bytes;
    }
};

void appendTranslation(ArchSnapshot& result, const TranslationRegisterState& tr) {
    result.words.push_back(tr.physicalAddress);
    result.words.push_back(tr.virtualAddress);
    result.words.push_back(tr.itir);
    result.words.push_back(tr.regionValue);
    result.words.push_back(tr.valid ? 1 : 0);
}

ArchSnapshot captureArch(const CPUState& cpu, const IA64ISAPlugin& plugin) {
    ArchSnapshot result;
    auto& words = result.words;
    auto& bytes = result.bytes;
    words.push_back(cpu.GetIP());
    words.push_back(static_cast<uint64_t>(plugin.getCurrentSlot()));
    words.push_back(cpu.GetCFM());
    words.push_back(cpu.GetPSR());
    for (size_t i = 0; i < NUM_PHYSICAL_GENERAL_REGISTERS; ++i) {
        words.push_back(cpu.GetGRPhysical(i));
        words.push_back(cpu.GetGRNaTPhysical(i) ? 1 : 0);
    }
    for (size_t i = 0; i < NUM_FLOAT_REGISTERS; ++i) {
        std::array<uint8_t, 16> fr{};
        cpu.GetFRPhysical(i, fr.data());
        bytes.insert(bytes.end(), fr.begin(), fr.end());
    }
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        bytes.push_back(cpu.GetPRPhysical(i) ? 1 : 0);
    }
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) words.push_back(cpu.GetBR(i));
    for (size_t i = 0; i < NUM_REGION_REGISTERS; ++i) words.push_back(cpu.GetRR(i));
    for (size_t i = 0; i < NUM_CONTROL_REGISTERS; ++i) words.push_back(cpu.GetCR(i));
    for (size_t i = 0; i < NUM_APPLICATION_REGISTERS; ++i) words.push_back(cpu.GetAR(i));
    for (size_t i = 0; i < NUM_CPUID_REGISTERS; ++i) words.push_back(cpu.GetCPUID(i));
    for (size_t i = 0; i < NUM_TRANSLATION_REGISTERS; ++i) {
        appendTranslation(result, cpu.GetITR(i));
        appendTranslation(result, cpu.GetDTR(i));
    }
    for (size_t i = 0; i < NUM_INSTRUCTION_TLB_ENTRIES; ++i) {
        appendTranslation(result, cpu.GetITLB(i));
    }
    for (size_t i = 0; i < NUM_DATA_TLB_ENTRIES; ++i) {
        appendTranslation(result, cpu.GetDTLB(i));
    }
    words.push_back(static_cast<uint64_t>(cpu.GetITLBReplacementIndex()));
    words.push_back(static_cast<uint64_t>(cpu.GetDTLBReplacementIndex()));
    const RSEState& rse = cpu.GetRSEState();
    words.insert(words.end(), {rse.cfm, rse.rsc, rse.bsp, rse.bspstore,
                               rse.rnat, rse.pfs, rse.bspLoad});
    words.insert(words.end(), {rse.bof, rse.storeReg, rse.loadReg,
                               rse.sof, rse.sol, rse.sor});
    for (RSERegisterPartition part : rse.physicalPartition) {
        bytes.push_back(static_cast<uint8_t>(part));
    }
    return result;
}

uint64_t fingerprint(const ArchSnapshot& snapshot) {
    uint64_t hash = 1469598103934665603ULL;
    auto add = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (uint64_t word : snapshot.words) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            add(static_cast<uint8_t>(word >> shift));
        }
    }
    for (uint8_t byte : snapshot.bytes) add(byte);
    return hash;
}

struct StepRecord {
    uint64_t ipBefore = 0;
    size_t slotBefore = 0;
    ArchSnapshot after;

    bool operator==(const StepRecord& other) const {
        return ipBefore == other.ipBefore && slotBefore == other.slotBefore &&
               after == other.after;
    }
};

struct RunEvidence {
    std::vector<StepRecord> trace;
    std::vector<uint64_t> switchTargets;
    std::vector<uint64_t> fillsFromA;
    std::vector<uint64_t> fillsFromB;
    bool sawARootReturn = false;
    bool sawBRootReturn = false;
    bool sawAutomaticSpillA = false;
    bool sawAutomaticSpillB = false;
};

std::vector<uint8_t> readRam(const VirtualMachine& vm) {
    std::vector<uint8_t> bytes(kGuestRamSize);
    vm.getMemory().Read(0, bytes.data(), bytes.size());
    return bytes;
}

size_t countPartition(const CPUState& state, RSERegisterPartition expected) {
    size_t count = 0;
    for (size_t i = 0; i < NUM_STACKED_GENERAL_REGISTERS; ++i) {
        if (state.GetRSEPhysicalPartition(i) == expected) ++count;
    }
    return count;
}

size_t watchBackingReads(VirtualMachine& vm,
                         std::vector<uint64_t>& readsA,
                         std::vector<uint64_t>& readsB) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    require(memory != nullptr, "checkpoint regression requires MMU-aware guest memory");
    return memory->GetMMU().RegisterReadHook(
        [&](HookContext& context, uint8_t*) {
            if (context.address >= kBackingA && context.address < kBackingA + 0x1000) {
                readsA.push_back(context.address);
            }
            if (context.address >= kBackingB && context.address < kBackingB + 0x1000) {
                readsB.push_back(context.address);
            }
        });
}

void unwatchBackingReads(VirtualMachine& vm, size_t hook) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    require(memory != nullptr, "checkpoint regression lost its guest memory instance");
    memory->GetMMU().UnregisterReadHook(hook);
}

RunEvidence runToCheckpoint(VirtualMachine& vm, uint64_t switchReturnIP,
                            IA64ISAPlugin& plugin,
                            size_t checkpointSwitchCount = 4) {
    RunEvidence result;
    CPU& cpu = vm.getCPU();
    CPUState& state = cpu.getState();
    size_t switches = 0;
    for (size_t step = 0; step < 12000; ++step) {
        const uint64_t beforeIP = state.GetIP();
        const size_t beforeSlot = plugin.getCurrentSlot();
        const uint64_t beforeBspstore = state.GetBSPSTORE();
        if (!vm.stepCPU(0)) {
            const RSEState& rse = state.GetRSEState();
            std::ostringstream message;
            message << "VM stopped before checkpoint at ip=" << hex(beforeIP)
                    << " slot=" << beforeSlot << " cfm=" << hex(state.GetCFM())
                    << " pfs=" << hex(state.GetPFS()) << " bsp=" << hex(state.GetBSP())
                    << " bspstore=" << hex(state.GetBSPSTORE())
                    << " bspLoad=" << hex(rse.bspLoad) << " bof="
                    << static_cast<unsigned>(rse.bof) << " storeReg="
                    << static_cast<unsigned>(rse.storeReg) << " loadReg="
                    << static_cast<unsigned>(rse.loadReg);
            throw std::runtime_error(message.str());
        }
        result.trace.push_back({beforeIP, beforeSlot, captureArch(state, plugin)});
        if ((beforeIP == kF1 || beforeIP == kF2) &&
            state.GetBSPSTORE() > beforeBspstore) {
            if (state.GetGR(26) == 0xA4A4A4A4A4A4A4A4ULL) result.sawAutomaticSpillA = true;
            if (state.GetGR(26) == 0xB4B4B4B4B4B4B4B4ULL) result.sawAutomaticSpillB = true;
        }
        if (beforeIP == switchReturnIP && beforeSlot == 2) {
            result.switchTargets.push_back(state.GetIP());
            ++switches;
            if (switches == checkpointSwitchCount &&
                state.GetIP() == kF2Continuation) {
                return result;
            }
        }
    }
    std::ostringstream message;
    message << "guest did not reach A-active/B-suspended checkpoint point; ip="
            << hex(state.GetIP()) << " switchReturn=" << hex(switchReturnIP)
            << " switchCount=" << switches << " gr28=" << hex(state.GetGR(28))
            << " cfm=" << hex(state.GetCFM()) << " pfs=" << hex(state.GetPFS())
            << " bsp=" << hex(state.GetBSP()) << " bspstore=" << hex(state.GetBSPSTORE())
            << " recent=";
    const size_t begin = result.trace.size() > 40 ? result.trace.size() - 40 : 0;
    for (size_t i = begin; i < result.trace.size(); ++i) {
        message << '[' << hex(result.trace[i].ipBefore) << ':'
                << result.trace[i].slotBefore << "->"
                << hex(result.trace[i].after.words[0]) << ']';
    }
    throw std::runtime_error(message.str());
}

RunEvidence continueToCompletion(VirtualMachine& vm, uint64_t switchReturnIP,
                                 IA64ISAPlugin& plugin) {
    RunEvidence result;
    CPU& cpu = vm.getCPU();
    CPUState& state = cpu.getState();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    require(memory != nullptr, "checkpoint continuation requires MMU-aware memory");
    const size_t hook = watchBackingReads(vm, result.fillsFromA, result.fillsFromB);
    bool sawThirdSwitchToB = false;
    for (size_t step = 0; step < 12000; ++step) {
        if (state.GetIP() == kAMain + 0x90) {
            unwatchBackingReads(vm, hook);
            require(sawThirdSwitchToB,
                    "post-checkpoint continuation did not alternate back into context B");
            return result;
        }
        const uint64_t beforeIP = state.GetIP();
        const size_t beforeSlot = plugin.getCurrentSlot();
        const uint64_t beforeBspstore = state.GetBSPSTORE();
        if (!vm.stepCPU(0)) {
            unwatchBackingReads(vm, hook);
            throw std::runtime_error("VM stopped during checkpoint continuation at IP=" +
                                     std::to_string(beforeIP));
        }
        result.trace.push_back({beforeIP, beforeSlot, captureArch(state, plugin)});
        if ((beforeIP == kF1 || beforeIP == kF2) &&
            state.GetBSPSTORE() > beforeBspstore) {
            if (state.GetGR(26) == 0xA4A4A4A4A4A4A4A4ULL) result.sawAutomaticSpillA = true;
            if (state.GetGR(26) == 0xB4B4B4B4B4B4B4B4ULL) result.sawAutomaticSpillB = true;
        }
        if (state.GetIP() == kBMain + 0x40 && state.GetCFM() == kRootCFM &&
            state.GetGRNaT(35) && state.GetGRNaT(94) && state.GetGRNaT(95)) {
            result.sawBRootReturn = true;
        }
        if (state.GetIP() == kAMain + 0x40 && state.GetCFM() == kRootCFM &&
            state.GetGRNaT(35) && state.GetGRNaT(94) && state.GetGRNaT(95)) {
            result.sawARootReturn = true;
        }
        if (beforeIP == switchReturnIP && beforeSlot == 2) {
            result.switchTargets.push_back(state.GetIP());
            if (state.GetIP() == kF2Continuation &&
                state.GetGR(26) == 0xB4B4B4B4B4B4B4B4ULL) {
                sawThirdSwitchToB = true;
            }
        }
    }
    unwatchBackingReads(vm, hook);
    throw std::runtime_error("guest exceeded the checkpoint continuation instruction bound");
}

void compareTrace(const std::vector<StepRecord>& reference,
                  const std::vector<StepRecord>& restored,
                  const std::string& label) {
    if (reference.size() != restored.size()) {
        throw std::runtime_error(label + " trace length mismatch: reference=" +
                                 std::to_string(reference.size()) + " restored=" +
                                 std::to_string(restored.size()));
    }
    for (size_t i = 0; i < reference.size(); ++i) {
        if (!(reference[i] == restored[i])) {
            throw std::runtime_error(label + " first architectural divergence at step " +
                                     std::to_string(i) + " ip=" +
                                     std::to_string(reference[i].ipBefore) +
                                     " slot=" + std::to_string(reference[i].slotBefore));
        }
    }
}

uint64_t traceHash(const std::vector<StepRecord>& trace) {
    uint64_t hash = 1469598103934665603ULL;
    auto add = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const StepRecord& step : trace) {
        for (uint64_t word : {step.ipBefore, static_cast<uint64_t>(step.slotBefore),
                              fingerprint(step.after)}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                add(static_cast<uint8_t>(word >> shift));
            }
        }
    }
    return hash;
}

void preserveTrace(const std::filesystem::path& path,
                   const std::vector<StepRecord>& trace) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create reference trace evidence");
    output << "step,ip,slot,next_ip,arch_hash\n";
    for (size_t i = 0; i < trace.size(); ++i) {
        output << i << ',' << std::hex << trace[i].ipBefore << std::dec << ','
               << trace[i].slotBefore << ',' << std::hex << trace[i].after.words[0]
               << ',' << fingerprint(trace[i].after) << std::dec << '\n';
    }
    require(output.good(), "reference trace evidence write failed");
}

void compareKeyGuestMemory(const std::vector<uint8_t>& expected,
                           const std::vector<uint8_t>& actual,
                           const std::string& label) {
    require(expected.size() == actual.size(), label + " RAM size mismatch");
    require(expected == actual, label + " guest RAM differs from uninterrupted reference");
}

void verifyRestoredResults(const VirtualMachine& vm) {
    const IMemory& memory = vm.getMemory();
    const CPUState& state = vm.getCPU().getState();
    require(state.GetIP() == kAMain + 0x90, "A did not reach its final continuation");
    require(state.GetCFM() == kRootCFM, "A did not restore its root CFM");
    require(state.GetGRNaT(35) && state.GetGRNaT(94) && state.GetGRNaT(95),
            "A lost NaT bits across the RNAT collection boundary");
    require(state.GetBSP() == state.GetBSPSTORE(),
            "A final BSP/BSPSTORE boundary is inconsistent");
    require(readU64(memory, kVerifyA + 0) == 0xA4A4A4A4A4A4A4A4ULL &&
                readU64(memory, kVerifyA + 8) == 0xA5A5A5A5A5A5A5A5ULL,
            "A's nested local values did not return through guest backing memory");
    require(readU64(memory, kVerifyA + 16) == 0xA4A4A4A4A4A4A4A4ULL &&
                readU64(memory, kVerifyA + 24) == 0xA5A5A5A5A5A5A5A5ULL &&
                readU64(memory, kVerifyA + 32) == 0xA000000000000000ULL &&
                readU64(memory, kVerifyA + 40) == 0xA00000000000003FULL,
            "A's root values were not restored after a nested call");
    require(readU64(memory, kVerifyB + 0) == 0xB4B4B4B4B4B4B4B4ULL &&
                readU64(memory, kVerifyB + 8) == 0xB5B5B5B5B5B5B5B5ULL,
            "B's nested local values did not survive its suspended returns");
    require(readU64(memory, kVerifyB + 16) == 0xB4B4B4B4B4B4B4B4ULL &&
                readU64(memory, kVerifyB + 24) == 0xB5B5B5B5B5B5B5B5ULL &&
                readU64(memory, kVerifyB + 32) == 0xB000000000000000ULL &&
                readU64(memory, kVerifyB + 40) == 0xB00000000000003FULL,
            "B's root values were not restored from B's backing store");
    require(readU64(memory, kBackingA + 0x1F8) != kSentinel &&
                readU64(memory, kBackingB + 0x1F8) != kSentinel,
            "RNAT collection data did not originate in both guest backing stores");
}

void testNoPressureNestedCallCheckpoint(const std::filesystem::path& outputDir) {
    constexpr uint64_t mainIP = 0x18000;
    constexpr uint64_t functionIP = 0x18100;
    constexpr uint64_t backing = 0xA000;
    constexpr uint64_t verify = 0xA800;
    constexpr uint64_t rootCFM = 8ULL | (8ULL << 7);
    constexpr uint64_t finalIP = mainIP + 0x20;
    const std::filesystem::path checkpoint = outputDir / "ia64-r7-no-pressure.gxcp";

    auto source = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    require(source->init(), "no-pressure source VM initialization failed");
    CPU& cpu = source->getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "no-pressure source VM has no IA-64 plugin");
    plugin->reset();
    IMemory& memory = source->getMemory();
    emitB(memory, mainIP, directBranch(0, mainIP, functionIP));
    emitM(memory, mainIP + 0x10, store8(28, 32, 8));
    emitNop(memory, finalIP);
    emitM(memory, functionIP, alloc(40, 8, 8));
    emitI(memory, functionIP + 0x10, movGR(32, 26));
    emitM(memory, functionIP + 0x20, movToAR(64, 40));
    emitB(memory, functionIP + 0x30, indirectReturn(0));
    CPUState& state = cpu.getState();
    state.SetCFM(rootCFM);
    state.SetRSC(0);
    state.SetBSP(backing);
    state.SetBSPSTORE(backing);
    state.SetRSEBOFForCheckpoint(0);
    state.SetGR(26, 0xAFAFAFAFAFAFAFAFULL);
    state.SetGR(28, verify);
    for (size_t i = 0; i < 8; ++i) state.SetGR(32 + i, 0x8100000000000000ULL + i);
    cpu.setIP(mainIP);

    while (state.GetIP() != functionIP) {
        require(source->stepCPU(0), "no-pressure guest stopped before checkpoint");
    }
    require(state.GetBSPSTORE() == backing,
            "ordinary nested call spilled a register before reaching physical capacity");
    require(state.GetBR(0) == mainIP + 0x10,
            "no-pressure checkpoint did not retain its outstanding root return");
    const ArchSnapshot checkpointArch = captureArch(state, *plugin);
    const std::vector<uint8_t> checkpointRam = readRam(*source);
    IA64ISAPluginCheckpointTestAccess::allowDiagnosticWrite(*plugin);
    std::string error;
    require(source->writeDiagnosticCheckpoint(checkpoint.string(), "ia64-r7-no-pressure",
                                              &error),
            "no-pressure checkpoint write failed: " + error);

    std::vector<StepRecord> referenceTrace;
    while (state.GetIP() != finalIP) {
        const uint64_t beforeIP = state.GetIP();
        const size_t beforeSlot = plugin->getCurrentSlot();
        require(source->stepCPU(0), "no-pressure reference stopped before return");
        referenceTrace.push_back({beforeIP, beforeSlot, captureArch(state, *plugin)});
    }
    require(readU64(source->getMemory(), verify) == 0x8100000000000000ULL,
            "no-pressure nested return did not restore the caller's stacked value");
    const ArchSnapshot finalArch = captureArch(state, *plugin);
    const std::vector<uint8_t> finalRam = readRam(*source);
    source.reset();

    auto restored = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    require(restored->init(), "no-pressure fresh VM initialization failed");
    CPU& restoredCPU = restored->getCPU();
    auto* restoredPlugin = dynamic_cast<IA64ISAPlugin*>(restoredCPU.getISAPlugin());
    require(restoredPlugin != nullptr, "no-pressure fresh VM has no IA-64 plugin");
    writeU64(restored->getMemory(), backing, 0xEEEEEEEEEEEEEEEEULL);
    require(restored->readDiagnosticCheckpoint(checkpoint.string(),
                                               "ia64-r7-no-pressure", &error),
            "no-pressure checkpoint restore failed: " + error);
    require(captureArch(restoredCPU.getState(), *restoredPlugin) == checkpointArch,
            "no-pressure architectural call state did not restore exactly");
    compareKeyGuestMemory(checkpointRam, readRam(*restored),
                          "no-pressure checkpoint restore");

    std::vector<StepRecord> restoredTrace;
    while (restoredCPU.getIP() != finalIP) {
        const uint64_t beforeIP = restoredCPU.getIP();
        const size_t beforeSlot = restoredPlugin->getCurrentSlot();
        require(restored->stepCPU(0), "no-pressure restored VM stopped before return");
        restoredTrace.push_back({beforeIP, beforeSlot,
                                 captureArch(restoredCPU.getState(), *restoredPlugin)});
    }
    compareTrace(referenceTrace, restoredTrace, "no-pressure nested-call continuation");
    require(captureArch(restoredCPU.getState(), *restoredPlugin) == finalArch,
            "no-pressure final architectural state differs after restore");
    compareKeyGuestMemory(finalRam, readRam(*restored), "no-pressure final continuation");
    restored.reset();
}

struct ReferenceResult {
    ArchSnapshot checkpointArch;
    ArchSnapshot finalArch;
    std::vector<uint8_t> checkpointRam;
    std::vector<uint8_t> finalRam;
    RunEvidence continuation;
    uint64_t checkpointCycles = 0;
    uint64_t finalCycles = 0;
    uint64_t maxCheckpointBspstoreA = 0;
    size_t currentAtCheckpoint = 0;
    size_t cleanAtCheckpoint = 0;
    size_t dirtyAtCheckpoint = 0;
    size_t invalidAtCheckpoint = 0;
    uint64_t cfmAtCheckpoint = 0;
    uint64_t rscAtCheckpoint = 0;
    uint64_t bspAtCheckpoint = 0;
    uint64_t rnatAtCheckpoint = 0;
    uint64_t pfsAtCheckpoint = 0;
    uint64_t bspLoadAtCheckpoint = 0;
    uint8_t bofAtCheckpoint = 0;
    uint8_t storeRegAtCheckpoint = 0;
    uint8_t loadRegAtCheckpoint = 0;
    uint8_t sofAtCheckpoint = 0;
    uint8_t solAtCheckpoint = 0;
    uint8_t sorAtCheckpoint = 0;
    uint64_t switchReturnIP = 0;
};

ReferenceResult createCheckpointAndReference(const std::filesystem::path& checkpoint,
                                             const std::filesystem::path& tracePath) {
    ReferenceResult result;
    auto source = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    result.switchReturnIP = initializeGuest(*source);
    CPU& cpu = source->getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "source VM lost its IA-64 plugin");
    RunEvidence preparation = runToCheckpoint(*source, result.switchReturnIP, *plugin);
    require(preparation.sawAutomaticSpillA,
            "A did not incur an alloc-mandated RSE spill before checkpoint");
    require(preparation.sawAutomaticSpillB,
            "B did not incur an alloc-mandated RSE spill before checkpoint");
    require(preparation.switchTargets.size() == 4 &&
                preparation.switchTargets[0] == kBMain &&
                preparation.switchTargets[1] == kF2Continuation &&
                preparation.switchTargets[2] == kF2Continuation &&
                preparation.switchTargets[3] == kF2Continuation,
            "guest did not reach the expected A/B/A/B/A switch sequence");
    require(cpu.getState().GetIP() == kF2Continuation &&
                cpu.getState().GetGR(26) == 0xA4A4A4A4A4A4A4A4ULL,
            "checkpoint cut point is not the active A F2 continuation");
    require(cpu.getState().GetBR(0) == kAMain + 0x40 &&
                cpu.getState().GetBR(1) == kF1Continuation &&
                cpu.getState().GetBR(2) == kF2Continuation,
            "A's outstanding main/F1/F2 return targets were not present at the cut point");
    require(readU64(source->getMemory(), kRecordB + 40) == kBMain + 0x40 &&
                readU64(source->getMemory(), kRecordB + 48) == kF1Continuation &&
                readU64(source->getMemory(), kRecordB + 56) == kF2Continuation,
            "B's guest record did not retain its outstanding main/F1/F2 returns");
    require(cpu.getState().GetBSPSTORE() > kBackingA,
            "A backing store contains no transferred register values at the cut point");
    require(readU64(source->getMemory(), kBackingA) == 0xA000000000000000ULL &&
                readU64(source->getMemory(), kBackingB) == 0xB000000000000000ULL,
            "spilled root values are not present in their guest-owned backing stores");
    require((readU64(source->getMemory(), kBackingA + 0x1F8) &
             ((1ULL << 3) | (1ULL << 62))) != 0,
            "A's saved RNAT collection lost the nonzero boundary bits");
    const size_t currentCount = countPartition(cpu.getState(), RSERegisterPartition::CURRENT);
    const size_t cleanCount = countPartition(cpu.getState(), RSERegisterPartition::CLEAN);
    require(currentCount > 0 &&
                countPartition(cpu.getState(), RSERegisterPartition::INVALID) > 0,
            "checkpoint RSE partitions are not nontrivial: current=" +
                std::to_string(currentCount) + " clean=" + std::to_string(cleanCount));

    const RSEState& rse = cpu.getState().GetRSEState();
    result.maxCheckpointBspstoreA = rse.bspstore;
    result.currentAtCheckpoint = currentCount;
    result.cleanAtCheckpoint = cleanCount;
    result.dirtyAtCheckpoint = countPartition(cpu.getState(), RSERegisterPartition::DIRTY);
    result.invalidAtCheckpoint = countPartition(cpu.getState(), RSERegisterPartition::INVALID);
    result.cfmAtCheckpoint = rse.cfm;
    result.rscAtCheckpoint = rse.rsc;
    result.bspAtCheckpoint = rse.bsp;
    result.rnatAtCheckpoint = rse.rnat;
    result.pfsAtCheckpoint = rse.pfs;
    result.bspLoadAtCheckpoint = rse.bspLoad;
    result.bofAtCheckpoint = rse.bof;
    result.storeRegAtCheckpoint = rse.storeReg;
    result.loadRegAtCheckpoint = rse.loadReg;
    result.sofAtCheckpoint = rse.sof;
    result.solAtCheckpoint = rse.sol;
    result.sorAtCheckpoint = rse.sor;
    result.checkpointArch = captureArch(cpu.getState(), *plugin);
    result.checkpointRam = readRam(*source);
    result.checkpointCycles = source->getCyclesExecuted();

    IA64ISAPluginCheckpointTestAccess::allowDiagnosticWrite(*plugin);
    std::string error;
    require(source->writeDiagnosticCheckpoint(checkpoint.string(), "ia64-r7-context-ab",
                                              &error),
            "production checkpoint writer failed: " + error);
    require(std::filesystem::exists(checkpoint) && std::filesystem::file_size(checkpoint) >
                result.checkpointRam.size(),
            "checkpoint file was not written with its full VM payload");

    result.continuation = continueToCompletion(*source, result.switchReturnIP, *plugin);
    require(result.continuation.sawARootReturn && result.continuation.sawBRootReturn,
            "uninterrupted reference did not return both contexts through their root frames");
    require(!result.continuation.fillsFromA.empty() && !result.continuation.fillsFromB.empty(),
            "reference continuation did not fill registers from both guest backing stores");
    require(result.continuation.switchTargets.size() == 2 &&
                result.continuation.switchTargets[0] == kF2Continuation &&
                result.continuation.switchTargets[1] == kF2Continuation,
            "reference continuation did not complete an additional A/B alternation");
    verifyRestoredResults(*source);
    result.finalArch = captureArch(cpu.getState(), *plugin);
    result.finalRam = readRam(*source);
    result.finalCycles = source->getCyclesExecuted();
    preserveTrace(tracePath, result.continuation.trace);

    // The old VM and CPU are destroyed before any restore VM is constructed.
    source.reset();
    require(!source, "source VM destruction did not complete");
    return result;
}

void restoreAndCompare(const std::filesystem::path& checkpoint,
                       const ReferenceResult& reference,
                       size_t restoreNumber) {
    auto restored = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    require(restored->init(), "fresh restore VM initialization failed");
    CPU& cpu = restored->getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "fresh restore VM did not create a new IA-64 plugin");
    // This sentinel belongs only to the new VM. The checkpoint loader must
    // replace it with the saved bytes without rebuilding guest context data.
    writeU64(restored->getMemory(), kBackingA, 0xEEEEEEEEEEEEEEEEULL);
    writeU64(restored->getMemory(), kBackingB, 0xDDDDDDDDDDDDDDDDULL);
    std::string error;
    require(restored->readDiagnosticCheckpoint(checkpoint.string(),
                                               "ia64-r7-context-ab", &error),
            "fresh VM checkpoint load failed: " + error);
    require(captureArch(cpu.getState(), *plugin) == reference.checkpointArch,
            "fresh VM did not independently restore the saved architectural/RSE state");
    compareKeyGuestMemory(reference.checkpointRam, readRam(*restored),
                          "checkpoint restore");
    require(restored->getCyclesExecuted() == reference.checkpointCycles,
            "VM execution counter did not restore from the checkpoint");

    RunEvidence continuation = continueToCompletion(*restored, reference.switchReturnIP, *plugin);
    require(continuation.sawARootReturn && continuation.sawBRootReturn,
            "restored continuation did not return both contexts through their root frames");
    require(!continuation.fillsFromA.empty() && !continuation.fillsFromB.empty(),
            "restored continuation did not fill from both serialized guest backing stores");
    require(continuation.switchTargets == reference.continuation.switchTargets,
            "restored A/B return targets differ from uninterrupted reference");
    compareTrace(reference.continuation.trace, continuation.trace,
                 "fresh restore " + std::to_string(restoreNumber));
    verifyRestoredResults(*restored);
    require(captureArch(cpu.getState(), *plugin) == reference.finalArch,
            "restored final architectural state differs from uninterrupted reference");
    compareKeyGuestMemory(reference.finalRam, readRam(*restored),
                          "restored final");
    restored.reset();
}

void testSuspendedContextCutPoint(const std::filesystem::path& outputDir) {
    const std::filesystem::path checkpoint =
        outputDir / "ia64-r7-a-suspended-b-active.gxcp";
    const std::filesystem::path tracePath =
        outputDir / "ia64-r7-a-suspended-b-active-reference.csv";
    auto source = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    const uint64_t switchReturnIP = initializeGuest(*source);
    CPU& sourceCPU = source->getCPU();
    auto* sourcePlugin = dynamic_cast<IA64ISAPlugin*>(sourceCPU.getISAPlugin());
    require(sourcePlugin != nullptr, "suspended-context source has no IA-64 plugin");
    const RunEvidence preparation =
        runToCheckpoint(*source, switchReturnIP, *sourcePlugin, 3);
    require(preparation.switchTargets.size() == 3 &&
                preparation.switchTargets[0] == kBMain &&
                preparation.switchTargets[1] == kF2Continuation &&
                preparation.switchTargets[2] == kF2Continuation,
            "suspended-context setup did not reach B after suspending A");
    require(preparation.sawAutomaticSpillA && preparation.sawAutomaticSpillB,
            "both suspended-context call chains must have spilled before this cut point");
    const CPUState& cutCPU = sourceCPU.getState();
    require(cutCPU.GetIP() == kF2Continuation &&
                cutCPU.GetGR(26) == 0xB4B4B4B4B4B4B4B4ULL,
            "suspended-context checkpoint must have B active and A suspended");
    require(cutCPU.GetBR(0) == kBMain + 0x10 &&
                cutCPU.GetBR(1) == kF1Continuation &&
                cutCPU.GetBR(2) == kF2Continuation,
            "B's active nested return chain was not restored at the cut point");
    require(readU64(source->getMemory(), kRecordA + 40) == kAMain + 0x40 &&
                readU64(source->getMemory(), kRecordA + 48) == kF1Continuation &&
                readU64(source->getMemory(), kRecordA + 56) == kF2Continuation,
            "A's suspended guest record lost its outstanding return chain");

    const ArchSnapshot checkpointArch = captureArch(cutCPU, *sourcePlugin);
    const std::vector<uint8_t> checkpointRam = readRam(*source);
    const uint64_t checkpointCycles = source->getCyclesExecuted();
    IA64ISAPluginCheckpointTestAccess::allowDiagnosticWrite(*sourcePlugin);
    std::string error;
    require(source->writeDiagnosticCheckpoint(checkpoint.string(),
                                              "ia64-r7-a-suspended-b-active", &error),
            "suspended-context checkpoint write failed: " + error);

    const RunEvidence reference =
        continueToCompletion(*source, switchReturnIP, *sourcePlugin);
    require(reference.sawARootReturn && reference.sawBRootReturn &&
                !reference.fillsFromA.empty() && !reference.fillsFromB.empty(),
            "suspended-context reference did not resume both contexts and backing stores");
    const ArchSnapshot finalArch = captureArch(sourceCPU.getState(), *sourcePlugin);
    const std::vector<uint8_t> finalRam = readRam(*source);
    preserveTrace(tracePath, reference.trace);
    source.reset();

    auto restored = std::make_unique<VirtualMachine>(kGuestRamSize, 1, "IA-64");
    require(restored->init(), "suspended-context fresh VM initialization failed");
    CPU& restoredCPU = restored->getCPU();
    auto* restoredPlugin = dynamic_cast<IA64ISAPlugin*>(restoredCPU.getISAPlugin());
    require(restoredPlugin != nullptr,
            "suspended-context fresh VM has no IA-64 plugin");
    writeU64(restored->getMemory(), kBackingA, 0xEEEEEEEEEEEEEEEEULL);
    writeU64(restored->getMemory(), kBackingB, 0xDDDDDDDDDDDDDDDDULL);
    require(restored->readDiagnosticCheckpoint(checkpoint.string(),
                                               "ia64-r7-a-suspended-b-active", &error),
            "suspended-context checkpoint restore failed: " + error);
    require(captureArch(restoredCPU.getState(), *restoredPlugin) == checkpointArch,
            "suspended-context architectural state did not restore exactly");
    compareKeyGuestMemory(checkpointRam, readRam(*restored),
                          "suspended-context checkpoint restore");
    require(restored->getCyclesExecuted() == checkpointCycles,
            "suspended-context VM cycle count did not restore");

    const RunEvidence resumed =
        continueToCompletion(*restored, switchReturnIP, *restoredPlugin);
    require(resumed.switchTargets == reference.switchTargets,
            "suspended-context switch targets differ after restore");
    require(resumed.fillsFromA == reference.fillsFromA &&
                resumed.fillsFromB == reference.fillsFromB,
            "suspended-context fill provenance differs after restore");
    compareTrace(reference.trace, resumed.trace,
                 "A-suspended/B-active checkpoint continuation");
    require(captureArch(restoredCPU.getState(), *restoredPlugin) == finalArch,
            "suspended-context final architecture differs after restore");
    compareKeyGuestMemory(finalRam, readRam(*restored),
                          "suspended-context final continuation");
    std::cout << "A-suspended/B-active cut passed: continuation instructions="
              << resumed.trace.size() << ", traceHash=0x" << std::hex
              << traceHash(resumed.trace) << std::dec << '\n';
    restored.reset();
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc > 0 && argv != nullptr, "test executable path is unavailable");
        const std::filesystem::path outputDir =
            std::filesystem::absolute(argv[0]).parent_path();
        std::filesystem::create_directories(outputDir);
        const std::filesystem::path checkpoint =
            outputDir / "ia64-r7-context-continuation.gxcp";
        const std::filesystem::path tracePath =
            outputDir / "ia64-r7-reference-continuation.csv";

        testNoPressureNestedCallCheckpoint(outputDir);
        ReferenceResult reference = createCheckpointAndReference(checkpoint, tracePath);
        restoreAndCompare(checkpoint, reference, 1);
        restoreAndCompare(checkpoint, reference, 2);
        testSuspendedContextCutPoint(outputDir);

        const uint64_t hash = traceHash(reference.continuation.trace);
        std::cout << "IA64-POST-MADT-9R7 checkpoint continuation passed: "
                  << "two guest-managed contexts, nested returns, automatic RSE spill/fill, "
                  << "RNAT boundary, fresh VM restores=2, continuation instructions="
                  << reference.continuation.trace.size() << ", traceHash=0x"
                  << std::hex << hash << std::dec << ", checkpoint="
                  << checkpoint.string() << ", referenceTrace=" << tracePath.string()
                  << '\n';
        std::cout << "Pre-save active-A RSE: cfm=0x" << std::hex
                  << reference.cfmAtCheckpoint << " rsc=0x" << reference.rscAtCheckpoint
                  << " bsp=0x" << reference.bspAtCheckpoint << " bspstore=0x"
                  << reference.maxCheckpointBspstoreA << " rnat=0x"
                  << reference.rnatAtCheckpoint << " pfs=0x" << reference.pfsAtCheckpoint
                  << " bspLoad=0x" << reference.bspLoadAtCheckpoint << std::dec
                  << " BOF/store/load=" << static_cast<unsigned>(reference.bofAtCheckpoint)
                  << '/' << static_cast<unsigned>(reference.storeRegAtCheckpoint)
                  << '/' << static_cast<unsigned>(reference.loadRegAtCheckpoint)
                  << " SOF/SOL/SOR=" << static_cast<unsigned>(reference.sofAtCheckpoint)
                  << '/' << static_cast<unsigned>(reference.solAtCheckpoint)
                  << '/' << static_cast<unsigned>(reference.sorAtCheckpoint)
                  << " partitions(current/clean/dirty/invalid)="
                  << reference.currentAtCheckpoint << '/' << reference.cleanAtCheckpoint
                  << '/' << reference.dirtyAtCheckpoint << '/'
                  << reference.invalidAtCheckpoint << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IA64 checkpoint context continuation failed: " << error.what() << '\n';
        return 1;
    }
}
