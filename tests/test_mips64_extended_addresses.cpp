// MIPS64-5 extended segment, TLB qualification, and a genuine UX user guest.
#include <cstdint>
#include <iostream>
#include <vector>

#include "mips64_extended_test_support.h"

using namespace mips64;
using namespace mips64_extended_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { if (expr) ++g_pass; else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } } while (0)

static Mips64Probe probeWithStatus(uint64_t status, uint64_t va,
                                  Mips64Access access = Mips64Access::Load) {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    CHECK(writeStatus(cpu, mem, status));
    return cpu.probeAddress(va, access);
}

static void testAddressWidthAndSegmentBoundaries() {
    CHECK(kMips64SegBits == 40 && kMips64PaBits == 36);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitUx, kXusegBase).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitUx, kXusegEnd).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitUx,
                          kXusegBase - 1).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitUx,
                          kMips64SegmentSpan).fault == Mips64Fault::AddressErrorLoad);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitSx,
                          kXssegBase).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitSx,
                          kXssegEnd).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitSx,
                          kXssegEnd + 1).fault == Mips64Fault::AddressErrorLoad);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          kXksegBase).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          kXksegEnd).isRefill);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          kXksegEnd + 1).fault == Mips64Fault::AddressErrorLoad);

    const Mips64Probe xusegDisabled = probeWithStatus(kStatusBitBev | kStatusBitKx,
                                                       kXusegBase);
    CHECK(xusegDisabled.fault == Mips64Fault::AddressErrorLoad);
    const Mips64Probe xssegDisabled = probeWithStatus(kStatusBitBev | kStatusBitKx,
                                                       kXssegBase);
    CHECK(xssegDisabled.fault == Mips64Fault::AddressErrorLoad);
    const Mips64Probe xksegDisabled = probeWithStatus(kStatusBitBev | kStatusBitUx,
                                                       kXksegBase);
    CHECK(xksegDisabled.fault == Mips64Fault::AddressErrorLoad);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          0xFFFFFFFF80000000ULL).phys == 0);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          0xFFFFFFFFA0000000ULL).phys == 0);
    CHECK(probeWithStatus(kStatusBitBev | kStatusBitKx,
                          0xFFFFFFFFC0000000ULL).isRefill);
}

static void testModeGatesAndExceptionOverrides() {
    const uint64_t user = (2ULL << kStatusKsuShift);
    const uint64_t supervisor = (1ULL << kStatusKsuShift);

    const Mips64Probe userXuseg = probeWithStatus(kStatusBitBev | user | kStatusBitUx,
                                                  kXusegBase);
    CHECK(userXuseg.fault == Mips64Fault::TlbRefillLoad && userXuseg.useXtlbVector);
    CHECK(probeWithStatus(kStatusBitBev | user | kStatusBitUx | kStatusBitSx,
                          kXssegBase).fault == Mips64Fault::AddressErrorLoad);
    CHECK(probeWithStatus(kStatusBitBev | user | kStatusBitKx,
                          0xFFFFFFFF80000000ULL).fault == Mips64Fault::AddressErrorLoad);

    const Mips64Probe supervisorXuseg = probeWithStatus(kStatusBitBev | supervisor |
                                                         kStatusBitUx,
                                                         kXusegBase);
    CHECK(supervisorXuseg.fault == Mips64Fault::TlbRefillLoad);
    CHECK(probeWithStatus(kStatusBitBev | supervisor | kStatusBitSx,
                          kXssegBase).fault == Mips64Fault::TlbRefillLoad);
    CHECK(probeWithStatus(kStatusBitBev | supervisor | kStatusBitKx,
                          kXksegBase).fault == Mips64Fault::AddressErrorLoad);
    CHECK(probeWithStatus(kStatusBitBev | supervisor | kStatusBitKx,
                          0x9000000000000000ULL).fault == Mips64Fault::AddressErrorLoad);

    const Mips64Probe exlOverridesKsu = probeWithStatus(
        kStatusBitBev | user | kStatusBitExl | kStatusBitKx,
        0x9000000000000000ULL);
    CHECK(exlOverridesKsu.ok && exlOverridesKsu.phys == 0);
    const Mips64Probe erlUseg = probeWithStatus(
        kStatusBitBev | user | kStatusBitErl | kStatusBitUx, 0x00001234ULL);
    CHECK(erlUseg.ok && erlUseg.phys == 0x1234 && erlUseg.cacheAttribute == 2);

    // Compatibility refill routing has its own table: UX selects the kuseg
    // refill vector, while KX selects compatibility CKSSEG/CKSEG3 XTLB routing.
    SparsePhysicalMemory mem;
    Mips64Cpu vectors;
    CHECK(writeStatus(vectors, mem, kStatusBitBev));
    CHECK(vectors.refillVector(0x4000) == kVectorBev1Refill);
    CHECK(writeStatus(vectors, mem, kStatusBitBev | kStatusBitUx));
    CHECK(vectors.refillVector(0x4000) == kVectorBev1Xtlb);
    CHECK(writeStatus(vectors, mem, kStatusBitBev));
    CHECK(vectors.refillVector(0xFFFFFFFFC0004000ULL) == kVectorBev1Refill);
    CHECK(vectors.refillVector(0xFFFFFFFFE0004000ULL) == kVectorBev1Refill);
    CHECK(writeStatus(vectors, mem, kStatusBitBev | kStatusBitKx));
    CHECK(vectors.refillVector(0xFFFFFFFFC0004000ULL) == kVectorBev1Xtlb);
    CHECK(vectors.refillVector(0xFFFFFFFFE0004000ULL) == kVectorBev1Xtlb);
    CHECK(writeStatus(vectors, mem, kStatusBitBev | kStatusBitSx));
    CHECK(vectors.refillVector(kXssegBase) == kVectorBev1Xtlb);
    CHECK(writeStatus(vectors, mem, kStatusBitBev | kStatusBitKx));
    CHECK(vectors.refillVector(kXksegBase) == kVectorBev1Xtlb);
}

