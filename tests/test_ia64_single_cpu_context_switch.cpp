#include "IA64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "decoder.h"
#include "memory.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ia64;

constexpr uint64_t kAMain = 0x10000;
constexpr uint64_t kBMain = 0x11000;
constexpr uint64_t kNested = 0x12000;
constexpr uint64_t kSwitcher = 0x13000;
constexpr uint64_t kRecordA = 0x6000;
constexpr uint64_t kRecordB = 0x6100;
constexpr uint64_t kBackingA = 0x8000;
constexpr uint64_t kBackingB = 0x9000;
constexpr uint64_t kVerify = 0x7000;
constexpr uint64_t kVerifyB = 0x7200;
constexpr uint64_t kGuestRamSize = 1U << 20;
constexpr uint64_t kPressureMain = 0x14000;
constexpr uint64_t kPressureF1 = 0x15000;
constexpr uint64_t kPressureF2 = 0x16000;
constexpr uint64_t kPressureBacking = 0xA000;
constexpr uint64_t kPressureVerify = 0xB000;
constexpr uint64_t kSwitchPressureF1 = 0x17000;
constexpr uint64_t kSwitchPressureF2 = 0x17100;

constexpr uint8_t kMiiStop = 0x01;
constexpr uint8_t kMibStop = 0x11;
constexpr uint64_t kSlotMask = (1ULL << 41) - 1;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

std::array<uint8_t, 16> makeBundle(uint8_t templateId,
                                  uint64_t slot0,
                                  uint64_t slot1,
                                  uint64_t slot2) {
    std::array<uint8_t, 16> bundle{};
    putBits(bundle, 0, 5, templateId);
    putBits(bundle, 5, 41, slot0 & kSlotMask);
    putBits(bundle, 46, 41, slot1 & kSlotMask);
    putBits(bundle, 87, 41, slot2 & kSlotMask);
    return bundle;
}

void writeBundle(IMemory& memory,
                 uint64_t ip,
                 uint8_t templateId,
                 uint64_t slot0 = 0,
                 uint64_t slot1 = 0,
                 uint64_t slot2 = 0) {
    const auto bundle = makeBundle(templateId, slot0, slot1, slot2);
    memory.Write(ip, bundle.data(), bundle.size());
}

uint64_t movFromAR(uint8_t destination, uint8_t applicationRegister) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(applicationRegister) << 20) |
           (0x32ULL << 27);
}

uint64_t movToAR(uint8_t applicationRegister, uint8_t source) {
    return (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(applicationRegister) << 20) |
           (0x2AULL << 27);
}

uint64_t alloc(uint8_t destination, uint8_t sof, uint8_t sol, uint8_t sor = 0) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(sof) << 13) |
           (static_cast<uint64_t>(sol) << 20) |
           (static_cast<uint64_t>(sor) << 27) |
           (0x6ULL << 33) |
           (0x1ULL << 37);
}

uint64_t load8(uint8_t destination, uint8_t base, uint16_t offset = 0) {
    require(offset < 512, "synthetic ld8 offset is too large");
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(offset & 0x7FU) << 13) |
           (static_cast<uint64_t>(base) << 20) |
           (static_cast<uint64_t>((offset >> 7) & 1U) << 27) |
           (static_cast<uint64_t>(0x03) << 30) |
           (static_cast<uint64_t>((offset >> 8) & 1U) << 36) |
           (0x5ULL << 37);
}

uint64_t store8(uint8_t base, uint8_t source, uint16_t offset = 0) {
    require(offset < 512, "synthetic st8 offset is too large");
    return (static_cast<uint64_t>(offset & 0x7FU) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(base) << 20) |
           (static_cast<uint64_t>((offset >> 7) & 1U) << 27) |
           (static_cast<uint64_t>(0x33) << 30) |
           (static_cast<uint64_t>((offset >> 8) & 1U) << 36) |
           (0x5ULL << 37);
}

uint64_t movGR(uint8_t destination, uint8_t source) {
    // IA-64 mov rD=rS is the register OR form rD=rS|r0.
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (0x2ULL << 27) |
           (0x3ULL << 29) |
           (0x8ULL << 37);
}

uint64_t andImmediate(uint8_t destination,
                      uint8_t source,
                      int8_t immediate) {
    // A3 AND immediate is imm8 & source.  A sign-extended -4 mask clears
    // RSC's low mode bits while preserving its other architectural fields.
    const uint8_t encoded = static_cast<uint8_t>(immediate);
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(encoded & 0x7FU) << 13) |
           (static_cast<uint64_t>(source) << 20) |
           (0xBULL << 29) |
           (0x8ULL << 37) |
           (static_cast<uint64_t>((encoded >> 7) & 1U) << 36);
}

uint64_t movFromBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (0x31ULL << 27);
}

uint64_t movToBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (0x7ULL << 33);
}

uint64_t flushrs() {
    return 0xCULL << 27;
}

uint64_t invala() {
    return 1ULL << 31;
}

