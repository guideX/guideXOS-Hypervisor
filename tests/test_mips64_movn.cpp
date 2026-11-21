// MIPS64-33 MOVN architectural qualification and authentic Linux continuation.

#include "IMemory.h"
#include "Mips64FullSnapshot.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kConfig0Pc = 0xFFFFFFFF8010B840ULL;
constexpr uint32_t kConfig0Word = 0x40068000U;
constexpr uint64_t kExtPc = 0xFFFFFFFF8010B85CULL;
constexpr uint32_t kExtWord = 0x7CC30B40U;
constexpr uint64_t kDinsuPc = 0xFFFFFFFF8010BA74ULL;
constexpr uint32_t kDinsuWord = 0x7CA3F806U;
constexpr uint64_t kDinsPc = 0xFFFFFFFF8010BA78ULL;
constexpr uint64_t kDinsPa = 0x00010BA78ULL;
constexpr uint32_t kDinsWord = 0x7CA3F807U;
constexpr uint64_t kMovnPc = 0xFFFFFFFF8010B8F4ULL;
constexpr uint64_t kMovnPa = 0x00010B8F4ULL;
constexpr uint32_t kMovnWord = 0x0043280BU;
constexpr uint64_t kTestRamBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;
constexpr uint64_t kRetiredBeforeConfig0 = 158205;
constexpr uint64_t kRetiredBeforeExt = 158214;
constexpr char kKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kPhase31Sha256[] =
    "fbd854a39ae96864abfb20535a6b7e99bfc8d08047a7e29f7bfdea11cce6fcd4";
constexpr char kPhase32Sha256[] =
    "e55c6606bc9bbb2cd792ca6cc7fdc8f86e66c725b345ce8e6a3010bd9ab68f2e";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

Mips64FullVmSnapshot captureAtBoundary(VirtualMachine& vm, const char* stage);

uint32_t encodeMovn(uint8_t rs, uint8_t rt, uint8_t rd) {
    return (uint32_t(rs & 31U) << 21) | (uint32_t(rt & 31U) << 16) |
           (uint32_t(rd & 31U) << 11) | 0x0BU;
}

uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | funct;
}

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(word >>
            (8U * (little ? index : 3U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    if (little) return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                        (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open file: " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open trace: " + path.string());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) lines.push_back(std::move(line));
    return lines;
}

std::vector<std::string> linesOfType(const std::vector<std::string>& lines,
                                     const std::string& type) {
    std::vector<std::string> result;
    const std::string marker = "\"type\":\"" + type + "\"";
    for (const std::string& line : lines) {
        if (line.find(marker) != std::string::npos) result.push_back(line);
    }
    return result;
}

std::string hexValue(uint64_t value, unsigned width) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase << std::setw(width)
           << std::setfill('0') << value;
    return result.str();
}

using Gprs = std::array<uint64_t, kNumGpr>;

Gprs gprsOf(const Mips64Cpu& cpu) {
    Gprs values{};
    for (unsigned index = 0; index < values.size(); ++index) {
        values[index] = cpu.readGpr(index);
    }
    return values;
}

std::array<uint64_t, 16> cp0ExceptCount(const Mips64Cpu& cpu) {
    return {{cpu.cp0Status(), cpu.cp0Cause(), cpu.cp0Epc(), cpu.cp0ErrorEpc(),
             cpu.cp0BadVAddr(), cpu.cp0EBase(), cpu.cp0Compare(), cpu.cp0Index(),
             cpu.cp0Random(), cpu.cp0EntryHi(), cpu.cp0EntryLo0(), cpu.cp0EntryLo1(),
             cpu.cp0Context(), cpu.cp0PageMask(), cpu.cp0Wired(), cpu.cp0XContext()}};
}

std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbEntriesOf(const Mips64Cpu& cpu) {
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> entries{};
    for (unsigned index = 0; index < entries.size(); ++index) cpu.tlbEntry(index, entries[index]);
    return entries;
}

bool sameTlb(const std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()>& a,
             const std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()>& b) {
    for (size_t index = 0; index < a.size(); ++index) {
        const auto& x = a[index];
        const auto& y = b[index];
        if (x.vpn2 != y.vpn2 || x.pfn0 != y.pfn0 || x.pfn1 != y.pfn1 ||
            x.mask != y.mask || x.asid != y.asid || x.c0 != y.c0 || x.c1 != y.c1 ||
            x.v0 != y.v0 || x.d0 != y.d0 || x.v1 != y.v1 || x.d1 != y.d1 ||
            x.g != y.g || x.badMask != y.badMask) return false;
    }
    return true;
}

bool sameReservation(const Mips64ReservationState& a, const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
           a.cacheAttribute == b.cacheAttribute && a.virtualAddress == b.virtualAddress &&
           a.physicalAddress == b.physicalAddress && a.backingId == b.backingId &&
           a.granuleOffset == b.granuleOffset && a.generation == b.generation;
}

bool sameReplaySnapshot(const Mips64FullVmSnapshot& a,
                        const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity()) return false;
    Mips64FullVmSnapshot normalizedA = a;
    Mips64FullVmSnapshot normalizedB = b;
    // Console generation is monotonic host bookkeeping across restore; guest
    // visible bytes and sequence remain part of the canonical comparison.
    normalizedA.console.generation = normalizedB.console.generation;
    return normalizedA.RefreshIntegrity() && normalizedB.RefreshIntegrity() &&
           normalizedA.integrityChecksum == normalizedB.integrityChecksum;
}

Gprs baseGprs() {
    Gprs values{};
    for (unsigned index = 1; index < values.size(); ++index) {
        values[index] = 0xA5A5000000000000ULL | index;
    }
    return values;
}

struct MovnCase {
    const char* name;
    uint8_t rs;
    uint8_t rt;
    uint8_t rd;
    Gprs before;
};

void executeMovnCase(Mips64Endianness endian, const MovnCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) cpu.writeGpr(reg, test.before[reg]);
    const uint32_t word = encodeMovn(test.rs, test.rt, test.rd);
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kTestPa + 4, 0xDEADBEEFU, little);
    storeWord(memory, kDataPa, 0x89ABCDEFU, little);
    std::array<uint8_t, 24> memoryBefore{};
    memory.Read(kTestPa, memoryBefore.data(), memoryBefore.size());

    const Mips64Decoded decoded = decodeInstruction(word);
    const Gprs before = gprsOf(cpu);
    const uint64_t source = before[test.rs];
    const uint64_t condition = before[test.rt];
    const uint64_t destination = before[test.rd];
    Gprs expected = before;
    if (condition != 0 && test.rd != 0) expected[test.rd] = source;
    const auto cp0Before = cp0ExceptCount(cpu);
    const auto tlbBefore = tlbEntriesOf(cpu);
    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();

    CHECK(decoded.valid && decoded.op == Mips64Op::Movn);
    CHECK(decoded.raw == word && decoded.rs == test.rs && decoded.rt == test.rt &&
          decoded.rd == test.rd && decoded.sa == 0);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(cpu.disassemble(decoded) == "movn $" + std::to_string(test.rd) + ", $" +
          std::to_string(test.rs) + ", $" + std::to_string(test.rt));
    CHECK(Mips64Cpu::opName(Mips64Op::Movn) == std::string("movn"));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(gprsOf(cpu) == expected);
    CHECK(cpu.readGpr(test.rd) == expected[test.rd]);
    CHECK((condition != 0 && test.rd != 0) ? cpu.readGpr(test.rd) == source
                                            : cpu.readGpr(test.rd) == destination);
    CHECK(cpu.pc() == kTestPc + 4 && cpu.cp0Count() == countBefore + 1);
    CHECK(cpu.fault() == Mips64Fault::None && cpu.cp0Cause() == cp0Before[1]);
    CHECK(cp0ExceptCount(cpu) == cp0Before);
    CHECK(sameTlb(tlbBefore, tlbEntriesOf(cpu)));
    CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
    CHECK(sameReservation(reservationBefore, cpu.reservationState()));
    std::array<uint8_t, 24> memoryAfter{};
    memory.Read(kTestPa, memoryAfter.data(), memoryAfter.size());
    CHECK(memoryAfter == memoryBefore && readWord(memory, kDataPa, little) == 0x89ABCDEFU);
    CHECK(!cpu.delayArmed() && !cpu.hardwareInterruptLine(0) &&
          !cpu.hardwareInterruptLine(1) && !cpu.hardwareInterruptLine(2) &&
          !cpu.hardwareInterruptLine(3) && !cpu.hardwareInterruptLine(4) &&
          !cpu.hardwareInterruptLine(5));
    (void)test.name;
}