static void testExtendedTlbKeepsHighVpnAndGuestOperations() {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    constexpr uint64_t vaA = 0x0000001000000000ULL;
    constexpr uint64_t vaB = 0x0000002000000000ULL;
    CHECK(writeStatus(cpu, mem, kStatusBitBev | kStatusBitUx));
    CHECK(installTlbPair(cpu, mem, vaA, entryLo(1, false), entryLo(2, false), 3, 3));
    CHECK(cpu.probeAddress(vaA, Mips64Access::Load).ok);
    CHECK(cpu.probeAddress(vaA + 0x1000, Mips64Access::Load).phys == 0x2000);
    const std::vector<uint8_t> extendedState = cpu.serialize();
    Mips64Cpu restored;
    CHECK(restored.deserialize(extendedState.data(), extendedState.size()));
    CHECK(restored.serialize() == extendedState);
    CHECK(restored.probeAddress(vaA, Mips64Access::Load).ok);
    CHECK(restored.probeAddress(vaA + 0x1000, Mips64Access::Load).phys == 0x2000);
    const Mips64Probe otherHighVpn = cpu.probeAddress(vaB, Mips64Access::Load);
    CHECK(!otherHighVpn.ok && otherHighVpn.isRefill);

    // TLBP and TLBR operate on the same extended EntryHi representation.
    CHECK(cpu.writeGpr(8, (vaA & (kEntryHiRegionMask | kEntryHiVpn2Mask)) | 3));
    CHECK(runKernel(cpu, mem, 0x6B00,
                    {encCp0(5, 8, 10), 0x42000008U}));  // dmtc0 EntryHi; tlbp
    CHECK((cpu.cp0Index() & kIndexPBit) == 0);
    CHECK(runKernel(cpu, mem, 0x6B40,
                    {encI(0x19, 0, 8, 3), encCp0(5, 8, 0), 0x42000001U,
                     encCp0(1, 9, 10)}));  // Index=3; tlbr; dmfc0 EntryHi
    CHECK(cpu.readGpr(9) == ((vaA & (kEntryHiRegionMask | kEntryHiVpn2Mask)) | 3));

    // A non-global mapping is ASID-qualified; a guest-installed global one
    // remains visible after the current ASID changes.
    const uint64_t entryHiAsid4 = (vaA & (kEntryHiRegionMask | kEntryHiVpn2Mask)) | 4;
    CHECK(cpu.writeGpr(8, entryHiAsid4));
    CHECK(runKernel(cpu, mem, 0x6B80, {encCp0(5, 8, 10)}));
    CHECK(cpu.probeAddress(vaA, Mips64Access::Load).isRefill);
    CHECK(installTlbPair(cpu, mem, vaB, entryLo(3), entryLo(4), 4, 4));
    CHECK(cpu.probeAddress(vaB, Mips64Access::Load).ok);
    CHECK(cpu.writeGpr(8, (vaB & (kEntryHiRegionMask | kEntryHiVpn2Mask)) | 5));
    CHECK(runKernel(cpu, mem, 0x6BC0, {encCp0(5, 8, 10)}));
    CHECK(cpu.probeAddress(vaB, Mips64Access::Load).ok);

    // TLBWR uses Random and can install an extended VPN without losing its
    // upper segment bits.
    CHECK(installTlbPairRandom(cpu, mem, 0x0000003000000000ULL,
                               entryLo(5), entryLo(6), 5));
    CHECK(cpu.probeAddress(0x0000003000000000ULL, Mips64Access::Load).ok);
    Mips64TlbEntry randomEntry;
    CHECK(cpu.tlbEntry(kTlbEntries - 1, randomEntry));
    CHECK(randomEntry.vpn2 == ((0x0000003000000000ULL >> 13) & kTlbVpn2Mask));
}

