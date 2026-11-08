// MIPS64-19 architectural DI/EI, EHB, interrupt, and full-VM replay tests.

#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_mmu_setup.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

constexpr uint64_t kRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTextPa = 0x1000;
constexpr uint64_t kTextVa = kCKSeg0Base | kTextPa;
constexpr uint64_t kPayloadVa = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kHandlerVa = 0xFFFFFFFF80000180ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80003000ULL;
constexpr uint64_t kUartVa = 0xFFFFFFFFBF000900ULL;
constexpr uint32_t kCauseRecordPa = 0x3000;
constexpr uint32_t kEpcRecordPa = 0x3008;
constexpr uint32_t kHandlerCountPa = 0x3010;
constexpr uint32_t kResumeMarkerPa = 0x3014;
constexpr uint32_t kRxBytePa = 0x3200;
constexpr uint32_t kRxCausePa = 0x3208;
constexpr uint32_t kRxEpcPa = 0x3210;
constexpr uint32_t kRxHandlerCountPa = 0x3218;
constexpr uint32_t kDiWord = 0x41606000U;
constexpr uint32_t kEiWord = 0x41606020U;
constexpr uint32_t kEhbWord = 0x000000C0U;
constexpr uint64_t kCompareValue = 80;
constexpr uint32_t kTimerStatus = 0x00008101U; // IE | IM7
constexpr uint32_t kUartStatus = 0x00001101U;  // IE | IM4

uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(sa) << 6) | funct;
}

uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel = 0) {
    return (0x10U << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | (uint32_t(rd) << 11) | sel;
}

uint32_t encDiEi(uint8_t rt, bool enable) {
    return (enable ? kEiWord : kDiWord) | (uint32_t(rt) << 16);
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(word >> (8 * (little ? i : 3 - i)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[i]) << (8 * (little ? i : 3 - i));
    }
    return value;
}

uint64_t readDoubleWord(const IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= uint64_t(bytes[i]) << (8 * (little ? i : 7 - i));
    }
    return value;
}

std::vector<uint8_t> encodeWords(const std::vector<uint32_t>& words,
                                 Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    std::vector<uint8_t> bytes(words.size() * 4);
    for (size_t index = 0; index < words.size(); ++index) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            bytes[index * 4 + byte] = static_cast<uint8_t>(
                words[index] >> (8 * (little ? byte : 3 - byte)));
        }
    }
    return bytes;
}

uint64_t signExtend32Expected(uint32_t value) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
           a.cacheAttribute == b.cacheAttribute &&
           a.virtualAddress == b.virtualAddress &&
           a.physicalAddress == b.physicalAddress &&
           a.backingId == b.backingId && a.granuleOffset == b.granuleOffset &&
           a.generation == b.generation;
}

void setupCpuStatusProgram(Mips64Cpu& cpu, IMemory& memory,
                           Mips64Endianness endian, uint32_t status,
                           uint32_t cause = 0) {
    const bool little = endian == Mips64Endianness::Little;
    cpu.setPc(kTextVa);
    cpu.writeGpr(8, 0x20);              // Count seed.
    cpu.writeGpr(9, 0x12345678);        // Compare seed.
    cpu.writeGpr(10, cause);            // Software Cause pending bits.
    cpu.writeGpr(11, status);           // Status seed.
    storeWord(memory, kTextPa, encCp0(4, 8, 9), little);       // mtc0 Count
    storeWord(memory, kTextPa + 4, encCp0(4, 9, 11), little); // mtc0 Compare
    storeWord(memory, kTextPa + 8, encCp0(4, 10, 13), little);// mtc0 Cause
    storeWord(memory, kTextPa + 12, encCp0(4, 11, 12), little);// mtc0 Status
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    }
}

void testDecodeFieldsAndDisassembly() {
    const Mips64Decoded di = decodeInstruction(kDiWord);
    const Mips64Decoded ei = decodeInstruction(kEiWord);
    CHECK(di.valid && di.op == Mips64Op::Di);
    CHECK(ei.valid && ei.op == Mips64Op::Ei);
    CHECK(di.raw == kDiWord && ei.raw == kEiWord);
    CHECK(di.rs == 11 && di.rt == 0 && di.rd == 12 && di.sa == 0);
    CHECK(ei.rs == 11 && ei.rt == 0 && ei.rd == 12 && ei.sa == 0);
    CHECK(Mips64Cpu{}.disassemble(di) == "di $0");
    CHECK(Mips64Cpu{}.disassemble(ei) == "ei $0");

    for (uint8_t rt = 0; rt < 32; ++rt) {
        const Mips64Decoded d = decodeInstruction(encDiEi(rt, false));
        const Mips64Decoded e = decodeInstruction(encDiEi(rt, true));
        CHECK(d.valid && d.op == Mips64Op::Di && d.rt == rt);
        CHECK(e.valid && e.op == Mips64Op::Ei && e.rt == rt);
    }
    CHECK(Mips64Cpu{}.disassemble(decodeInstruction(encDiEi(9, false))) == "di $9");
    CHECK(Mips64Cpu{}.disassemble(decodeInstruction(encDiEi(13, true))) == "ei $13");

    const Mips64Decoded jrHb = decodeInstruction(encR(8, 0, 0, 16, 0x08));
    const Mips64Decoded jalrHb = decodeInstruction(encR(8, 0, 9, 16, 0x09));
    CHECK(jrHb.valid && jrHb.op == Mips64Op::JrHb);
    CHECK(jalrHb.valid && jalrHb.op == Mips64Op::JalrHb);
    CHECK(Mips64Cpu{}.disassemble(jrHb) == "jr.hb $8");
    CHECK(Mips64Cpu{}.disassemble(jalrHb) == "jalr.hb $9, $8");

    const std::array<uint32_t, 5> malformed{{
        kDiWord | (1U << 6),           // nonzero sa
        kDiWord | 1U,                  // reserved low field
        kDiWord | (1U << 4),           // reserved bit 4
        kDiWord ^ (1U << 11),           // rd is not Status
        (0x10U << 26) | (10U << 21) | (12U << 11), // other MFMC0 rs
    }};
    for (uint32_t word : malformed) {
        const Mips64Decoded decoded = decodeInstruction(word);
        CHECK(!decoded.valid);
        CHECK(decoded.fault == Mips64Fault::ReservedInstruction);
    }
    const Mips64Decoded mfc0 = decodeInstruction(encCp0(0, 9, 12));
    const Mips64Decoded dmfc0 = decodeInstruction(encCp0(1, 9, 12));
    const Mips64Decoded mtc0 = decodeInstruction(encCp0(4, 9, 12));
    const Mips64Decoded dmtc0 = decodeInstruction(encCp0(5, 9, 12));
    CHECK(mfc0.valid && mfc0.op == Mips64Op::Mfc0);
    CHECK(dmfc0.valid && dmfc0.op == Mips64Op::Dmfc0);
    CHECK(mtc0.valid && mtc0.op == Mips64Op::Mtc0);
    CHECK(dmtc0.valid && dmtc0.op == Mips64Op::Dmtc0);

    const Mips64Decoded ehb = decodeInstruction(kEhbWord);
    const Mips64Decoded ssnop = decodeInstruction(0x00000040U);
    const Mips64Decoded ordinarySll = decodeInstruction(0x00000080U);
    CHECK(ehb.valid && ehb.op == Mips64Op::Ehb);
    CHECK(Mips64Cpu{}.disassemble(ehb) == "ehb");
    CHECK(ssnop.valid && ssnop.op == Mips64Op::Sll);
    CHECK(ordinarySll.valid && ordinarySll.op == Mips64Op::Sll);
}

