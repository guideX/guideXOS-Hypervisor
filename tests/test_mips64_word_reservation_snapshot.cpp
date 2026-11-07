// MIPS64-18 width-qualified reservation serialization and replay checks.

#include "PhysicalAddressMap.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <cstdint>
#include <iostream>
#include <limits>
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

uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void write32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * (3 - i)));
}

void put64(std::vector<uint8_t>& bytes, size_t at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

bool stepAt(Mips64Cpu& cpu, Memory& memory, uint64_t pc,
            uint32_t instruction) {
    uint8_t bytes[4]{};
    write32(bytes, instruction);
    memory.Write(kTextPa + (pc - kTextVa), bytes, sizeof(bytes));
    cpu.setPc(pc);
    return cpu.step(memory) == Mips64StepResult::Continue;
}

void setup(Mips64Cpu& cpu) {
    cpu.configureEndianness(Mips64Endianness::Big);
    cpu.setPc(kTextVa);
    cpu.writeGpr(4, kDataVa);
}

bool restoreMap(PhysicalAddressMap& map, const PhysicalAddressMapSnapshot& snapshot) {
    PreparedPhysicalAddressMapRestore prepared;
    std::string error;
    if (!map.PrepareRestore(snapshot, prepared, &error) || !error.empty()) return false;
    map.CommitRestore(std::move(prepared));
    return true;
}

void testActiveLlSerializationAndReplay() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setup(cpu);
    uint8_t initial[4] = {0x00, 0x00, 0x00, 0x07};
    memory.Write(kDataPa, initial, sizeof(initial));
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x30, 4, 5, 0)));
    const std::vector<uint8_t> beforeCpu = cpu.serialize();
    const PhysicalAddressMapSnapshot beforeMemory = map.CaptureSnapshot();
    CHECK(beforeCpu.size() == 1001 && beforeCpu[4] == 6);
    CHECK(beforeCpu[958] == 4);
    CHECK(cpu.reservationState().valid && cpu.reservationState().size == 4);
    CHECK(beforeMemory.reservationTrackingVersion == 1);

    Mips64Cpu restored(Mips64Endianness::Little);
    CHECK(restored.deserialize(beforeCpu.data(), beforeCpu.size()));
    CHECK(restored.serialize() == beforeCpu);
    restored.writeGpr(5, 0xABCDEF0198765432ULL);
    CHECK(stepAt(restored, memory, kTextVa + 4, encodeI(0x38, 4, 5, 0)));
    CHECK(restored.readGpr(5) == 1);
    uint8_t afterFirst[4]{};
    memory.Read(kDataPa, afterFirst, sizeof(afterFirst));
    CHECK(afterFirst[0] == 0x98 && afterFirst[3] == 0x32);

    CHECK(restored.deserialize(beforeCpu.data(), beforeCpu.size()));
    CHECK(restoreMap(map, beforeMemory));
    restored.writeGpr(5, 0xABCDEF0198765432ULL);
    CHECK(stepAt(restored, memory, kTextVa + 4, encodeI(0x38, 4, 5, 0)));
    CHECK(restored.readGpr(5) == 1);
    uint8_t afterReplay[4]{};
    memory.Read(kDataPa, afterReplay, sizeof(afterReplay));
    CHECK(std::equal(std::begin(afterFirst), std::end(afterFirst), std::begin(afterReplay)));

    // CPU v6 still serializes and restores the pre-existing eight-byte form.
    setup(cpu);
    CHECK(stepAt(cpu, memory, kTextVa + 8, encodeI(0x34, 4, 6, 0)));
    const auto lldBlob = cpu.serialize();
    Mips64Cpu lldRestored;
    CHECK(lldRestored.deserialize(lldBlob.data(), lldBlob.size()));
    CHECK(lldRestored.reservationState().valid && lldRestored.reservationState().size == 8);
}

