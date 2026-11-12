// MIPS64-26 encoded SYNC qualification and authenticated Linux continuation.

#include "Mips64FullSnapshot.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "Ns16550Uart.h"
#include "PhysicalAddressMap.h"
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
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kCpu1TestPa = 0x1100ULL;
constexpr uint64_t kDataPa = 0x4000ULL;
constexpr uint64_t kDataCachedVa = 0xFFFFFFFF80004000ULL;
constexpr uint64_t kDataUncachedVa = 0xFFFFFFFFA0004000ULL;
constexpr uint64_t kFlagUncachedVa = 0xFFFFFFFFA0004004ULL;
constexpr uint64_t kMmioBase = 0x02000000ULL;
constexpr uint64_t kMmioUncachedVa = 0xFFFFFFFFA2000000ULL;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kSyncPc = 0xFFFFFFFF8091EB50ULL;
constexpr uint64_t kRetiredBeforeSync = 157780;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kContinuationBudget = 1000000;
constexpr uint64_t kKernelSize = 11013776;
constexpr char kKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kPhase25TraceSha256[] =
    "222593af3cf8e35b2b415190daf3bf1a0b9c00d90eebb03b8d86f6e8752c0c2f";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}
uint32_t encodeSync(uint8_t stype = 0) {
    return (uint32_t(stype) << 6) | 0x0FU;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(word >>
            (8U * (little ? index : 3U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

void writeWord(PhysicalAddressMap& memory, uint64_t address, uint32_t word,
               bool little) {
    uint8_t bytes[4]{};
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(word >>
            (8U * (little ? index : 3U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const PhysicalAddressMap& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) {
        result |= uint32_t(bytes[index]) <<
            (8U * (little ? index : 3U - index));
    }
    return result;
}

void writeDoubleword(PhysicalAddressMap& memory, uint64_t address,
                     uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >>
            (8U * (little ? index : 7U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t readDoubleword(const PhysicalAddressMap& memory, uint64_t address,
                        bool little) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index) {
        result |= uint64_t(bytes[index]) <<
            (8U * (little ? index : 7U - index));
    }
    return result;
}

std::array<uint64_t, kNumGpr> gprsOf(const Mips64Cpu& cpu) {
    std::array<uint64_t, kNumGpr> values{};
    for (unsigned index = 0; index < values.size(); ++index) {
        values[index] = cpu.readGpr(index);
    }
    return values;
}

std::array<uint64_t, 22> cp0Of(const Mips64Cpu& cpu) {
    return {{cpu.cp0Status(), cpu.cp0Cause(), cpu.cp0Epc(), cpu.cp0ErrorEpc(),
             cpu.cp0BadVAddr(), cpu.cp0EBase(), cpu.cp0Count(), cpu.cp0Compare(),
             cpu.cp0Index(), cpu.cp0Random(), cpu.cp0EntryHi(), cpu.cp0EntryLo0(),
             cpu.cp0EntryLo1(), cpu.cp0Context(), cpu.cp0PageMask(), cpu.cp0Wired(),
             cpu.cp0XContext(), cpu.statusExl() ? 1ULL : 0ULL,
             cpu.statusErl() ? 1ULL : 0ULL, cpu.statusBev() ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(0) ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(1) ? 1ULL : 0ULL}};
}

bool sameReservation(const Mips64ReservationState& left,
                     const Mips64ReservationState& right) {
    return left.valid == right.valid && left.size == right.size &&
           left.cacheAttribute == right.cacheAttribute &&
           left.virtualAddress == right.virtualAddress &&
           left.physicalAddress == right.physicalAddress &&
           left.backingId == right.backingId &&
           left.granuleOffset == right.granuleOffset &&
           left.generation == right.generation;
}

void prepareProgram(Memory& memory, uint64_t pa, const std::vector<uint32_t>& words,
                    bool little) {
    for (size_t index = 0; index < words.size(); ++index) {
        writeWord(memory, pa + index * 4, words[index], little);
    }
}

void testDecodeAndClassification() {
    const Mips64Decoded sync = decodeInstruction(0x0000000FU);
    CHECK(sync.valid);
    CHECK(sync.op == Mips64Op::Sync);
    CHECK(sync.raw == 0x0000000FU);
    CHECK(sync.rs == 0 && sync.rt == 0 && sync.rd == 0);
    CHECK(sync.stype == 0 && sync.sa == 0 && (sync.raw & 0x3FU) == 0x0FU);
    CHECK(Mips64Cpu{}.disassemble(sync) == "sync");
    CHECK(std::string(Mips64Cpu::opName(Mips64Op::Sync)) == "sync");

    for (uint32_t fixedField : {uint32_t{1} << 21, uint32_t{1} << 16,
                                uint32_t{1} << 11}) {
        const Mips64Decoded malformed = decodeInstruction(fixedField | 0x0FU);
        CHECK(!malformed.valid);
        CHECK(malformed.fault == Mips64Fault::ReservedInstruction);
        CHECK(malformed.op == Mips64Op::Invalid);
    }

    const Mips64Decoded futureType = decodeInstruction(encodeSync(1));
    CHECK(!futureType.valid);
    CHECK(futureType.op == Mips64Op::Sync);
    CHECK(futureType.stype == 1);
    CHECK(futureType.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(Mips64Cpu{}.disassemble(futureType) == "sync 1");

    const Mips64Decoded ehb = decodeInstruction(0x000000C0U);
    CHECK(ehb.valid && ehb.op == Mips64Op::Ehb);
    CHECK(Mips64Cpu{}.disassemble(ehb) == "ehb");
    const Mips64Decoded ssnop = decodeInstruction(0x00000040U);
    CHECK(ssnop.valid && ssnop.op == Mips64Op::Sll);
    CHECK(ssnop.op != sync.op);
    const Mips64Decoded synci = decodeInstruction(0x041F0000U);
    CHECK(synci.op != Mips64Op::Sync);
    CHECK(synci.fault == Mips64Fault::UnsupportedInstruction ||
          synci.fault == Mips64Fault::ReservedInstruction);
    CHECK(decodeInstruction(0x0000000CU).op == Mips64Op::Syscall);
    CHECK(decodeInstruction(0x0000000DU).op == Mips64Op::Break);
}

void testRetirementAndPreservation(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    cpu.setHardwareInterruptLine(1, true);
    writeWord(memory, kTestPa, 0x0000000FU, little);
    writeWord(memory, kTestPa + 0x100, 0xDEADBEEFU, little);

    const auto gprBefore = gprsOf(cpu);
    auto cp0Before = cp0Of(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const auto reservationBefore = cpu.reservationState();
    uint8_t codeBefore[4]{};
    uint8_t dataBefore[4]{};
    memory.Read(kTestPa, codeBefore, sizeof(codeBefore));
    memory.Read(kTestPa + 0x100, dataBefore, sizeof(dataBefore));

    uint32_t fetched = 0;
    CHECK(cpu.fetchWord(memory, kTestPc, fetched));
    CHECK(fetched == 0x0000000FU);
    const Mips64Decoded decoded = decodeInstruction(fetched);
    CHECK(decoded.valid && decoded.op == Mips64Op::Sync && decoded.stype == 0);
    CHECK(cpu.disassemble(decoded) == "sync");
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    cp0Before[6] += 1;
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(!cpu.delayArmed());
    CHECK(cpu.cp0Count() == 1);
    CHECK(cp0Of(cpu) == cp0Before);
    CHECK(gprsOf(cpu) == gprBefore);
    CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
    CHECK(sameReservation(cpu.reservationState(), reservationBefore));
    CHECK(cpu.hardwareInterruptLine(1));
    CHECK(cpu.cp0Cause() == cp0Before[1]);
    CHECK(cpu.fault() == Mips64Fault::None);
    uint8_t codeAfter[4]{};
    uint8_t dataAfter[4]{};
    memory.Read(kTestPa, codeAfter, sizeof(codeAfter));
    memory.Read(kTestPa + 0x100, dataAfter, sizeof(dataAfter));
    CHECK(std::equal(std::begin(codeBefore), std::end(codeBefore), std::begin(codeAfter)));
    CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
}

void testBranchDelaySlot(Mips64Endianness endian, bool taken) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, 7);
    cpu.writeGpr(2, taken ? 7 : 8);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x04, 1, 2, 2), 0x0000000FU,
                    0x00000000U, 0x00000000U}, little);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.pc() == kTestPc + 4 && cpu.delayArmed());
    CHECK(cpu.delayTarget() == (taken ? kTestPc + 12 : kTestPc + 8));
    const std::vector<uint8_t> beforeSync = cpu.serialize();
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    const std::vector<uint8_t> afterSync = cpu.serialize();
    CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
    CHECK(!cpu.delayArmed());
    CHECK(cpu.cp0Count() == 2);
    CHECK(cpu.causeExcCode() == 0 && cpu.cp0Epc() == 0);
    CHECK(cpu.deserialize(beforeSync.data(), beforeSync.size()));
    CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.serialize() == afterSync);
}

void testStoreSyncLoad(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kDataUncachedVa);
    cpu.writeGpr(2, 0x13579BDFU);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x2B, 1, 2, 0), 0x0000000FU,
                    encodeI(0x23, 1, 3, 0)}, little);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa, little) == 0x13579BDFU);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.pc() == kTestPc + 8 && cpu.cp0Count() == 2);
    CHECK(readWord(map, kDataPa, little) == 0x13579BDFU);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(3) == 0x13579BDFU);
    CHECK(cpu.cp0Count() == 3 && cpu.pc() == kTestPc + 12);
}