void executeStatusCase(Mips64Endianness endian, uint32_t status,
                       bool enable, uint8_t destination) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(kRamBytes, false);
    Mips64Cpu cpu(endian);
    setupCpuStatusProgram(cpu, memory, endian, status);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA55A000000000000ULL | reg);
    }
    // Restore the setup value of the destination so its old value is tested.
    if (destination != 0) cpu.writeGpr(destination, 0xFEEDFACECAFEBEEFULL);
    const uint64_t beforeStatus = cpu.cp0Status();
    const uint64_t beforeCause = cpu.cp0Cause();
    const uint64_t beforeCount = cpu.cp0Count();
    const uint64_t beforeCompare = cpu.cp0Compare();
    const uint64_t beforeEpc = cpu.cp0Epc();
    const uint64_t beforeErrorEpc = cpu.cp0ErrorEpc();
    const uint64_t beforeBadVAddr = cpu.cp0BadVAddr();
    const uint64_t oldDest = cpu.readGpr(destination);
    const uint64_t oldPc = cpu.pc();
    const uint32_t instruction = encDiEi(destination, enable);
    storeWord(memory, kTextPa + 16, instruction, little);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    const uint64_t expectedStatus = (beforeStatus & ~kStatusBitIe) |
        (enable ? kStatusBitIe : 0ULL);
    CHECK(cpu.cp0Status() == expectedStatus);
    CHECK(cpu.cp0Status() == ((beforeStatus & ~kStatusBitIe) |
        (enable ? 1ULL : 0ULL)));
    CHECK(cpu.readGpr(destination) == (destination == 0 ? oldDest :
        signExtend32Expected(static_cast<uint32_t>(beforeStatus))));
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == oldPc + 4);
    CHECK(cpu.cp0Count() == beforeCount + 1);
    CHECK(cpu.cp0Compare() == beforeCompare);
    CHECK(cpu.cp0Cause() == beforeCause);
    CHECK(cpu.cp0Epc() == beforeEpc);
    CHECK(cpu.cp0ErrorEpc() == beforeErrorEpc);
    CHECK(cpu.cp0BadVAddr() == beforeBadVAddr);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(cpu.interruptsEnabledForDelivery() == ((expectedStatus & kStatusBitIe) != 0));
}

void testAtomicStatusSemanticsAndPreservation() {
    constexpr uint32_t allSupportedStatusBits = 0x0040FFFFU;
    constexpr uint32_t statusIeOn = 0x004091E1U;
    constexpr uint32_t statusIeOff = 0x004091E0U;
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeStatusCase(endian, statusIeOn, false, 9);  // DI from IE=1.
        executeStatusCase(endian, statusIeOff, false, 0);// DI from IE=0, $zero.
        executeStatusCase(endian, statusIeOff, true, 13);// EI from IE=0.
        executeStatusCase(endian, statusIeOn, true, 0); // EI from IE=1, $zero.
        executeStatusCase(endian, allSupportedStatusBits, false, 8);
        executeStatusCase(endian, allSupportedStatusBits & ~1U, true, 8);

        Memory memory(kRamBytes, false);
        Mips64Cpu cpu(endian);
        setupCpuStatusProgram(cpu, memory, endian, statusIeOn);
        storeWord(memory, kTextPa + 16, encDiEi(9, false), endian == Mips64Endianness::Little);
        storeWord(memory, kTextPa + 20, encDiEi(10, true), endian == Mips64Endianness::Little);
        storeWord(memory, kTextPa + 24, encDiEi(11, false), endian == Mips64Endianness::Little);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == signExtend32Expected(statusIeOn));
        CHECK(cpu.cp0Status() == (statusIeOn & ~1U));
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(10) == signExtend32Expected(statusIeOn & ~1U));
        CHECK(cpu.cp0Status() == statusIeOn);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(11) == signExtend32Expected(statusIeOn));
        CHECK(cpu.cp0Status() == (statusIeOn & ~1U));

        // A saved Status with EXL, ERL, BEV, UX/SX/KX, KSU and all IM bits
        // set proves that DI changes only IE despite ordinary MTC0 masking.
        Mips64Cpu preserved(endian);
        setupCpuStatusProgram(preserved, memory, endian, allSupportedStatusBits);
        storeWord(memory, kTextPa + 16, encDiEi(12, false), endian == Mips64Endianness::Little);
        const uint64_t before = preserved.cp0Status();
        CHECK(preserved.step(memory) == Mips64StepResult::Continue);
        CHECK(preserved.cp0Status() == (before & ~kStatusBitIe));
        CHECK((preserved.cp0Status() & (kStatusBitExl | kStatusBitErl |
              kStatusBitBev | kStatusBitUx | kStatusBitSx | kStatusBitKx |
              kStatusKsuMask | kStatusImMask)) ==
              (before & (kStatusBitExl | kStatusBitErl | kStatusBitBev |
              kStatusBitUx | kStatusBitSx | kStatusBitKx | kStatusKsuMask |
              kStatusImMask)));
    }
}