uint64_t directBranch(uint8_t major,
                      uint8_t branchRegister,
                      uint64_t ip,
                      uint64_t target) {
    require((ip & 0xFULL) == 0 && (target & 0xFULL) == 0,
            "synthetic branch endpoints must be bundle aligned");
    const int64_t displacement =
        (static_cast<int64_t>(target) - static_cast<int64_t>(ip)) / 16;
    require(displacement >= -(1LL << 20) && displacement < (1LL << 20),
            "synthetic branch displacement is out of range");
    const uint64_t encoded = static_cast<uint64_t>(displacement) & 0x1FFFFFULL;
    return (static_cast<uint64_t>(branchRegister & 0x7U) << 6) |
           ((encoded & 0xFFFFFULL) << 13) |
           (((encoded >> 20) & 1ULL) << 36) |
           (static_cast<uint64_t>(major) << 37);
}

uint64_t indirectReturn(uint8_t source) {
    return (0x4ULL << 6) |
           (static_cast<uint64_t>(source & 0x7U) << 13) |
           (0x4ULL << 27) |
           (0x1ULL << 37);
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

std::string hex(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

struct TraceEntry {
    uint64_t ip = 0;
    size_t slot = 0;
    uint64_t raw = 0;
    InstructionType type = InstructionType::UNKNOWN;
    std::string disassembly;
    uint64_t cfm = 0;
    uint64_t pfs = 0;
    uint64_t bsp = 0;
    uint64_t bspstore = 0;
    uint64_t rsc = 0;
    uint64_t rnat = 0;
    uint8_t bof = 0;
    std::array<uint64_t, 8> stacked{};
    std::array<bool, 8> nats{};
    uint64_t nextIP = 0;
    uint64_t currentContext = 0;
    uint64_t nextContext = 0;
    std::array<uint64_t, 9> switchRegisters{};
};

void emitM(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMiiStop, instruction, 0, 0);
}

void emitI(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMiiStop, 0, instruction, 0);
}

void emitB(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMibStop, 0, 0, instruction);
}

void emitNop(IMemory& memory, uint64_t ip) {
    writeBundle(memory, ip, kMiiStop, 0, 0, 0);
}

void emitNestedFunction(IMemory& memory) {
    // Keep the alloc destination inside the six-register frame. r38 would be
    // the seventh stacked register and sits outside CFM.SOF=6.
    emitM(memory, kNested + 0x00, alloc(37, 6, 4));
    emitI(memory, kNested + 0x10, movGR(34, 26));
    emitI(memory, kNested + 0x20, movGR(35, 27));
    emitB(memory, kNested + 0x50,
          directBranch(5, 1, kNested + 0x50, kSwitcher));
    emitM(memory, kNested + 0x60, store8(28, 34, 8));
    emitM(memory, kNested + 0x70, store8(28, 35, 8));
    emitB(memory, kNested + 0x80, indirectReturn(0));
}

void emitMainA(IMemory& memory) {
    emitI(memory, kAMain + 0x00, movGR(36, 30));
    emitI(memory, kAMain + 0x10, movGR(37, 31));
    emitB(memory, kAMain + 0x20,
          directBranch(5, 0, kAMain + 0x20, kNested));
    emitM(memory, kAMain + 0x30, store8(28, 32, 8));
    emitM(memory, kAMain + 0x40, store8(28, 33, 8));
    emitM(memory, kAMain + 0x50, store8(28, 34, 8));
    emitM(memory, kAMain + 0x60, store8(28, 35, 8));
    emitM(memory, kAMain + 0x70, store8(28, 12, 8));
    emitI(memory, kAMain + 0x80, movGR(36, 30));
    emitI(memory, kAMain + 0x90, movGR(37, 31));
    emitB(memory, kAMain + 0xA0,
          directBranch(5, 0, kAMain + 0xA0, kNested));
    emitM(memory, kAMain + 0xB0, store8(28, 32, 8));
    emitM(memory, kAMain + 0xC0, store8(28, 33, 8));
    emitM(memory, kAMain + 0xD0, store8(28, 34, 8));
    emitM(memory, kAMain + 0xE0, store8(28, 35, 8));
    emitM(memory, kAMain + 0xF0, store8(28, 12, 8));
    emitNop(memory, kAMain + 0x100);
}

void emitMainB(IMemory& memory) {
    emitI(memory, kBMain + 0x00, movGR(36, 31));
    emitI(memory, kBMain + 0x10, movGR(37, 30));
    emitB(memory, kBMain + 0x20,
          directBranch(5, 0, kBMain + 0x20, kNested));
    emitM(memory, kBMain + 0x30, store8(28, 32, 8));
    emitM(memory, kBMain + 0x40, store8(28, 33, 8));
    emitM(memory, kBMain + 0x50, store8(28, 34, 8));
    emitM(memory, kBMain + 0x60, store8(28, 35, 8));
    emitM(memory, kBMain + 0x70, store8(28, 12, 8));
    emitM(memory, kBMain + 0x80, store8(28, 1, 8));
    emitM(memory, kBMain + 0x90, store8(28, 13, 8));
    emitB(memory, kBMain + 0xA0,
          directBranch(5, 0, kBMain + 0xA0, kNested));
    emitM(memory, kBMain + 0xB0, store8(28, 32, 8));
    emitM(memory, kBMain + 0xC0, store8(28, 33, 8));
    emitM(memory, kBMain + 0xD0, store8(28, 34, 8));
    emitM(memory, kBMain + 0xE0, store8(28, 35, 8));
    emitM(memory, kBMain + 0xF0, store8(28, 12, 8));
    emitNop(memory, kBMain + 0x100);
}

