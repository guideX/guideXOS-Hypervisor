// MIPS64-4R1 interrupt qualification. Every state change is performed by
// encoded MIPS64r2 guest instructions and every transition uses Mips64Cpu::step.
#include <cstdint>
#include <iostream>
#include <string>

#include "mips64_interrupt_test_support.h"

using namespace mips64;
using namespace mips64_interrupt_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { ++g_pass; } \
    else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

static constexpr uint64_t kTimerMask = kCauseBitTi | kCauseIp7Bit;
static constexpr uint32_t kStatusIeIm7 = 0x00008001U;

static bool installAndRunToTimer(Mips64Cpu& cpu, SparsePhysicalMemory& memory,
                                 uint64_t base, uint32_t status, uint32_t count,
                                 uint32_t compare, uint64_t& nextPc) {
    return installTimerSetup(cpu, memory, base, status, count, compare, nextPc);
}

static void testVirtualClockWrapAndCauseLatch() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, kStatusIeIm7,
                               0xFFFFFFFCU, 0, nextPc));
    CHECK(cpu.cp0Count() == 0xFFFFFFFFULL);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Count() == 0);
    CHECK((cpu.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK(cpu.causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::Int));
    CHECK(cpu.pc() == kGeneralVectorBev0); // Guest status selected the EBase vector.
}

static void testTimerDeliveryAndCauseFields() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, kStatusIeIm7,
                               40, 44, nextPc));
    CHECK(cpu.cp0Count() == 43);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Count() == 44);
    CHECK((cpu.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK((cpu.cp0Cause() & kCauseExcCodeMask) == 0);
    CHECK(cpu.statusExl());
    CHECK(cpu.cp0Epc() == nextPc);
    CHECK(cpu.pc() == kGeneralVectorBev0);
}

static void testMaskedPendingRetention(uint32_t maskedStatus, uint32_t enabledStatus) {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, maskedStatus,
                               100, 104, nextPc));
    storeWord(memory, nextPc, 0); // The masked boundary continues to guest fetch.
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.cp0Count() == 104);
    CHECK((cpu.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK(cpu.statusExl() == ((maskedStatus & 0x00000002U) != 0));
    CHECK(cpu.statusErl() == ((maskedStatus & 0x00000004U) != 0));

    GuestProgram enable(memory, cpu.pc());
    enable.load32(8, enabledStatus);
    enable.emit(encCp0(4, 8, 12)); // mtc0 Status; still masked until this retires.
    for (size_t i = 0; i < enable.wordCount(); ++i) {
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK((cpu.cp0Cause() & kTimerMask) == kTimerMask);
    }
    const uint64_t restartPc = cpu.pc();
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Epc() == restartPc);
    CHECK((cpu.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK(cpu.statusExl());
}

static void testIeAndIm7Masks() {
    testMaskedPendingRetention(0x00008000U, kStatusIeIm7); // IE=0
    testMaskedPendingRetention(0x00000001U, kStatusIeIm7); // IM7=0
}

static void testExlAndErlSuppression() {
    testMaskedPendingRetention(0x00008003U, kStatusIeIm7); // EXL=1
    testMaskedPendingRetention(0x00008005U, kStatusIeIm7); // ERL=1
}

static void testSoftwareInterruptIp0Ip1() {
    for (const auto [pendingBit, statusMask] : {
             std::pair<uint32_t, uint32_t>{0x100U, 0x101U},
             std::pair<uint32_t, uint32_t>{0x200U, 0x201U}}) {
        SparsePhysicalMemory memory;
        Mips64Cpu cpu;
        uint64_t nextPc = 0;
        CHECK(installAndRunToTimer(cpu, memory, kProgramBase, statusMask,
                                   10, 1000, nextPc));
        GuestProgram pending(memory, nextPc);
        pending.emit(encI(0x09, 0, 8, static_cast<uint16_t>(pendingBit)));
        pending.emit(encCp0(4, 8, 13)); // mtc0 $t0, Cause (IP0/IP1)
        CHECK(appendRun(cpu, memory, nextPc, pending.wordCount()));
        CHECK((cpu.cp0Cause() & pendingBit) == pendingBit);
        CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
        CHECK(cpu.causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::Int));
        CHECK((cpu.cp0Cause() & pendingBit) == pendingBit);
        CHECK((cpu.cp0Cause() & kTimerMask) == 0);
        CHECK(cpu.pc() == kGeneralVectorBev0);
    }
}

static void testCauseWritePreservesPendingSourcesAndCompareAcknowledges() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, 0,
                               100, 104, nextPc));
    storeWord(memory, nextPc, 0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // Timer latches but is masked.

    GuestProgram writes(memory, cpu.pc());
    writes.load32(8, 0x00800100U); // IV plus software IP0.
    writes.emit(encCp0(4, 8, 13));
    writes.load32(9, 200);
    writes.emit(encCp0(5, 9, 11)); // Compare acknowledges the timer.
    CHECK(appendRun(cpu, memory, cpu.pc(), writes.wordCount()));
    CHECK((cpu.cp0Cause() & kTimerMask) == 0);
    CHECK((cpu.cp0Cause() & (kCauseBitIv | 0x100ULL)) == (kCauseBitIv | 0x100ULL));

    GuestProgram enable(memory, cpu.pc());
    enable.load32(8, 0x101U);
    enable.emit(encCp0(4, 8, 12));
    CHECK(appendRun(cpu, memory, cpu.pc(), enable.wordCount()));
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.pc() == kInterruptVectorBev0);
    CHECK((cpu.cp0Cause() & 0x100ULL) != 0);
    CHECK((cpu.cp0Cause() & kTimerMask) == 0);
}