void testDataSyncFlagOrdering() {
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kDataUncachedVa);
    cpu.writeGpr(2, 0x2468ACE0U);
    cpu.writeGpr(3, kFlagUncachedVa);
    cpu.writeGpr(4, 1);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x2B, 1, 2, 0), 0x0000000FU,
                    encodeI(0x2B, 3, 4, 0)}, false);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa, false) == 0x2468ACE0U);
    CHECK(readWord(map, kDataPa + 4, false) == 0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa, false) == 0x2468ACE0U);
    CHECK(readWord(map, kDataPa + 4, false) == 0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa, false) == 0x2468ACE0U);
    CHECK(readWord(map, kDataPa + 4, false) == 1);
}

void testTwoCpuDeterministicHandoff() {
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu0(Mips64Endianness::Big);
    Mips64Cpu cpu1(Mips64Endianness::Big);
    cpu0.setPc(kTestPc);
    cpu1.setPc(kTestPc + 0x100);
    cpu0.writeGpr(1, kDataUncachedVa);
    cpu0.writeGpr(2, 0x61B2C3D4U);
    cpu0.writeGpr(3, kFlagUncachedVa);
    cpu0.writeGpr(4, 1);
    cpu1.writeGpr(1, kFlagUncachedVa);
    cpu1.writeGpr(2, kDataUncachedVa);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x2B, 1, 2, 0), 0x0000000FU,
                    encodeI(0x2B, 3, 4, 0)}, false);
    prepareProgram(memory, kTestPa + 0x100,
                   {encodeI(0x23, 1, 5, 0), 0x0000000FU,
                    encodeI(0x23, 2, 6, 0)}, false);

    // The explicit schedule is CPU0: DATA, SYNC, FLAG; then CPU1: FLAG, SYNC, DATA.
    CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa, false) == 0x61B2C3D4U);
    CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
    CHECK(readWord(map, kDataPa + 4, false) == 1);
    CHECK(cpu1.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu1.readGpr(5) == 1);
    CHECK(cpu1.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu1.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu1.readGpr(6) == 0x61B2C3D4U);
    CHECK(cpu0.cp0Count() == 3 && cpu1.cp0Count() == 3);
}

void testLlSyncSc(bool doubleword) {
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kDataCachedVa);
    const uint64_t initial = doubleword ? 0x0102030405060708ULL : 0x10203040U;
    const uint64_t replacement = doubleword ? 0x8877665544332211ULL : 0x55667788U;
    if (doubleword) writeDoubleword(map, kDataPa, initial, false);
    else writeWord(map, kDataPa, static_cast<uint32_t>(initial), false);
    const uint8_t loadOpcode = doubleword ? 0x34 : 0x30;
    const uint8_t storeOpcode = doubleword ? 0x3C : 0x38;
    prepareProgram(memory, kTestPa,
                   {encodeI(loadOpcode, 1, 2, 0), 0x0000000FU,
                    encodeI(storeOpcode, 1, 2, 0)}, false);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(2) == initial);
    CHECK(cpu.reservationState().valid);
    const Mips64ReservationState reservationBeforeSync = cpu.reservationState();
    const auto generationsBeforeSync = map.CaptureSnapshot().reservationGenerations;
    cpu.writeGpr(2, replacement);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(sameReservation(cpu.reservationState(), reservationBeforeSync));
    CHECK(map.CaptureSnapshot().reservationGenerations == generationsBeforeSync);
    CHECK(cpu.cp0Count() == 2);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(2) == 1);
    CHECK(!cpu.reservationState().valid);
    if (doubleword) CHECK(readDoubleword(map, kDataPa, false) == replacement);
    else CHECK(readWord(map, kDataPa, false) == replacement);
}