MovnCase simpleCase(const char* name, uint8_t rs, uint8_t rt, uint8_t rd,
                    uint64_t source, uint64_t condition, uint64_t destination) {
    Gprs before = baseGprs();
    if (rs != 0) before[rs] = source;
    if (rt != 0) before[rt] = condition;
    if (rd != 0 && rd != rs && rd != rt) before[rd] = destination;
    return {name, rs, rt, rd, before};
}

void testMovnEncodingAndClassification() {
    constexpr uint32_t exact = 0x0043280BU;
    const Mips64Decoded decoded = decodeInstruction(exact);
    CHECK(encodeMovn(2, 3, 5) == exact);
    CHECK(((exact >> 26) & 0x3FU) == 0x00U);
    CHECK(((exact >> 21) & 0x1FU) == 2U);
    CHECK(((exact >> 16) & 0x1FU) == 3U);
    CHECK(((exact >> 11) & 0x1FU) == 5U);
    CHECK(((exact >> 6) & 0x1FU) == 0U);
    CHECK((exact & 0x3FU) == 0x0BU);
    CHECK(decoded.valid && decoded.op == Mips64Op::Movn && decoded.rs == 2 &&
          decoded.rt == 3 && decoded.rd == 5 && decoded.sa == 0);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "movn $5, $2, $3");

    for (unsigned sa = 1; sa < 32; ++sa) {
        const Mips64Decoded reserved = decodeInstruction(exact | (sa << 6));
        CHECK(!reserved.valid && reserved.fault == Mips64Fault::ReservedInstruction);
        CHECK(reserved.error.find("MOVN") != std::string::npos);
    }
    const uint32_t movzWord = (exact & ~0x3FU) | 0x0AU;
    const Mips64Decoded movz = decodeInstruction(movzWord);
    CHECK(!movz.valid && movz.fault == Mips64Fault::UnsupportedInstruction &&
          movz.op != Mips64Op::Movn);
    const Mips64Decoded fpMovn = decodeInstruction(0x4602088BU);
    CHECK(!fpMovn.valid && fpMovn.fault == Mips64Fault::UnsupportedInstruction &&
          fpMovn.op != Mips64Op::Movn);

    const Mips64Decoded slti = decodeInstruction(encodeI(0x0A, 2, 8, 2));
    const Mips64Decoded sltiu = decodeInstruction(encodeI(0x0B, 2, 9, 2));
    const Mips64Decoded slt = decodeInstruction(encodeR(2, 3, 10, 0x2A));
    const Mips64Decoded sltu = decodeInstruction(encodeR(2, 3, 11, 0x2B));
    CHECK(slti.valid && slti.op == Mips64Op::Slti);
    CHECK(sltiu.valid && sltiu.op == Mips64Op::Sltiu);
    CHECK(slt.valid && slt.op == Mips64Op::Slt);
    CHECK(sltu.valid && sltu.op == Mips64Op::Sltu);
}

void testMovnFullWidthAndAliases() {
    std::vector<MovnCase> cases;
    cases.push_back(simpleCase("condition-one", 2, 3, 5,
        0xFEDCBA9876543210ULL, 1, 0x1122334455667788ULL));
    cases.push_back(simpleCase("condition-two", 2, 3, 5,
        0x1234567887654321ULL, 2, 0xAAAAAAAAAAAAAAAAULL));
    cases.push_back(simpleCase("negative-condition", 2, 3, 5,
        0x89ABCDEF01234567ULL, UINT64_MAX, 0xAAAAAAAAAAAAAAAAULL));
    cases.push_back(simpleCase("condition-bit-63", 2, 3, 5,
        0, 0x8000000000000000ULL, UINT64_MAX));
    cases.push_back(simpleCase("condition-bit-32", 2, 3, 5,
        0x1234567887654321ULL, 0x0000000100000000ULL, UINT64_MAX));
    cases.push_back(simpleCase("false-zero-condition", 2, 3, 5,
        0xFEDCBA9876543210ULL, 0, 0x1122334455667788ULL));
    cases.push_back(simpleCase("false-preserves-bit-63", 2, 3, 5,
        0xFEDCBA9876543210ULL, 0, 0x8122334455667788ULL));
    cases.push_back(simpleCase("copy-low-word-zero", 2, 3, 5,
        0x1234567800000000ULL, 1, 0xAAAAAAAAAAAAAAAAULL));
    cases.push_back(simpleCase("copy-upper-word-set", 2, 3, 5,
        0xFEDCBA9876543210ULL, 1, 0));
    cases.push_back(simpleCase("copy-min-signed", 2, 3, 5,
        0x8000000000000000ULL, 2, 0x7FFFFFFFFFFFFFFFULL));
    cases.push_back(simpleCase("copy-all-ones", 2, 3, 5,
        UINT64_MAX, 1, 0));

    Gprs aliasSource = baseGprs();
    aliasSource[5] = 0xFEDCBA9876543210ULL;
    aliasSource[3] = 1;
    cases.push_back({"rd-equals-rs", 5, 3, 5, aliasSource});
    Gprs aliasSourceFalse = baseGprs();
    aliasSourceFalse[5] = 0xFEDCBA9876543210ULL;
    aliasSourceFalse[3] = 0;
    cases.push_back({"rd-equals-rs-false", 5, 3, 5, aliasSourceFalse});
    Gprs aliasConditionTrue = baseGprs();
    aliasConditionTrue[2] = 0xFEDCBA9876543210ULL;
    aliasConditionTrue[3] = 1;
    cases.push_back({"rd-equals-rt-true", 2, 3, 3, aliasConditionTrue});
    Gprs aliasConditionFalse = baseGprs();
    aliasConditionFalse[2] = 0xFEDCBA9876543210ULL;
    aliasConditionFalse[3] = 0;
    cases.push_back({"rd-equals-rt-false", 2, 3, 3, aliasConditionFalse});
    Gprs aliasSourceCondition = baseGprs();
    aliasSourceCondition[2] = 0xFEDCBA9876543210ULL;
    aliasSourceCondition[5] = 0x0123456789ABCDEFULL;
    cases.push_back({"rs-equals-rt-nonzero", 2, 2, 5, aliasSourceCondition});
    Gprs aliasSourceConditionZero = baseGprs();
    aliasSourceConditionZero[2] = 0;
    aliasSourceConditionZero[5] = 0x0123456789ABCDEFULL;
    cases.push_back({"rs-equals-rt-zero", 2, 2, 5, aliasSourceConditionZero});
    Gprs allSameZero = baseGprs();
    allSameZero[4] = 0;
    cases.push_back({"all-registers-same-zero", 4, 4, 4, allSameZero});
    Gprs allSameNonzero = baseGprs();
    allSameNonzero[4] = 0xFEDCBA9876543210ULL;
    cases.push_back({"all-registers-same-nonzero", 4, 4, 4, allSameNonzero});
    cases.push_back(simpleCase("source-zero", 0, 3, 5,
        0, 1, 0xFEDCBA9876543210ULL));
    cases.push_back(simpleCase("condition-zero-register", 2, 0, 5,
        UINT64_MAX, 0, 0xFEDCBA9876543210ULL));
    cases.push_back(simpleCase("destination-zero-register", 2, 3, 0,
        UINT64_MAX, 1, 0xFEDCBA9876543210ULL));
    cases.push_back(simpleCase("source-and-condition-zero", 0, 0, 5,
        0, 0, 0xFEDCBA9876543210ULL));
    cases.push_back(simpleCase("source-destination-zero", 0, 3, 0,
        0, 1, 0));

    for (const Mips64Endianness endian : {Mips64Endianness::Little,
                                           Mips64Endianness::Big}) {
        for (const MovnCase& test : cases) executeMovnCase(endian, test);
    }
}

