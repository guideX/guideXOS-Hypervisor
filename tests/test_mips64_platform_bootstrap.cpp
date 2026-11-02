#include <cstdint>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>

#include "PhysicalAddressMap.h"
#include "memory.h"
#include "mips64_extended_test_support.h"

using namespace ia64;
using namespace mips64;
using namespace mips64_extended_test;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { \
    if (expr) ++g_pass; \
    else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

static std::vector<uint8_t> encodeLittleEndian(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    for (uint32_t word : words) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
    }
    return bytes;
}

static void testSyntheticReadOnlyBootRom() {
    constexpr uint64_t romPa = 0x1FC00000ULL;
    constexpr uint64_t romVa = kCKSeg1Base | (romPa & kDirectMapMask);
    Memory memory(0x4000, false);
    PhysicalAddressMap& map = memory.EnablePhysicalAddressMap();
    const std::vector<uint32_t> romProgram = {
        encI(0x19, 0, 8, 0x1234),       // guest computes a marker
        encI(0x2B, 0, 8, 0x3000),      // and stores it into writable RAM
        encI(0x0F, 0, 9, 0x9FC0),      // address the ROM through CKSEG0
        encI(0x19, 0, 10, 0x0055),
        encI(0x2B, 9, 10, 0),          // rejected ROM write -> DBE
    };
    const std::vector<uint8_t> bytes = encodeLittleEndian(romProgram);
    map.RegisterRom(romPa, bytes);
    CHECK(map.Regions().size() == 2);
    CHECK(map.Regions()[1].type == PhysicalRegionType::Rom);

    Mips64Cpu cpu;
    cpu.setPc(romVa);
    CHECK(cpu.probeAddress(romVa, Mips64Access::Fetch).phys == romPa);
    for (unsigned i = 0; i < 4; ++i) CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    uint8_t marker[4] = {};
    memory.Read(0x3000, marker, sizeof(marker));
    CHECK(marker[0] == 0x34 && marker[1] == 0x12);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(((cpu.cp0Cause() >> 2) & 0x1FULL) == static_cast<uint64_t>(Mips64ExcCode::DBE));
    CHECK(cpu.fault() == Mips64Fault::MemoryFaultStore);
    uint8_t romReadback[4] = {};
    memory.Read(romPa, romReadback, sizeof(romReadback));
    CHECK(std::equal(bytes.begin(), bytes.begin() + 4, std::begin(romReadback)));
}

int main() {
    testSyntheticReadOnlyBootRom();
    std::cout << "test_mips64_platform_bootstrap: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