void testTrackedConflictAndDifferentGranule() {
    {
        Memory memory(static_cast<size_t>(kRamBytes));
        PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(Mips64Endianness::Big);
        cpu.setPc(kTestPc);
        cpu.writeGpr(1, kDataCachedVa);
        writeWord(map, kDataPa, 0x11111111U, false);
        prepareProgram(memory, kTestPa,
                       {encodeI(0x30, 1, 2, 0), 0x0000000FU,
                        encodeI(0x38, 1, 2, 0)}, false);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        const auto active = cpu.reservationState();
        uint8_t hostReplacement[4] = {0x22, 0x22, 0x22, 0x22};
        memory.loadBuffer(kDataPa, hostReplacement, sizeof(hostReplacement));
        CHECK(cpu.reservationState().valid);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(sameReservation(cpu.reservationState(), active));
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(2) == 0);
        CHECK(readWord(map, kDataPa, false) == 0x22222222U);
    }

    {
        Memory memory(static_cast<size_t>(kRamBytes));
        PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu0(Mips64Endianness::Big);
        Mips64Cpu cpu1(Mips64Endianness::Big);
        cpu0.setPc(kTestPc);
        cpu1.setPc(kTestPc + 0x100);
        cpu0.writeGpr(1, kDataCachedVa);
        cpu0.writeGpr(2, 0x33333333U);
        cpu1.writeGpr(1, kDataCachedVa);
        cpu1.writeGpr(2, 0x44444444U);
        writeWord(map, kDataPa, 0x11111111U, false);
        prepareProgram(memory, kTestPa,
                       {encodeI(0x30, 1, 3, 0), 0x0000000FU,
                        encodeI(0x38, 1, 3, 0)}, false);
        prepareProgram(memory, kTestPa + 0x100,
                       {encodeI(0x2B, 1, 2, 16)}, false);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu1.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.readGpr(3) == 1);
        CHECK(readWord(map, kDataPa + 16, false) == 0x44444444U);
    }

    {
        Memory memory(static_cast<size_t>(kRamBytes));
        PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu0(Mips64Endianness::Big);
        Mips64Cpu cpu1(Mips64Endianness::Big);
        cpu0.setPc(kTestPc);
        cpu1.setPc(kTestPc + 0x100);
        cpu0.writeGpr(1, kDataCachedVa);
        cpu0.writeGpr(2, 0x33333333U);
        cpu1.writeGpr(1, kDataCachedVa);
        cpu1.writeGpr(2, 0x44444444U);
        writeWord(map, kDataPa, 0x11111111U, false);
        prepareProgram(memory, kTestPa,
                       {encodeI(0x30, 1, 3, 0), 0x0000000FU,
                        encodeI(0x38, 1, 3, 0)}, false);
        prepareProgram(memory, kTestPa + 0x100,
                       {encodeI(0x2B, 1, 2, 0)}, false);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu1.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu0.readGpr(3) == 0);
        CHECK(readWord(map, kDataPa, false) == 0x44444444U);
    }

    {
        Memory memory(static_cast<size_t>(kRamBytes));
        PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(Mips64Endianness::Big);
        cpu.setPc(kTestPc);
        cpu.writeGpr(1, kDataCachedVa);
        writeWord(map, kDataPa, 0x76543210U, false);
        prepareProgram(memory, kTestPa,
                       {encodeI(0x30, 1, 2, 0), 0x0000000FU,
                        encodeI(0x3C, 1, 2, 0)}, false); // LL, SYNC, SCD
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(2) == 0);
        CHECK(readWord(map, kDataPa, false) == 0x76543210U);
    }
}

void testSyncAndMemoryOrderingSnapshotReplay(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kDataUncachedVa);
    cpu.writeGpr(2, 0x23456789U);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x2B, 1, 2, 0), 0x0000000FU,
                    encodeI(0x23, 1, 3, 0)}, little);
    writeWord(map, kDataPa, 0, little);

    const std::vector<uint8_t> beforeCpu = cpu.serialize();
    const PhysicalAddressMapSnapshot beforeMemory = map.CaptureSnapshot();
    const auto executeOrderingProgram = [&]() {
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(readWord(map, kDataPa, little) == 0x23456789U);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(readWord(map, kDataPa, little) == 0x23456789U);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(3) == 0x23456789U);
    };
    executeOrderingProgram();
    const std::vector<uint8_t> afterCpu = cpu.serialize();
    const PhysicalAddressMapSnapshot afterMemory = map.CaptureSnapshot();

    CHECK(cpu.deserialize(beforeCpu.data(), beforeCpu.size()));
    PreparedPhysicalAddressMapRestore restore;
    std::string error;
    CHECK(map.PrepareRestore(beforeMemory, restore, &error));
    CHECK(error.empty());
    map.CommitRestore(std::move(restore));
    executeOrderingProgram();
    CHECK(cpu.serialize() == afterCpu);
    const PhysicalAddressMapSnapshot replayMemory = map.CaptureSnapshot();
    CHECK(replayMemory.reservationGenerations == afterMemory.reservationGenerations);
    CHECK(replayMemory.nextReservationGeneration == afterMemory.nextReservationGeneration);
    CHECK(readWord(map, kDataPa, little) == 0x23456789U);

    Memory syncOnlyMemory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& syncOnlyMap = syncOnlyMemory.EnablePhysicalAddressMap();
    Mips64Cpu syncOnlyCpu(endian);
    syncOnlyCpu.setPc(kTestPc);
    writeWord(syncOnlyMemory, kTestPa, 0x0000000FU, little);
    const std::vector<uint8_t> syncOnlyCpuBefore = syncOnlyCpu.serialize();
    const PhysicalAddressMapSnapshot syncOnlyMemoryBefore = syncOnlyMap.CaptureSnapshot();
    CHECK(syncOnlyCpu.step(syncOnlyMemory) == Mips64StepResult::Continue);
    const std::vector<uint8_t> syncOnlyAfter = syncOnlyCpu.serialize();
    CHECK(syncOnlyCpu.deserialize(syncOnlyCpuBefore.data(), syncOnlyCpuBefore.size()));
    PreparedPhysicalAddressMapRestore syncOnlyRestore;
    CHECK(syncOnlyMap.PrepareRestore(syncOnlyMemoryBefore, syncOnlyRestore, &error));
    syncOnlyMap.CommitRestore(std::move(syncOnlyRestore));
    CHECK(syncOnlyCpu.step(syncOnlyMemory) == Mips64StepResult::Continue);
    CHECK(syncOnlyCpu.serialize() == syncOnlyAfter);
    CHECK(readWord(syncOnlyMap, kTestPa, little) == 0x0000000FU);
    CHECK(syncOnlyMap.CaptureSnapshot().reservationGenerations ==
          syncOnlyMemoryBefore.reservationGenerations);
}