static void testEncodedExtendedUserLoadsAndStores() {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    constexpr uint64_t userVa = 0x0000001000000000ULL;
    const uint32_t lo0 = entryLo(1, true, true, true, 3);
    const uint32_t lo1 = entryLo(2, true, true, true, 2);
    CHECK(installTlbPair(cpu, mem, userVa, lo0, lo1, 0, 0));

    std::vector<uint32_t> userProgram;
    emitLoad64(userProgram, 8, userVa + 0x1000);
    userProgram.push_back(encI(0x19, 0, 9, 0x1234));
    userProgram.push_back(encI(0x3F, 8, 9, 0));  // sd $t1,0($t0)
    userProgram.push_back(encI(0x37, 8, 10, 0)); // ld $t2,0($t0)
    for (size_t i = 0; i < userProgram.size(); ++i) {
        storeWord(mem, 0x1000 + i * 4, userProgram[i], cpu.isLittleEndian());
    }

    const uint64_t userStatus = kStatusBitBev | kStatusBitUx |
                                (2ULL << kStatusKsuShift);
    cpu.setPc(userVa);
    CHECK(writeStatus(cpu, mem, userStatus, 0x6D00));
    CHECK(cpu.isUserMode() && !cpu.statusExl() && !cpu.statusErl());
    for (size_t i = 0; i < userProgram.size(); ++i) {
        CHECK(cpu.step(mem) == Mips64StepResult::Continue);
    }
    CHECK(cpu.readGpr(10) == 0x1234);
    CHECK(load64(mem, 0x2000, true) == 0x1234);
    CHECK(cpu.probeAddress(userVa + 0x1000, Mips64Access::Load).cacheAttribute == 2);

    // Enter EXL through a real user-mode privilege fault, then clear UX while
    // returning to the extended PC. UX=0 makes instruction fetch reject it.
    cpu.setPc(kCKSsegBase);
    CHECK(cpu.step(mem) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::AddressErrorFetch && cpu.statusExl());
    cpu.setPc(userVa);
    CHECK(writeStatus(cpu, mem, kStatusBitBev | (2ULL << kStatusKsuShift), 0x6D40));
    CHECK(cpu.step(mem) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::AddressErrorFetch);
    CHECK(cpu.cp0BadVAddr() == userVa && cpu.causeExcCode() == 4);
}

int main() {
    testAddressWidthAndSegmentBoundaries();
    testModeGatesAndExceptionOverrides();
    testExtendedTlbKeepsHighVpnAndGuestOperations();
    testEncodedExtendedUserLoadsAndStores();
    std::cout << "test_mips64_extended_addresses: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