void testComparisonRegression() {
    for (const Mips64Endianness endian : {Mips64Endianness::Little,
                                           Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(2, 1);
        cpu.writeGpr(3, 2);
        storeWord(memory, kTestPa + 0, encodeI(0x0A, 2, 8, 2), little);
        storeWord(memory, kTestPa + 4, encodeI(0x0B, 2, 9, 2), little);
        storeWord(memory, kTestPa + 8, encodeR(2, 3, 10, 0x2A), little);
        storeWord(memory, kTestPa + 12, encodeR(2, 3, 11, 0x2B), little);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(8) == 1 && cpu.readGpr(9) == 1 &&
              cpu.readGpr(10) == 1 && cpu.readGpr(11) == 1);
        CHECK(cpu.cp0Count() == 4 && cpu.pc() == kTestPc + 16);
    }
}

void testBranchDelayAndCpuSerialization() {
    for (const Mips64Endianness endian : {Mips64Endianness::Little,
                                           Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool branchTaken : {false, true}) {
            for (const uint64_t condition : {0ULL, 0x100000000ULL}) {
                Memory memory(static_cast<size_t>(kTestRamBytes));
                Mips64Cpu cpu(endian);
                cpu.setPc(kTestPc);
                cpu.writeGpr(2, 0xFEDCBA9876543210ULL);
                cpu.writeGpr(3, condition);
                cpu.writeGpr(5, 0x1122334455667788ULL);
                storeWord(memory, kTestPa,
                          encodeI(branchTaken ? 0x04 : 0x05, 0, 0, 2), little);
                storeWord(memory, kTestPa + 4, encodeMovn(2, 3, 5), little);
                CHECK(cpu.step(memory) == Mips64StepResult::Continue);
                CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
                const std::vector<uint8_t> beforeSlot = cpu.serialize();
                Mips64Cpu replay(endian);
                CHECK(beforeSlot.size() == Mips64Cpu::kSerializedSize &&
                      beforeSlot[4] == 6 && replay.deserialize(beforeSlot.data(), beforeSlot.size()));
                const uint64_t expected = condition != 0
                    ? 0xFEDCBA9876543210ULL : 0x1122334455667788ULL;
                CHECK(cpu.step(memory) == Mips64StepResult::Continue);
                CHECK(replay.step(memory) == Mips64StepResult::Continue);
                CHECK(cpu.readGpr(5) == expected && replay.readGpr(5) == expected);
                CHECK(cpu.pc() == (branchTaken ? kTestPc + 12 : kTestPc + 8));
                CHECK(!cpu.delayArmed() && cpu.cp0Count() == 2);
                CHECK(cpu.serialize() == replay.serialize());
            }
        }
    }
}