void emitSwitcher(IMemory& memory) {
    uint64_t ip = kSwitcher;
    auto m = [&](uint64_t instruction) {
        emitM(memory, ip, instruction);
        ip += 16;
    };
    auto i = [&](uint64_t instruction) {
        emitI(memory, ip, instruction);
        ip += 16;
    };

    // alloc captures the caller's PFS in r40 before installing the switcher's
    // own frame. r40 is the architectural PFS value the synchronous switch
    // procedure must preserve for the incoming br.ret.
    m(alloc(40, 40, 32));
    m(movGR(57, 30)); // outgoing context save cursor
    m(movFromAR(41, 16)); // RSC
    m(movFromAR(42, 17)); // BSP
    m(store8(57, 41, 8));
    m(store8(57, 42, 8));
    m(store8(57, 40, 8)); // saved caller PFS from alloc

    // flushrs is first in its own instruction group and in slot 0.
    m(flushrs());
    i(andImmediate(43, 41, -4));
    m(movToAR(16, 43)); // enforced-lazy mode, preserving other RSC fields
    m(movFromAR(44, 19)); // RNAT after flushrs
    m(movFromAR(45, 18)); // BSPSTORE after flushrs
    m(store8(57, 44, 8));
    m(store8(57, 45, 8));
    i(movFromBR(46, 0));
    i(movFromBR(47, 1));
    m(store8(57, 46, 8));
    m(store8(57, 47, 8));
    m(store8(57, 1, 8));  // GP
    m(store8(57, 12, 8)); // SP
    m(store8(57, 13, 8)); // thread pointer
    m(store8(57, 28, 8)); // context-owned verification/static cursor
    m(store8(57, 26, 8)); // context-owned static marker
    m(store8(57, 27, 8));
    m(invala());

    // Read the incoming record with legal M5 post-increment operations. The
    // context's architectural BSP is restored after BSPSTORE is switched.
    m(movGR(58, 31));
    m(load8(51, 58, 8)); // RSC
    m(load8(52, 58, 8)); // BSP
    m(load8(49, 58, 8)); // PFS
    m(load8(50, 58, 8)); // RNAT
    m(load8(48, 58, 8)); // BSPSTORE
    m(movToAR(16, 0)); // enforced-lazy mode while changing BSPSTORE
    m(movToAR(18, 48));
    m(movToAR(17, 52));
    m(movToAR(64, 49));
    m(movToAR(19, 50));
    m(movToAR(16, 51));
    m(load8(46, 58, 8));
    i(movToBR(0, 46));
    m(load8(47, 58, 8));
    i(movToBR(1, 47));
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
    emitB(memory, ip, indirectReturn(1));
}

void seedBackingRegister(IMemory& memory,
                         uint64_t address,
                         uint64_t value,
                         uint64_t& rnat,
                         bool nat) {
    writeU64(memory, address, value);
    const unsigned bit = static_cast<unsigned>((address >> 3) & 0x3FULL);
    if (nat) rnat |= 1ULL << bit;
    else rnat &= ~(1ULL << bit);
}

void seedContextB(IMemory& memory) {
    uint64_t rnat = 0;
    seedBackingRegister(memory, kBackingB + 0, 0xB0B0B0B0B0B0B0B0ULL, rnat, false);
    seedBackingRegister(memory, kBackingB + 8, 0xB1B1B1B1B1B1B1B1ULL, rnat, false);
    seedBackingRegister(memory, kBackingB + 16, 0xB2B2B2B2B2B2B2B2ULL, rnat, false);
    seedBackingRegister(memory, kBackingB + 24, 0xB3B3B3B3B3B3B3B3ULL, rnat, true);
    writeU64(memory, kBackingB + 0x1F8, rnat);

    // B is a ready-to-resume synthetic guest context: PFS names the four
    // register locals, BSPSTORE is past their guest backing-store image, and
    // b1 names the guest instruction address at which B should start.
    writeU64(memory, kRecordB + 0, 0); // RSC
    writeU64(memory, kRecordB + 8, kBackingB + 32); // saved BSP
    writeU64(memory, kRecordB + 16, 6ULL | (4ULL << 7)); // PFS: SOF=6, SOL=4
    writeU64(memory, kRecordB + 24, rnat);
    writeU64(memory, kRecordB + 32, kBackingB + 32); // BSPSTORE
    writeU64(memory, kRecordB + 40, 0); // b0
    writeU64(memory, kRecordB + 48, kBMain); // b1 entry
    writeU64(memory, kRecordB + 56, 0xB100000000000001ULL); // GP
    writeU64(memory, kRecordB + 64, 0xB200000000000002ULL); // SP
    writeU64(memory, kRecordB + 72, 0xB300000000000003ULL); // thread pointer
    writeU64(memory, kRecordB + 80, kVerifyB); // B verification cursor
    writeU64(memory, kRecordB + 88, 0xB4B4B4B4B4B4B4B4ULL); // static marker 0
    writeU64(memory, kRecordB + 96, 0xB5B5B5B5B5B5B5B5ULL); // static marker 1
}