void testReservationSnapshotReplayAndIsolation() {
    Memory firstMemory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& firstMap = firstMemory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kDataCachedVa);
    writeWord(firstMap, kDataPa, 0x12345678U, false);
    prepareProgram(firstMemory, kTestPa,
                   {encodeI(0x30, 1, 2, 0), 0x0000000FU,
                    encodeI(0x38, 1, 2, 0)}, false);
    CHECK(cpu.step(firstMemory) == Mips64StepResult::Continue);
    const std::vector<uint8_t> cpuBefore = cpu.serialize();
    const PhysicalAddressMapSnapshot memoryBefore = firstMap.CaptureSnapshot();
    CHECK(cpu.reservationState().valid);
    CHECK(cpu.step(firstMemory) == Mips64StepResult::Continue);
    cpu.writeGpr(2, 0xAABBCCDDU);
    CHECK(cpu.step(firstMemory) == Mips64StepResult::Continue);
    const std::vector<uint8_t> firstCpuAfter = cpu.serialize();
    const PhysicalAddressMapSnapshot firstMemoryAfter = firstMap.CaptureSnapshot();
    const uint32_t firstValue = readWord(firstMap, kDataPa, false);
    CHECK(firstValue == 0xAABBCCDDU);

    CHECK(cpu.deserialize(cpuBefore.data(), cpuBefore.size()));
    PreparedPhysicalAddressMapRestore restore;
    std::string error;
    CHECK(firstMap.PrepareRestore(memoryBefore, restore, &error));
    CHECK(error.empty());
    firstMap.CommitRestore(std::move(restore));
    CHECK(cpu.reservationState().valid);
    CHECK(cpu.step(firstMemory) == Mips64StepResult::Continue);
    cpu.writeGpr(2, 0xAABBCCDDU);
    CHECK(cpu.step(firstMemory) == Mips64StepResult::Continue);
    CHECK(cpu.serialize() == firstCpuAfter);
    const PhysicalAddressMapSnapshot replayMemoryAfter = firstMap.CaptureSnapshot();
    CHECK(replayMemoryAfter.reservationGenerations == firstMemoryAfter.reservationGenerations);
    CHECK(replayMemoryAfter.nextReservationGeneration == firstMemoryAfter.nextReservationGeneration);
    CHECK(readWord(firstMap, kDataPa, false) == firstValue);

    PhysicalAddressMapSnapshot malformed = replayMemoryAfter;
    malformed.reservationTrackingVersion = 99;
    PreparedPhysicalAddressMapRestore rejected;
    const auto liveCpu = cpu.serialize();
    const uint32_t liveData = readWord(firstMap, kDataPa, false);
    CHECK(!firstMap.PrepareRestore(malformed, rejected, &error));
    CHECK(cpu.serialize() == liveCpu);
    CHECK(readWord(firstMap, kDataPa, false) == liveData);

    Memory secondMemory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& secondMap = secondMemory.EnablePhysicalAddressMap();
    Mips64Cpu secondCpu(Mips64Endianness::Big);
    secondCpu.setPc(kTestPc);
    secondCpu.writeGpr(1, kDataCachedVa);
    prepareProgram(secondMemory, kTestPa,
                   {encodeI(0x23, 1, 2, 0)}, false);
    CHECK(readWord(secondMap, kDataPa, false) == 0);
    CHECK(secondCpu.step(secondMemory) == Mips64StepResult::Continue);
    CHECK(secondCpu.readGpr(2) == 0);
    CHECK(readWord(firstMap, kDataPa, false) == 0xAABBCCDDU);
}

class ImmediateMmio final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 1; }
    void Reset() override { bytes.fill(0); writes.clear(); }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override {
        if (offset + size > bytes.size()) throw std::out_of_range("MMIO test read range");
        std::copy_n(bytes.data() + offset, size, destination);
    }
    void Write(uint64_t offset, const uint8_t* source, size_t size) override {
        if (offset + size > bytes.size()) throw std::out_of_range("MMIO test write range");
        for (size_t index = 0; index < size; ++index) {
            bytes[static_cast<size_t>(offset) + index] = source[index];
            writes.emplace_back(static_cast<size_t>(offset) + index, source[index]);
        }
    }

    std::array<uint8_t, 8> bytes{};
    std::vector<std::pair<size_t, uint8_t>> writes;
};

void testSynchronousDeviceTransactionBoundary() {
    Memory memory(static_cast<size_t>(kRamBytes));
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    auto device = std::make_shared<ImmediateMmio>();
    map.RegisterMmio(kMmioBase, 8, device, {1});
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(1, kMmioUncachedVa);
    cpu.writeGpr(2, 0x5AU);
    cpu.writeGpr(3, 0xA5U);
    prepareProgram(memory, kTestPa,
                   {encodeI(0x28, 1, 2, 0), 0x0000000FU,
                    encodeI(0x28, 1, 3, 1)}, false);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(device->writes.size() == 1);
    CHECK(device->writes[0] == std::make_pair(size_t{0}, uint8_t{0x5A}));
    CHECK(device->bytes[0] == 0x5A);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(device->writes.size() == 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(device->writes.size() == 2);
    CHECK(device->writes[1] == std::make_pair(size_t{1}, uint8_t{0xA5}));
    CHECK(device->bytes[0] == 0x5A && device->bytes[1] == 0xA5);
}

void runFocusedTests() {
    testDecodeAndClassification();
    testRetirementAndPreservation(Mips64Endianness::Big);
    testRetirementAndPreservation(Mips64Endianness::Little);
    testBranchDelaySlot(Mips64Endianness::Big, true);
    testBranchDelaySlot(Mips64Endianness::Big, false);
    testBranchDelaySlot(Mips64Endianness::Little, true);
    testBranchDelaySlot(Mips64Endianness::Little, false);
    testSyncAndMemoryOrderingSnapshotReplay(Mips64Endianness::Big);
    testSyncAndMemoryOrderingSnapshotReplay(Mips64Endianness::Little);
    testStoreSyncLoad(Mips64Endianness::Big);
    testStoreSyncLoad(Mips64Endianness::Little);
    testDataSyncFlagOrdering();
    testTwoCpuDeterministicHandoff();
    testLlSyncSc(false);
    testLlSyncSc(true);
    testTrackedConflictAndDifferentGranule();
    testReservationSnapshotReplayAndIsolation();
    testSynchronousDeviceTransactionBoundary();
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open input artifact: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string hexValue(uint64_t value, unsigned width = 0) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase;
    if (width != 0) result << std::setw(static_cast<int>(width)) << std::setfill('0');
    result << value;
    return result.str();
}

std::vector<std::string> linesOfType(const std::filesystem::path& path,
                                     const std::string& type) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open accepted Phase 25 trace");
    const std::string marker = "\"type\":\"" + type + "\"";
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(marker) != std::string::npos) result.push_back(line);
    }
    return result;
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t* physical = nullptr) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction fetch");
    if (physical) *physical = probe.phys;
    uint8_t bytes[4]{};
    vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

unsigned validTlbEntries(const Mips64Cpu& cpu) {
    unsigned count = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu.tlbEntry(index, entry) && (entry.v0 || entry.v1)) ++count;
    }
    return count;
}