void storeDoubleword(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >>
            (8U * (little ? index : 7U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

Mips64ISAPlugin* initializeSyntheticVm(VirtualMachine& vm,
                                       Mips64Endianness endian) {
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    if (!vm.init()) return nullptr;
    return getMipsPlugin(vm);
}

void testSyntheticFullVmSnapshotReplay() {
    std::vector<MovnCase> cases;
    cases.push_back(simpleCase("snapshot-true", 2, 3, 5,
        0xFEDCBA9876543210ULL, 1, 0x1122334455667788ULL));
    cases.push_back(simpleCase("snapshot-false", 2, 3, 5,
        0xFEDCBA9876543210ULL, 0, 0x8122334455667788ULL));
    cases.push_back(simpleCase("snapshot-high-word-condition", 2, 3, 5,
        0x1234567887654321ULL, 0x0000000100000000ULL, UINT64_MAX));
    cases.push_back(simpleCase("snapshot-rd-equals-rt", 2, 3, 3,
        0xFEDCBA9876543210ULL, 1, 0));
    Gprs rdRs = baseGprs();
    rdRs[5] = 0xFEDCBA9876543210ULL;
    rdRs[3] = 1;
    cases.push_back({"snapshot-rd-equals-rs", 5, 3, 5, rdRs});
    Gprs rsRt = baseGprs();
    rsRt[2] = 0x8000000000000000ULL;
    rsRt[5] = 0x0123456789ABCDEFULL;
    cases.push_back({"snapshot-rs-equals-rt", 2, 2, 5, rsRt});
    Gprs allSame = baseGprs();
    allSame[4] = 0xFEDCBA9876543210ULL;
    cases.push_back({"snapshot-all-registers-same", 4, 4, 4, allSame});

    for (const Mips64Endianness endian : {Mips64Endianness::Little,
                                           Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            const MovnCase& test = cases[caseIndex];
            const bool branchTaken = (caseIndex % 2) == 0;
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CPUContext* context = vm.getCPUContext(0);
            CHECK(plugin != nullptr && context != nullptr);
            if (!plugin || !context) continue;

            storeWord(vm.getMemory(), kTestPa,
                      encodeI(0x34, 16, 8, 0), little);  // LLD establishes a live reservation.
            storeWord(vm.getMemory(), kTestPa + 4,
                      encodeI(branchTaken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(vm.getMemory(), kTestPa + 8,
                      encodeMovn(test.rs, test.rt, test.rd), little);
            storeDoubleword(vm.getMemory(), kDataPa, 0xA1B2C3D4E5F60718ULL, little);
            Mips64Cpu& cpu = plugin->core();
            Gprs before = test.before;
            before[16] = kDataVa;
            before[8] = 0xA1B2C3D4E5F60718ULL;
            for (unsigned reg = 1; reg < kNumGpr; ++reg) cpu.writeGpr(reg, before[reg]);
            vm.setEntryPoint(kTestPc);
            CHECK(vm.run(2) == 2);
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 8);
            CHECK(cpu.reservationState().valid && cpu.reservationState().size == 8);

            const Mips64FullVmSnapshot beforeSlot = captureAtBoundary(vm, "synthetic before MOVN");
            CHECK(beforeSlot.ValidateIntegrity() && beforeSlot.formatVersion == 3);
            CHECK(beforeSlot.cpuComponentVersion == 6 &&
                  beforeSlot.cpus[0].architecturalState.size() == 1001);
            CHECK(beforeSlot.memoryComponentVersion == 2 && beforeSlot.uartComponentVersion == 4 &&
                  beforeSlot.irqRoutingComponentVersion == 1);
            const Mips64ReservationState reservation = cpu.reservationState();

            Mips64FullVmSnapshot malformed = beforeSlot;
            malformed.integrityChecksum ^= 1ULL;
            const uint64_t cyclesBeforeMalformed = vm.getCyclesExecuted();
            const uint64_t retiredBeforeMalformed = context->instructionsExecuted;
            const std::vector<uint8_t> cpuBeforeMalformed = cpu.serialize();
            std::string malformedError;
            CHECK(!vm.restoreMips64FullSnapshot(malformed, &malformedError));
            CHECK(!malformedError.empty() && vm.getCyclesExecuted() == cyclesBeforeMalformed &&
                  context->instructionsExecuted == retiredBeforeMalformed &&
                  cpu.serialize() == cpuBeforeMalformed);
            CHECK(captureAtBoundary(vm, "synthetic after malformed restore").integrityChecksum ==
                  beforeSlot.integrityChecksum);

            const uint64_t source = cpu.readGpr(test.rs);
            const uint64_t condition = cpu.readGpr(test.rt);
            const uint64_t destination = cpu.readGpr(test.rd);
            Gprs expected = gprsOf(cpu);
            if (condition != 0 && test.rd != 0) expected[test.rd] = source;
            const uint64_t countBefore = cpu.cp0Count();
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot after = captureAtBoundary(vm, "synthetic after MOVN");
            CHECK(after.ValidateIntegrity() && context->instructionsExecuted == 3 &&
                  vm.getCyclesExecuted() == 3 && cpu.cp0Count() == countBefore + 1);
            CHECK(gprsOf(cpu) == expected && cpu.readGpr(test.rd) == expected[test.rd]);
            CHECK((condition != 0 && test.rd != 0) ? cpu.readGpr(test.rd) == source
                                                    : cpu.readGpr(test.rd) == destination);
            CHECK(cpu.pc() == (branchTaken ? kTestPc + 16 : kTestPc + 12));
            CHECK(!cpu.delayArmed() && sameReservation(reservation, cpu.reservationState()));
            CHECK(after.physicalMemory.payloadBytes == beforeSlot.physicalMemory.payloadBytes);

            std::string restoreError;
            CHECK(vm.restoreMips64FullSnapshot(beforeSlot, &restoreError) && restoreError.empty());
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 8 &&
                  vm.getCyclesExecuted() == cyclesBeforeMalformed &&
                  context->instructionsExecuted == retiredBeforeMalformed);
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot replay = captureAtBoundary(vm, "synthetic after replay");
            CHECK(sameReplaySnapshot(replay, after));
            CHECK(cpu.serialize() == after.cpus[0].architecturalState &&
                  cpu.readGpr(test.rd) == expected[test.rd]);
            (void)test.name;
        }
    }
}

bool recordHas(const std::string& line, const std::string& key,
               const std::string& value) {
    return line.find("\"" + key + "\":\"" + value + "\"") != std::string::npos;
}

const std::string& continuationRecordAt(const std::vector<std::string>& records,
                                        uint64_t cycle) {
    const std::string marker = "\"cycle\":" + std::to_string(cycle);
    for (const std::string& line : records) {
        if (line.find(marker) != std::string::npos) return line;
    }
    throw std::runtime_error("Phase 29 trace lacks continuation cycle " +
                             std::to_string(cycle));
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t* physical = nullptr, uint8_t* rawBytes = nullptr) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction address");
    if (physical) *physical = probe.phys;
    uint8_t bytes[4]{};
    vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
    if (rawBytes) std::copy(std::begin(bytes), std::end(bytes), rawBytes);
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

Mips64FullVmSnapshot captureAtBoundary(VirtualMachine& vm, const char* stage) {
    try {
        return vm.captureMips64FullSnapshot();
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(stage) + " snapshot failed at VM state " +
            std::to_string(static_cast<unsigned>(vm.getState())) + ": " + error.what());
    }
}

struct AuthenticSession {
    std::unique_ptr<VirtualMachine> vm;
    Mips64ISAPlugin* plugin = nullptr;
    CPUContext* context = nullptr;
    MaltaBootstrapBoard* board = nullptr;
    Mips64Cpu* cpu = nullptr;
    uint32_t word = 0;
    uint64_t physical = 0;
    std::array<uint8_t, 4> bytes{};
    Mips64FullVmSnapshot beforeMovn;
};

AuthenticSession reachAuthenticMovn(const std::filesystem::path& imagePath,
                                    const std::filesystem::path& phase29Path,
                                    const std::filesystem::path& phase30Path,
                                    const std::filesystem::path& phase31Path,
                                    const std::filesystem::path& phase32Path) {
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected kernel ELF size");
    const std::vector<std::string> phase29 = readLines(phase29Path);
    const auto initial = linesOfType(phase29, "instruction");
    const auto continuation = linesOfType(phase29, "continuation_instruction");
    const auto phase30 = readLines(phase30Path);
    const auto phase31 = readLines(phase31Path);
    const auto phase32 = readLines(phase32Path);
    if (initial.size() != 256 || continuation.size() != 102 || phase30.size() != 4 ||
        phase31.size() != 4 || phase32.size() != 4 ||
        phase31[2].find("\"next_frontier\":\"0xffffffff8010ba78\"") == std::string::npos ||
        phase32[0].find("\"pc_va\":\"0xffffffff8010ba78\"") == std::string::npos ||
        phase32[1].find("\"observed_v1\":\"0x0000001000000010\"") == std::string::npos ||
        phase32[2].find("\"next_frontier\":\"0xffffffff8010b8f4\"") == std::string::npos ||
        phase32[3].find("\"instruction\":\"0x0043280b\"") == std::string::npos) {
        throw std::runtime_error("accepted Phase 29-32 trace chain changed");
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    AuthenticSession session;
    session.vm = std::make_unique<VirtualMachine>(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!session.vm->configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("Malta ELF handoff setup failed: " + error);
    }
    if (!session.vm->init()) throw std::runtime_error("production VM initialization failed");
    session.context = session.vm->getCPUContext(0);
    session.plugin = session.context && session.context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(session.context->isaPlugin.get()) : nullptr;
    session.board = session.vm->getMips64MaltaBootstrapBoard();
    if (!session.plugin || !session.context || !session.board || !session.board->Uart() ||
        !session.board->ConsoleSink()) throw std::runtime_error("Malta VM components unavailable");
    session.cpu = &session.plugin->core();
    if (session.cpu->pc() != kLinuxEntry && session.cpu->pc() != 0xFFFFFFFFBFC00000ULL) {
        throw std::runtime_error("production VM did not start at the Malta handoff");
    }

    for (uint64_t cycle = 0; cycle < initial.size(); ++cycle) {
        const std::string& accepted = initial[static_cast<size_t>(cycle)];
        uint64_t physical = 0;
        const uint32_t word = fetchWord(*session.vm, *session.cpu,
                                        session.cpu->pc(), &physical);
        if (accepted.find("\"cycle\":" + std::to_string(cycle)) == std::string::npos ||
            !recordHas(accepted, "pc_va", hexValue(session.cpu->pc(), 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) ||
            session.vm->run(1) != 1) throw std::runtime_error("Phase 29 startup diverged");
    }
    const uint64_t toConfig0 = kRetiredBeforeConfig0 - initial.size();
    if (session.vm->run(toConfig0) != toConfig0 || session.cpu->pc() != kConfig0Pc ||
        session.cpu->cp0Count() != kRetiredBeforeConfig0 ||
        session.context->instructionsExecuted != kRetiredBeforeConfig0 ||
        session.vm->getCyclesExecuted() != kRetiredBeforeConfig0) {
        throw std::runtime_error("Phase 29 Config0 boundary diverged");
    }
    uint64_t configPhysical = 0;
    if (fetchWord(*session.vm, *session.cpu, session.cpu->pc(), &configPhysical) != kConfig0Word ||
        configPhysical != 0x00010B840ULL || session.cpu->readGpr(6) != 0x00000000007C0000ULL ||
        session.vm->run(1) != 1 || session.cpu->readGpr(6) != 0xFFFFFFFF8000C483ULL) {
        throw std::runtime_error("authentic Config0 instruction diverged");
    }
    for (uint64_t cycle = kRetiredBeforeConfig0 + 1; cycle < kRetiredBeforeExt; ++cycle) {
        const std::string& accepted = continuationRecordAt(continuation, cycle);
        uint64_t physical = 0;
        const uint32_t word = fetchWord(*session.vm, *session.cpu,
                                        session.cpu->pc(), &physical);
        if (!recordHas(accepted, "pc_va", hexValue(session.cpu->pc(), 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) ||
            session.vm->run(1) != 1) throw std::runtime_error("Phase 29 continuation diverged");
    }
    if (session.cpu->pc() != kExtPc || session.cpu->cp0Count() != kRetiredBeforeExt ||
        session.context->instructionsExecuted != kRetiredBeforeExt ||
        session.vm->getCyclesExecuted() != kRetiredBeforeExt ||
        fetchWord(*session.vm, *session.cpu, kExtPc) != kExtWord ||
        session.vm->run(1) != 1 || session.cpu->readGpr(3) != 2) {
        throw std::runtime_error("authentic EXT prefix diverged");
    }
    if (session.vm->run(50) != 50 || session.cpu->pc() != kDinsuPc ||
        session.cpu->cp0Count() != 158265 || session.context->instructionsExecuted != 158265 ||
        session.vm->getCyclesExecuted() != 158265 ||
        fetchWord(*session.vm, *session.cpu, kDinsuPc) != kDinsuWord ||
        session.vm->run(1) != 1 || session.cpu->readGpr(3) != 0x0000001000000000ULL) {
        throw std::runtime_error("Phase 31 DINSU sequence diverged");
    }
    if (session.cpu->pc() != kDinsPc || session.cpu->cp0Count() != 158266 ||
        session.context->instructionsExecuted != 158266 ||
        session.vm->getCyclesExecuted() != 158266 ||
        fetchWord(*session.vm, *session.cpu, kDinsPc) != kDinsWord ||
        session.vm->run(1) != 1 || session.cpu->readGpr(3) != 0x0000001000000010ULL) {
        throw std::runtime_error("Phase 32 DINS sequence diverged");
    }
    if (session.cpu->pc() != 0xFFFFFFFF8010BA7CULL || session.cpu->cp0Count() != 158267 ||
        session.context->instructionsExecuted != 158267 ||
        session.vm->getCyclesExecuted() != 158267) {
        throw std::runtime_error("post-DINS Phase 32 boundary diverged");
    }
    if (session.vm->run(27) != 27 || session.cpu->pc() != kMovnPc ||
        session.cpu->cp0Count() != 158294 || session.context->instructionsExecuted != 158294 ||
        session.vm->getCyclesExecuted() != 158294) {
        throw std::runtime_error("failed to reproduce 27-instruction Linux continuation");
    }
    session.word = fetchWord(*session.vm, *session.cpu, session.cpu->pc(),
                             &session.physical, session.bytes.data());
    if (session.word != kMovnWord || session.physical != kMovnPa ||
        session.bytes != std::array<uint8_t, 4>{{0x00, 0x43, 0x28, 0x0B}}) {
        throw std::runtime_error("authentic MOVN bytes or physical address differ");
    }
    session.beforeMovn = captureAtBoundary(*session.vm, "authentic before MOVN");
    if (!session.beforeMovn.ValidateIntegrity() || session.beforeMovn.formatVersion != 3 ||
        session.beforeMovn.cpuComponentVersion != 6 ||
        session.beforeMovn.cpus[0].architecturalState.size() != 1001 ||
        session.beforeMovn.memoryComponentVersion != 2 ||
        session.beforeMovn.uartComponentVersion != 4 ||
        session.beforeMovn.irqRoutingComponentVersion != 1 || session.cpu->delayArmed()) {
        throw std::runtime_error("pre-MOVN authentic full-VM snapshot invalid");
    }
    return session;
}

struct ContinuationStop {
    std::string reason;
    std::string detail;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    std::array<uint8_t, 4> bytes{};
    uint64_t attempts = 0;
    uint64_t vmAttempts = 0;
    uint64_t retired = 0;
    Mips64Fault attemptedFault = Mips64Fault::None;
    uint64_t countBefore = 0;
    uint64_t cyclesBefore = 0;
    uint64_t retiredBefore = 0;
};

ContinuationStop runToFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                               CPUContext& context, uint64_t budget) {
    ContinuationStop stop;
    stop.reason = "instruction_budget_exhausted";
    for (uint64_t index = 0; index < budget; ++index) {
        stop.pc = cpu.pc();
        const Mips64Probe probe = cpu.probeAddress(stop.pc, Mips64Access::Fetch);
        ++stop.attempts;
        if (!probe.ok) {
            stop.reason = Mips64Cpu::faultName(probe.fault);
            stop.physical = probe.phys;
            return stop;
        }
        stop.word = fetchWord(vm, cpu, stop.pc, &stop.physical, stop.bytes.data());
        const Mips64Decoded decoded = decodeInstruction(stop.word);
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            return stop;  // Preserve the valid pre-failure snapshot and do not retire it.
        }
        if (decoded.valid && (decoded.op == Mips64Op::Mfc0 ||
                              decoded.op == Mips64Op::Dmfc0 ||
                              decoded.op == Mips64Op::Mtc0 ||
                              decoded.op == Mips64Op::Dmtc0)) {
            // CP0 register/selector support is a run-time property. Probe the
            // CPU copy so an unsupported selector can be captured at a valid
            // STOPPED boundary before the production VM records its failed
            // attempt. The only memory access is the instruction fetch.
            Mips64Cpu probeCpu = cpu;
            const Mips64StepResult probeResult = probeCpu.step(vm.getMemory());
            if (probeResult == Mips64StepResult::Fault &&
                probeCpu.fault() == Mips64Fault::Cp0Violation) {
                stop.reason = "unsupported_cp0_access";
                stop.detail = probeCpu.lastError();
                stop.attemptedFault = probeCpu.fault();
                stop.countBefore = cpu.cp0Count();
                stop.cyclesBefore = vm.getCyclesExecuted();
                stop.retiredBefore = context.instructionsExecuted;
                return stop;  // Preserve the valid pre-failure full-VM snapshot.
            }
        }
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        ++stop.vmAttempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted == retiredBefore + 1) {
            ++stop.retired;
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cyclesBefore + 1) {
            stop.reason = "architectural_exception";
        } else {
            stop.reason = vm.getState() == VMState::ERROR
                ? "vm_execution_error" : "stopped_without_retirement";
        }
        return stop;
    }
    stop.pc = cpu.pc();
    return stop;
}

unsigned validTlbEntries(const Mips64Cpu& cpu) {
    unsigned count = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu.tlbEntry(index, entry) && (entry.v0 || entry.v1)) ++count;
    }
    return count;
}