void testMalformedEncodingRaisesRiWithoutStatusMutation() {
    Memory memory(kRamBytes, false);
    Mips64Cpu cpu(Mips64Endianness::Big);
    setupCpuStatusProgram(cpu, memory, Mips64Endianness::Big, 0x004091E1U);
    const uint64_t before = cpu.cp0Status();
    storeWord(memory, kTextPa + 16, kDiWord | (1U << 6), false);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
    CHECK(cpu.cp0Epc() == kTextVa + 16);
    CHECK(cpu.cp0Status() == (before | kStatusBitExl));
    CHECK(cpu.readGpr(0) == 0);
}

void testUserModeAccessRejectedThroughPrivilegePath() {
    Memory memory(kRamBytes, false);
    Mips64Cpu cpu(Mips64Endianness::Little);
    CHECK(mips64_test::setupKusegIdentityMap(cpu, memory));
    const std::vector<uint32_t> enterUser{
        encI(0x0F, 0, 8, 0x0040),
        encI(0x0D, 8, 8, 0x0010), // BEV | KSU=user, EXL/ERL clear.
        encCp0(4, 8, 12),
    };
    for (size_t i = 0; i < enterUser.size(); ++i) {
        mips64_test::storeWordPhys(memory, 0x5000 + i * 4, enterUser[i], true);
    }
    cpu.setPc(0x5000);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.isUserMode());
    cpu.writeGpr(9, 0xAABBCCDDEEFF0011ULL);
    mips64_test::storeWordPhys(memory, 0x500C, encDiEi(9, false), true);
    cpu.setPc(0x500C);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::PrivilegeViolation);
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
    CHECK(cpu.cp0Epc() == 0x500C);
    CHECK(cpu.statusExl());
    CHECK(cpu.readGpr(9) == 0xAABBCCDDEEFF0011ULL);
}

void testInterruptBoundaryAndTimerPendingBehavior() {
    Memory memory(kRamBytes, false);
    Mips64Cpu cpu(Mips64Endianness::Big);
    setupCpuStatusProgram(cpu, memory, Mips64Endianness::Big, 0x00001101U);
    cpu.writeGpr(9, 0xCAFEBABE12345678ULL);
    storeWord(memory, kTextPa + 16, encDiEi(9, false), false);
    cpu.setHardwareInterruptLine(2, true); // IP4 already eligible before DI.
    const uint64_t countBefore = cpu.cp0Count();
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.pc() == 0xFFFFFFFF80000180ULL);
    CHECK(cpu.cp0Epc() == kTextVa + 16);
    CHECK(cpu.statusExl());
    CHECK(cpu.cp0Count() == countBefore + 1);
    CHECK(cpu.readGpr(9) == 0xCAFEBABE12345678ULL); // DI was preempted.
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);

    Memory timerMemory(kRamBytes, false);
    Mips64Cpu timer(Mips64Endianness::Little);
    const bool little = true;
    timer.setPc(kTextVa);
    timer.writeGpr(8, 4);
    timer.writeGpr(9, 0);
    timer.writeGpr(10, kStatusImMask & kCauseIp7Bit);
    storeWord(timerMemory, kTextPa, encCp0(4, 8, 11), little); // Compare = 4.
    storeWord(timerMemory, kTextPa + 4, encCp0(4, 10, 12), little); // IE=0, IM7=1.
    storeWord(timerMemory, kTextPa + 8, encDiEi(10, false), little);
    storeWord(timerMemory, kTextPa + 12, kEhbWord, little);
    storeWord(timerMemory, kTextPa + 16, encDiEi(11, true), little);
    storeWord(timerMemory, kTextPa + 20, kEhbWord, little);
    CHECK(timer.step(timerMemory) == Mips64StepResult::Continue);
    CHECK(timer.cp0Compare() == 4);
    CHECK(timer.step(timerMemory) == Mips64StepResult::Continue);
    CHECK(timer.cp0Status() == (kStatusImMask & kCauseIp7Bit));
    CHECK(timer.step(timerMemory) == Mips64StepResult::Continue); // DI
    CHECK(timer.cp0Status() == 0x8000U);
    CHECK(timer.step(timerMemory) == Mips64StepResult::Continue); // EHB, timer latches
    CHECK((timer.cp0Cause() & (kCauseBitTi | kCauseIp7Bit)) ==
          (kCauseBitTi | kCauseIp7Bit));
    CHECK(timer.step(timerMemory) == Mips64StepResult::Continue); // EI
    CHECK((timer.cp0Cause() & kCauseIp7Bit) != 0);
    CHECK(timer.interruptsEnabledForDelivery());
    CHECK(timer.step(timerMemory) == Mips64StepResult::Interrupt); // sampled before EHB.
    CHECK(timer.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::Int));
    CHECK((timer.cp0Cause() & kCauseIp7Bit) != 0); // EI did not acknowledge timer.

    // A line masked by IM remains visible and is not delivered.
    Memory maskedMemory(kRamBytes, false);
    Mips64Cpu masked(Mips64Endianness::Big);
    setupCpuStatusProgram(masked, maskedMemory, Mips64Endianness::Big, 0x00000101U);
    storeWord(maskedMemory, kTextPa + 16, 0U, false); // NOP
    masked.setHardwareInterruptLine(2, true);
    CHECK(masked.step(maskedMemory) == Mips64StepResult::Continue);
    CHECK((masked.cp0Cause() & kCauseIp4Bit) != 0);
    CHECK(!masked.statusExl());
}