void installGuest(VirtualMachine& vm) {
    IMemory& memory = vm.getMemory();
    emitMainA(memory);
    emitMainB(memory);
    emitNestedFunction(memory);
    emitSwitcher(memory);

    // The context save areas and verification slots live in guest RAM. The
    // synthetic program writes its outgoing context state through st8/mov-to-AR.
    for (uint64_t offset = 0; offset < 256; offset += 8) {
        writeU64(memory, kRecordA + offset, 0);
        writeU64(memory, kRecordB + offset, readU64(memory, kRecordB + offset));
        writeU64(memory, kVerify + offset, 0xCCCCCCCCCCCCCCCCULL);
        writeU64(memory, kVerifyB + offset, 0xDDDDDDDDDDDDDDDDULL);
    }
    seedContextB(memory);
    writeU64(memory, kRecordA + 80, kVerify);
    writeU64(memory, kRecordA + 88, 0xA4A4A4A4A4A4A4A4ULL);
    writeU64(memory, kRecordA + 96, 0xA5A5A5A5A5A5A5A5ULL);
}

void dumpTrace(const std::vector<TraceEntry>& trace) {
    std::cerr << "\nIA-64 one-CPU guest trace (instruction, RSE state, stacked GR32-39, NaT):\n";
    std::vector<const TraceEntry*> recent;
    for (auto it = trace.rbegin(); it != trace.rend() && recent.size() < 100; ++it) {
        if (it->type != InstructionType::NOP) recent.push_back(&*it);
    }
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        const TraceEntry& row = **it;
        std::cerr << "ip=" << hex(row.ip)
                  << " slot=" << row.slot
                  << " raw=" << hex(row.raw)
                  << " type=" << static_cast<unsigned>(row.type)
                  << " insn=\"" << row.disassembly << "\""
                  << " cfm=" << hex(row.cfm)
                  << " pfs=" << hex(row.pfs)
                  << " bsp=" << hex(row.bsp)
                  << " bspstore=" << hex(row.bspstore)
                  << " rsc=" << hex(row.rsc)
                  << " rnat=" << hex(row.rnat)
                  << " ctx=" << hex(row.currentContext)
                  << " next=" << hex(row.nextContext)
                  << " bof=" << static_cast<unsigned>(row.bof)
                  << " nextIP=" << hex(row.nextIP)
                  << " switchRegs=[";
        for (size_t i = 0; i < row.switchRegisters.size(); ++i) {
            if (i) std::cerr << ',';
            std::cerr << hex(row.switchRegisters[i]);
        }
        std::cerr << "] gr=[";
        for (size_t i = 0; i < row.stacked.size(); ++i) {
            if (i) std::cerr << ',';
            std::cerr << hex(row.stacked[i]) << (row.nats[i] ? "N" : "");
        }
        std::cerr << "]\n";
    }
}

void testReturnFillUsesLiveRnatCollection() {
    constexpr uint64_t backing = 0x8000;
    Memory memory(0x10000);
    CPUState state;
    state.SetCFM(6ULL | (4ULL << 7));
    state.SetPFS(6ULL | (4ULL << 7));
    state.SetBSP(backing + 32);
    state.SetBSPSTORE(backing + 32);
    state.SetRNAT(1ULL << 3);
    state.SetRSEBOFForCheckpoint(4);

    for (size_t index = 0; index < 4; ++index) {
        writeU64(memory, backing + index * 8,
                 0xD000000000000000ULL + static_cast<uint64_t>(index));
    }
    InstructionDecoder decoder;
    const InstructionEx ret = decoder.DecodeSlot(
        indirectReturn(0), UnitType::B_UNIT, 0x1000);
    require(ret.GetType() == InstructionType::BR_RET,
            "focused RNAT return regression did not decode br.ret");
    ret.Execute(state, memory);

    for (size_t index = 0; index < 4; ++index) {
        require(state.GetGR(32 + index) ==
                    0xD000000000000000ULL + static_cast<uint64_t>(index),
                "br.ret fill restored the wrong backing-store value");
    }
    require(state.GetGRNaT(35),
            "br.ret fill ignored the live AR.RNAT collection for BSPSTORE's region");
    require(state.GetRNAT() == (1ULL << 3),
            "br.ret fill replaced the live RNAT collection with an unwritten RAM slot");
    require(state.GetBSP() == backing && state.GetBSPSTORE() == backing,
            "br.ret fill did not restore the expected BSP/BSPSTORE return boundary");
    require(readU64(memory, backing + 0x1F8) == 0,
            "focused regression unexpectedly materialized a backing-store RNAT slot");
}

