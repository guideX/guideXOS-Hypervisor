// MIPS64-15 linked-load and atomic conditional-store qualification.

#include "PhysicalAddressMap.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 0x10000;
constexpr uint64_t kTextPa = 0x1000;
constexpr uint64_t kDataPa = 0x2000;
constexpr uint64_t kTextVa = kCKSeg0Base | kTextPa;
constexpr uint64_t kDataVa = kCKSeg0Base | kDataPa;
unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

class ReservationTestMmio final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 8; }
    void Read(uint64_t, uint8_t* destination, size_t size) const override {
        ++readCount;
        for (size_t index = 0; index < size; ++index) destination[index] = 0;
    }
    void Write(uint64_t, const uint8_t*, size_t) override { ++writeCount; }
    void Reset() override { readCount = 0; writeCount = 0; }

    mutable uint64_t readCount = 0;
    uint64_t writeCount = 0;
};

uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t value, bool little) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = little ? 8 * i : 8 * (3 - i);
        bytes[i] = static_cast<uint8_t>(value >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

void writeDouble(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned shift = little ? 8 * i : 8 * (7 - i);
        bytes[i] = static_cast<uint8_t>(value >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t readDouble(IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned shift = little ? 8 * i : 8 * (7 - i);
        value |= uint64_t(bytes[i]) << shift;
    }
    return value;
}

bool stepAt(Mips64Cpu& cpu, Memory& memory, uint64_t pc, uint32_t word, bool little) {
    writeWord(memory, kTextPa + (pc - kTextVa), word, little);
    cpu.setPc(pc);
    return cpu.step(memory) == Mips64StepResult::Continue;
}

void setupCpu(Mips64Cpu& cpu, Mips64Endianness endian) {
    cpu.configureEndianness(endian);
    cpu.setPc(kTextVa);
    cpu.writeGpr(4, kDataVa);
}

void testDecodeAndEndianExecution() {
    const Mips64Decoded lld = decodeInstruction(0xD0850000U);
    CHECK(lld.valid && lld.op == Mips64Op::Lld && lld.rs == 4 && lld.rt == 5 && lld.imm == 0);
    CHECK(Mips64Cpu{}.disassemble(lld) == "lld $5, 0($4)");
    const Mips64Decoded scd = decodeInstruction(encodeI(0x3C, 4, 5, 0));
    CHECK(scd.valid && scd.op == Mips64Op::Scd && scd.rs == 4 && scd.rt == 5);
    CHECK(Mips64Cpu{}.disassemble(scd) == "scd $5, 0($4)");

    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(endian);
        setupCpu(cpu, endian);
        const uint64_t original = 0x1020304050607080ULL;
        const uint64_t replacement = 0x8877665544332211ULL;
        writeDouble(memory, kDataPa, original, little);
        CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x34, 4, 5, 0), little));
        CHECK(cpu.readGpr(5) == original);
        CHECK(cpu.reservationState().valid);
        CHECK(cpu.reservationState().size == 8);
        CHECK(cpu.reservationState().virtualAddress == kDataVa);
        CHECK(cpu.reservationState().physicalAddress == kDataPa);
        CHECK(cpu.reservationState().cacheAttribute == 3);
        cpu.writeGpr(5, replacement); // SCD source/result alias.
        CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), little));
        CHECK(cpu.readGpr(5) == 1);
        CHECK(readDouble(memory, kDataPa, little) == replacement);
        CHECK(!cpu.reservationState().valid);
    }
}