struct FailedAttemptObservation {
    uint64_t countAfter = 0;
    uint64_t cyclesAfter = 0;
    uint64_t retiredAfter = 0;
    uint64_t pcAfter = 0;
    Mips64Fault fault = Mips64Fault::None;
    std::string detail;
    VMState vmState = VMState::STOPPED;
    std::vector<uint8_t> cpuState;
};

FailedAttemptObservation attemptUnsupportedCp0Frontier(
    VirtualMachine& vm, Mips64Cpu& cpu, CPUContext& context,
    const ContinuationStop& stop) {
    if (stop.reason != "unsupported_cp0_access" ||
        stop.attemptedFault != Mips64Fault::Cp0Violation || cpu.pc() != stop.pc ||
        cpu.cp0Count() != stop.countBefore || vm.getCyclesExecuted() != stop.cyclesBefore ||
        context.instructionsExecuted != stop.retiredBefore ||
        (vm.getState() != VMState::STOPPED && vm.getState() != VMState::PAUSED)) {
        throw std::runtime_error("unsupported CP0 pre-failure boundary is not intact");
    }
    const uint64_t cyclesRun = vm.run(1);
    FailedAttemptObservation result;
    result.countAfter = cpu.cp0Count();
    result.cyclesAfter = vm.getCyclesExecuted();
    result.retiredAfter = context.instructionsExecuted;
    result.pcAfter = cpu.pc();
    result.fault = cpu.fault();
    result.detail = cpu.lastError();
    result.vmState = vm.getState();
    result.cpuState = cpu.serialize();
    if (cyclesRun != 0 || result.countAfter != stop.countBefore + 1 ||
        result.cyclesAfter != stop.cyclesBefore || result.retiredAfter != stop.retiredBefore ||
        result.pcAfter != stop.pc || result.fault != Mips64Fault::Cp0Violation ||
        Mips64Cpu::faultEntersException(result.fault) || result.vmState != VMState::ERROR ||
        result.detail != stop.detail || result.cpuState.empty()) {
        throw std::runtime_error("failed CP0 attempt accounting or no-exception behavior differs");
    }
    return result;
}

