// MIPS64-18 encoded LL/SC word qualification.

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

uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t value, bool isLittle) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = isLittle ? 8 * i : 8 * (3 - i);
        bytes[i] = static_cast<uint8_t>(value >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(IMemory& memory, uint64_t address, bool isLittle) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[i]) << (isLittle ? 8 * i : 8 * (3 - i));
    }
    return value;
}

void setup(Mips64Cpu& cpu, Mips64Endianness endian) {
    cpu.configureEndianness(endian);
    cpu.setPc(kTextVa);
    cpu.writeGpr(4, kDataVa);
}

bool stepAt(Mips64Cpu& cpu, Memory& memory, uint64_t pc,
            uint32_t word, bool isLittle) {
    writeWord(memory, kTextPa + (pc - kTextVa), word, isLittle);
    cpu.setPc(pc);
    return cpu.step(memory) == Mips64StepResult::Continue;
}

bool stepCurrent(Mips64Cpu& cpu, Memory& memory) {
    return cpu.step(memory) == Mips64StepResult::Continue;
}

uint32_t encodeCp0(uint8_t rs, uint8_t rt, uint8_t rd) {
    return (0x10U << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | (uint32_t(rd) << 11);
}

class ReservationMmio final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 4; }
    void Read(uint64_t, uint8_t* destination, size_t size) const override {
        ++readCount;
        for (size_t index = 0; index < size; ++index) destination[index] = 0;
    }
    void Write(uint64_t, const uint8_t*, size_t) override { ++writeCount; }
    void Reset() override { readCount = 0; writeCount = 0; }

    mutable unsigned readCount = 0;
    unsigned writeCount = 0;
};

void testExactFrontierDecodeAndLwc2Separation() {
    const Mips64Decoded ll = decodeInstruction(0xC2020060U);
    CHECK(ll.valid && ll.op == Mips64Op::Ll);
    CHECK(ll.rs == 16 && ll.rt == 2 && ll.imm == 0x0060);
    CHECK(Mips64Cpu{}.disassemble(ll) == "ll $2, 96($16)");

    const Mips64Decoded lwc2 = decodeInstruction(0xC8020060U);
    CHECK(!lwc2.valid && lwc2.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(lwc2.error.find("LWC2") != std::string::npos);
    CHECK(Mips64Cpu{}.disassemble(lwc2).find("LWC2") != std::string::npos);

    const Mips64Decoded sc = decodeInstruction(0xE2020060U);
    CHECK(sc.valid && sc.op == Mips64Op::Sc && sc.rs == 16 && sc.rt == 2);
    CHECK(Mips64Cpu{}.disassemble(sc) == "sc $2, 96($16)");
    CHECK(decodeInstruction(0xD0850000U).op == Mips64Op::Lld);
    CHECK(decodeInstruction(0xF0810000U).op == Mips64Op::Scd);

    const Mips64Decoded di = decodeInstruction(0x41606000U);
    CHECK(!di.valid && di.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(di.error.find("DI") != std::string::npos);
    const Mips64Decoded ei = decodeInstruction(0x41606020U);
    CHECK(!ei.valid && ei.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(ei.error.find("EI") != std::string::npos);
    const Mips64Decoded malformedMfmc0 = decodeInstruction(0x41606800U);
    CHECK(!malformedMfmc0.valid && malformedMfmc0.fault == Mips64Fault::ReservedInstruction);
}

void testLlWidthEndianSignExtensionAndOffsets() {
    for (Mips64Endianness endian : {Mips64Endianness::Big, Mips64Endianness::Little}) {
        const bool isLittle = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(endian);
        setup(cpu, endian);
        writeWord(memory, kDataPa, 0x800000A5U, isLittle);
        writeWord(memory, kDataPa + 4, 0x7FFFFFFFU, isLittle);

        // The authentic word loads through $s0 + 96, with no address/value
        // assumed by the decoder or memory path.
        cpu.writeGpr(16, kDataVa - 96);
        CHECK(stepAt(cpu, memory, kTextVa, 0xC2020060U, isLittle));
        CHECK(cpu.readGpr(2) == 0xFFFFFFFF800000A5ULL);
        CHECK(cpu.reservationState().valid && cpu.reservationState().size == 4);
        CHECK(cpu.reservationState().virtualAddress == kDataVa);
        CHECK(cpu.reservationState().physicalAddress == kDataPa);
        CHECK(cpu.reservationState().cacheAttribute == 3);

        // Signed negative offset and positive-word sign extension.
        cpu.writeGpr(4, kDataVa + 4);
        CHECK(stepAt(cpu, memory, kTextVa + 4,
                     encodeI(0x30, 4, 3, 0xFFFC), isLittle));
        CHECK(cpu.readGpr(3) == 0xFFFFFFFF800000A5ULL);
        CHECK(cpu.reservationState().size == 4);
        cpu.writeGpr(4, kDataVa);
        CHECK(stepAt(cpu, memory, kTextVa + 8,
                     encodeI(0x30, 4, 3, 4), isLittle));
        CHECK(cpu.readGpr(3) == 0x000000007FFFFFFFULL);
    }
}

void testLlFaultsAliasAndZeroRegister() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setup(cpu, Mips64Endianness::Big);
    writeWord(memory, kDataPa, 0x12345678U, false);

    // rs == rt: base is sampled before LL replaces the register.
    cpu.writeGpr(5, kDataVa + 4);
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x30, 5, 5, 0xFFFC), false));
    CHECK(cpu.readGpr(5) == 0x0000000012345678ULL);
    CHECK(cpu.reservationState().virtualAddress == kDataVa);

    // rt == zero still performs the load-linked memory transaction.
    CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x30, 4, 0, 0), false));
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.reservationState().valid && cpu.reservationState().size == 4);

    const uint64_t before = cpu.readGpr(2);
    cpu.writeGpr(4, kDataVa + 1);
    CHECK(!stepAt(cpu, memory, kTextVa + 8, encodeI(0x30, 4, 2, 0), false));
    CHECK(cpu.fault() == Mips64Fault::MisalignedLoad);
    CHECK(cpu.cp0BadVAddr() == kDataVa + 1);
    CHECK(cpu.readGpr(2) == before);
    CHECK(!cpu.reservationState().valid);
}