static void testCauseIvAndBevVectors() {
    struct VectorCase { bool bev; bool iv; uint64_t expected; };
    const VectorCase cases[] = {
        {false, false, kGeneralVectorBev0},
        {false, true, kInterruptVectorBev0},
        {true, false, kGeneralVectorBev1},
        {true, true, kInterruptVectorBev1},
    };
    for (const VectorCase& item : cases) {
        SparsePhysicalMemory memory;
        Mips64Cpu cpu;
        const uint32_t status = kStatusIeIm7 | (item.bev ? 0x00400000U : 0U);
        const uint32_t compare = item.iv ? 107U : 104U;
        uint64_t nextPc = 0;
        CHECK(installAndRunToTimer(cpu, memory, kProgramBase, status,
                                   100, compare, nextPc));
        if (item.iv) {
            GuestProgram setIv(memory, nextPc);
            setIv.load32(8, 0x00800000U);
            setIv.emit(encCp0(4, 8, 13)); // Cause.IV is written by guest code.
            CHECK(appendRun(cpu, memory, nextPc, setIv.wordCount()));
        }
        CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
        CHECK(cpu.pc() == item.expected);
        CHECK(cpu.causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::Int));
    }
}

static void testInterruptAtBranchDelayBoundary(uint8_t branchOpcode,
                                               uint8_t branchRs,
                                               uint8_t branchRt,
                                               uint32_t compareOffset) {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, kStatusIeIm7,
                               100, 100 + compareOffset, nextPc));
    const uint64_t branchPc = nextPc;
    GuestProgram branch(memory, branchPc);
    branch.emit(encI(branchOpcode, branchRs, branchRt, 1));
    branch.emit(encI(0x09, 0, 12, 99)); // must not run when IRQ wins at the slot boundary
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.delayArmed());
    CHECK(cpu.pc() == branchPc + 4);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Epc() == branchPc);
    CHECK(cpu.causeBd());
    CHECK(!cpu.delayArmed());
    CHECK(cpu.readGpr(12) == 0);
    CHECK(cpu.pc() == kGeneralVectorBev0);
}

static void testTakenAndNotTakenBranchDelaySlots() {
    // Both taken BEQ and not-taken BNE execute a delay slot; the asynchronous
    // boundary before that slot saves the branch PC and sets Cause.BD.
    testInterruptAtBranchDelayBoundary(0x04, 0, 0, 5);
    testInterruptAtBranchDelayBoundary(0x05, 0, 0, 5);
}

static void testBranchLikelyAnnulment() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, kStatusIeIm7,
                               100, 106, nextPc));
    GuestProgram branch(memory, nextPc);
    branch.emit(encI(0x09, 0, 1, 1)); // $at = 1
    const uint64_t branchPc = branch.pc();
    branch.emit(encI(0x01, 0, 2, 1)); // bltzl $zero,+1 (not taken; annul slot)
    branch.emit(encI(0x09, 0, 12, 99)); // annulled delay slot
    CHECK(appendRun(cpu, memory, nextPc, 2));
    CHECK(!cpu.delayArmed());
    CHECK(cpu.pc() == branchPc + 8);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Epc() == branchPc + 8);
    CHECK(!cpu.causeBd());
    CHECK(cpu.readGpr(12) == 0);
}