void testExlErlSuppressionAndReservationPreservation() {
    for (uint64_t exceptionLevel : {kStatusBitExl, kStatusBitErl}) {
        Memory memory(kRamBytes, false);
        Mips64Cpu cpu(Mips64Endianness::Big);
        setupCpuStatusProgram(cpu, memory, Mips64Endianness::Big,
                              static_cast<uint32_t>(exceptionLevel | kStatusImMask));
        storeWord(memory, kTextPa + 16, encDiEi(8, true), false);
        storeWord(memory, kTextPa + 20, kEhbWord, false);
        cpu.setHardwareInterruptLine(2, true);
        const uint64_t before = cpu.cp0Status();
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.cp0Status() == (before | kStatusBitIe));
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);
        CHECK(!cpu.statusExl() || exceptionLevel == kStatusBitExl);
        CHECK(!cpu.statusErl() || exceptionLevel == kStatusBitErl);
    }

    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTextVa);
    cpu.writeGpr(8, 0);
    cpu.writeGpr(9, 0x2000);
    cpu.writeGpr(4, kCKSeg0Base | 0x2000);
    storeWord(memory, kTextPa, encCp0(4, 8, 12), false); // IE=0.
    storeWord(memory, kTextPa + 4, encI(0x30, 4, 2, 0), false); // LL.
    storeWord(memory, kTextPa + 8, encDiEi(3, false), false);
    storeWord(memory, kTextPa + 12, kEhbWord, false);
    storeWord(memory, 0x2000, 0x11223344U, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.reservationState().valid && cpu.reservationState().size == 4);
    const Mips64ReservationState reserved = cpu.reservationState();
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(sameReservation(cpu.reservationState(), reserved));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(sameReservation(cpu.reservationState(), reserved));

    // CPU v6 state preserves the LL reservation; EHB needs no deferred CPU state.
    const std::vector<uint8_t> beforeReplay = cpu.serialize();
    CHECK(beforeReplay.size() == Mips64Cpu::kSerializedSize);
    CHECK(beforeReplay[4] == 6);
    cpu.setPc(kTextVa + 16);
    storeWord(memory, kTextPa + 16, encDiEi(6, true), false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    const std::vector<uint8_t> afterFirst = cpu.serialize();
    Mips64Cpu replay(Mips64Endianness::Big);
    CHECK(replay.deserialize(beforeReplay.data(), beforeReplay.size()));
    replay.setPc(kTextVa + 16);
    CHECK(replay.step(memory) == Mips64StepResult::Continue);
    CHECK(replay.serialize() == afterFirst);
}

void testImmediateInterruptEnableAndEhbDelivery() {
    Memory memory(kRamBytes, false);
    Mips64Cpu cpu(Mips64Endianness::Big);
    setupCpuStatusProgram(cpu, memory, Mips64Endianness::Big, 0x00001100U);
    storeWord(memory, kTextPa + 16, encDiEi(8, false), false);
    storeWord(memory, kTextPa + 20, kEhbWord, false);
    storeWord(memory, kTextPa + 24, encDiEi(9, true), false);
    storeWord(memory, kTextPa + 28, kEhbWord, false);
    cpu.setHardwareInterruptLine(2, true);

    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // DI, prior IE=0.
    CHECK(!cpu.interruptsEnabledForDelivery());
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0); // Pending source is not acknowledged.
    std::array<uint64_t, kNumGpr> beforeEhbGprs{};
    for (unsigned reg = 0; reg < kNumGpr; ++reg) beforeEhbGprs[reg] = cpu.readGpr(reg);
    const uint64_t beforeEhbStatus = cpu.cp0Status();
    const uint64_t beforeEhbCause = cpu.cp0Cause();
    const uint64_t beforeEhbCount = cpu.cp0Count();
    const uint64_t beforeEhbCompare = cpu.cp0Compare();
    const uint64_t beforeEhbPc = cpu.pc();
    const uint32_t beforeEhbWord = readWord(memory, kTextPa + 20, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // EHB retires with IE still clear.
    CHECK(cpu.cp0Status() == beforeEhbStatus);
    CHECK(cpu.cp0Cause() == beforeEhbCause && cpu.cp0Compare() == beforeEhbCompare);
    CHECK(cpu.cp0Count() == beforeEhbCount + 1 && cpu.pc() == beforeEhbPc + 4);
    for (unsigned reg = 0; reg < kNumGpr; ++reg) CHECK(cpu.readGpr(reg) == beforeEhbGprs[reg]);
    CHECK(readWord(memory, kTextPa + 20, false) == beforeEhbWord);
    CHECK(!cpu.interruptsEnabledForDelivery());
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // EI updates Status atomically.
    CHECK((cpu.cp0Status() & kStatusBitIe) != 0);
    CHECK(cpu.interruptsEnabledForDelivery());

    // A full CPU snapshot at this boundary stays in the accepted v6 format.
    // The next boundary checks the now-visible IE and delivers before EHB.
    const std::vector<uint8_t> afterEi = cpu.serialize();
    CHECK(afterEi.size() == Mips64Cpu::kSerializedSize && afterEi.size() == 1001);
    CHECK(afterEi[4] == Mips64Cpu::kSerializationVersion && afterEi[4] == 6);
    Mips64Cpu restored;
    CHECK(restored.deserialize(afterEi.data(), afterEi.size()));
    CHECK(restored.interruptsEnabledForDelivery());
    restored.setHardwareInterruptLine(2, true); // IRQ pins are board-owned.
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(restored.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.serialize() == restored.serialize());
    CHECK(cpu.cp0Epc() == kTextVa + 28);

    Memory eretMemory(kRamBytes, false);
    Mips64Cpu eretCpu(Mips64Endianness::Big);
    setupCpuStatusProgram(eretCpu, eretMemory, Mips64Endianness::Big, 0x00001102U);
    storeWord(eretMemory, kTextPa + 16, encDiEi(8, true), false);
    storeWord(eretMemory, kTextPa + 20, 0x42000018U, false); // ERET clears EXL.
    eretCpu.setHardwareInterruptLine(2, true);
    CHECK(eretCpu.step(eretMemory) == Mips64StepResult::Continue);
    CHECK(eretCpu.statusExl() && eretCpu.interruptsEnabledForDelivery());
    CHECK(eretCpu.step(eretMemory) == Mips64StepResult::Continue);
    CHECK(!eretCpu.statusExl() && eretCpu.pc() == eretCpu.cp0Epc());
    CHECK(eretCpu.step(eretMemory) == Mips64StepResult::Interrupt);

    Memory jrMemory(kRamBytes, false);
    Mips64Cpu jrCpu(Mips64Endianness::Big);
    setupCpuStatusProgram(jrCpu, jrMemory, Mips64Endianness::Big, 0x00001100U);
    storeWord(jrMemory, kTextPa + 16, encR(9, 0, 0, 16, 0x08), false); // JR.HB $t1
    storeWord(jrMemory, kTextPa + 24, 0, false);
    jrCpu.writeGpr(9, kTextVa + 32);
    CHECK(jrCpu.step(jrMemory) == Mips64StepResult::Continue);
    CHECK(jrCpu.pc() == kTextVa + 20 && jrCpu.delayArmed());
    CHECK(jrCpu.step(jrMemory) == Mips64StepResult::Continue);
    CHECK(jrCpu.pc() == kTextVa + 32 && !jrCpu.delayArmed());

    Memory jalrMemory(kRamBytes, false);
    Mips64Cpu jalrCpu(Mips64Endianness::Big);
    setupCpuStatusProgram(jalrCpu, jalrMemory, Mips64Endianness::Big, 0x00001100U);
    storeWord(jalrMemory, kTextPa + 16, encR(9, 0, 10, 16, 0x09), false);
    storeWord(jalrMemory, kTextPa + 24, 0, false);
    jalrCpu.writeGpr(9, kTextVa + 32);
    CHECK(jalrCpu.step(jalrMemory) == Mips64StepResult::Continue);
    CHECK(jalrCpu.readGpr(10) == kTextVa + 24);
    CHECK(jalrCpu.step(jalrMemory) == Mips64StepResult::Continue);
    CHECK(jalrCpu.pc() == kTextVa + 32);
}