void writeInitialRecord(std::ostream& trace, VirtualMachine& vm,
                        Mips64Cpu& cpu, CPUContext& context) {
    const uint64_t cycle = vm.getCyclesExecuted();
    const uint64_t pc = cpu.pc();
    const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
    if (!fetch.ok) throw std::runtime_error("initial Malta trace fetch failed");
    const uint32_t word = fetchWord(vm, cpu, pc);
    const uint64_t retiredBefore = context.instructionsExecuted;
    if (vm.run(1) != 1 || context.instructionsExecuted != retiredBefore + 1) {
        throw std::runtime_error("non-retirement in Malta prefix");
    }
    trace << "{\"type\":\"instruction\",\"cycle\":" << cycle
          << ",\"pc_va\":\"" << hexValue(pc, 16) << "\",\"fetch_ok\":true"
          << ",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
          << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"outcome\":\"retired\",\"exc_code\":"
          << cpu.causeExcCode() << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"mips_fault_code\":" << static_cast<unsigned>(cpu.fault())
          << ",\"mips_fault_name\":\"" << Mips64Cpu::faultName(cpu.fault())
          << "\"}\n";
}

struct ContinuationStop {
    std::string reason;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    uint64_t attempts = 0;
    uint64_t retired = 0;
    uint64_t distinctRetiredPcs = 0;
};

ContinuationStop runToFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                               CPUContext& context, uint64_t budget,
                               std::ostream* trace) {
    ContinuationStop stop;
    stop.reason = "instruction_budget_exhausted";
    std::set<uint64_t> retiredPcs;
    for (uint64_t index = 0; index < budget; ++index) {
        const uint64_t pc = cpu.pc();
        const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
        if (!fetch.ok) {
            stop.reason = Mips64Cpu::faultName(fetch.fault);
            stop.pc = pc;
            ++stop.attempts;
            stop.distinctRetiredPcs = retiredPcs.size();
            return stop;
        }
        const uint32_t word = fetchWord(vm, cpu, pc);
        const Mips64Decoded decoded = decodeInstruction(word);
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            stop.pc = pc;
            stop.physical = fetch.phys;
            stop.word = word;
            ++stop.attempts;
            stop.distinctRetiredPcs = retiredPcs.size();
            return stop;
        }

        const uint64_t cycleBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        const bool delayBefore = cpu.delayArmed();
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted > retiredBefore) {
            ++stop.retired;
            retiredPcs.insert(pc);
            if (trace) {
                *trace << "{\"type\":\"continuation_instruction\",\"cycle\":"
                       << cycleBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                       << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                       << "\",\"instruction\":\"" << hexValue(word, 8)
                       << "\",\"outcome\":\"retired\",\"exc_code\":"
                       << cpu.causeExcCode() << ",\"epc\":\""
                       << hexValue(cpu.cp0Epc(), 16) << "\",\"cause\":\""
                       << hexValue(cpu.cp0Cause(), 16) << "\",\"count_after\":"
                       << cpu.cp0Count() << ",\"vm_cycles_after\":"
                       << vm.getCyclesExecuted() << ",\"retired_after\":"
                       << context.instructionsExecuted << ",\"delay_armed_before\":"
                       << (delayBefore ? "true" : "false")
                       << ",\"delay_armed_after\":"
                       << (cpu.delayArmed() ? "true" : "false") << "}\n";
            }
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cycleBefore + 1) {
            if (trace) {
                *trace << "{\"type\":\"continuation_exception_entry\",\"cycle\":"
                       << cycleBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                       << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                       << "\",\"instruction\":\"" << hexValue(word, 8)
                       << "\",\"exc_code\":" << cpu.causeExcCode()
                       << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
                       << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
                       << "\",\"count_after\":" << cpu.cp0Count()
                       << ",\"vm_cycles_after\":" << vm.getCyclesExecuted() << "}\n";
            }
            continue;
        }
        stop.reason = vm.getState() == VMState::ERROR
            ? "vm_execution_error" : "vm_stopped_without_retirement";
        stop.pc = cpu.pc();
        stop.distinctRetiredPcs = retiredPcs.size();
        return stop;
    }
    stop.pc = cpu.pc();
    stop.distinctRetiredPcs = retiredPcs.size();
    return stop;
}

bool samePhysicalMemory(const PhysicalAddressMapSnapshot& left,
                        const PhysicalAddressMapSnapshot& right) {
    if (left.payloadBytes != right.payloadBytes ||
        left.reservationTrackingVersion != right.reservationTrackingVersion ||
        left.nextReservationGeneration != right.nextReservationGeneration ||
        left.reservationGenerations != right.reservationGenerations ||
        left.regions.size() != right.regions.size()) return false;
    for (size_t index = 0; index < left.regions.size(); ++index) {
        const auto& a = left.regions[index];
        const auto& b = right.regions[index];
        if (a.info.base != b.info.base || a.info.size != b.info.size ||
            a.info.type != b.info.type || a.info.readable != b.info.readable ||
            a.info.writable != b.info.writable || a.info.accessWidths != b.info.accessWidths ||
            a.backendKind != b.backendKind || a.backingId != b.backingId ||
            a.sparseCapacity != b.sparseCapacity || a.denseBytes != b.denseBytes ||
            a.sparseBytes != b.sparseBytes) return false;
    }
    return true;
}