void testBoundedAutomaticRSEPressureProbe() {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "pressure-probe VM initialization failed");
    CPU& cpu = vm.getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "pressure-probe VM did not select IA64ISAPlugin");
    plugin->reset();

    IMemory& memory = vm.getMemory();
    emitB(memory, kPressureMain,
          directBranch(5, 1, kPressureMain, kPressureF1));
    emitM(memory, kPressureMain + 0x10, store8(28, 32, 8));
    emitM(memory, kPressureMain + 0x20, store8(28, 64, 8));
    emitNop(memory, kPressureMain + 0x30);

    emitM(memory, kPressureF1, alloc(40, 64, 64));
    uint64_t f1ip = kPressureF1 + 0x10;
    for (uint8_t reg = 64; reg < 96; ++reg, f1ip += 0x10) {
        emitI(memory, f1ip, movGR(reg, 30));
    }
    emitB(memory, f1ip, directBranch(5, 2, f1ip, kPressureF2));
    emitM(memory, f1ip + 0x10, movToAR(64, 40));
    emitB(memory, f1ip + 0x20, indirectReturn(1));

    emitM(memory, kPressureF2, alloc(41, 64, 64));
    uint64_t f2ip = kPressureF2 + 0x10;
    for (uint8_t reg = 32; reg < 40; ++reg, f2ip += 0x10) {
        emitI(memory, f2ip, movGR(reg, 30));
    }
    emitM(memory, f2ip, movToAR(64, 41));
    emitB(memory, f2ip + 0x10, indirectReturn(2));

    constexpr uint64_t kInitialBackingWord = 0xABABABABABABABABULL;
    for (uint64_t offset = 0; offset < 0x220; offset += 8) {
        writeU64(memory, kPressureBacking + offset, kInitialBackingWord);
    }
    CPUState& state = cpu.getState();
    state.SetCFM(64ULL | (64ULL << 7));
    state.SetRSC(0);
    state.SetBSP(kPressureBacking);
    state.SetBSPSTORE(kPressureBacking);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(0);
    state.SetGR(28, kPressureVerify);
    state.SetGR(30, 0xDEADDEADDEADDEADULL);
    for (size_t index = 0; index < 64; ++index) {
        state.SetGR(32 + index, 0xC000000000000000ULL + index);
    }
    cpu.setIP(kPressureMain);

    uint64_t maximumBspstore = state.GetBSPSTORE();
    size_t steps = 0;
    std::string failure;
    constexpr size_t kMaximumSteps = 1500;
    for (; steps < kMaximumSteps && state.GetIP() != kPressureMain + 0x30; ++steps) {
        try {
            if (!vm.stepCPU(0)) {
                failure = "VM stopped before the post-pressure continuation";
                break;
            }
        } catch (const std::exception& error) {
            failure = error.what();
            break;
        }
        maximumBspstore = std::max(maximumBspstore, state.GetBSPSTORE());
    }

    const bool completed = state.GetIP() == kPressureMain + 0x30;
    const bool mainValuesRestored =
        state.GetGR(32) == 0xC000000000000000ULL &&
        state.GetGR(64) == 0xC000000000000020ULL;
    bool backingStoreChanged = false;
    for (uint64_t offset = 0; offset < 0x220; offset += 8) {
        if (readU64(memory, kPressureBacking + offset) != kInitialBackingWord) {
            backingStoreChanged = true;
            break;
        }
    }

    require(completed,
            "automatic RSE pressure probe did not return to the caller continuation" +
                (failure.empty() ? std::string{} : ": " + failure));
    require(mainValuesRestored,
            "automatic RSE pressure probe did not restore the distinguished caller registers");
    require(maximumBspstore > kPressureBacking || backingStoreChanged,
            "pressure values survived without any evidence of backing-store transfer");
    std::cout << "automatic RSE pressure probe passed: spill and return fill restored "
              << "the two distinguished caller registers\n";
}