static void testGuestHandlerAcknowledgesRearmsAndReturnsTwice() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, kStatusIeIm7,
                               0, 20, nextPc));

    GuestProgram main(memory, nextPc);
    main.emit(encI(0x19, 0, 16, 7));             // daddiu $s0,$zero,7
    main.emit(encI(0x19, 0, 17, 5));             // daddiu $s1,$zero,5
    main.emit(encR(16, 17, 18, 0, 0x2D));       // daddu $s2,$s0,$s1
    main.emit((0x0FU << 26) | (26U << 16) | 0x8000U); // lui $k0,0x8000
    main.emit(encI(0x0D, 26, 26, 0x3010));       // ori $k0,$k0,0x3010
    main.emit(encI(0x3F, 26, 18, 0));            // sd $s2,0($k0)
    main.emit(encI(0x37, 26, 19, 0));            // ld $s3,0($k0)
    main.emit(encI(0x19, 0, 20, 1));             // daddiu $s4,$zero,1
    const uint64_t loopPc = main.pc();
    main.emit(encI(0x09, 0, 21, 1));             // addiu $s5,$zero,1
    main.emit(encI(0x04, 0, 0, 0xFFFF));          // beq $zero,$zero,-1
    main.emit(0);                                  // delay-slot NOP

    const uint64_t vector = kGeneralVectorBev0;
    GuestProgram handler(memory, vector);
    handler.emit(encCp0(1, 27, 13));              // dmfc0 $k1, Cause before ack
    handler.emit(encCp0(1, 26, 9));               // dmfc0 $k0, Count
    handler.emit(encI(0x19, 26, 26, 16));         // daddiu $k0,$k0,16
    handler.emit(encCp0(5, 26, 11));              // dmtc0 $k0, Compare (ack + rearm)
    handler.emit((0x0FU << 26) | (24U << 16) | 0x8000U); // lui $t8,0x8000
    handler.emit(encI(0x0D, 24, 24, 0x3000));     // ori $t8,$t8,0x3000
    handler.emit(encI(0x3F, 24, 27, 0));          // sd $k1,0($t8)
    handler.emit(encI(0x37, 24, 26, 8));          // ld $k0,8($t8)
    handler.emit(encI(0x19, 26, 26, 1));          // daddiu $k0,$k0,1
    handler.emit(encI(0x3F, 24, 26, 8));          // sd $k0,8($t8)
    handler.emit(0x42000018U);                     // eret

    const uint8_t zeros[24]{};
    memory.Write(0x3000, zeros, sizeof(zeros));
    cpu.setPc(kProgramBase);
    unsigned interrupts = 0;
    bool reachedSecondReturn = false;
    for (unsigned step = 0; step < 160; ++step) {
        const Mips64StepResult result = cpu.step(memory);
        CHECK(result == Mips64StepResult::Continue || result == Mips64StepResult::Interrupt);
        if (result == Mips64StepResult::Interrupt) {
            ++interrupts;
        }
        if (memory.read64(0x3008) == 2 && !cpu.statusExl() && cpu.pc() != vector) {
            reachedSecondReturn = true;
            break;
        }
    }

    CHECK(reachedSecondReturn);
    CHECK(interrupts == 2);
    CHECK(cpu.readGpr(16) == 7);
    CHECK(cpu.readGpr(17) == 5);
    CHECK(cpu.readGpr(18) == 12);
    CHECK(cpu.readGpr(19) == 12);
    CHECK(cpu.readGpr(20) == 1);
    CHECK(cpu.readGpr(21) >= 1);
    CHECK(memory.read64(0x3008) == 2);
    CHECK((memory.read64(0x3000) & kTimerMask) == kTimerMask);
    CHECK(((memory.read64(0x3000) & kCauseExcCodeMask) >> kCauseExcCodeShift) ==
          static_cast<uint64_t>(Mips64ExcCode::Int));
    CHECK(cpu.pc() >= loopPc && cpu.pc() < loopPc + 12);
    CHECK(!cpu.statusExl() && !cpu.statusErl());
    CHECK((cpu.cp0Cause() & kTimerMask) == 0); // Compare writes acknowledged each request.
    CHECK(cpu.cp0Compare() > cpu.cp0Count()); // Handler rearmed the next match.
}