int runAuthenticContinuation(const std::filesystem::path& image,
                             const std::filesystem::path& phase29,
                             const std::filesystem::path& phase30,
                             const std::filesystem::path& phase31,
                             const std::filesystem::path& phase32,
                             const std::filesystem::path& output) {
    if (std::filesystem::exists(output)) {
        throw std::runtime_error("refusing to overwrite trace: " + output.string());
    }
    AuthenticSession session = reachAuthenticMovn(image, phase29, phase30, phase31, phase32);
    VirtualMachine& vm = *session.vm;
    Mips64Cpu& cpu = *session.cpu;
    CPUContext& context = *session.context;
    const Mips64Decoded decoded = decodeInstruction(session.word);
    if (!decoded.valid || decoded.op != Mips64Op::Movn || decoded.rs != 2 ||
        decoded.rt != 3 || decoded.rd != 5 || decoded.sa != 0 ||
        (session.word >> 26) != 0 || (session.word & 0x3FU) != 0x0BU ||
        cpu.disassemble(decoded) != "movn $5, $2, $3") {
        throw std::runtime_error("authentic MOVN decoder fields or mnemonic differ");
    }
    const uint64_t sourceBefore = cpu.readGpr(decoded.rs);
    const uint64_t conditionBefore = cpu.readGpr(decoded.rt);
    const uint64_t destinationBefore = cpu.readGpr(decoded.rd);
    const bool conditionTrue = conditionBefore != 0;
    const uint64_t expected = conditionTrue ? sourceBefore : destinationBefore;
    if (sourceBefore != 2 || conditionBefore != 0 || destinationBefore != 3 ||
        cpu.pc() != kMovnPc || session.physical != kMovnPa || cpu.cp0Count() != 158294 ||
        vm.getCyclesExecuted() != 158294 || context.instructionsExecuted != 158294 ||
        session.beforeMovn.integrityChecksum != 0xFF60849FD5AA0264ULL) {
        throw std::runtime_error("authentic MOVN prestate differs from pre-implementation capture");
    }

    const Gprs gprsBefore = gprsOf(cpu);
    Gprs expectedGprs = gprsBefore;
    if (conditionTrue) expectedGprs[decoded.rd] = sourceBefore;
    const auto cp0Before = cp0ExceptCount(cpu);
    const auto tlbBefore = tlbEntriesOf(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t retiredBefore = context.instructionsExecuted;
    const Mips64FullVmSnapshot& snapshotBefore = session.beforeMovn;

    if (vm.run(1) != 1) throw std::runtime_error("production VM did not retire MOVN");
    const uint64_t observed = cpu.readGpr(decoded.rd);
    const Mips64FullVmSnapshot afterMovn = captureAtBoundary(vm, "authentic after MOVN");
    if (observed != expected || gprsOf(cpu) != expectedGprs ||
        cpu.pc() != kMovnPc + 4 || cpu.delayArmed() ||
        cpu.cp0Count() != countBefore + 1 || vm.getCyclesExecuted() != cyclesBefore + 1 ||
        context.instructionsExecuted != retiredBefore + 1 || cp0ExceptCount(cpu) != cp0Before ||
        !sameTlb(tlbBefore, tlbEntriesOf(cpu)) || cpu.hi() != hiBefore || cpu.lo() != loBefore ||
        !sameReservation(reservationBefore, cpu.reservationState()) ||
        cpu.fault() != Mips64Fault::None || !afterMovn.ValidateIntegrity()) {
        throw std::runtime_error("production MOVN result or side-effect qualification failed");
    }

    std::string error;
    if (!vm.restoreMips64FullSnapshot(snapshotBefore, &error) || !error.empty() ||
        cpu.serialize() != snapshotBefore.cpus[0].architecturalState ||
        vm.getCyclesExecuted() != cyclesBefore || context.instructionsExecuted != retiredBefore ||
        vm.run(1) != 1 || cpu.serialize() != afterMovn.cpus[0].architecturalState ||
        !sameReplaySnapshot(captureAtBoundary(vm, "authentic immediate replay"), afterMovn)) {
        throw std::runtime_error("authentic pre-MOVN full-VM immediate replay diverged: " + error);
    }

    constexpr uint64_t continuationBudget = 1000000;
    const ContinuationStop firstStop = runToFrontier(vm, cpu, context, continuationBudget);
    if (firstStop.reason != "unsupported_instruction" &&
        firstStop.reason != "unsupported_cp0_access" &&
        firstStop.reason != "instruction_budget_exhausted") {
        throw std::runtime_error("continuation stopped at " + firstStop.reason + " pc=" +
            hexValue(firstStop.pc, 16) + " word=" + hexValue(firstStop.word, 8) +
            " fault=" + Mips64Cpu::faultName(cpu.fault()) + " detail=" + cpu.lastError() +
            " count=" + std::to_string(cpu.cp0Count()) + " vm_cycles=" +
            std::to_string(vm.getCyclesExecuted()) + " retired=" +
            std::to_string(context.instructionsExecuted));
    }
    const Mips64FullVmSnapshot firstTerminal = captureAtBoundary(vm, "authentic first terminal");
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid pre-failure snapshot");
    const uint64_t totalRetired = context.instructionsExecuted;
    const uint64_t preFailureCount = cpu.cp0Count();
    const uint64_t preFailureCycles = vm.getCyclesExecuted();
    const uint64_t preFailureRetired = context.instructionsExecuted;

    if (!vm.restoreMips64FullSnapshot(snapshotBefore, &error) || !error.empty() ||
        vm.run(1) != 1 || cpu.serialize() != afterMovn.cpus[0].architecturalState) {
        throw std::runtime_error("authentic pre-MOVN restore before continuation failed: " + error);
    }
    const ContinuationStop replayStop = runToFrontier(vm, cpu, context, continuationBudget);
    const bool replaySnapshotMatches = sameReplaySnapshot(
        captureAtBoundary(vm, "authentic replay terminal"), firstTerminal);
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.bytes != replayStop.bytes || firstStop.attempts != replayStop.attempts ||
        firstStop.vmAttempts != replayStop.vmAttempts || firstStop.retired != replayStop.retired ||
        firstStop.detail != replayStop.detail ||
        firstStop.attemptedFault != replayStop.attemptedFault ||
        totalRetired != context.instructionsExecuted || preFailureCount != cpu.cp0Count() ||
        preFailureCycles != vm.getCyclesExecuted() ||
        preFailureRetired != context.instructionsExecuted || !replaySnapshotMatches) {
        throw std::runtime_error("authentic Linux continuation replay diverged");
    }

    const auto preFailureCp0 = cp0ExceptCount(cpu);
    FailedAttemptObservation firstFailure;
    FailedAttemptObservation replayFailure;
    uint64_t faultingVmAttempts = 0;
    if (firstStop.reason == "unsupported_cp0_access") {
        firstFailure = attemptUnsupportedCp0Frontier(vm, cpu, context, firstStop);
        ++faultingVmAttempts;
        if (cp0ExceptCount(cpu) != preFailureCp0) {
            throw std::runtime_error("failed CP0 access changed architectural CP0 state");
        }
        AuthenticSession replaySession = reachAuthenticMovn(
            image, phase29, phase30, phase31, phase32);
        replaySession.beforeMovn = Mips64FullVmSnapshot{};
        std::string replayError;
        const bool restoredTerminal = replaySession.vm->restoreMips64FullSnapshot(
            firstTerminal, &replayError);
        if (!restoredTerminal ||
            !replayError.empty() ||
            (replaySession.vm->getState() != VMState::STOPPED &&
             replaySession.vm->getState() != VMState::PAUSED) ||
            replaySession.cpu->pc() != firstStop.pc ||
            replaySession.cpu->serialize() != firstTerminal.cpus[0].architecturalState ||
            replaySession.cpu->cp0Count() != firstStop.countBefore ||
            replaySession.vm->getCyclesExecuted() != firstStop.cyclesBefore ||
            replaySession.context->instructionsExecuted != firstStop.retiredBefore) {
            throw std::runtime_error("authentic pre-failure full-VM snapshot restore failed: restored=" +
                std::to_string(restoredTerminal) + " state=" +
                vmStateToString(replaySession.vm->getState()) + " pc=" +
                hexValue(replaySession.cpu->pc(), 16) + " count=" +
                std::to_string(replaySession.cpu->cp0Count()) + " cycles=" +
                std::to_string(replaySession.vm->getCyclesExecuted()) + " retired=" +
                std::to_string(replaySession.context->instructionsExecuted) + " error=" + replayError);
        }
        replayFailure = attemptUnsupportedCp0Frontier(
            *replaySession.vm, *replaySession.cpu, *replaySession.context, firstStop);
        ++faultingVmAttempts;
        if (replayFailure.countAfter != firstFailure.countAfter ||
            replayFailure.cyclesAfter != firstFailure.cyclesAfter ||
            replayFailure.retiredAfter != firstFailure.retiredAfter ||
            replayFailure.pcAfter != firstFailure.pcAfter ||
            replayFailure.fault != firstFailure.fault ||
            replayFailure.detail != firstFailure.detail ||
            replayFailure.vmState != firstFailure.vmState ||
            replayFailure.cpuState != firstFailure.cpuState ||
            cp0ExceptCount(*replaySession.cpu) != preFailureCp0) {
            throw std::runtime_error("failed CP0 attempt did not replay deterministically");
        }
    }
    const uint64_t finalCount = cpu.cp0Count();
    const uint64_t finalCycles = vm.getCyclesExecuted();
    const uint64_t finalRetired = context.instructionsExecuted;
    const uint64_t finalPc = cpu.pc();

    const auto uart = session.board->Uart()->CaptureSnapshot();
    const auto console = session.board->ConsoleSink()->CaptureSnapshot();
    const Mips64Decoded frontier = decodeInstruction(firstStop.word);
    const std::string frontierClass = firstStop.reason == "unsupported_cp0_access"
        ? "valid-supported-encoding-runtime-cp0-selector-unsupported"
        : frontier.valid ? "valid-supported" :
        (frontier.fault == Mips64Fault::UnsupportedInstruction ?
             "valid-but-unsupported" : "reserved-or-invalid");

    std::ofstream file(output, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot create MOVN trace: " + output.string());
    file << "{\"type\":\"authentic_movn_pre_instruction\","
         << "\"kernel_sha256\":\"" << kKernelSha256 << "\","
         << "\"phase31_trace_sha256\":\"" << kPhase31Sha256 << "\","
         << "\"phase32_trace_sha256\":\"" << kPhase32Sha256 << "\","
         << "\"pc_va\":\"" << hexValue(kMovnPc, 16) << "\","
         << "\"fetch_pa\":\"" << hexValue(session.physical, 9) << "\","
         << "\"raw_bytes_be\":\"0043280b\",\"word\":\"0x0043280b\","
         << "\"opcode\":0,\"rs\":2,\"rt\":3,\"rd\":5,\"sa\":0,\"funct\":11,"
         << "\"fixed_zero_valid\":true,\"disassembly\":\"movn $5, $2, $3\","
         << "\"v0_source\":\"" << hexValue(sourceBefore, 16) << "\","
         << "\"v1_condition\":\"" << hexValue(conditionBefore, 16) << "\","
         << "\"a1_destination_before\":\"" << hexValue(destinationBefore, 16) << "\","
         << "\"condition_true\":" << (conditionTrue ? "true" : "false") << ","
         << "\"independent_expected_a1\":\"" << hexValue(expected, 16) << "\","
         << "\"status\":\"" << hexValue(cp0Before[0], 16) << "\","
         << "\"cause\":\"" << hexValue(cp0Before[1], 16) << "\","
         << "\"epc\":\"" << hexValue(cp0Before[2], 16) << "\","
         << "\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16) << "\","
         << "\"count\":" << countBefore << ",\"vm_cycles\":" << cyclesBefore
         << ",\"retired\":" << retiredBefore << ",\"delay_armed\":false,"
         << "\"rom_base\":\"" << hexValue(snapshotBefore.romBase, 16) << "\","
         << "\"rom_size\":" << snapshotBefore.romSize << ","
         << "\"rom_content_hash\":\"" << hexValue(snapshotBefore.romContentHash, 16) << "\","
         << "\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001,"
         << "\"memory_version\":2,\"uart_version\":4,\"irq_version\":1,"
         << "\"snapshot_total_size\":" << snapshotBefore.totalSize << ","
         << "\"snapshot_checksum\":\"" << hexValue(snapshotBefore.integrityChecksum, 16)
         << "\"}\n";
    file << "{\"type\":\"authentic_movn_retirement\","
         << "\"observed_a1\":\"" << hexValue(observed, 16) << "\","
         << "\"expected_a1\":\"" << hexValue(expected, 16) << "\","
         << "\"post_pc\":\"" << hexValue(kMovnPc + 4, 16) << "\","
         << "\"count_after\":" << (countBefore + 1) << ",\"vm_cycles_after\":"
         << (cyclesBefore + 1) << ",\"retired_after\":" << (retiredBefore + 1)
         << ",\"normal_retirement\":true,\"condition_false_preserved_destination\":true,"
         << "\"other_gprs_unchanged\":true,\"cp0_unchanged_except_count\":true,"
         << "\"hi_lo_tlb_reservation_unchanged\":true,\"no_architectural_exception\":true,"
         << "\"delay_armed_after\":false,\"immediate_snapshot_replay_verified\":true}\n";
    file << "{\"type\":\"authentic_movn_continuation_summary\","
         << "\"total_retired\":" << totalRetired
         << ",\"retired_since_linux_entry\":" << (totalRetired - 17)
         << ",\"retirements_beyond_former_movn_frontier\":" << (totalRetired - retiredBefore)
         << ",\"retired_after_movn\":" << firstStop.retired
         << ",\"continuation_vm_attempts\":" << (firstStop.vmAttempts + faultingVmAttempts / 2)
         << ",\"unsupported_frontier_observations\":"
         << (firstStop.reason == "unsupported_instruction" ? 1 : 0)
         << ",\"failed_frontier_attempts_verified\":" << faultingVmAttempts
         << ",\"total_attempt_observations\":"
         << (totalRetired + (firstStop.reason == "unsupported_cp0_access" ? 1 : 0))
         << ",\"authentic_full_vm_snapshot_replay_verified\":true"
         << ",\"pre_failure_snapshot_preserved\":true"
         << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
         << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
         << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
         << "\",\"stop_reason\":\"" << firstStop.reason
         << "\",\"attempts_after_movn\":" << (firstStop.retired + faultingVmAttempts / 2)
         << ",\"frontier_probe_count\":" << (firstStop.reason == "unsupported_cp0_access" ? 1 : 0)
         << ",\"frontier_cpu_fault\":\""
         << Mips64Cpu::faultName(firstStop.attemptedFault) << "\""
         << ",\"frontier_detail\":\"" << firstStop.detail << "\""
         << ",\"pre_failure_count\":" << preFailureCount
         << ",\"pre_failure_vm_cycles\":" << preFailureCycles
         << ",\"pre_failure_retired\":" << preFailureRetired
         << ",\"failed_attempt_count_after\":" << firstFailure.countAfter
         << ",\"failed_attempt_vm_cycles_after\":" << firstFailure.cyclesAfter
         << ",\"failed_attempt_retired_after\":" << firstFailure.retiredAfter
         << ",\"failed_attempt_pc_after\":\"" << hexValue(firstFailure.pcAfter, 16) << "\""
         << ",\"failed_attempt_vm_state\":\"" << vmStateToString(firstFailure.vmState) << "\""
         << ",\"failed_attempt_fault\":\"" << Mips64Cpu::faultName(firstFailure.fault) << "\""
         << ",\"failed_attempt_enters_exception\":false"
         << ",\"count\":" << finalCount << ",\"vm_cycles\":" << finalCycles
         << ",\"retired\":" << finalRetired
         << ",\"post_fault_pc\":\"" << hexValue(finalPc, 16) << "\""
         << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
         << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
         << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
         << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
         << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
         << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
         << ",\"reservation_valid\":" << (cpu.reservationState().valid ? "true" : "false")
         << ",\"reservation_size\":" << static_cast<unsigned>(cpu.reservationState().size)
         << ",\"interrupt_controller_pending\":"
         << (vm.getInterruptController()->HasPendingInterrupt() ? "true" : "false")
         << ",\"uart_tx_bytes\":" << uart.transmitCount
         << ",\"uart_rx_fifo_bytes\":" << uart.receiveFifo.size()
         << ",\"console_bytes\":" << console.bytes.size()
         << ",\"console_sequence\":" << console.sequence
         << ",\"mmio_observation\":\"not instrumented\","
         << "\"continuation_budget\":" << continuationBudget
         << ",\"ram_devices_scheduler_and_stop_replayed\":true}\n";
    file << "{\"type\":\"continuation_frontier\","
         << "\"pc_va\":\"" << hexValue(firstStop.pc, 16) << "\","
         << "\"pc_pa\":\"" << hexValue(firstStop.physical, 9) << "\","
         << "\"raw_bytes_be\":\"";
    for (uint8_t byte : firstStop.bytes) {
        file << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    }
    file << std::dec << "\",\"instruction\":\"" << hexValue(firstStop.word, 8)
         << "\",\"opcode\":" << ((firstStop.word >> 26) & 0x3FU)
         << ",\"rs\":" << ((firstStop.word >> 21) & 0x1FU)
         << ",\"rt\":" << ((firstStop.word >> 16) & 0x1FU)
         << ",\"rd\":" << ((firstStop.word >> 11) & 0x1FU)
         << ",\"sa\":" << ((firstStop.word >> 6) & 0x1FU)
         << ",\"funct\":" << (firstStop.word & 0x3FU)
         << ",\"classification\":\"" << frontierClass
         << "\",\"disassembly\":\"" << cpu.disassemble(frontier)
         << "\",\"architectural_exception\":"
         << (firstStop.reason == "architectural_exception" ? "true" : "false") << "}\n";
    file.close();
    if (!file) throw std::runtime_error("failed writing MOVN continuation trace");
    std::cout << "MOVN: PC=" << hexValue(kMovnPc, 16)
              << " PA=" << hexValue(session.physical, 9)
              << " source=" << hexValue(sourceBefore, 16)
              << " condition=" << hexValue(conditionBefore, 16)
              << " destination=" << hexValue(destinationBefore, 16)
              << " result=" << hexValue(observed, 16)
              << " total_retired=" << totalRetired
              << " next=" << hexValue(firstStop.pc, 16)
              << " reason=" << firstStop.reason << " replay=true\n";
    return 0;
}

void runFocusedTests() {
    testMovnEncodingAndClassification();
    testMovnFullWidthAndAliases();
    testComparisonRegression();
    testBranchDelayAndCpuSerialization();
    testSyntheticFullVmSnapshotReplay();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 8 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3], argv[4], argv[5],
                                            argv[6], argv[7]);
        }
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_movn: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        std::cerr << "Usage: test_mips64_movn [--continue-authentic-kernel <ELF> "
                     "<phase29> <phase30> <phase31> <phase32> <output-jsonl>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_movn: " << error.what() << "\n";
        return 1;
    }
}