void testScWordCommitFailureAndWidth() {
    for (Mips64Endianness endian : {Mips64Endianness::Big, Mips64Endianness::Little}) {
        const bool isLittle = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(endian);
        setup(cpu, endian);
        writeWord(memory, kDataPa, 0x01020304U, isLittle);
        writeWord(memory, kDataPa + 4, 0xAABBCCDDU, isLittle);
        CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x30, 4, 5, 0), isLittle));
        cpu.writeGpr(5, 0xFEDCBA9876543210ULL);
        CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x38, 4, 5, 0), isLittle));
        CHECK(cpu.readGpr(5) == 1);
        CHECK(readWord(memory, kDataPa, isLittle) == 0x76543210U);
        CHECK(readWord(memory, kDataPa + 4, isLittle) == 0xAABBCCDDU);
        CHECK(!cpu.reservationState().valid);

        // No active LL: SC reports failure, stores nothing, and writes a
        // full-width zero even when the original source is nonzero.
        cpu.writeGpr(5, 0xFFFFFFFFABCDEF01ULL);
        CHECK(stepAt(cpu, memory, kTextVa + 8, encodeI(0x38, 4, 5, 0), isLittle));
        CHECK(cpu.readGpr(5) == 0);
        CHECK(readWord(memory, kDataPa, isLittle) == 0x76543210U);

        // Base and source may alias: SC uses the original address value for
        // both EA and the low word before replacing that GPR with success.
        CHECK(stepAt(cpu, memory, kTextVa + 12, encodeI(0x30, 4, 5, 0), isLittle));
        CHECK(stepAt(cpu, memory, kTextVa + 16, encodeI(0x38, 4, 4, 0), isLittle));
        CHECK(cpu.readGpr(4) == 1);
        CHECK(readWord(memory, kDataPa, isLittle) == static_cast<uint32_t>(kDataVa));
    }

    Memory faultMemory(kRamBytes, false);
    faultMemory.EnablePhysicalAddressMap();
    Mips64Cpu faultCpu(Mips64Endianness::Big);
    setup(faultCpu, Mips64Endianness::Big);
    writeWord(faultMemory, kDataPa, 0x01020304U, false);
    CHECK(stepAt(faultCpu, faultMemory, kTextVa, encodeI(0x30, 4, 5, 0), false));
    faultCpu.writeGpr(5, 0xAABBCCDDEEFF0011ULL);
    CHECK(!stepAt(faultCpu, faultMemory, kTextVa + 4, encodeI(0x38, 4, 5, 1), false));
    CHECK(faultCpu.fault() == Mips64Fault::MisalignedStore);
    CHECK(faultCpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::AdES));
    CHECK(faultCpu.readGpr(5) == 0xAABBCCDDEEFF0011ULL);
    CHECK(!faultCpu.reservationState().valid);
    CHECK(readWord(faultMemory, kDataPa, false) == 0x01020304U);
}