void testAtomicTwoCpuStoreAndGranuleConflicts() {
    for (const uint64_t conflictingOffset : {uint64_t{8}, uint64_t{16}}) {
        const bool conflicts = conflictingOffset == 8;
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpuA(Mips64Endianness::Big);
        Mips64Cpu cpuB(Mips64Endianness::Big);
        setupCpu(cpuA, Mips64Endianness::Big);
        setupCpu(cpuB, Mips64Endianness::Big);
        writeDouble(memory, kDataPa, 0x1111222233334444ULL, false);
        CHECK(stepAt(cpuA, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
        CHECK(stepAt(cpuB, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
        CHECK(cpuA.reservationState().generation == cpuB.reservationState().generation);
        uint8_t changed[8]{};
        writeDouble(memory, kDataPa + conflictingOffset,
                    0xAABBCCDDEEFF0011ULL, false);
        memory.Read(kDataPa + conflictingOffset, changed, sizeof(changed));
        cpuA.writeGpr(5, 0x5555666677778888ULL);
        CHECK(stepAt(cpuA, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
        CHECK(cpuA.readGpr(5) == (conflicts ? 0ULL : 1ULL));
        CHECK(readDouble(memory, kDataPa, false) ==
              (conflicts ? 0x1111222233334444ULL : 0x5555666677778888ULL));
    }

    // Two CPUs can both reserve the same unchanged granule. The first SCD
    // commits under the map lock and advances its generation; the second fails.
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpuA(Mips64Endianness::Big);
    Mips64Cpu cpuB(Mips64Endianness::Big);
    setupCpu(cpuA, Mips64Endianness::Big);
    setupCpu(cpuB, Mips64Endianness::Big);
    writeDouble(memory, kDataPa, 1, false);
    CHECK(stepAt(cpuA, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    CHECK(stepAt(cpuB, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    cpuA.writeGpr(5, 0xAAAAAAAAAAAAAAAAULL);
    cpuB.writeGpr(5, 0xBBBBBBBBBBBBBBBBULL);
    CHECK(stepAt(cpuA, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
    CHECK(stepAt(cpuB, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
    CHECK(cpuA.readGpr(5) == 1);
    CHECK(cpuB.readGpr(5) == 0);
    CHECK(readDouble(memory, kDataPa, false) == 0xAAAAAAAAAAAAAAAAULL);

    // Distinct VMs own separate monitors, even when the guest addresses and
    // image bytes are identical.
    Memory vmAMemory(kRamBytes, false);
    Memory vmBMemory(kRamBytes, false);
    vmAMemory.EnablePhysicalAddressMap();
    vmBMemory.EnablePhysicalAddressMap();
    Mips64Cpu vmACpu(Mips64Endianness::Big);
    setupCpu(vmACpu, Mips64Endianness::Big);
    writeDouble(vmAMemory, kDataPa, 7, false);
    CHECK(stepAt(vmACpu, vmAMemory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    writeDouble(vmBMemory, kDataPa, 0xFEED, false);
    vmACpu.writeGpr(5, 0x1234567890ABCDEFULL);
    CHECK(stepAt(vmACpu, vmAMemory, kTextVa + 4,
                 encodeI(0x3C, 4, 5, 0), false));
    CHECK(vmACpu.readGpr(5) == 1);
    CHECK(readDouble(vmAMemory, kDataPa, false) == 0x1234567890ABCDEFULL);
}

void testCachedAccessRequirementAndGuestRetryLoop() {
    // Release 2 defines LL/SC behavior only for cached access types. The
    // modeled KSEG1 alias is uncached and must not start a reservation.
    Memory uncachedMemory(kRamBytes, false);
    uncachedMemory.EnablePhysicalAddressMap();
    Mips64Cpu uncachedCpu(Mips64Endianness::Big);
    setupCpu(uncachedCpu, Mips64Endianness::Big);
    writeDouble(uncachedMemory, kDataPa, 0x0102030405060708ULL, false);
    uncachedCpu.writeGpr(4, kCKSeg1Base | kDataPa);
    uncachedCpu.writeGpr(5, 0xAABBCCDDEEFF0011ULL);
    CHECK(!stepAt(uncachedCpu, uncachedMemory, kTextVa,
                  encodeI(0x34, 4, 5, 0), false));
    CHECK(uncachedCpu.readGpr(5) == 0xAABBCCDDEEFF0011ULL);
    CHECK(!uncachedCpu.reservationState().valid);
    CHECK(((uncachedCpu.cp0Cause() >> 2) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::DBE));
    CHECK(readDouble(uncachedMemory, kDataPa, false) == 0x0102030405060708ULL);

    // A different uncached alias cannot consume a cached KSEG0 reservation.
    Memory aliasMemory(kRamBytes, false);
    aliasMemory.EnablePhysicalAddressMap();
    Mips64Cpu aliasCpu(Mips64Endianness::Big);
    setupCpu(aliasCpu, Mips64Endianness::Big);
    writeDouble(aliasMemory, kDataPa, 0x0102030405060708ULL, false);
    CHECK(stepAt(aliasCpu, aliasMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    aliasCpu.writeGpr(4, kCKSeg1Base | kDataPa);
    aliasCpu.writeGpr(5, 0x8877665544332211ULL);
    CHECK(stepAt(aliasCpu, aliasMemory, kTextVa + 4,
                 encodeI(0x3C, 4, 5, 0), false));
    CHECK(aliasCpu.readGpr(5) == 0);
    CHECK(!aliasCpu.reservationState().valid);
    CHECK(readDouble(aliasMemory, kDataPa, false) == 0x0102030405060708ULL);

    // Encoded LLD; DADDIU; SCD; BEQZ retry loop. CPU1's ordinary SD lands
    // between CPU0's LLD and SCD, forcing the first attempt to fail. CPU0
    // then retries from the newly written value and commits successfully.
    Memory retryMemory(kRamBytes, false);
    retryMemory.EnablePhysicalAddressMap();
    Mips64Cpu cpu0(Mips64Endianness::Big);
    Mips64Cpu cpu1(Mips64Endianness::Big);
    setupCpu(cpu0, Mips64Endianness::Big);
    setupCpu(cpu1, Mips64Endianness::Big);
    const uint64_t initial = 9;
    const uint64_t concurrentValue = 100;
    writeDouble(retryMemory, kDataPa, initial, false);
    writeWord(retryMemory, kTextPa, encodeI(0x34, 4, 5, 0), false);
    writeWord(retryMemory, kTextPa + 4, encodeI(0x19, 5, 5, 1), false);
    writeWord(retryMemory, kTextPa + 8, encodeI(0x3C, 4, 5, 0), false);
    writeWord(retryMemory, kTextPa + 12, encodeI(0x04, 5, 0, 0xFFFC), false);
    writeWord(retryMemory, kTextPa + 16, 0, false);
    writeWord(retryMemory, kTextPa + 32, encodeI(0x3F, 4, 6, 0), false);
    cpu0.setPc(kTextVa);
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // LLD
    cpu1.writeGpr(6, concurrentValue);
    cpu1.setPc(kTextVa + 32);
    CHECK(cpu1.step(retryMemory) == Mips64StepResult::Continue); // SD conflict
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // DADDIU
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // SCD fails
    CHECK(cpu0.readGpr(5) == 0);
    CHECK(!cpu0.reservationState().valid);
    CHECK(readDouble(retryMemory, kDataPa, false) == concurrentValue);
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // BEQ taken
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // delay slot
    CHECK(cpu0.pc() == kTextVa);
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // retry LLD
    CHECK(cpu0.readGpr(5) == concurrentValue);
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // DADDIU
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // SCD succeeds
    CHECK(cpu0.readGpr(5) == 1);
    CHECK(readDouble(retryMemory, kDataPa, false) == concurrentValue + 1);
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // BEQ not taken
    CHECK(cpu0.step(retryMemory) == Mips64StepResult::Continue); // delay slot
    CHECK(cpu0.pc() == kTextVa + 20);
}

void testTrackedHostWritesAndSnapshotReplay() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setupCpu(cpu, Mips64Endianness::Big);
    writeDouble(memory, kDataPa, 0x0102030405060708ULL, false);
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    const std::vector<uint8_t> savedCpu = cpu.serialize();
    const PhysicalAddressMapSnapshot savedMemory = map.CaptureSnapshot();
    CHECK(savedMemory.reservationTrackingVersion == 1);
    CHECK(savedMemory.reservationGenerations.size() == 1);
    CHECK(savedCpu[4] == Mips64Cpu::kSerializationVersion);

    const uint8_t hostPatch[1] = {0xFE};
    memory.loadBuffer(kDataPa + 3, hostPatch, sizeof(hostPatch));
    CHECK(map.WriteRamBatchTransactional({PhysicalRamWrite{kDataPa + 4, hostPatch, 1}}));
    PreparedPhysicalAddressMapRestore prepared;
    std::string error;
    CHECK(map.PrepareRestore(savedMemory, prepared, &error));
    CHECK(error.empty());
    map.CommitRestore(std::move(prepared));
    CHECK(readDouble(memory, kDataPa, false) == 0x0102030405060708ULL);

    Mips64Cpu restored(Mips64Endianness::Big);
    CHECK(restored.deserialize(savedCpu.data(), savedCpu.size()));
    CHECK(restored.reservationState().valid);
    restored.writeGpr(5, 0x8877665544332211ULL);
    CHECK(stepAt(restored, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
    CHECK(restored.readGpr(5) == 1);
    CHECK(readDouble(memory, kDataPa, false) == 0x8877665544332211ULL);

    // A CPU state restored without its corresponding monitor generation must
    // fail its old reservation, even when the bytes happen to match again.
    CHECK(map.PrepareRestore(savedMemory, prepared, &error));
    map.CommitRestore(std::move(prepared));
    Mips64Cpu stale(Mips64Endianness::Big);
    CHECK(stale.deserialize(savedCpu.data(), savedCpu.size()));
    stale.writeGpr(5, 0x9999999999999999ULL);
    writeDouble(memory, kDataPa, 0x0102030405060708ULL, false);
    CHECK(stepAt(stale, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
    CHECK(stale.readGpr(5) == 0);
    CHECK(readDouble(memory, kDataPa, false) == 0x0102030405060708ULL);

    Memory clearedMemory(kRamBytes, false);
    clearedMemory.EnablePhysicalAddressMap();
    Mips64Cpu clearedCpu(Mips64Endianness::Big);
    setupCpu(clearedCpu, Mips64Endianness::Big);
    writeDouble(clearedMemory, kDataPa, 0x1111222233334444ULL, false);
    CHECK(stepAt(clearedCpu, clearedMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    clearedMemory.Clear();
    writeWord(clearedMemory, kTextPa + 4, encodeI(0x3C, 4, 5, 0), false);
    clearedCpu.writeGpr(5, 0x5555666677778888ULL);
    clearedCpu.setPc(kTextVa + 4);
    CHECK(clearedCpu.step(clearedMemory) == Mips64StepResult::Continue);
    CHECK(clearedCpu.readGpr(5) == 0);

    Memory restoredMemory(kRamBytes, false);
    restoredMemory.EnablePhysicalAddressMap();
    Mips64Cpu restoredCpu(Mips64Endianness::Big);
    setupCpu(restoredCpu, Mips64Endianness::Big);
    writeWord(restoredMemory, kTextPa + 4, encodeI(0x3C, 4, 5, 0), false);
    writeDouble(restoredMemory, kDataPa, 0x1111222233334444ULL, false);
    CHECK(stepAt(restoredCpu, restoredMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    const MemorySnapshot hostSnapshot = restoredMemory.CreateSnapshot();
    writeDouble(restoredMemory, kDataPa, 0x9999, false);
    restoredMemory.RestoreSnapshot(hostSnapshot);
    restoredCpu.writeGpr(5, 0x5555666677778888ULL);
    restoredCpu.setPc(kTextVa + 4);
    CHECK(restoredCpu.step(restoredMemory) == Mips64StepResult::Continue);
    CHECK(restoredCpu.readGpr(5) == 0);
}

void testFaultsEretAndReset() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    map.RegisterRom(0x10000, std::vector<uint8_t>(16, 0x5A));
    auto device = std::make_shared<ReservationTestMmio>();
    map.RegisterMmio(0x10020, 16, device, {8});
    Mips64Cpu cpu(Mips64Endianness::Big);
    setupCpu(cpu, Mips64Endianness::Big);

    // SCD to a mapped ROM target fails with rt=0 and does not touch the ROM.
    cpu.writeGpr(4, kCKSeg0Base | 0x10000);
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x3C, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(!cpu.reservationState().valid);

    // LLD from ROM is a data bus fault and leaves the destination unchanged.
    cpu.writeGpr(5, 0x123456789ABCDEF0ULL);
    CHECK(!stepAt(cpu, memory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0x123456789ABCDEF0ULL);
    CHECK(((cpu.cp0Cause() >> 2) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::DBE));
    CHECK(!cpu.reservationState().valid);

    cpu.writeGpr(4, kCKSeg0Base | 0x10020);
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x3C, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(device->writeCount == 0 && device->readCount == 0);

    Memory holeMemory(kRamBytes, false);
    PhysicalAddressMap& holeMap = holeMemory.EnablePhysicalAddressMap();
    CHECK(holeMap.RemoveRegion(0));
    holeMap.RegisterRam(0, 0x4000);
    Mips64Cpu unmapped(Mips64Endianness::Big);
    setupCpu(unmapped, Mips64Endianness::Big);
    unmapped.writeGpr(4, kCKSeg0Base | 0x5000);
    CHECK(!stepAt(unmapped, holeMemory, kTextVa,
                  encodeI(0x3C, 4, 5, 0), false));
    CHECK(((unmapped.cp0Cause() >> 2) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::DBE));
    CHECK(!unmapped.reservationState().valid);

    Memory alignedMemory(kRamBytes, false);
    alignedMemory.EnablePhysicalAddressMap();
    Mips64Cpu misaligned(Mips64Endianness::Big);
    setupCpu(misaligned, Mips64Endianness::Big);
    writeDouble(alignedMemory, kDataPa, 0x1234, false);
    CHECK(stepAt(misaligned, alignedMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    CHECK(!stepAt(misaligned, alignedMemory, kTextVa + 4,
                  encodeI(0x3C, 4, 5, 1), false));
    CHECK(!misaligned.reservationState().valid);
    CHECK(((misaligned.cp0Cause() >> 2) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::AdES));

    Mips64Cpu eret(Mips64Endianness::Big);
    setupCpu(eret, Mips64Endianness::Big);
    CHECK(stepAt(eret, alignedMemory, kTextVa, encodeI(0x34, 4, 5, 0), false));
    writeWord(alignedMemory, kTextPa + 8, 0x42000018U, false); // ERET
    eret.setPc(kTextVa + 8);
    CHECK(eret.step(alignedMemory) == Mips64StepResult::Continue);
    CHECK(!eret.reservationState().valid);
    CHECK(eret.reservationState().generation == 0);

    CHECK(stepAt(eret, alignedMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    CHECK(eret.reservationState().valid);
    eret.reset();
    CHECK(!eret.reservationState().valid);
    eret.writeGpr(4, kDataVa);
    CHECK(stepAt(eret, alignedMemory, kTextVa,
                 encodeI(0x34, 4, 5, 0), false));
    eret.configureEndianness(Mips64Endianness::Little);
    CHECK(!eret.reservationState().valid);
}

}  // namespace

int main() {
    testDecodeAndEndianExecution();
    testAtomicTwoCpuStoreAndGranuleConflicts();
    testCachedAccessRequirementAndGuestRetryLoop();
    testTrackedHostWritesAndSnapshotReplay();
    testFaultsEretAndReset();
    std::cout << "test_mips64_lld_scd: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