struct VmImage {
    std::vector<uint32_t> boot;
    std::vector<uint32_t> payload;
    std::vector<uint32_t> handler;
};

VmImage makeTimerImage(uint32_t status, bool uart) {
    constexpr uint8_t t0 = 8, t1 = 9, t2 = 10, s3 = 19, s4 = 20;
    VmImage image;
    image.boot = {
        encI(0x0F, 0, t0, 0x8000),
        encI(0x0D, t0, t0, 0x1000),      // payload KSEG0 VA
        encI(0x09, 0, t1, 0),
        encCp0(4, t1, 9),                // Count = 0
        encI(0x09, 0, t1, uart ? 0x7FFF : static_cast<uint16_t>(kCompareValue)),
        encCp0(4, t1, 11),               // Compare
        encI(0x0F, 0, s3, 0x8000),
        encI(0x0D, s3, s3, 0x3000),      // $s3 = data KSEG0 VA
    };
    if (uart) {
        image.boot.push_back(encI(0x0F, 0, s4, 0xBF00));
        image.boot.push_back(encI(0x0D, s4, s4, 0x0900)); // $s4 = UART KSEG1 VA
        image.boot.push_back(encI(0x09, 0, t2, 3));
        image.boot.push_back(encI(0x28, s4, t2, 0x18));   // LCR=8N1
        image.boot.push_back(encI(0x09, 0, t2, 1));
        image.boot.push_back(encI(0x28, s4, t2, 8));      // IER.RX
    }
    image.boot.push_back(encI(0x0D, 0, t1, static_cast<uint16_t>(status)));
    image.boot.push_back(encCp0(4, t1, 12));               // Status
    image.boot.push_back(encR(t0, 0, 0, 0, 0x08));        // jr $t0
    image.boot.push_back(0);                              // delay slot

    image.payload = {
        encDiEi(10, false), // DI $t2
        kEhbWord,
        encCp0(0, 9, 13),    // mfc0 $t1, Cause
        encI(0x0C, 9, 10, uart ? 0x1000 : 0x8000),
        encI(0x04, 10, 0, 0xFFFC), // beq $t2,$zero, back to mfc0
        0,
        encDiEi(11, true),  // EI $t3
        kEhbWord,
        encI(0x09, 0, 16, uart ? 0x66 : 0x77),
        encI(0x2B, s3, 16, 0x14), // publish resumed guest marker
        encI(0x04, 0, 0, 0xFFFF),
        0,
    };

    if (!uart) {
        image.handler = {
            encCp0(1, 26, 13),           // dmfc0 $k0,Cause
            encI(0x3F, s3, 26, 0),        // sd $k0,0($s3)
            encCp0(1, 27, 14),           // dmfc0 $k1,EPC
            encI(0x3F, s3, 27, 8),        // sd $k1,8($s3)
            encCp0(1, 27, 9),            // dmfc0 $k1,Count
            encCp0(4, 27, 11),           // mtc0 $k1,Compare (timer ack)
            encI(0x09, 0, 27, 1),
            encI(0x2B, s3, 27, 0x10),     // handler count
            0x42000018U,                  // eret
        };
    } else {
        image.handler = {
            encCp0(1, 26, 13),            // dmfc0 $k0,Cause
            encI(0x3F, s3, 26, 0x208),    // record Cause
            encI(0x24, s4, 26, 0),        // lbu $k0,RBR($s4), acknowledge RX
            encI(0x2B, s3, 26, 0x200),    // record received byte
            encCp0(1, 27, 14),            // dmfc0 $k1,EPC
            encI(0x3F, s3, 27, 0x210),    // record EPC
            encI(0x09, 0, 27, 1),
            encI(0x2B, s3, 27, 0x218),    // handler count
            0x42000018U,                  // eret
        };
    }
    return image;
}