void testGuestDrivenSingleCPUContextSwitch() {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "VirtualMachine::init failed");
    CPU& cpu = vm.getCPU();
    require(cpu.isUsingISAPlugin(), "VM did not select the production IA-64 ISA plugin");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "VM CPU does not own an IA64ISAPlugin");

    // VM initialization may have probed the firmware handoff layout and left
    // the production plugin with a cached bundle cursor. Reset the same
    // plugin once before installing the synthetic guest, then keep it and its
    // CPUState alive for every context transition below.
    plugin->reset();
    installGuest(vm);
    CPUState& state = cpu.getState();
    const CPUState* const persistentCPU = &state;

    // Begin A in one active frame. Context B is represented only by its guest
    // record/backing-store image; no second CPUState or host snapshot exists.
    state.SetCFM(6ULL | (4ULL << 7));
    state.SetRSC(0);
    state.SetBSP(kBackingA);
    state.SetBSPSTORE(kBackingA);
    state.SetRNAT(1ULL << 3);
    state.SetPFS(0);
    state.SetGR(30, kRecordA);
    state.SetGR(31, kRecordB);
    state.SetGR(28, kVerify);
    state.SetGR(26, 0xA4A4A4A4A4A4A4A4ULL);
    state.SetGR(27, 0xA5A5A5A5A5A5A5A5ULL);
    state.SetGR(1, 0xA100000000000001ULL);
    state.SetGR(12, 0xA200000000000002ULL);
    state.SetGR(13, 0xA300000000000003ULL);
    state.SetGR(32, 0xA0A0A0A0A0A0A0A0ULL);
    state.SetGR(33, 0xA1A1A1A1A1A1A1A1ULL);
    state.SetGR(34, 0xA2A2A2A2A2A2A2A2ULL);
    state.SetGR(35, 0xA3A3A3A3A3A3A3A3ULL);
    state.SetGRNaT(35, true);
    cpu.setIP(kAMain);

    InstructionDecoder traceDecoder;
    std::vector<TraceEntry> trace;
    std::vector<uint64_t> switchReturnTargets;
    constexpr size_t kMaximumInstructions = 3000;
    bool reachedACompletion = false;
    std::string failure;

    for (size_t step = 0; step < kMaximumInstructions; ++step) {
        require(&cpu.getState() == persistentCPU,
                "CPUState identity changed during guest execution");
        const uint64_t ip = state.GetIP();
        if (ip == kAMain + 0x100) {
            reachedACompletion = true;
            break;
        }

        std::array<uint8_t, 16> bytes{};
        vm.getMemory().Read(ip, bytes.data(), bytes.size());
        const Bundle bundle = traceDecoder.DecodeBundleAt(bytes.data(), ip);
        // CPU::getCurrentSlot() tracks the legacy interpreter path.  The
        // production plugin owns the slot cursor for this VM, so use it to
        // make the trace describe the instruction vm.stepCPU() will execute.
        const size_t pluginSlot = plugin->getCurrentSlot();
        const size_t slot = pluginSlot < bundle.instructions.size() ? pluginSlot : 0;
        require(slot < bundle.instructions.size(), "CPU slot exceeded decoded bundle");
        const InstructionEx& instruction = bundle.instructions[slot];

        TraceEntry row;
        row.ip = ip;
        row.slot = slot;
        row.raw = instruction.GetRawBits();
        row.type = instruction.GetType();
        row.disassembly = instruction.GetDisassembly();
        row.cfm = state.GetCFM();
        row.pfs = state.GetPFS();
        row.bsp = state.GetBSP();
        row.bspstore = state.GetBSPSTORE();
        row.rsc = state.GetRSC();
        row.rnat = state.GetRNAT();
        row.bof = state.GetRSEBOF();
        row.currentContext = state.GetGR(30);
        row.nextContext = state.GetGR(31);
        for (size_t i = 0; i < row.switchRegisters.size(); ++i) {
            row.switchRegisters[i] = state.GetGR(48 + i);
        }
        for (size_t i = 0; i < row.stacked.size(); ++i) {
            row.stacked[i] = state.GetGR(32 + i);
            row.nats[i] = state.GetGRNaT(32 + i);
        }

        const uint64_t priorIP = state.GetIP();
        const size_t priorSlot = slot;
        const bool isSwitchRet = ip >= kSwitcher &&
            ip < kSwitcher + 0x1000 && instruction.GetType() == InstructionType::BR_RET;

        try {
            if (!vm.stepCPU(0)) {
                failure = "VM step failed at " + hex(priorIP) + ":" +
                          std::to_string(priorSlot);
                row.nextIP = state.GetIP();
                trace.push_back(row);
                break;
            }
        } catch (const std::exception& error) {
            failure = "exception at " + hex(priorIP) + ":" +
                      std::to_string(priorSlot) + ": " + error.what();
            row.nextIP = state.GetIP();
            trace.push_back(row);
            break;
        }

        row.nextIP = state.GetIP();
        trace.push_back(row);
        if (isSwitchRet && state.GetIP() != priorIP) {
            switchReturnTargets.push_back(state.GetIP());
        }
    }

    if (!reachedACompletion) {
        dumpTrace(trace);
        std::ostringstream context;
        context << "A did not reach its second post-call continuation";
        if (!failure.empty()) context << "; " << failure;
        context << "; finalIP=" << hex(state.GetIP())
                << " cfm=" << hex(state.GetCFM())
                << " pfs=" << hex(state.GetPFS())
                << " bsp=" << hex(state.GetBSP())
                << " bspstore=" << hex(state.GetBSPSTORE());
        throw std::runtime_error(context.str());
    }

    const IMemory& memory = vm.getMemory();
    try {
        require(&cpu.getState() == persistentCPU,
                "guest-driven A/B switching replaced the persistent CPUState");
        require(switchReturnTargets.size() >= 4,
                "guest did not execute the A -> B -> A -> B -> A return sequence");
        require(switchReturnTargets[0] == kBMain,
                "first guest context switch did not enter B's saved instruction pointer");
        require(switchReturnTargets[1] == kNested + 0x60 &&
                    switchReturnTargets[2] == kNested + 0x60 &&
                    switchReturnTargets[3] == kNested + 0x60,
                "A/B did not resume outstanding calls through their shared return target");

        require(readU64(memory, kVerify + 0) == 0xA4A4A4A4A4A4A4A4ULL &&
                    readU64(memory, kVerify + 8) == 0xA5A5A5A5A5A5A5A5ULL,
                "A's suspended nested locals were not restored");
        require(readU64(memory, kVerify + 16) == 0xA0A0A0A0A0A0A0A0ULL &&
                    readU64(memory, kVerify + 24) == 0xA1A1A1A1A1A1A1A1ULL &&
                    readU64(memory, kVerify + 32) == 0xA2A2A2A2A2A2A2A2ULL &&
                    readU64(memory, kVerify + 40) == 0xA3A3A3A3A3A3A3A3ULL &&
                    readU64(memory, kVerify + 48) == 0xA200000000000002ULL,
                "A's original main register frame or SP was not restored");
        require(readU64(memory, kVerify + 56) == 0xA4A4A4A4A4A4A4A4ULL &&
                    readU64(memory, kVerify + 64) == 0xA5A5A5A5A5A5A5A5ULL,
                "A's second suspended nested frame did not retain its locals");
        require(readU64(memory, kVerify + 72) == 0xA0A0A0A0A0A0A0A0ULL &&
                    readU64(memory, kVerify + 80) == 0xA1A1A1A1A1A1A1A1ULL &&
                    readU64(memory, kVerify + 88) == 0xA2A2A2A2A2A2A2A2ULL &&
                    readU64(memory, kVerify + 96) == 0xA3A3A3A3A3A3A3A3ULL &&
                    readU64(memory, kVerify + 104) == 0xA200000000000002ULL,
                "A's second main frame did not resume correctly");
        require(state.GetGRNaT(35),
                "A main frame GR35 NaT state was not restored after its nested return");
        const bool bMainNatRestored = std::any_of(
            trace.begin(), trace.end(), [](const TraceEntry& row) {
                return row.ip == kBMain + 0x30 && row.nats[3];
            });
        require(bMainNatRestored,
                "B main frame GR35 NaT state was not restored after its nested return");
        require(readU64(memory, kVerifyB + 0) == 0xB4B4B4B4B4B4B4B4ULL &&
                    readU64(memory, kVerifyB + 8) == 0xB5B5B5B5B5B5B5B5ULL,
                "B's suspended nested locals were not restored");
        require(readU64(memory, kVerifyB + 16) == 0xB0B0B0B0B0B0B0B0ULL &&
                    readU64(memory, kVerifyB + 24) == 0xB1B1B1B1B1B1B1B1ULL &&
                    readU64(memory, kVerifyB + 32) == 0xB2B2B2B2B2B2B2B2ULL &&
                    readU64(memory, kVerifyB + 40) == 0xB3B3B3B3B3B3B3B3ULL &&
                    readU64(memory, kVerifyB + 48) == 0xB200000000000002ULL,
                "B's main frame or SP was not restored after A -> B -> A");
        require(readU64(memory, kVerifyB + 56) == 0xB100000000000001ULL &&
                    readU64(memory, kVerifyB + 64) == 0xB300000000000003ULL,
                "B's GP or TP was not restored after A -> B -> A");
        require(readU64(memory, kRecordB + 80) == kVerifyB + 72 &&
                    readU64(memory, kRecordB + 88) == 0xB4B4B4B4B4B4B4B4ULL &&
                    readU64(memory, kRecordB + 96) == 0xB5B5B5B5B5B5B5B5ULL,
                "B's second suspended call did not retain its static context state");
        require(readU64(memory, kRecordA + 48) == kNested + 0x60 &&
                    readU64(memory, kRecordB + 48) == kNested + 0x60,
                "guest records did not preserve identical nested return addresses");
        require(readU64(memory, kRecordA + 40) == kAMain + 0xB0 &&
                    readU64(memory, kRecordB + 40) == kBMain + 0xB0,
                "guest records did not preserve each context's outer main return address");
        require(readU64(memory, kRecordA + 0) == 0 && readU64(memory, kRecordB + 0) == 0,
                "guest records did not preserve the original RSC values");
        require(readU64(memory, kRecordA + 16) == (6ULL | (4ULL << 7)) &&
                    readU64(memory, kRecordB + 16) == (6ULL | (4ULL << 7)),
                "guest records did not preserve the suspended PFS/CFM values");
        require(readU64(memory, kRecordA + 32) == readU64(memory, kRecordA + 8) &&
                    readU64(memory, kRecordB + 32) == readU64(memory, kRecordB + 8),
                "a guest BSPSTORE does not match its flushed BSP boundary");
        require((readU64(memory, kRecordA + 24) & (1ULL << 3)) != 0 &&
                    (readU64(memory, kRecordB + 24) & (1ULL << 3)) != 0,
                "guest switch records lost the frame RNAT bit");
        require(readU64(memory, kBackingB + 0) == 0xB0B0B0B0B0B0B0B0ULL,
                "A's transfer overwrote B's backing-store contents");
        require(readU64(memory, kRecordA + 56) == 0xA100000000000001ULL &&
                    state.GetGR(1) == 0xA100000000000001ULL &&
                    readU64(memory, kRecordA + 64) == 0xA200000000000002ULL &&
                    state.GetGR(12) == 0xA200000000000002ULL &&
                    readU64(memory, kRecordA + 72) == 0xA300000000000003ULL &&
                    state.GetGR(13) == 0xA300000000000003ULL,
                "A's guest-saved GP/SP/TP were not restored");
        require(readU64(memory, kRecordB + 56) == 0xB100000000000001ULL &&
                    readU64(memory, kRecordB + 64) == 0xB200000000000002ULL &&
                    readU64(memory, kRecordB + 72) == 0xB300000000000003ULL,
                "B's guest-saved GP/SP/TP were not preserved across its second suspend");
    } catch (const std::exception&) {
        dumpTrace(trace);
        throw;
    }

    std::cout << "single-CPU guest context switch test passed: "
              << "A -> B -> A -> B -> A, one persistent VM CPUState, "
              << "shared return target=" << hex(kNested + 0x60)
              << ", trace instructions=" << trace.size() << '\n';
}