void testInvalidCpuAndMemoryReservationSnapshotsAreTransactional() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu source(Mips64Endianness::Big);
    setup(source);
    uint8_t value[4] = {0, 0, 0, 1};
    memory.Write(kDataPa, value, sizeof(value));
    CHECK(stepAt(source, memory, kTextVa, encodeI(0x30, 4, 5, 0)));
    const std::vector<uint8_t> validBlob = source.serialize();
    Mips64Cpu target(Mips64Endianness::Big);
    target.writeGpr(7, 0x1122334455667788ULL);
    const auto targetBefore = target.serialize();
    const size_t reservationOffset = 957;
    std::vector<std::vector<uint8_t>> badBlobs;
    auto bad = validBlob; bad[reservationOffset + 1] = 2; badBlobs.push_back(bad);
    bad = validBlob; bad[reservationOffset + 3] = 1; badBlobs.push_back(bad);
    bad = validBlob; bad[reservationOffset + 2] = 2; badBlobs.push_back(bad);
    bad = validBlob; bad[reservationOffset + 2] = 8; badBlobs.push_back(bad);
    bad = validBlob; put64(bad, reservationOffset + 4, kDataVa + 1); badBlobs.push_back(bad);
    bad = validBlob; put64(bad, reservationOffset + 12, kPhysicalAddressLimit); badBlobs.push_back(bad);
    bad = validBlob; put64(bad, reservationOffset + 20, 0); badBlobs.push_back(bad);
    bad = validBlob; put64(bad, reservationOffset + 28, 1); badBlobs.push_back(bad);
    bad = validBlob; put64(bad, reservationOffset + 36, std::numeric_limits<uint64_t>::max()); badBlobs.push_back(bad);
    for (const auto& corrupted : badBlobs) {
        CHECK(!target.deserialize(corrupted.data(), corrupted.size()));
        CHECK(target.serialize() == targetBefore);
    }

    const PhysicalAddressMapSnapshot validMemory = map.CaptureSnapshot();
    const PhysicalAddressMapSnapshot liveBefore = map.CaptureSnapshot();
    for (unsigned corruption = 0; corruption < 4; ++corruption) {
        auto invalid = validMemory;
        if (invalid.reservationGenerations.empty()) {
            CHECK(false);
            continue;
        }
        if (corruption == 0) invalid.reservationGenerations[0].backingId = 99;
        if (corruption == 1) invalid.reservationGenerations[0].granuleOffset = kRamBytes;
        if (corruption == 2) invalid.reservationGenerations[0].generation = invalid.nextReservationGeneration;
        if (corruption == 3) invalid.reservationGenerations[0].granuleOffset += 1;
        PreparedPhysicalAddressMapRestore prepared;
        std::string error;
        CHECK(!map.PrepareRestore(invalid, prepared, &error));
        CHECK(!error.empty());
        CHECK(map.CaptureSnapshot().reservationGenerations == liveBefore.reservationGenerations);
    }
}

void testConflictReplayAndVmIsolation() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setup(cpu);
    uint8_t original[4] = {0x00, 0x00, 0x00, 0x09};
    memory.Write(kDataPa, original, sizeof(original));
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x30, 4, 5, 0)));
    const auto savedCpu = cpu.serialize();
    const auto savedMap = map.CaptureSnapshot();

    uint8_t conflict[4] = {0, 0, 0, 0x40};
    memory.loadBuffer(kDataPa + 4, conflict, sizeof(conflict)); // same granule
    cpu.writeGpr(5, 0x12345678);
    CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x38, 4, 5, 0)));
    CHECK(cpu.readGpr(5) == 0);
    const auto failedCpu = cpu.serialize();
    const auto failedMap = map.CaptureSnapshot();

    CHECK(cpu.deserialize(savedCpu.data(), savedCpu.size()));
    CHECK(restoreMap(map, savedMap));
    memory.loadBuffer(kDataPa + 4, conflict, sizeof(conflict));
    cpu.writeGpr(5, 0x12345678);
    CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x38, 4, 5, 0)));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(cpu.serialize() == failedCpu);
    const auto replayMap = map.CaptureSnapshot();
    CHECK(replayMap.reservationGenerations == failedMap.reservationGenerations);
    CHECK(replayMap.nextReservationGeneration == failedMap.nextReservationGeneration);

    // Separate VM-owned maps have independent reservation generations.
    Memory vmA(kRamBytes, false), vmB(kRamBytes, false);
    vmA.EnablePhysicalAddressMap();
    vmB.EnablePhysicalAddressMap();
    Mips64Cpu cpuA(Mips64Endianness::Big);
    setup(cpuA);
    vmA.Write(kDataPa, original, sizeof(original));
    CHECK(stepAt(cpuA, vmA, kTextVa, encodeI(0x30, 4, 5, 0)));
    vmB.Write(kDataPa, conflict, sizeof(conflict));
    cpuA.writeGpr(5, 0x55667788);
    CHECK(stepAt(cpuA, vmA, kTextVa + 4, encodeI(0x38, 4, 5, 0)));
    CHECK(cpuA.readGpr(5) == 1);
}

}  // namespace

int main() {
    testActiveLlSerializationAndReplay();
    testInvalidCpuAndMemoryReservationSnapshotsAreTransactional();
    testConflictReplayAndVmIsolation();
    std::cout << "test_mips64_word_reservation_snapshot: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