Mips64ISAPlugin* plugin(VirtualMachine& vm, int cpu = 0) {
    CPUContext* context = vm.getCPUContext(cpu);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

std::unique_ptr<VirtualMachine> makeProductionVm(Mips64Endianness endian,
                                                 uint32_t status, bool uart) {
    VmImage image = makeTimerImage(status, uart);
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.useBuiltInDemoRom = false;
    config.bootRom = encodeWords(image.boot, endian);
    auto vm = std::make_unique<VirtualMachine>(static_cast<size_t>(kRamBytes), 1, "MIPS64");
    vm->configureMips64MaltaBootstrap(config);
    if (!vm->init()) return {};
    const std::vector<uint8_t> payload = encodeWords(image.payload, endian);
    const std::vector<uint8_t> handler = encodeWords(image.handler, endian);
    if (!vm->loadProgram(payload.data(), payload.size(), 0x1000) ||
        !vm->loadProgram(handler.data(), handler.size(), 0x180)) return {};
    return vm;
}

bool runOneCycle(VirtualMachine& vm) {
    return vm.run(1) == 1;
}

bool sameCpuBoundary(const Mips64FullVmSnapshot& a,
                     const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity() ||
        a.formatVersion != b.formatVersion ||
        a.totalVirtualCycles != b.totalVirtualCycles ||
        a.activeCpuIndex != b.activeCpuIndex || a.cpus.size() != b.cpus.size() ||
        a.cpus.empty() || a.cpus[0].architecturalState != b.cpus[0].architecturalState ||
        a.cpus[0].instructionsExecuted != b.cpus[0].instructionsExecuted ||
        a.cpus[0].cyclesExecuted != b.cpus[0].cyclesExecuted ||
        a.console.bytes != b.console.bytes || a.console.sequence != b.console.sequence ||
        a.uart.receiveFifo != b.uart.receiveFifo ||
        a.uart.pendingHostInput != b.uart.pendingHostInput ||
        a.uart.ier != b.uart.ier || a.uart.virtualTimeCycle != b.uart.virtualTimeCycle ||
        a.uart.transmitCount != b.uart.transmitCount ||
        a.routedUartInterruptLevel != b.routedUartInterruptLevel ||
        a.physicalMemory.regions.size() != b.physicalMemory.regions.size() ||
        a.physicalMemory.reservationGenerations != b.physicalMemory.reservationGenerations) {
        return false;
    }
    for (size_t i = 0; i < a.physicalMemory.regions.size(); ++i) {
        const auto& left = a.physicalMemory.regions[i];
        const auto& right = b.physicalMemory.regions[i];
        if (left.info.base != right.info.base || left.info.size != right.info.size ||
            left.denseBytes != right.denseBytes || left.sparseBytes != right.sparseBytes) {
            return false;
        }
    }
    return true; // Restore intentionally advances the host-only console generation.
}