void testPressureSurvivesGuestContextSwitch() {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "pressure-switch VM initialization failed");
    CPU& cpu = vm.getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
    require(plugin != nullptr, "pressure-switch VM did not select IA64ISAPlugin");
    plugin->reset();
    installGuest(vm);

    IMemory& memory = vm.getMemory();
    emitB(memory, kAMain,
          directBranch(5, 0, kAMain, kSwitchPressureF1));
    emitM(memory, kAMain + 0x10, store8(28, 32, 8));
    emitM(memory, kAMain + 0x20, store8(28, 95, 8));
    emitNop(memory, kAMain + 0x30);

    emitM(memory, kSwitchPressureF1, alloc(40, 64, 64));
    emitB(memory, kSwitchPressureF1 + 0x10,
          directBranch(5, 1, kSwitchPressureF1 + 0x10, kSwitchPressureF2));
    emitM(memory, kSwitchPressureF1 + 0x20, movToAR(64, 40));
    emitB(memory, kSwitchPressureF1 + 0x30, indirectReturn(0));

    emitM(memory, kSwitchPressureF2, alloc(41, 64, 64));
    emitI(memory, kSwitchPressureF2 + 0x10, movFromBR(46, 1));
    emitB(memory, kSwitchPressureF2 + 0x20,
          directBranch(5, 1, kSwitchPressureF2 + 0x20, kSwitcher));
    emitI(memory, kSwitchPressureF2 + 0x30, movToBR(1, 46));
    emitM(memory, kSwitchPressureF2 + 0x40, movToAR(64, 41));
    emitB(memory, kSwitchPressureF2 + 0x50, indirectReturn(1));

    // B is deliberately shallow and uses its own record and backing-store
    // region. It switches back to A from guest code after one return.
    emitB(memory, kBMain,
          directBranch(5, 1, kBMain, kSwitcher));
    emitNop(memory, kBMain + 0x10);
    emitSwitcher(memory);

    for (uint64_t offset = 0; offset < 0x800; offset += 8) {
        writeU64(memory, kBackingA + offset, 0xABABABABABABABABULL);
    }
    CPUState& state = cpu.getState();
    state.SetCFM(64ULL | (64ULL << 7));
    state.SetPFS(0);
    state.SetRSC(0);
    state.SetBSP(kBackingA);
    state.SetBSPSTORE(kBackingA);
    state.SetRNAT((1ULL << 3) | (1ULL << 62));
    state.SetRSEBOFForCheckpoint(0);
    state.SetGR(30, kRecordA);
    state.SetGR(31, kRecordB);
    state.SetGR(28, kVerify);
    state.SetGR(26, 0xA4A4A4A4A4A4A4A4ULL);
    state.SetGR(27, 0xA5A5A5A5A5A5A5A5ULL);
    state.SetGR(1, 0xA100000000000001ULL);
    state.SetGR(12, 0xA200000000000002ULL);
    state.SetGR(13, 0xA300000000000003ULL);
    for (size_t index = 0; index < 64; ++index) {
        state.SetGR(32 + index, 0xC100000000000000ULL + index);
    }
    state.SetGRNaT(35, true);
    state.SetGRNaT(94, true);
    state.SetGRNaT(95, true);
    cpu.setIP(kAMain);

    auto* ram = dynamic_cast<Memory*>(&memory);
    require(ram != nullptr, "pressure-switch test requires MMU-aware guest memory");
    std::vector<uint64_t> readFromA;
    const size_t readHook = ram->GetMMU().RegisterReadHook(
        [&](HookContext& context, uint8_t*) {
            if (context.address >= kBackingA && context.address < kBackingA + 0x800) {
                readFromA.push_back(context.address);
            }
        });

    constexpr uint64_t completionIP = kAMain + 0x30;
    bool completed = false;
    std::string failure;
    for (size_t step = 0; step < 3000; ++step) {
        if (state.GetIP() == completionIP) {
            completed = true;
            break;
        }
        if (!vm.stepCPU(0)) {
            failure = "VM stopped at IP=" + hex(state.GetIP());
            break;
        }
    }
    ram->GetMMU().UnregisterReadHook(readHook);

    require(completed,
            "pressure context did not resume and unwind A" +
                (failure.empty() ? std::string{} : ": " + failure));
    require(readU64(memory, kVerify) == 0xC100000000000000ULL &&
                readU64(memory, kVerify + 8) == 0xC10000000000003FULL,
            "pressure context switch did not restore A's distinguished root registers");
    require(state.GetGRNaT(35) && state.GetGRNaT(94) && state.GetGRNaT(95),
            "pressure context switch lost A's NaT state across RNAT collections");
    require(std::find(readFromA.begin(), readFromA.end(), kBackingA) != readFromA.end(),
            "A resumed without a guest-memory read of its spilled root frame");
    require(readU64(memory, kBackingA) == 0xC100000000000000ULL,
            "A's spilled root value was not present in A's guest backing store");
    require(readU64(memory, kBackingB) == 0xB0B0B0B0B0B0B0B0ULL &&
                readU64(memory, kBackingB + 24) == 0xB3B3B3B3B3B3B3B3ULL,
            "A's automatic spill or fill contaminated B's backing-store region");
    require(state.GetBSP() == kBackingA && state.GetBSPSTORE() == kBackingA,
            "pressure context unwind did not restore A's original BSP boundary");

    std::cout << "pressure context switch passed: A spilled, guest-switched to B, "
              << "resumed from A backing memory, and unwound\n";
}

} // namespace

int main() {
    try {
        testReturnFillUsesLiveRnatCollection();
        testGuestDrivenSingleCPUContextSwitch();
        testBoundedAutomaticRSEPressureProbe();
        testPressureSurvivesGuestContextSwitch();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "single-CPU guest context switch test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