bool sameTerminalState(const Mips64FullVmSnapshot& a,
                       const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity() || a.magic != b.magic ||
        a.formatVersion != b.formatVersion || a.isaIdentity != b.isaIdentity ||
        a.boardProfileIdentity != b.boardProfileIdentity || a.cpuCount != b.cpuCount ||
        a.endianness != b.endianness || a.byteOrderTag != b.byteOrderTag ||
        a.ramSize != b.ramSize || a.romBase != b.romBase || a.romSize != b.romSize ||
        a.romContentHash != b.romContentHash ||
        a.flashMirrorMapped != b.flashMirrorMapped ||
        a.configuredSwitches != b.configuredSwitches ||
        a.configuredJumpers != b.configuredJumpers || a.builtInDemoRom != b.builtInDemoRom ||
        a.mmuEnabled != b.mmuEnabled || a.cpuComponentVersion != b.cpuComponentVersion ||
        a.memoryComponentVersion != b.memoryComponentVersion ||
        a.fpgaComponentVersion != b.fpgaComponentVersion ||
        a.uartComponentVersion != b.uartComponentVersion ||
        a.irqRoutingComponentVersion != b.irqRoutingComponentVersion ||
        a.consoleComponentVersion != b.consoleComponentVersion ||
        a.schedulerComponentVersion != b.schedulerComponentVersion ||
        a.vmComponentVersion != b.vmComponentVersion || a.vmStateValue != b.vmStateValue ||
        a.cpuComponentLength != b.cpuComponentLength ||
        a.memoryComponentLength != b.memoryComponentLength ||
        a.deviceComponentLength != b.deviceComponentLength ||
        a.vmComponentLength != b.vmComponentLength || a.totalSize != b.totalSize ||
        a.cpus.size() != 1 || b.cpus.size() != 1 ||
        a.cpus[0].architecturalState != b.cpus[0].architecturalState ||
        a.cpus[0].executionState != b.cpus[0].executionState ||
        a.cpus[0].instructionsExecuted != b.cpus[0].instructionsExecuted ||
        a.cpus[0].cyclesExecuted != b.cpus[0].cyclesExecuted ||
        a.cpus[0].idleCycles != b.cpus[0].idleCycles ||
        a.cpus[0].enabled != b.cpus[0].enabled ||
        a.cpus[0].lastActivationTime != b.cpus[0].lastActivationTime ||
        a.totalVirtualCycles != b.totalVirtualCycles || a.activeCpuIndex != b.activeCpuIndex ||
        a.pendingDeferredBoardReset != b.pendingDeferredBoardReset ||
        a.uartInterruptTargetCpu != b.uartInterruptTargetCpu ||
        a.uartHardwareInterruptLine != b.uartHardwareInterruptLine ||
        a.uartCauseIp != b.uartCauseIp || a.routedUartInterruptLevel != b.routedUartInterruptLevel ||
        !samePhysicalMemory(a.physicalMemory, b.physicalMemory) ||
        a.fpga.version != b.fpga.version || a.fpga.endianness != b.fpga.endianness ||
        a.fpga.switches != b.fpga.switches || a.fpga.jumpers != b.fpga.jumpers ||
        a.fpga.ledBar != b.fpga.ledBar || a.fpga.asciiDisplay != b.fpga.asciiDisplay ||
        a.fpga.breakReset != b.fpga.breakReset || a.fpga.resetPending != b.fpga.resetPending ||
        a.fpga.resetRequestCount != b.fpga.resetRequestCount ||
        a.uart.transmitCount != b.uart.transmitCount || a.uart.version != b.uart.version ||
        a.uart.ier != b.uart.ier || a.uart.fcrLastWrite != b.uart.fcrLastWrite ||
        a.uart.lcr != b.uart.lcr || a.uart.mcr != b.uart.mcr ||
        a.uart.scratch != b.uart.scratch ||
        a.uart.divisorLatchLow != b.uart.divisorLatchLow ||
        a.uart.divisorLatchHigh != b.uart.divisorLatchHigh || a.uart.dlab != b.uart.dlab ||
        a.uart.transmitterReady != b.uart.transmitterReady ||
        a.uart.fifoEnabled != b.uart.fifoEnabled ||
        a.uart.receiveTriggerLevel != b.uart.receiveTriggerLevel ||
        a.uart.receiveOverrun != b.uart.receiveOverrun ||
        a.uart.receiveFifo != b.uart.receiveFifo ||
        a.uart.pendingHostInput != b.uart.pendingHostInput ||
        a.uart.virtualTimeCycle != b.uart.virtualTimeCycle ||
        a.uart.receiveTimeoutDeadlineCycle != b.uart.receiveTimeoutDeadlineCycle ||
        a.uart.receiveTimeoutArmed != b.uart.receiveTimeoutArmed ||
        a.uart.receiveTimeoutPending != b.uart.receiveTimeoutPending ||
        a.console.capacityBytes != b.console.capacityBytes || a.console.bytes != b.console.bytes ||
        a.console.sequence != b.console.sequence ||
        a.scheduler.lastCPUIndex != b.scheduler.lastCPUIndex ||
        a.scheduler.numCPUs != b.scheduler.numCPUs ||
        a.scheduler.quantumSize != b.scheduler.quantumSize ||
        a.scheduler.cpuBundleCounters != b.scheduler.cpuBundleCounters) return false;
    return true;
}