void testMixedReservationWidthsAndConflicts() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setup(cpu, Mips64Endianness::Big);
    writeWord(memory, kDataPa, 7, false);
    CHECK(stepAt(cpu, memory, kTextVa, encodeI(0x30, 4, 5, 0), false));
    cpu.writeGpr(5, 0x1122334455667788ULL);
    CHECK(stepAt(cpu, memory, kTextVa + 4, encodeI(0x3C, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(readWord(memory, kDataPa, false) == 7);

    CHECK(stepAt(cpu, memory, kTextVa + 8, encodeI(0x34, 4, 5, 0), false));
    cpu.writeGpr(5, 9);
    CHECK(stepAt(cpu, memory, kTextVa + 12, encodeI(0x38, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(readWord(memory, kDataPa, false) == 7);

    // LL followed by LLD replaces the reservation with the eight-byte width.
    CHECK(stepAt(cpu, memory, kTextVa + 16, encodeI(0x30, 4, 5, 0), false));
    CHECK(cpu.reservationState().size == 4);
    CHECK(stepAt(cpu, memory, kTextVa + 20, encodeI(0x34, 4, 5, 0), false));
    CHECK(cpu.reservationState().size == 8);
    cpu.writeGpr(5, 0x123456789ABCDEF0ULL);
    CHECK(stepAt(cpu, memory, kTextVa + 24, encodeI(0x38, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(readWord(memory, kDataPa, false) == 7);

    // A CPU1 write in the same 16-byte granule invalidates SC. A write in a
    // distinct granule does not invalidate it.
    Mips64Cpu cpu1(Mips64Endianness::Big);
    setup(cpu1, Mips64Endianness::Big);
    CHECK(stepAt(cpu, memory, kTextVa + 28, encodeI(0x30, 4, 5, 0), false));
    writeWord(memory, kDataPa + 8, 0x55667788U, false);
    cpu.writeGpr(5, 0x80000000ULL);
    CHECK(stepAt(cpu, memory, kTextVa + 32, encodeI(0x38, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0);
    CHECK(readWord(memory, kDataPa, false) == 7);

    CHECK(stepAt(cpu, memory, kTextVa + 36, encodeI(0x30, 4, 5, 0), false));
    writeWord(memory, kDataPa + 16, 0xCAFEBABEU, false);
    cpu.writeGpr(5, 0xDEADBEEFULL);
    CHECK(stepAt(cpu, memory, kTextVa + 40, encodeI(0x38, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 1);
    CHECK(readWord(memory, kDataPa, false) == 0xDEADBEEFU);

    // LLD followed by LL replaces the eight-byte reservation with a word LL.
    CHECK(stepAt(cpu, memory, kTextVa + 44, encodeI(0x34, 4, 5, 0), false));
    CHECK(cpu.reservationState().size == 8);
    CHECK(stepAt(cpu, memory, kTextVa + 48, encodeI(0x30, 4, 5, 0), false));
    CHECK(cpu.reservationState().size == 4);
    cpu.writeGpr(5, 0xABCDEF0187654321ULL);
    CHECK(stepAt(cpu, memory, kTextVa + 52, encodeI(0x38, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 1);
    CHECK(readWord(memory, kDataPa, false) == 0x87654321U);
}

void testTlbMappedCachedLlAndUserPrivilege() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    setup(cpu, Mips64Endianness::Big);
    writeWord(memory, kDataPa, 0x800000A5U, false);

    // Build a cached pair of TLB pages: user VA 0x4000 -> code PA 0x1000,
    // user VA 0x5000 -> data PA 0x2000.
    const uint32_t install[] = {
        encodeI(0x0F, 0, 8, 0x0040),                 // lui $8,0x0040
        encodeCp0(4, 8, 12),                         // mtc0 $8,Status (clear ERL)
        encodeI(0x19, 0, 8, 0x4000),                 // daddiu $8,$0,0x4000
        encodeCp0(5, 8, 10),                         // dmtc0 $8,EntryHi
        encodeI(0x19, 0, 8, 0x005F),                 // EntryLo0 pfn1,C=3,V,D,G
        encodeCp0(5, 8, 2),
        encodeI(0x19, 0, 8, 0x009F),                 // EntryLo1 pfn2,C=3,V,D,G
        encodeCp0(5, 8, 3),
        encodeI(0x19, 0, 8, 0),
        encodeCp0(5, 8, 0),                          // Index 0
        0x42000002U,                                 // tlbwi
    };
    for (unsigned index = 0; index < std::size(install); ++index) {
        CHECK(stepAt(cpu, memory, kTextVa + index * 4ULL, install[index], false));
    }
    const Mips64Probe mappedData = cpu.probeAddress(0x5000, Mips64Access::Load);
    CHECK(mappedData.ok && mappedData.phys == kDataPa && mappedData.cacheAttribute == 3);

    cpu.writeGpr(4, 0x5000);
    CHECK(stepAt(cpu, memory, kTextVa + 44, encodeI(0x30, 4, 5, 0), false));
    CHECK(cpu.readGpr(5) == 0xFFFFFFFF800000A5ULL);
    CHECK(cpu.reservationState().valid && cpu.reservationState().physicalAddress == kDataPa);

    // Run MTC0 from the mapped user code page, then LL through the odd mapped
    // page while in user mode.
    cpu.writeGpr(9, kStatusBitBev | (2ULL << kStatusKsuShift));
    writeWord(memory, kTextPa, encodeCp0(4, 9, 12), false);
    writeWord(memory, kTextPa + 4, encodeI(0x30, 4, 5, 0), false);
    writeWord(memory, kTextPa + 8, encodeI(0x30, 4, 5, 0), false);
    cpu.writeGpr(4, 0x5000);
    cpu.setPc(0x4000);
    CHECK(stepCurrent(cpu, memory));
    CHECK(cpu.isUserMode());
    CHECK(stepCurrent(cpu, memory));
    CHECK(cpu.readGpr(5) == 0xFFFFFFFF800000A5ULL);
    CHECK(cpu.reservationState().valid && cpu.reservationState().size == 4);

    // A user LL to a kernel direct-mapped segment raises AdEL and leaves the
    // destination unchanged without establishing an active reservation.
    cpu.writeGpr(4, kDataVa);
    cpu.writeGpr(5, 0x1122334455667788ULL);
    cpu.setPc(0x4008);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::AddressErrorLoad);
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::AdEL));
    CHECK(cpu.cp0BadVAddr() == kDataVa);
    CHECK(cpu.readGpr(5) == 0x1122334455667788ULL);
    CHECK(!cpu.reservationState().valid);
}

void testUncachedRomAndMmioLlRejection() {
    Memory uncachedMemory(kRamBytes, false);
    uncachedMemory.EnablePhysicalAddressMap();
    Mips64Cpu uncached(Mips64Endianness::Big);
    setup(uncached, Mips64Endianness::Big);
    writeWord(uncachedMemory, kDataPa, 0x12345678U, false);
    uncached.writeGpr(4, kCKSeg1Base | kDataPa);
    CHECK(uncached.probeAddress(uncached.readGpr(4), Mips64Access::Load).cacheAttribute == 2);
    uncached.writeGpr(5, 0xAABBCCDDEEFF0011ULL);
    CHECK(!stepAt(uncached, uncachedMemory, kTextVa, encodeI(0x30, 4, 5, 0), false));
    CHECK(uncached.fault() == Mips64Fault::MemoryFaultLoad);
    CHECK(uncached.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::DBE));
    CHECK(uncached.readGpr(5) == 0xAABBCCDDEEFF0011ULL);
    CHECK(!uncached.reservationState().valid);

    Memory romMemory(0x2000, false);
    PhysicalAddressMap& romMap = romMemory.EnablePhysicalAddressMap();
    romMap.RegisterRom(0x4000, std::vector<uint8_t>{0x12, 0x34, 0x56, 0x78});
    Mips64Cpu romCpu(Mips64Endianness::Big);
    setup(romCpu, Mips64Endianness::Big);
    romCpu.writeGpr(4, kCKSeg0Base | 0x4000);
    romCpu.writeGpr(5, 0x9988776655443322ULL);
    CHECK(!stepAt(romCpu, romMemory, kTextVa, encodeI(0x30, 4, 5, 0), false));
    CHECK(romCpu.fault() == Mips64Fault::MemoryFaultLoad);
    CHECK(romCpu.readGpr(5) == 0x9988776655443322ULL);
    CHECK(!romCpu.reservationState().valid);

    Memory mmioMemory(0x2000, false);
    PhysicalAddressMap& mmioMap = mmioMemory.EnablePhysicalAddressMap();
    auto device = std::make_shared<ReservationMmio>();
    mmioMap.RegisterMmio(0x4000, 16, device, {4});
    Mips64Cpu mmioCpu(Mips64Endianness::Big);
    setup(mmioCpu, Mips64Endianness::Big);
    mmioCpu.writeGpr(4, kCKSeg0Base | 0x4000);
    mmioCpu.writeGpr(5, 0x8877665544332211ULL);
    CHECK(!stepAt(mmioCpu, mmioMemory, kTextVa, encodeI(0x30, 4, 5, 0), false));
    CHECK(mmioCpu.fault() == Mips64Fault::MemoryFaultLoad);
    CHECK(mmioCpu.readGpr(5) == 0x8877665544332211ULL);
    CHECK(!mmioCpu.reservationState().valid);
    CHECK(device->readCount == 0 && device->writeCount == 0);
}

void testCpu1ConflictAndGuestRetryLoop() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu0(Mips64Endianness::Big);
    Mips64Cpu cpu1(Mips64Endianness::Big);
    setup(cpu0, Mips64Endianness::Big);
    setup(cpu1, Mips64Endianness::Big);
    writeWord(memory, kDataPa, 0, false);
    writeWord(memory, kDataPa + 4, 0, false);
    const uint64_t retryPc = kTextVa + 0x100;
    const uint32_t retryProgram[] = {
        encodeI(0x30, 4, 5, 0),              // ll $5,0($4)
        encodeI(0x09, 5, 5, 1),              // addiu $5,$5,1
        encodeI(0x38, 4, 5, 0),              // sc $5,0($4)
        encodeI(0x04, 5, 0, 0xFFFC),         // beq $5,$0,retry
        0x00000000U,                         // delay slot
    };
    for (unsigned index = 0; index < std::size(retryProgram); ++index) {
        writeWord(memory, kTextPa + 0x100 + index * 4ULL, retryProgram[index], false);
    }
    cpu0.setPc(retryPc);
    CHECK(stepCurrent(cpu0, memory)); // first LL

    cpu1.writeGpr(6, 9);
    CHECK(stepAt(cpu1, memory, kTextVa + 0x200, encodeI(0x2B, 4, 6, 4), false));
    CHECK(readWord(memory, kDataPa + 4, false) == 9);

    CHECK(stepCurrent(cpu0, memory)); // addiu
    CHECK(stepCurrent(cpu0, memory)); // first SC fails after CPU1's same-granule write
    CHECK(cpu0.readGpr(5) == 0);
    CHECK(readWord(memory, kDataPa, false) == 0);
    CHECK(stepCurrent(cpu0, memory)); // beq taken
    CHECK(cpu0.delayArmed());
    CHECK(stepCurrent(cpu0, memory)); // delay slot returns to LL
    CHECK(cpu0.pc() == retryPc);
    CHECK(stepCurrent(cpu0, memory)); // retry LL
    CHECK(stepCurrent(cpu0, memory)); // addiu
    CHECK(stepCurrent(cpu0, memory)); // retry SC succeeds
    CHECK(cpu0.readGpr(5) == 1);
    CHECK(readWord(memory, kDataPa, false) == 1);
    CHECK(readWord(memory, kDataPa + 4, false) == 9);
}

void testLinkedApiWidthsAndTargetPolicy() {
    Memory memory(kRamBytes, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    writeWord(memory, kDataPa, 0x01020304U, false);
    uint8_t bytes[8]{};
    Mips64ReservationToken token;
    map.ReadLinked(kDataPa, bytes, 4, token);
    CHECK(token.accessSize == 4 && token.backingId != 0);
    CHECK(map.StoreConditional(kDataPa, reinterpret_cast<const uint8_t*>("\xAA\xBB\xCC\xDD"),
                               4, token) == Mips64ConditionalStoreResult::Stored);
    CHECK(readWord(memory, kDataPa, false) == 0xAABBCCDDU);
    CHECK(map.StoreConditional(kDataPa, bytes, 8, token) ==
          Mips64ConditionalStoreResult::ReservationFailed);
    bool rejectedWidth = false;
    try { map.ReadLinked(kDataPa, bytes, 2, token); }
    catch (const PhysicalBusError&) { rejectedWidth = true; }
    CHECK(rejectedWidth);
    bool rejectedAlignment = false;
    try { map.ReadLinked(kDataPa + 1, bytes, 4, token); }
    catch (const PhysicalBusError&) { rejectedAlignment = true; }
    CHECK(rejectedAlignment);
}

}  // namespace

int main() {
    testExactFrontierDecodeAndLwc2Separation();
    testLlWidthEndianSignExtensionAndOffsets();
    testLlFaultsAliasAndZeroRegister();
    testScWordCommitFailureAndWidth();
    testMixedReservationWidthsAndConflicts();
    testTlbMappedCachedLlAndUserPrivilege();
    testUncachedRomAndMmioLlRejection();
    testCpu1ConflictAndGuestRetryLoop();
    testLinkedApiWidthsAndTargetPolicy();
    std::cout << "test_mips64_ll_sc_word: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