static void testIndependentTimerStatePerCpu() {
    SparsePhysicalMemory memory;
    Mips64Cpu first;
    Mips64Cpu second;
    uint64_t firstNext = 0;
    uint64_t secondNext = 0;
    CHECK(installAndRunToTimer(first, memory, kProgramBase, kStatusIeIm7,
                               100, 104, firstNext));
    CHECK(installAndRunToTimer(second, memory, kProgramBase + 0x100,
                               kStatusIeIm7, 300, 304, secondNext));
    CHECK(first.step(memory) == Mips64StepResult::Interrupt);
    CHECK((first.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK(second.cp0Count() == 303);
    CHECK((second.cp0Cause() & kTimerMask) == 0);
    CHECK(second.pc() == secondNext);
}

static void testExternalHardwareInt2MapsToIp4AndHonorsMasks() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, 0, 100, 1000, nextPc));
    storeWord(memory, nextPc, 0);
    storeWord(memory, kGeneralVectorBev0, 0x42000018U); // eret

    const std::vector<uint8_t> cpuV6BeforePin = cpu.serialize();
    cpu.setHardwareInterruptLine(2, true); // Malta INT2 is Cause.IP4.
    CHECK(cpu.hardwareInterruptLine(2));
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);
    CHECK(cpuV6BeforePin.size() == Mips64Cpu::kSerializedSize &&
          Mips64Cpu::kSerializedSize == 1001);
    CHECK(cpu.serialize() == cpuV6BeforePin); // Board pins stay outside CPU state.
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // IM4/IE are clear.
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0); // Masking retains the line.

    GuestProgram clearCause(memory, cpu.pc());
    clearCause.emit(encI(0x09, 0, 8, 0));
    clearCause.emit(encCp0(4, 8, 13)); // MTC0 cannot clear external IP4.
    CHECK(appendRun(cpu, memory, cpu.pc(), clearCause.wordCount()));
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);

    GuestProgram enable(memory, cpu.pc());
    enable.emit(encI(0x09, 0, 8, 0x1101)); // IE | IM4.
    enable.emit(encCp0(4, 8, 12));
    CHECK(appendRun(cpu, memory, cpu.pc(), enable.wordCount()));
    const uint64_t restartPc = cpu.pc();
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.cp0Epc() == restartPc);
    CHECK(cpu.causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::Int));
    CHECK(cpu.pc() == kGeneralVectorBev0 && cpu.statusExl());
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);

    // EXL suppresses resampling; deasserting the source clears IP4 without
    // needing to read or write Cause, then ERET resumes at EPC.
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // eret while line asserted
    CHECK(!cpu.statusExl());
    CHECK(cpu.pc() == restartPc);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt); // level remains asserted
    CHECK(cpu.statusExl());
    cpu.setHardwareInterruptLine(2, false);
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) == 0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // eret now resumes
    CHECK(!cpu.statusExl() && cpu.pc() == restartPc);

    // External INT2 can arrive while EXL is already set and remains pending
    // until software clears EXL; it is delivered at the next boundary.
    GuestProgram enterExl(memory, cpu.pc());
    enterExl.emit(encI(0x09, 0, 8, 0x1103));
    enterExl.emit(encCp0(4, 8, 12));
    CHECK(appendRun(cpu, memory, cpu.pc(), enterExl.wordCount()));
    storeWord(memory, cpu.pc(), 0);
    cpu.setHardwareInterruptLine(2, true);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.statusExl() && (cpu.cp0Cause() & kCauseIp4Bit) != 0);
    GuestProgram leaveExl(memory, cpu.pc());
    leaveExl.emit(encI(0x09, 0, 8, 0x1101));
    leaveExl.emit(encCp0(4, 8, 12));
    CHECK(appendRun(cpu, memory, cpu.pc(), leaveExl.wordCount()));
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt);
    CHECK(cpu.statusExl() && cpu.causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::Int));
    cpu.setHardwareInterruptLine(2, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    // ERL independently suppresses the same external request.
    SparsePhysicalMemory erlMemory;
    Mips64Cpu erlCpu;
    uint64_t erlNext = 0;
    CHECK(installAndRunToTimer(erlCpu, erlMemory, kProgramBase, 0x00000004U,
                               10, 1000, erlNext));
    storeWord(erlMemory, erlNext, 0);
    erlCpu.setHardwareInterruptLine(2, true);
    CHECK(erlCpu.step(erlMemory) == Mips64StepResult::Continue);
    CHECK(erlCpu.statusErl() && (erlCpu.cp0Cause() & kCauseIp4Bit) != 0);
}