void testProductionTimerMaskingAndSnapshotReplay() {
    for (Mips64Endianness endian : {Mips64Endianness::Big, Mips64Endianness::Little}) {
        const bool little = endian == Mips64Endianness::Little;
        auto vm = makeProductionVm(endian, kTimerStatus, false);
        CHECK(vm != nullptr);
        if (!vm) continue;
        CHECK(vm->run(12) == 12); // execute the encoded Malta ROM handoff
        Mips64ISAPlugin* cpu = plugin(*vm);
        CHECK(cpu != nullptr);
        if (!cpu) continue;
        CHECK(cpu->core().pc() == kPayloadVa);
        CHECK(cpu->core().cp0Status() == kTimerStatus);
        Mips64FullVmSnapshot beforeDi = vm->captureMips64FullSnapshot();
        CHECK(beforeDi.formatVersion == 3 && beforeDi.cpuComponentVersion == 6);
        CHECK(beforeDi.memoryComponentVersion == 2 && beforeDi.uartComponentVersion == 4);
        CHECK(beforeDi.irqRoutingComponentVersion == 1);

        CPUContext* context = vm->getCPUContext(0);
        const uint64_t beforeDiInstructions = context->instructionsExecuted;
        CHECK(runOneCycle(*vm));
        CHECK(context->instructionsExecuted == beforeDiInstructions + 1);
        CHECK(cpu->core().pc() == kPayloadVa + 4);
        CHECK(cpu->core().cp0Status() == (kTimerStatus & ~kStatusBitIe));
        CHECK(cpu->core().readGpr(10) == signExtend32Expected(kTimerStatus));
        Mips64FullVmSnapshot afterDi = vm->captureMips64FullSnapshot();

        std::string restoreError;
        CHECK(vm->restoreMips64FullSnapshot(beforeDi, &restoreError));
        CHECK(restoreError.empty());
        CHECK(runOneCycle(*vm));
        Mips64FullVmSnapshot afterDiReplay = vm->captureMips64FullSnapshot();
        CHECK(sameCpuBoundary(afterDi, afterDiReplay));
        CHECK(cpu->core().cp0Status() == (kTimerStatus & ~kStatusBitIe));

        // The EHB at payload+4 retires normally. The timer becomes pending
        // later while IE=0 and remains visible through Cause.IP7/TI.
        CHECK(runOneCycle(*vm));
        CHECK(cpu->core().pc() == kPayloadVa + 8);
        bool reachedEnable = false;
        for (unsigned step = 0; step < 700; ++step) {
            if (cpu->core().pc() == kPayloadVa + 24 &&
                (cpu->core().cp0Cause() & kCauseIp7Bit) != 0) {
                reachedEnable = true;
                break;
            }
            if (!runOneCycle(*vm)) break;
        }
        CHECK(reachedEnable);
        CHECK((cpu->core().cp0Cause() & (kCauseBitTi | kCauseIp7Bit)) ==
              (kCauseBitTi | kCauseIp7Bit));
        CHECK((cpu->core().cp0Status() & kStatusBitIe) == 0);
        CHECK(!cpu->core().statusExl() && !cpu->core().statusErl());
        Mips64FullVmSnapshot beforeEi = vm->captureMips64FullSnapshot();

        const uint64_t eiPc = cpu->core().pc();
        const uint64_t eiOldStatus = cpu->core().cp0Status();
        const uint64_t eiInstructions = context->instructionsExecuted;
        CHECK(runOneCycle(*vm)); // EI retires and returns old Status.
        CHECK(context->instructionsExecuted == eiInstructions + 1);
        CHECK(cpu->core().readGpr(11) == signExtend32Expected(
              static_cast<uint32_t>(eiOldStatus)));
        CHECK(cpu->core().cp0Status() == (eiOldStatus | kStatusBitIe));
        CHECK(cpu->core().interruptsEnabledForDelivery());
        const uint64_t beforeInterruptInstructions = context->instructionsExecuted;
        CHECK(runOneCycle(*vm)); // pending timer is sampled before the following EHB.
        CHECK(context->instructionsExecuted == beforeInterruptInstructions);
        CHECK(cpu->core().cp0Epc() == eiPc + 4);
        CHECK(cpu->core().pc() == kHandlerVa);
        CHECK(cpu->core().causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::Int));
        CHECK(cpu->core().statusExl());
        CHECK((cpu->core().cp0Cause() & kCauseIp7Bit) != 0);

        bool resumed = false;
        for (unsigned step = 0; step < 80; ++step) {
            if (readWord(vm->getMemory(), kResumeMarkerPa, little) == 0x77) {
                resumed = true;
                break;
            }
            if (!runOneCycle(*vm)) break;
        }
        CHECK(resumed);
        CHECK(readWord(vm->getMemory(), kHandlerCountPa, little) == 1);
        CHECK((readDoubleWord(vm->getMemory(), kCauseRecordPa, little) & kCauseIp7Bit) != 0);
        CHECK(readDoubleWord(vm->getMemory(), kEpcRecordPa, little) == eiPc + 4);
        CHECK((cpu->core().cp0Cause() & kCauseIp7Bit) == 0); // Compare write acknowledged.
        CHECK(!cpu->core().statusExl());
        Mips64FullVmSnapshot timerFinal = vm->captureMips64FullSnapshot();

        CHECK(vm->restoreMips64FullSnapshot(beforeEi, &restoreError));
        CHECK(restoreError.empty());
        CHECK(runOneCycle(*vm)); // EI again
        CHECK(runOneCycle(*vm)); // interrupt entry before EHB again
        resumed = false;
        for (unsigned step = 0; step < 80; ++step) {
            if (readWord(vm->getMemory(), kResumeMarkerPa, little) == 0x77) {
                resumed = true;
                break;
            }
            if (!runOneCycle(*vm)) break;
        }
        CHECK(resumed);
        CHECK(sameCpuBoundary(timerFinal, vm->captureMips64FullSnapshot()));

        Mips64FullVmSnapshot invalid = beforeDi;
        invalid.formatVersion = 99;
        CHECK(invalid.RefreshIntegrity());
        const uint64_t checksumBeforeInvalidRestore = vm->captureMips64FullSnapshot().integrityChecksum;
        CHECK(!vm->restoreMips64FullSnapshot(invalid, &restoreError));
        CHECK(!restoreError.empty());
        CHECK(vm->captureMips64FullSnapshot().integrityChecksum == checksumBeforeInvalidRestore);
    }
}

