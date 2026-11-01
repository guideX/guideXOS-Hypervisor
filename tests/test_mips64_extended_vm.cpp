// MIPS64-5 production VM scheduler demonstrations for XKPHYS and guest XTLB.
#include <cstdint>
#include <iostream>
#include <vector>

#include "Mips64ISAPlugin.h"
#include "VirtualMachine.h"
#include "mips64_extended_test_support.h"

using namespace ia64;
using namespace mips64;
using namespace mips64_extended_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { if (expr) ++g_pass; else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } } while (0)

static std::vector<uint8_t> encodeWords(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 24));
    }
    return bytes;
}

static Mips64ISAPlugin* plugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

static uint64_t xkphys(uint8_t cca, uint64_t pa) {
    return 0x8000000000000000ULL | (uint64_t(cca & 7U) << 59) | pa;
}

static void testSchedulerExecutesAtExtendedXkphysEntry() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    CHECK(vm.init());
    const std::vector<uint32_t> bootstrap = {
        encI(0x19, 0, 8, static_cast<uint16_t>(kStatusBitKx)),
        encCp0(5, 8, 12), // dmtc0 $t0,Status; KX=1, ERL/BEV clear
    };
    const std::vector<uint8_t> bootstrapBytes = encodeWords(bootstrap);
    CHECK(vm.loadProgram(bootstrapBytes.data(), bootstrapBytes.size(), 0x1000));
    vm.setEntryPoint(kCKSeg0Base | 0x1000);
    CHECK(vm.run(2) == 2);
    Mips64ISAPlugin* mips = plugin(vm);
    CHECK(mips != nullptr);
    if (!mips) return;
    CHECK((mips->core().cp0Status() & kStatusBitKx) != 0);

    constexpr uint64_t codePa = 0x2000;
    constexpr uint64_t dataPa = 0x4000;
    const uint64_t extendedEntry = xkphys(3, codePa);
    std::vector<uint32_t> guest;
    emitLoad64(guest, 8, xkphys(2, dataPa));
    guest.push_back(encI(0x19, 0, 9, 0x7777));
    guest.push_back(encI(0x3F, 8, 9, 0));
    guest.push_back(encI(0x37, 8, 10, 0));
    for (size_t i = 0; i < guest.size(); ++i) {
        storeWord(vm.getMemory(), codePa + i * 4, guest[i], true);
    }

    vm.setEntryPoint(extendedEntry);
    CHECK(vm.getIP() == extendedEntry); // no 32-bit narrowing in the VM path
    CHECK(vm.run(guest.size()) == guest.size());
    CHECK(mips->core().readGpr(10) == 0x7777);
    CHECK(load64(vm.getMemory(), dataPa, true) == 0x7777);
    const Mips64Probe fetch = mips->core().probeAddress(extendedEntry, Mips64Access::Fetch);
    CHECK(fetch.ok && fetch.phys == codePa && fetch.cacheAttribute == 3);
    CHECK(mips->core().probeAddress(xkphys(2, dataPa), Mips64Access::Load).cacheAttribute == 2);
}

static void testSchedulerRunsGuestXtlbHandlerAndEretRetry() {
    VirtualMachine vm(4 * 1024 * 1024, "MIPS64");
    CHECK(vm.init());
    constexpr uint64_t dataVa = 0x0000001200002000ULL;
    constexpr uint64_t dataPa = 0x5000;
    constexpr uint64_t markerPa = 0x3000;
    constexpr uint64_t expected = 0x13579BDF2468ACE0ULL;

    std::vector<uint32_t> main;
    main.push_back(encI(0x19, 0, 8, static_cast<uint16_t>(kStatusBitUx)));
    main.push_back(encCp0(5, 8, 12)); // enable UX, clear reset ERL
    emitLoad64(main, 4, dataVa);
    main.push_back(encI(0x37, 4, 8, 0)); // miss -> XTLB, then ERET retries
    main.push_back((0x0FU << 26) | (6U << 16) | 0x8000U);
    main.push_back(encI(0x0D, 6, 6, 0x3000));
    main.push_back(encI(0x3F, 6, 8, 0)); // publish loaded value to CKSEG0
    const std::vector<uint8_t> mainBytes = encodeWords(main);
    CHECK(vm.loadProgram(mainBytes.data(), mainBytes.size(), 0x1000));
    store64(vm.getMemory(), dataPa, expected, true);

    // BEV=0 XTLB vector is EBase+0x80, a CKSEG0 address at PA 0x80.
    std::vector<uint32_t> handler;
    emitLoad64(handler, 26, dataVa & (kEntryHiRegionMask | kEntryHiVpn2Mask));
    handler.push_back(encCp0(5, 26, 10)); // dmtc0 EntryHi
    handler.push_back(encI(0x19, 0, 26, static_cast<uint16_t>(entryLo(5))));
    handler.push_back(encCp0(5, 26, 2)); // EntryLo0 -> PA 0x5000, V/D/G
    handler.push_back(encI(0x19, 0, 26, static_cast<uint16_t>(entryLo(6))));
    handler.push_back(encCp0(5, 26, 3)); // EntryLo1 -> adjacent page
    handler.push_back(encCp0(5, 0, 5)); // PageMask=0
    handler.push_back(encCp0(5, 0, 0)); // Index=0
    handler.push_back(0x42000002U); // tlbwi
    handler.push_back(0x42000018U); // eret
    for (size_t i = 0; i < handler.size(); ++i) {
        storeWord(vm.getMemory(), 0x80 + i * 4, handler[i], true);
    }

    vm.setEntryPoint(kCKSeg0Base | 0x1000);
    const uint64_t expectedCycles = main.size() + handler.size() + 4;
    CHECK(vm.run(expectedCycles) == expectedCycles);
    Mips64ISAPlugin* mips = plugin(vm);
    CHECK(mips != nullptr);
    if (!mips) return;
    CHECK(mips->core().readGpr(8) == expected);
    CHECK(load64(vm.getMemory(), markerPa, true) == expected);
    CHECK(mips->core().cp0BadVAddr() == dataVa);
    CHECK(mips->core().cp0Epc() == kCKSeg0Base + 0x1000 + 9 * 4);
    CHECK(mips->core().cp0XContext() != 0);
    CHECK(!mips->core().statusExl()); // ERET returned and retried successfully
    Mips64TlbEntry entry;
    CHECK(mips->core().tlbEntry(0, entry) && entry.v0 && entry.d0 && entry.pfn0 == 5);
}

int main() {
    testSchedulerExecutesAtExtendedXkphysEntry();
    testSchedulerRunsGuestXtlbHandlerAndEretRetry();
    std::cout << "test_mips64_extended_vm: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
