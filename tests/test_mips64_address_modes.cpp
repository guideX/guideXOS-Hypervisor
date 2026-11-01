// MIPS64-5 effective-address compatibility and XTLB exception routing.
#include <cstdint>
#include <iostream>
#include <vector>

#include "mips64_extended_test_support.h"

using namespace mips64;
using namespace mips64_extended_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { if (expr) ++g_pass; else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } } while (0)

static void testUxZeroDataNormalizationDoesNotChangeFetch() {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    CHECK(installTlbPair(cpu, mem, 0, entryLo(0), entryLo(1), 0, 0));
    CHECK(installTlbPair(cpu, mem, 0x2000, entryLo(2), entryLo(3), 0, 1));
    store64(mem, 0x2000, 0xABCDEF1234567890ULL, true);

    std::vector<uint32_t> program;
    emitLoad64(program, 8, 0x0000000100002000ULL);
    program.push_back(encI(0x37, 8, 9, 0));  // ld $t1,0($t0)
    for (size_t i = 0; i < program.size(); ++i) {
        storeWord(mem, 0x1000 + i * 4, program[i], true);
    }

    cpu.setPc(0x1000);  // compatibility kuseg fetch, not normalized
    CHECK(writeStatus(cpu, mem, kStatusBitBev | (2ULL << kStatusKsuShift), 0x6E00));
    CHECK(cpu.isUserMode());
    for (size_t i = 0; i < program.size(); ++i) {
        CHECK(cpu.step(mem) == Mips64StepResult::Continue);
    }
    CHECK(cpu.readGpr(9) == 0xABCDEF1234567890ULL);

    constexpr uint64_t extendedFetch = 0x0000000100001000ULL;
    cpu.setPc(extendedFetch);
    CHECK(cpu.step(mem) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::AddressErrorFetch);
    CHECK(cpu.cp0BadVAddr() == extendedFetch); // fetch keeps its full address
}

static void runDelaySlotXtlbFault(bool bev) {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    constexpr uint64_t mainPhys = 0x1000;
    constexpr uint64_t mainVirt = kCKSeg0Base | mainPhys;
    constexpr uint64_t faultVa = 0x0000001200003000ULL;
    storeWord(mem, mainPhys, encI(0x04, 0, 0, 1), true); // beq $zero,$zero,+1
    storeWord(mem, mainPhys + 4, encI(0x37, 8, 9, 0), true); // delay slot: ld

    cpu.setPc(mainVirt);
    const uint64_t status = (bev ? kStatusBitBev : 0) | kStatusBitUx;
    CHECK(writeStatus(cpu, mem, status, 0x6E80));
    cpu.writeGpr(8, faultVa);
    CHECK(cpu.step(mem) == Mips64StepResult::Continue);
    CHECK(cpu.delayArmed() && cpu.pc() == mainVirt + 4);
    CHECK(cpu.step(mem) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::TlbRefillLoad);
    CHECK(cpu.causeBd() && cpu.cp0Epc() == mainVirt);
    CHECK(cpu.cp0BadVAddr() == faultVa && cpu.statusExl());
    CHECK(cpu.pc() == (bev ? kVectorBev1Xtlb : 0xFFFFFFFF80000080ULL));
    CHECK(((cpu.cp0XContext() >> 4) & ((1ULL << (kMips64SegBits - 13)) - 1)) ==
          ((faultVa >> 13) & ((1ULL << (kMips64SegBits - 13)) - 1)));
    CHECK(((cpu.cp0XContext() >> 31) & 3) == (faultVa >> 62));
    CHECK((cpu.cp0EntryHi() & kEntryHiRegionMask) ==
          (faultVa & kEntryHiRegionMask));
}

static void testXtlbVectorsAndDelaySlotState() {
    runDelaySlotXtlbFault(true);
    runDelaySlotXtlbFault(false);
}

int main() {
    testUxZeroDataNormalizationDoesNotChangeFetch();
    testXtlbVectorsAndDelaySlotState();
    std::cout << "test_mips64_address_modes: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
