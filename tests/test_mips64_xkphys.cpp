// MIPS64-5 direct physical mapping tests with bounded sparse backing memory.
#include <cstdint>
#include <iostream>
#include <vector>

#include "mips64_extended_test_support.h"

using namespace mips64;
using namespace mips64_extended_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { if (expr) ++g_pass; else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } } while (0)

static uint64_t xkphys(uint8_t cca, uint64_t pa) {
    return 0x8000000000000000ULL | (uint64_t(cca & 7U) << 59) | pa;
}

static void testDirectMapWidthsAndReservedBits() {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu;
    CHECK(writeStatus(cpu, mem, kStatusBitBev | kStatusBitErl | kStatusBitKx));
    const Mips64Probe low = cpu.probeAddress(xkphys(2, 0), Mips64Access::Load);
    CHECK(low.ok && low.phys == 0 && low.cacheAttribute == 2);
    const Mips64Probe high = cpu.probeAddress(xkphys(3, 0x100000000ULL),
                                               Mips64Access::Load);
    CHECK(high.ok && high.phys == 0x100000000ULL && high.cacheAttribute == 3);
    const Mips64Probe maximum = cpu.probeAddress(xkphys(2, kPhysicalAddressMask),
                                                  Mips64Access::Load);
    CHECK(maximum.ok && maximum.phys == kPhysicalAddressMask);
    for (uint8_t cca : {0, 1, 4, 5, 6, 7}) {
        const Mips64Probe invalidCca = cpu.probeAddress(xkphys(cca, 0x1000),
                                                         Mips64Access::Load);
        CHECK(!invalidCca.ok && invalidCca.fault == Mips64Fault::AddressErrorLoad);
    }
    const uint64_t reserved = xkphys(2, (1ULL << 40) | 0x1000ULL);
    CHECK(cpu.probeAddress(reserved, Mips64Access::Load).fault ==
          Mips64Fault::AddressErrorLoad);
    CHECK(cpu.probeAddress(0xBFFFFFFFFFFFFFFFULL, Mips64Access::Load).fault ==
          Mips64Fault::AddressErrorLoad);
    CHECK(cpu.probeAddress(xkphys(2, 0x1000), Mips64Access::Store).ok);
    CHECK(cpu.probeAddress(xkphys(2, 0x1002), Mips64Access::Fetch).ok);

    Mips64Cpu disabled;
    CHECK(writeStatus(disabled, mem, kStatusBitBev | kStatusBitErl));
    CHECK(disabled.probeAddress(xkphys(2, 0), Mips64Access::Load).fault ==
          Mips64Fault::AddressErrorLoad);
}

static void runXkphysProgram(Mips64Endianness endian) {
    SparsePhysicalMemory mem;
    Mips64Cpu cpu(endian);
    constexpr uint64_t codePa = 0x100000000ULL;
    constexpr uint64_t dataPa = 0x100001000ULL;
    const uint64_t codeVa = xkphys(3, codePa);
    const uint64_t dataVa = xkphys(2, dataPa);

    std::vector<uint32_t> program;
    emitLoad64(program, 8, dataVa);
    program.push_back(encI(0x19, 0, 9, 0x5A5A));
    program.push_back(encI(0x3F, 8, 9, 0));
    program.push_back(encI(0x37, 8, 10, 0));
    constexpr uint64_t topAlignedPa = kPhysicalAddressMask & ~7ULL;
    emitLoad64(program, 11, xkphys(2, topAlignedPa));
    program.push_back(encI(0x19, 0, 12, 0x6B6B));
    program.push_back(encI(0x3F, 11, 12, 0));
    program.push_back(encI(0x37, 11, 13, 0));
    for (size_t i = 0; i < program.size(); ++i) {
        storeWord(mem, codePa + i * 4, program[i], endian == Mips64Endianness::Little);
    }

    cpu.setPc(codeVa);
    CHECK(writeStatus(cpu, mem, kStatusBitBev | kStatusBitErl | kStatusBitKx, 0x6C00));
    const Mips64Probe fetch = cpu.probeAddress(codeVa, Mips64Access::Fetch);
    const Mips64Probe data = cpu.probeAddress(dataVa, Mips64Access::Load);
    CHECK(fetch.ok && fetch.phys == codePa && fetch.cacheAttribute == 3);
    CHECK(data.ok && data.phys == dataPa && data.cacheAttribute == 2);
    for (size_t i = 0; i < program.size(); ++i) {
        CHECK(cpu.step(mem) == Mips64StepResult::Continue);
    }
    CHECK(cpu.readGpr(10) == 0x5A5A);
    CHECK(load64(mem, dataPa, endian == Mips64Endianness::Little) == 0x5A5A);
    CHECK(cpu.readGpr(13) == 0x6B6B);
    CHECK(load64(mem, topAlignedPa, endian == Mips64Endianness::Little) == 0x6B6B);

    // The XKPHYS region is kernel-only even when KX remains enabled.
    cpu.setPc(codeVa);
    CHECK(writeStatus(cpu, mem, kStatusBitBev | kStatusBitKx |
                                (2ULL << kStatusKsuShift), 0x6C40));
    CHECK(cpu.step(mem) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::AddressErrorFetch);
    CHECK(cpu.cp0BadVAddr() == codeVa);
}

static void testEncodedHighPhysicalExecutionBothEndians() {
    runXkphysProgram(Mips64Endianness::Little);
    runXkphysProgram(Mips64Endianness::Big);
}

int main() {
    testDirectMapWidthsAndReservedBits();
    testEncodedHighPhysicalExecutionBothEndians();
    std::cout << "test_mips64_xkphys: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