int runAuthenticSyncContinuation(const std::filesystem::path& imagePath,
                                 const std::filesystem::path& tracePath,
                                 const std::filesystem::path& phase25TracePath) {
    if (std::filesystem::exists(tracePath)) {
        throw std::runtime_error("refusing to overwrite trace: " + tracePath.string());
    }
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != kKernelSize) throw std::runtime_error("unexpected kernel ELF size");
    const auto acceptedPrefix = linesOfType(phase25TracePath, "instruction");
    const auto acceptedSummary = linesOfType(phase25TracePath,
                                              "authentic_dextm_continuation_summary");
    const auto acceptedFrontier = linesOfType(phase25TracePath, "continuation_frontier");
    const auto acceptedContinuation = linesOfType(phase25TracePath, "continuation_instruction");
    if (acceptedPrefix.size() != 256 || acceptedSummary.size() != 1 ||
        acceptedFrontier.size() != 1 || acceptedContinuation.size() != 5678 ||
        acceptedSummary[0].find("\"next_frontier\":\"0xffffffff8091eb50\"") == std::string::npos ||
        acceptedSummary[0].find("\"next_word\":\"0x0000000f\"") == std::string::npos ||
        acceptedFrontier[0].find("\"stop_reason\":\"unsupported_instruction\"") == std::string::npos) {
        throw std::runtime_error("accepted Phase 25 frontier or trace counts changed");
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    VirtualMachine vm(static_cast<size_t>(256ULL * 1024 * 1024), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = 256ULL * 1024 * 1024;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("Malta ELF handoff setup failed: " + error);
    }
    if (!vm.init()) throw std::runtime_error("production VM initialization failed");
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    if (!plugin || !context || !board || !board->Uart() || !board->ConsoleSink()) {
        throw std::runtime_error("production Malta CPU/devices are unavailable");
    }
    Mips64Cpu& cpu = plugin->core();

    std::ostringstream prefixTrace;
    for (unsigned index = 0; index < acceptedPrefix.size(); ++index) {
        writeInitialRecord(prefixTrace, vm, cpu, *context);
    }
    std::istringstream prefixLines(prefixTrace.str());
    std::string line;
    for (const std::string& accepted : acceptedPrefix) {
        if (!std::getline(prefixLines, line) || line != accepted) {
            throw std::runtime_error("accepted Phase 25 initial prefix diverged");
        }
    }
    if (vm.run(kRetiredBeforeSync - acceptedPrefix.size()) !=
            kRetiredBeforeSync - acceptedPrefix.size() ||
        cpu.pc() != kSyncPc || context->instructionsExecuted != kRetiredBeforeSync ||
        cpu.cp0Count() != kRetiredBeforeSync || vm.getCyclesExecuted() != kRetiredBeforeSync) {
        throw std::runtime_error("production replay failed to reach the accepted SYNC frontier");
    }

    uint64_t syncPhysical = 0;
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &syncPhysical);
    const Mips64Decoded decoded = decodeInstruction(word);
    if (word != 0x0000000FU || syncPhysical != 0x00091EB50ULL || !decoded.valid ||
        decoded.op != Mips64Op::Sync || decoded.stype != 0 || cpu.pc() != kSyncPc ||
        cpu.cp0Status() != 0x00400080ULL || cpu.cp0Cause() != 0 || cpu.cp0Epc() != 0 ||
        cpu.cp0BadVAddr() != 0 || cpu.cp0Count() != 157780 || vm.getCyclesExecuted() != 157780 ||
        context->instructionsExecuted != 157780 || cpu.delayArmed() ||
        cpu.reservationState().valid || validTlbEntries(cpu) != 0 ||
        board->Uart()->ReceiveFifoSize() != 0) {
        throw std::runtime_error("authentic SYNC encoding or pre-execution state changed");
    }
    for (unsigned lineIndex = 0; lineIndex < 6; ++lineIndex) {
        if (cpu.hardwareInterruptLine(lineIndex)) {
            throw std::runtime_error("unexpected pending hardware interrupt at SYNC");
        }
    }

    if (!tracePath.parent_path().empty()) {
        std::filesystem::create_directories(tracePath.parent_path());
    }
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create Phase 26 SYNC trace");
    trace << prefixTrace.str();
    trace << "{\"type\":\"production_environment\",\"kernel_size\":" << image.size()
          << ",\"kernel_sha256\":\"" << kKernelSha256
          << "\",\"accepted_phase25_trace_sha256\":\"" << kPhase25TraceSha256
          << "\",\"linux_entry\":\"" << hexValue(kLinuxEntry, 16)
          << "\",\"guest_ram_bytes\":268435456,\"endianness\":\"big\""
          << ",\"prom_command_line\":\"console=ttyS0,38400n8\""
          << ",\"rom_bytes\":" << board->BootRomImage().size()
          << ",\"phase25_prefix_records_matched\":256"
          << ",\"accepted_phase25_continuation_records\":"
          << acceptedContinuation.size() << "}\n";
    trace << "{\"type\":\"gap\",\"region\":\"phase25_prefix_to_sync\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":"
          << kRetiredBeforeSync << ",\"skipped_instructions\":"
          << (kRetiredBeforeSync - 256) << "}\n";
    trace << "{\"type\":\"phase25_pre_sync_memory_activity\",\"events\":[";
    bool firstActivity = true;
    for (const std::string& record : acceptedContinuation) {
        const bool relevantCycle = record.find("\"cycle\":157758") != std::string::npos ||
            record.find("\"cycle\":157762") != std::string::npos ||
            record.find("\"cycle\":157765") != std::string::npos ||
            record.find("\"cycle\":157777") != std::string::npos;
        if (!relevantCycle) continue;
        if (!firstActivity) trace << ',';
        trace << record;
        firstActivity = false;
    }
    trace << "]}\n";

    const auto preGprs = gprsOf(cpu);
    const auto preCp0 = cp0Of(cpu);
    const auto preCpuBytes = cpu.serialize();
    const auto preReservation = cpu.reservationState();
    const uint64_t preHi = cpu.hi();
    const uint64_t preLo = cpu.lo();
    const Mips64FullVmSnapshot beforeSync = vm.captureMips64FullSnapshot();
    if (!beforeSync.ValidateIntegrity() || beforeSync.formatVersion != 3 ||
        beforeSync.cpuComponentVersion != 6 || beforeSync.cpus[0].architecturalState.size() != 1001 ||
        beforeSync.memoryComponentVersion != 2 || beforeSync.uartComponentVersion != 4 ||
        beforeSync.irqRoutingComponentVersion != 1) {
        throw std::runtime_error("authentic pre-SYNC VM snapshot is invalid or migrated");
    }

    trace << "{\"type\":\"authentic_sync_pre_instruction\",\"pc_va\":\""
          << hexValue(cpu.pc(), 16) << "\",\"pc_pa\":\"" << hexValue(syncPhysical, 9)
          << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"opcode\":0,\"rs\":0,\"rt\":0,\"rd\":0,\"stype\":0"
          << ",\"funct\":15,\"disassembly\":\"" << cpu.disassemble(decoded)
          << "\",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << cpu.cp0Count() << ",\"vm_cycles\":"
          << vm.getCyclesExecuted() << ",\"retired\":" << context->instructionsExecuted
          << ",\"delay_armed\":false,\"hi\":\"" << hexValue(preHi, 16)
          << "\",\"lo\":\"" << hexValue(preLo, 16) << "\",\"gpr\":[";
    for (size_t index = 0; index < preGprs.size(); ++index) {
        if (index != 0) trace << ',';
        trace << '"' << hexValue(preGprs[index], 16) << '"';
    }
    trace << "],\"reservation\":{\"valid\":"
          << (preReservation.valid ? "true" : "false") << ",\"size\":"
          << static_cast<unsigned>(preReservation.size) << ",\"cca\":"
          << static_cast<unsigned>(preReservation.cacheAttribute) << ",\"va\":\""
          << hexValue(preReservation.virtualAddress, 16) << "\",\"pa\":\""
          << hexValue(preReservation.physicalAddress, 16) << "\",\"backing\":"
          << preReservation.backingId << ",\"granule\":\""
          << hexValue(preReservation.granuleOffset, 16) << "\",\"generation\":"
          << preReservation.generation << "},\"hardware_irq_lines\":[";
    for (unsigned index = 0; index < 6; ++index) {
        if (index != 0) trace << ',';
        trace << (cpu.hardwareInterruptLine(index) ? "true" : "false");
    }
    trace << "],\"uart_rx_fifo_bytes\":" << board->Uart()->ReceiveFifoSize()
          << ",\"cp0_state_ordered\":[";
    for (size_t index = 0; index < preCp0.size(); ++index) {
        if (index != 0) trace << ',';
        trace << '"' << hexValue(preCp0[index], 16) << '"';
    }
    trace << "],\"baseline_typed_result\":\"UnsupportedInstruction\""
          << ",\"baseline_execution\":\"not executed\""
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1}\n";

    const uint64_t retiredBeforeSync = context->instructionsExecuted;
    const uint64_t cyclesBeforeSync = vm.getCyclesExecuted();
    const uint64_t uartTxBeforeSync = board->Uart()->CaptureSnapshot().transmitCount;
    const size_t uartRxBeforeSync = board->Uart()->ReceiveFifoSize();
    const std::vector<uint8_t>& memoryBeforeSync =
        beforeSync.physicalMemory.regions.front().denseBytes;
    const bool syncStepRetired = vm.run(1) == 1;
    const auto postSyncCp0 = cp0Of(cpu);
    bool cp0ExceptCountUnchanged = true;
    for (size_t index = 0; index < postSyncCp0.size(); ++index) {
        if (index != 6 && postSyncCp0[index] != preCp0[index]) {
            cp0ExceptCountUnchanged = false;
        }
    }
    if (!syncStepRetired || context->instructionsExecuted != retiredBeforeSync + 1 ||
        cpu.cp0Count() != 157781 || vm.getCyclesExecuted() != cyclesBeforeSync + 1 ||
        cpu.pc() != kSyncPc + 4 || cpu.delayArmed() ||
        cpu.fault() != Mips64Fault::None || cpu.causeExcCode() != 0 ||
        cpu.hi() != preHi || cpu.lo() != preLo || gprsOf(cpu) != preGprs ||
        !sameReservation(preReservation, cpu.reservationState()) ||
        board->Uart()->CaptureSnapshot().transmitCount != uartTxBeforeSync ||
        board->Uart()->ReceiveFifoSize() != uartRxBeforeSync ||
        !cp0ExceptCountUnchanged) {
        throw std::runtime_error("production Malta VM failed to retire authentic SYNC precisely");
    }
    const auto* currentRam = vm.getMemory().GetRawData();
    if (!currentRam || memoryBeforeSync.size() != vm.getMemory().GetTotalSize() ||
        !std::equal(memoryBeforeSync.begin(), memoryBeforeSync.end(), currentRam)) {
        throw std::runtime_error("SYNC changed guest RAM");
    }
    const std::vector<uint8_t> cpuAfterSync = cpu.serialize();
    trace << "{\"type\":\"authentic_sync_retirement\",\"pre_pc\":\""
          << hexValue(kSyncPc, 16) << "\",\"word\":\"" << hexValue(word, 8)
          << "\",\"post_pc\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"count_before\":157780,\"count_after\":" << cpu.cp0Count()
          << ",\"cycles_before\":" << cyclesBeforeSync << ",\"cycles_after\":"
          << vm.getCyclesExecuted() << ",\"retired_before\":" << retiredBeforeSync
          << ",\"retired_after\":" << context->instructionsExecuted
          << ",\"delay_armed_after\":false,\"gpr_unchanged\":true"
          << ",\"hi_lo_unchanged\":true,\"cp0_except_count_unchanged\":true"
          << ",\"ram_unchanged\":true,\"reservation_unchanged\":true"
          << ",\"uart_tx_after\":" << board->Uart()->CaptureSnapshot().transmitCount
          << ",\"uart_rx_after\":" << board->Uart()->ReceiveFifoSize() << "}\n";

    Mips64FullVmSnapshot invalidSnapshot = beforeSync;
    invalidSnapshot.integrityChecksum ^= 1U;
    const std::vector<uint8_t> liveCpuBeforeReject = cpu.serialize();
    const uint64_t liveCyclesBeforeReject = vm.getCyclesExecuted();
    std::string invalidError;
    if (invalidSnapshot.ValidateIntegrity() ||
        vm.restoreMips64FullSnapshot(invalidSnapshot, &invalidError) || invalidError.empty() ||
        cpu.serialize() != liveCpuBeforeReject || vm.getCyclesExecuted() != liveCyclesBeforeReject) {
        throw std::runtime_error("invalid full-VM snapshot was not rejected transactionally");
    }
    trace << "{\"type\":\"invalid_snapshot_rejection\",\"rejected\":true"
          << ",\"live_cpu_unchanged\":true,\"virtual_time_unchanged\":true}\n";

    const ContinuationStop firstStop = runToFrontier(vm, cpu, *context,
                                                      kContinuationBudget, &trace);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid terminal snapshot");
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRx = board->Uart()->ReceiveFifoSize();
    const auto console = board->ConsoleSink()->CaptureSnapshot();

    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(beforeSync, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kSyncPc || context->instructionsExecuted != retiredBeforeSync ||
        vm.getCyclesExecuted() != cyclesBeforeSync || cpu.serialize() != preCpuBytes) {
        throw std::runtime_error("pre-SYNC full-VM snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.serialize() != cpuAfterSync ||
        context->instructionsExecuted != retiredBeforeSync + 1 || cpu.pc() != kSyncPc + 4) {
        throw std::runtime_error("authentic SYNC immediate snapshot replay diverged");
    }
    const ContinuationStop replayStop = runToFrontier(vm, cpu, *context,
                                                       kContinuationBudget, nullptr);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        firstRetired != context->instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() || !sameTerminalState(firstTerminal, replayTerminal)) {
        throw std::runtime_error("authentic SYNC continuation snapshot replay diverged");
    }

    const uint8_t opcode = static_cast<uint8_t>((firstStop.word >> 26) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((firstStop.word >> 21) & 0x1FU);
    const uint8_t rt = static_cast<uint8_t>((firstStop.word >> 16) & 0x1FU);
    const uint8_t rd = static_cast<uint8_t>((firstStop.word >> 11) & 0x1FU);
    const uint8_t stype = static_cast<uint8_t>((firstStop.word >> 6) & 0x1FU);
    const uint8_t funct = static_cast<uint8_t>(firstStop.word & 0x3FU);
    trace << "{\"type\":\"authentic_sync_continuation_summary\",\"retired\":"
          << firstRetired << ",\"retired_since_linux_entry\":"
          << (firstRetired - kLinuxEntryRetiredBefore)
          << ",\"retired_beyond_former_frontier\":"
          << (firstRetired - kRetiredBeforeSync)
          << ",\"instructions_beyond_sync\":"
          << (firstRetired - (kRetiredBeforeSync + 1))
          << ",\"attempts\":" << (kRetiredBeforeSync + 1 + firstStop.attempts)
          << ",\"sync_verified\":true,\"authentic_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true,\"next_frontier\":\""
          << hexValue(firstStop.pc, 16) << "\",\"next_fetch_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"next_word\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"frontier_attempts\":" << firstStop.attempts
          << ",\"frontier_retired\":" << firstStop.retired
          << ",\"distinct_retired_pcs\":" << firstStop.distinctRetiredPcs
          << ",\"count\":" << cpu.cp0Count() << ",\"vm_cycles\":"
          << vm.getCyclesExecuted() << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"reservation_valid\":" << (cpu.reservationState().valid ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"uart_tx_bytes\":" << uartTx << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"console_sequence\":" << console.sequence
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1"
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"opcode\":" << static_cast<unsigned>(opcode)
          << ",\"rs\":" << static_cast<unsigned>(rs) << ",\"rt\":"
          << static_cast<unsigned>(rt) << ",\"rd\":" << static_cast<unsigned>(rd)
          << ",\"stype\":" << static_cast<unsigned>(stype) << ",\"funct\":"
          << static_cast<unsigned>(funct) << ",\"disassembly\":\""
          << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    trace.flush();
    if (!trace) throw std::runtime_error("failed writing Phase 26 SYNC trace");

    std::cout << "MIPS64_SYNC_LINUX_CONTINUATION pc=0x" << std::hex << kSyncPc
              << " word=0x" << word << " stype=0x" << static_cast<unsigned>(decoded.stype)
              << " total_retired=" << std::dec << firstRetired
              << " beyond_old=" << (firstRetired - kRetiredBeforeSync)
              << " beyond_sync=" << (firstRetired - (kRetiredBeforeSync + 1))
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << cpu.cp0Count()
              << " cycles=" << vm.getCyclesExecuted() << " uart_tx=" << uartTx
              << " uart_rx=" << uartRx << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_sync: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 5 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticSyncContinuation(argv[2], argv[3], argv[4]);
        }
        std::cerr << "Usage: test_mips64_sync [--continue-authentic-kernel <ELF> <JSONL> <phase25-jsonl>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_sync: " << error.what() << "\n";
        return 1;
    }
}