void testProductionUartMaskingAndSnapshotReplay() {
    const Mips64Endianness endian = Mips64Endianness::Big;
    auto vm = makeProductionVm(endian, kUartStatus, true);
    CHECK(vm != nullptr);
    if (!vm) return;
    CHECK(vm->run(18) == 18);
    Mips64ISAPlugin* cpu = plugin(*vm);
    CPUContext* context = vm->getCPUContext(0);
    CHECK(cpu != nullptr && context != nullptr);
    if (!cpu || !context) return;
    CHECK(cpu->core().pc() == kPayloadVa);

    const uint64_t beforeDiInstructions = context->instructionsExecuted;
    CHECK(runOneCycle(*vm));
    CHECK(context->instructionsExecuted == beforeDiInstructions + 1);
    CHECK(cpu->core().cp0Status() == (kUartStatus & ~kStatusBitIe));
    CHECK(cpu->core().readGpr(10) == signExtend32Expected(kUartStatus));
    CHECK(runOneCycle(*vm)); // DI followed by EHB.

    const std::vector<uint8_t> input{0x5A};
    CHECK(vm->enqueueMips64UartInput(input));
    const Ns16550Uart::Snapshot afterEnqueue =
        vm->getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
    CHECK(afterEnqueue.ier == 1);
    CHECK(afterEnqueue.receiveFifo.size() == 1);
    CHECK(cpu->core().hardwareInterruptLine(2));
    CHECK((cpu->core().cp0Cause() & kCauseIp4Bit) != 0);
    bool reachedEnable = false;
    for (unsigned step = 0; step < 400; ++step) {
        if (cpu->core().pc() == kPayloadVa + 24 &&
            (cpu->core().cp0Cause() & kCauseIp4Bit) != 0) {
            reachedEnable = true;
            break;
        }
        if (!runOneCycle(*vm)) break;
    }
    CHECK(reachedEnable);
    CHECK((cpu->core().cp0Cause() & kCauseIp4Bit) != 0);
    CHECK((cpu->core().cp0Status() & kStatusBitIe) == 0);
    CHECK(!cpu->core().statusExl());
    Mips64FullVmSnapshot beforeEi = vm->captureMips64FullSnapshot();
    CHECK(beforeEi.uartInterruptTargetCpu == 0);
    CHECK(beforeEi.uartHardwareInterruptLine == 2);
    CHECK(beforeEi.uartCauseIp == 4);
    CHECK(beforeEi.routedUartInterruptLevel);
    CHECK(!beforeEi.uart.receiveFifo.empty());
    if (!beforeEi.uart.receiveFifo.empty()) {
        CHECK(beforeEi.uart.receiveFifo.front().data == 0x5A);
    }

    const uint64_t eiPc = cpu->core().pc();
    const uint64_t oldStatus = cpu->core().cp0Status();
    const uint64_t beforeEiInstructions = context->instructionsExecuted;
    CHECK(runOneCycle(*vm));
    CHECK(context->instructionsExecuted == beforeEiInstructions + 1);
    CHECK(cpu->core().readGpr(11) == signExtend32Expected(static_cast<uint32_t>(oldStatus)));
    CHECK(cpu->core().cp0Status() == (oldStatus | kStatusBitIe));
    CHECK(cpu->core().interruptsEnabledForDelivery());
    const uint64_t beforeInterruptInstructions = context->instructionsExecuted;
    CHECK(runOneCycle(*vm)); // pending UART request is sampled before the following EHB.
    CHECK(context->instructionsExecuted == beforeInterruptInstructions);
    CHECK(cpu->core().pc() == kHandlerVa);
    CHECK(cpu->core().cp0Epc() == eiPc + 4);
    CHECK((cpu->core().cp0Cause() & kCauseIp4Bit) != 0);

    bool resumed = false;
    for (unsigned step = 0; step < 60; ++step) {
        if (readWord(vm->getMemory(), kResumeMarkerPa, false) == 0x66) {
            resumed = true;
            break;
        }
        if (!runOneCycle(*vm)) break;
    }
    CHECK(resumed);
    CHECK(readWord(vm->getMemory(), kRxBytePa, false) == 0x5A);
    CHECK(readWord(vm->getMemory(), kRxHandlerCountPa, false) == 1);
    CHECK(readDoubleWord(vm->getMemory(), kRxEpcPa, false) == eiPc + 4);
    CHECK((readDoubleWord(vm->getMemory(), kRxCausePa, false) & kCauseIp4Bit) != 0);
    CHECK((cpu->core().cp0Cause() & kCauseIp4Bit) == 0); // RBR read deasserted INT2.
    CHECK(!cpu->core().statusExl());
    Mips64FullVmSnapshot finalState = vm->captureMips64FullSnapshot();

    std::string error;
    CHECK(vm->restoreMips64FullSnapshot(beforeEi, &error));
    CHECK(error.empty());
    CHECK(runOneCycle(*vm));
    CHECK(runOneCycle(*vm));
    resumed = false;
    for (unsigned step = 0; step < 60; ++step) {
        if (readWord(vm->getMemory(), kResumeMarkerPa, false) == 0x66) {
            resumed = true;
            break;
        }
        if (!runOneCycle(*vm)) break;
    }
    CHECK(resumed);
    CHECK(sameCpuBoundary(finalState, vm->captureMips64FullSnapshot()));
}

void testCpuAndVmIsolation() {
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.useBuiltInDemoRom = false;
    config.bootRom = MaltaBootstrapBoard::BuildDemoBootRom(config.endianness);
    VirtualMachine vm(static_cast<size_t>(kRamBytes), 2, "MIPS64");
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* cpu0 = plugin(vm, 0);
    Mips64ISAPlugin* cpu1 = plugin(vm, 1);
    CHECK(cpu0 != nullptr && cpu1 != nullptr);
    if (!cpu0 || !cpu1) return;
    storeWord(vm.getMemory(), kTextPa, encCp0(4, 8, 12), false);
    storeWord(vm.getMemory(), kTextPa + 4, encDiEi(9, false), false);
    cpu0->core().setPc(kTextVa);
    cpu1->core().setPc(kTextVa);
    cpu0->core().writeGpr(8, 0x1101);
    cpu1->core().writeGpr(8, 0x1101);
    CHECK(cpu0->core().step(vm.getMemory()) == Mips64StepResult::Continue);
    CHECK(cpu1->core().step(vm.getMemory()) == Mips64StepResult::Continue);
    const uint64_t cpu1Status = cpu1->core().cp0Status();
    CHECK(cpu0->core().step(vm.getMemory()) == Mips64StepResult::Continue);
    CHECK((cpu0->core().cp0Status() & kStatusBitIe) == 0);
    CHECK(cpu1->core().cp0Status() == cpu1Status);

    // An independent production VM retains its own CPU state and execution.
    auto other = makeProductionVm(Mips64Endianness::Big, kTimerStatus, false);
    CHECK(other != nullptr);
    if (!other) return;
    CHECK(other->run(12) == 12);
    Mips64ISAPlugin* otherCpu = plugin(*other);
    CHECK(otherCpu != nullptr);
    if (otherCpu) {
        CHECK(otherCpu->core().cp0Status() == kTimerStatus);
        CHECK(otherCpu->core().pc() == kPayloadVa);
    }
}

} // namespace

int main() {
    testDecodeFieldsAndDisassembly();
    testAtomicStatusSemanticsAndPreservation();
    testMalformedEncodingRaisesRiWithoutStatusMutation();
    testUserModeAccessRejectedThroughPrivilegePath();
    testInterruptBoundaryAndTimerPendingBehavior();
    testExlErlSuppressionAndReservationPreservation();
    testImmediateInterruptEnableAndEhbDelivery();
    testProductionTimerMaskingAndSnapshotReplay();
    testProductionUartMaskingAndSnapshotReplay();
    testCpuAndVmIsolation();
    std::cout << "MIPS64 DI/EI: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