static void testExternalInterruptBranchDelayAndTimerIndependence() {
    SparsePhysicalMemory memory;
    Mips64Cpu cpu;
    uint64_t nextPc = 0;
    CHECK(installAndRunToTimer(cpu, memory, kProgramBase, 0x1101, 10, 1000, nextPc));
    const uint64_t branchPc = nextPc;
    storeWord(memory, branchPc, encI(0x04, 0, 0, 1)); // beq zero,zero,+1
    storeWord(memory, branchPc + 4, encI(0x09, 0, 16, 1)); // delay-slot marker
    storeWord(memory, branchPc + 8, encI(0x09, 0, 17, 2)); // branch target
    storeWord(memory, branchPc + 12, 0);
    storeWord(memory, kGeneralVectorBev0, 0x42000018U);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // retire branch
    CHECK(cpu.delayArmed() && cpu.pc() == branchPc + 4);
    cpu.setHardwareInterruptLine(2, true);
    CHECK(cpu.step(memory) == Mips64StepResult::Interrupt); // delay-slot boundary
    CHECK(cpu.causeBd() && cpu.cp0Epc() == branchPc);
    cpu.setHardwareInterruptLine(2, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // ERET to branch
    CHECK(cpu.pc() == branchPc);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.delayArmed());
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(16) == 1 && cpu.readGpr(17) == 2);

    // The pending UART input and Count/Compare are independent causes.
    SparsePhysicalMemory overlapMemory;
    Mips64Cpu overlap;
    uint64_t overlapPc = 0;
    CHECK(installAndRunToTimer(overlap, overlapMemory, kProgramBase, 0,
                               10, 100, overlapPc));
    overlap.setHardwareInterruptLine(2, true);
    for (unsigned i = 0; i < 90; ++i) storeWord(overlapMemory, overlapPc + i * 4, 0);
    for (unsigned i = 0; i < 90; ++i) {
        CHECK(overlap.step(overlapMemory) == Mips64StepResult::Continue);
    }
    CHECK((overlap.cp0Cause() & (kCauseIp4Bit | kTimerMask)) ==
          (kCauseIp4Bit | kTimerMask));
    CHECK((overlap.cp0Cause() & kCauseIp7Bit) != 0); // timer IP7

    GuestProgram acknowledgeTimer(overlapMemory, overlap.pc());
    acknowledgeTimer.load32(9, 200);
    acknowledgeTimer.emit(encCp0(5, 9, 11)); // Compare clears TI/IP7 only.
    CHECK(appendRun(overlap, overlapMemory, overlap.pc(), acknowledgeTimer.wordCount()));
    CHECK((overlap.cp0Cause() & (kCauseBitTi | kCauseIp7Bit)) == 0);
    CHECK((overlap.cp0Cause() & kCauseIp4Bit) != 0);
    overlap.setHardwareInterruptLine(2, false);
    CHECK((overlap.cp0Cause() & kCauseIp4Bit) == 0);
}

int main() {
    testVirtualClockWrapAndCauseLatch();
    testTimerDeliveryAndCauseFields();
    testIeAndIm7Masks();
    testExlAndErlSuppression();
    testSoftwareInterruptIp0Ip1();
    testCauseWritePreservesPendingSourcesAndCompareAcknowledges();
    testCauseIvAndBevVectors();
    testTakenAndNotTakenBranchDelaySlots();
    testBranchLikelyAnnulment();
    testGuestHandlerAcknowledgesRearmsAndReturnsTwice();
    testIndependentTimerStatePerCpu();
    testExternalHardwareInt2MapsToIp4AndHonorsMasks();
    testExternalInterruptBranchDelayAndTimerIndependence();
    std::cout << "MIPS64 interrupt qualification: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
