// MIPS64-4R1 qualification of snapshot version 4 and historical v3 handling.
#include <cstdint>
#include <iostream>
#include <vector>

#include "mips64_interrupt_test_support.h"

using namespace mips64;
using namespace mips64_interrupt_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { ++g_pass; } \
    else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

static constexpr size_t kCauseOffset = 317;
static constexpr size_t kCountOffset = 357;
static constexpr size_t kCompareOffset = 365;
static constexpr uint64_t kTimerMask = kCauseBitTi | kCauseIp7Bit;

static uint64_t load64le(const std::vector<uint8_t>& blob, size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(blob[offset + i]) << (8 * i);
    }
    return value;
}

static void store64le(std::vector<uint8_t>& blob, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        blob[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

static void testRoundTripBeforeCompareMatch() {
    SparsePhysicalMemory memory;
    Mips64Cpu source;
    uint64_t nextPc = 0;
    CHECK(installTimerSetup(source, memory, kProgramBase, 0x00008000U,
                            100, 200, nextPc));
    const std::vector<uint8_t> blob = source.serialize();
    CHECK(blob.size() == Mips64Cpu::kSerializedSize);
    CHECK(blob[4] == Mips64Cpu::kSerializationVersion);
    CHECK(blob[4] == 4);
    CHECK(load64le(blob, kCountOffset) == 103);
    CHECK(load64le(blob, kCompareOffset) == 200);

    Mips64Cpu restored;
    CHECK(restored.deserialize(blob.data(), blob.size()));
    CHECK(restored.serialize() == blob);
    CHECK(restored.cp0Count() == 103);
    CHECK(restored.cp0Compare() == 200);
    CHECK(restored.pc() == nextPc);
}

static void testPendingMaskedRoundTripAndContinuedDelivery() {
    SparsePhysicalMemory memory;
    Mips64Cpu source;
    uint64_t nextPc = 0;
    CHECK(installTimerSetup(source, memory, kProgramBase, 0x00008000U,
                            100, 104, nextPc));
    GuestProgram continuation(memory, nextPc);
    continuation.emit(0); // timer latches while masked; this NOP still retires
    continuation.load32(8, 0x00008001U);
    continuation.emit(encCp0(4, 8, 12));
    CHECK(source.step(memory) == Mips64StepResult::Continue);
    CHECK((source.cp0Cause() & kTimerMask) == kTimerMask);
    CHECK(!source.statusExl());

    const std::vector<uint8_t> pendingBlob = source.serialize();
    CHECK(load64le(pendingBlob, kCauseOffset) & kTimerMask);
    Mips64Cpu restored;
    CHECK(restored.deserialize(pendingBlob.data(), pendingBlob.size()));
    CHECK(restored.serialize() == pendingBlob);

    for (int i = 0; i < 3; ++i) {
        const Mips64StepResult a = source.step(memory);
        const Mips64StepResult b = restored.step(memory);
        CHECK(a == Mips64StepResult::Continue && b == a);
        CHECK(source.serialize() == restored.serialize());
    }
    CHECK(source.step(memory) == Mips64StepResult::Interrupt);
    CHECK(restored.step(memory) == Mips64StepResult::Interrupt);
    CHECK(source.serialize() == restored.serialize());
    CHECK(source.pc() == kGeneralVectorBev0);
    CHECK((source.cp0Cause() & kTimerMask) == kTimerMask);
}

static void testHandlerAndBeforeEretRoundTripsAfterCompareRearm() {
    SparsePhysicalMemory memory;
    Mips64Cpu source;
    uint64_t nextPc = 0;
    CHECK(installTimerSetup(source, memory, kProgramBase, 0x00008001U,
                            100, 104, nextPc));
    storeWord(memory, nextPc, 0);
    GuestProgram handler(memory, kGeneralVectorBev0);
    handler.emit(encCp0(1, 8, 9));              // dmfc0 $t0, Count
    handler.emit(encI(0x19, 8, 8, 20));          // daddiu $t0,$t0,20
    handler.emit(encCp0(5, 8, 11));              // dmtc0 $t0, Compare
    handler.emit(0);
    handler.emit(0x42000018U);                    // eret

    CHECK(source.step(memory) == Mips64StepResult::Interrupt);
    const std::vector<uint8_t> inHandler = source.serialize();
    Mips64Cpu restored;
    CHECK(restored.deserialize(inHandler.data(), inHandler.size()));
    CHECK(restored.pc() == kGeneralVectorBev0);
    CHECK(restored.statusExl());
    CHECK((restored.cp0Cause() & kTimerMask) == kTimerMask);

    for (int i = 0; i < 3; ++i) {
        CHECK(source.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
    }
    CHECK((source.cp0Cause() & kTimerMask) == 0);
    CHECK(source.cp0Compare() > source.cp0Count());
    CHECK(source.serialize() == restored.serialize());

    const std::vector<uint8_t> afterRearm = source.serialize();
    Mips64Cpu rearmed;
    CHECK(rearmed.deserialize(afterRearm.data(), afterRearm.size()));
    CHECK(rearmed.cp0Compare() == source.cp0Compare());
    CHECK(rearmed.cp0Count() == source.cp0Count());
    CHECK((rearmed.cp0Cause() & kTimerMask) == 0);

    CHECK(source.step(memory) == Mips64StepResult::Continue); // before ERET
    CHECK(restored.step(memory) == Mips64StepResult::Continue);
    CHECK(source.pc() == kGeneralVectorBev0 + 16);
    const std::vector<uint8_t> beforeEret = source.serialize();
    Mips64Cpu beforeEretCpu;
    CHECK(beforeEretCpu.deserialize(beforeEret.data(), beforeEret.size()));
    CHECK(beforeEretCpu.pc() == kGeneralVectorBev0 + 16);
    CHECK(source.step(memory) == Mips64StepResult::Continue);
    CHECK(restored.step(memory) == Mips64StepResult::Continue);
    CHECK(beforeEretCpu.step(memory) == Mips64StepResult::Continue);
    CHECK(source.serialize() == restored.serialize());
    CHECK(source.serialize() == beforeEretCpu.serialize());
    CHECK(source.pc() == nextPc);
    CHECK(!source.statusExl());
    CHECK(source.cp0Epc() == nextPc);
}

static void testOutstandingDelaySlotRoundTrip() {
    SparsePhysicalMemory memory;
    Mips64Cpu source;
    uint64_t nextPc = 0;
    CHECK(installTimerSetup(source, memory, kProgramBase, 0x00008001U,
                            100, 105, nextPc));
    GuestProgram branch(memory, nextPc);
    branch.emit(encI(0x04, 0, 0, 1));
    branch.emit(encI(0x09, 0, 12, 99));
    CHECK(source.step(memory) == Mips64StepResult::Continue);
    CHECK(source.delayArmed());
    const std::vector<uint8_t> branchState = source.serialize();
    Mips64Cpu restored;
    CHECK(restored.deserialize(branchState.data(), branchState.size()));
    CHECK(restored.delayArmed());
    CHECK(restored.delayBranch() == nextPc);
    CHECK(restored.delayTarget() == source.delayTarget());

    CHECK(source.step(memory) == Mips64StepResult::Interrupt);
    CHECK(restored.step(memory) == Mips64StepResult::Interrupt);
    CHECK(source.serialize() == restored.serialize());
    CHECK(source.cp0Epc() == nextPc);
    CHECK(source.causeBd());
    CHECK(source.pc() == kGeneralVectorBev0);
}

static void testVersionMigrationAndRejectedRestoresAreAtomic() {
    SparsePhysicalMemory memory;
    Mips64Cpu target;
    uint64_t nextPc = 0;
    CHECK(installTimerSetup(target, memory, kProgramBase, 0,
                            30, 100, nextPc));
    target.setPc(nextPc + 0x80);
    const std::vector<uint8_t> valid = target.serialize();
    CHECK(valid[4] == 4);

    std::vector<uint8_t> historicalV3 = valid;
    historicalV3[4] = 3; // MIPS64-3/MIPS64-4 layout, unambiguous Cause bits.
    Mips64Cpu migrated;
    CHECK(migrated.deserialize(historicalV3.data(), historicalV3.size()));
    CHECK(migrated.serialize()[4] == 4);
    CHECK(migrated.pc() == target.pc());

    const std::vector<uint8_t> before = target.serialize();
    const auto rejectWithoutMutation = [&](std::vector<uint8_t> bad) {
        CHECK(!target.deserialize(bad.data(), bad.size()));
        CHECK(target.serialize() == before);
    };

    std::vector<uint8_t> bad = valid;
    bad[0] = 'X';
    rejectWithoutMutation(bad);
    bad = valid;
    bad[4] = 99;
    rejectWithoutMutation(bad);
    bad = valid;
    bad[6] = 1;
    rejectWithoutMutation(bad);
    bad = valid;
    bad[7] = 1;
    rejectWithoutMutation(bad);
    bad = valid;
    bad.pop_back();
    rejectWithoutMutation(bad);
    bad = valid;
    bad[4] = 3;
    store64le(bad, kCauseOffset, load64le(bad, kCauseOffset) | kCauseBitIv);
    rejectWithoutMutation(bad);
    bad = valid;
    bad[4] = 3;
    store64le(bad, kCauseOffset, load64le(bad, kCauseOffset) | kCauseBitDc);
    rejectWithoutMutation(bad);
    bad = valid;
    bad[4] = 3;
    store64le(bad, kCauseOffset, load64le(bad, kCauseOffset) | (1ULL << 24));
    rejectWithoutMutation(bad);
}

int main() {
    testRoundTripBeforeCompareMatch();
    testPendingMaskedRoundTripAndContinuedDelivery();
    testHandlerAndBeforeEretRoundTripsAfterCompareRearm();
    testOutstandingDelaySlotRoundTrip();
    testVersionMigrationAndRejectedRestoresAreAtomic();
    std::cout << "MIPS64 serialization qualification: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
