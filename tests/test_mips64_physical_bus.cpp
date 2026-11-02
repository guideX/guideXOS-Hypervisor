#include <cstdint>
#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
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

class CountingDevice final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 1; }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override {
        if (size != 1 || offset >= values.size()) {
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped, "invalid counting-device read");
        }
        *destination = values[static_cast<size_t>(offset)];
    }
    void Write(uint64_t offset, const uint8_t* source, size_t size) override {
        if (size != 1 || offset >= values.size()) {
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped, "invalid counting-device write");
        }
        ++writeCount;
        values[static_cast<size_t>(offset)] = source[0];
    }
    void Reset() override { values.fill(0); writeCount = 0; }
    std::map<std::string, uint64_t> InspectState() const override {
        return {{"writes", writeCount}};
    }

    std::array<uint8_t, 8> values{};
    uint64_t writeCount = 0;
};

template <typename Fn>
static PhysicalBusErrorCode expectBusError(Fn&& operation) {
    try {
        operation();
    } catch (const PhysicalBusError& error) {
        return error.code();
    }
    return PhysicalBusErrorCode::DeviceFailure;
}

static void testRegionRegistrationAndTransactionRules() {
    PhysicalAddressMap map;
    map.RegisterRam(0, 0x1000);
    map.RegisterRom(0x2000, {0x12, 0x34, 0x56, 0x78});
    auto device = std::make_shared<CountingDevice>();
    std::weak_ptr<CountingDevice> weakDevice = device;
    map.RegisterMmio(0x3000, 8, device, {1});
    device.reset();
    CHECK(!weakDevice.expired());  // Registry owns the target for the mapping lifetime.
    CHECK(map.Regions().size() == 3);

    uint8_t word[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    map.Write(0x100, word, sizeof(word));
    uint8_t readback[4] = {};
    map.Read(0x100, readback, sizeof(readback));
    CHECK(std::equal(std::begin(word), std::end(word), std::begin(readback)));
    map.Read(0x2000, readback, sizeof(readback));
    CHECK(readback[0] == 0x12 && readback[3] == 0x78);
    CHECK(expectBusError([&] { map.Write(0x2000, word, 1); }) ==
          PhysicalBusErrorCode::PermissionDenied);

    CHECK(expectBusError([&] { map.RegisterRam(0x3004, 0x10); }) ==
          PhysicalBusErrorCode::Overlap);
    CHECK(map.Regions().size() == 3);  // Rejected registration was not published.
    CHECK(expectBusError([&] { map.RegisterRam(0x4000, 0); }) ==
          PhysicalBusErrorCode::InvalidRegion);
    CHECK(expectBusError([&] {
        map.RegisterSparseRam(UINT64_MAX - 3, 8, 4);
    }) == PhysicalBusErrorCode::AddressOverflow);

    CHECK(expectBusError([&] { map.Read(0x1000, readback, 1); }) ==
          PhysicalBusErrorCode::Unmapped);
    CHECK(expectBusError([&] { map.Read(UINT64_MAX - 1, readback, 4); }) ==
          PhysicalBusErrorCode::AddressOverflow);
    CHECK(expectBusError([&] { map.Read(0x0FFF, readback, 2); }) ==
          PhysicalBusErrorCode::BoundaryCrossing);

    const uint8_t byte = 0x5A;
    map.Write(0x3000, &byte, 1);
    auto state = weakDevice.lock();
    CHECK(state && state->writeCount == 1 && state->values[0] == byte);
    CHECK(expectBusError([&] { map.Write(0x3000, word, 2); }) ==
          PhysicalBusErrorCode::InvalidWidth);
    CHECK(expectBusError([&] { map.Write(0x3007, word, 2); }) ==
          PhysicalBusErrorCode::BoundaryCrossing);
    CHECK(state->writeCount == 1);  // Invalid width/boundary had no device side effect.
    map.ResetDevices();
    CHECK(state->writeCount == 0 && state->values[0] == 0);
}

static void testHighPhysicalRangeAndStrictMemoryOptIn() {
    PhysicalAddressMap map;
    map.RegisterSparseRam(0x100000000ULL, 0x2000, 32);
    map.RegisterSparseRam(kPhysicalAddressLimit - 0x1000, 0x1000, 16);
    uint8_t byte = 0xE7;
    uint8_t result = 0;
    map.Write(0x100000123ULL, &byte, 1);
    map.Read(0x100000123ULL, &result, 1);
    CHECK(result == byte);
    result = 0xFF;
    map.Read(0x100000124ULL, &result, 1);
    CHECK(result == 0);  // Unwritten sparse RAM is zero-initialized RAM.
    map.Write(kPhysicalAddressMaximum, &byte, 1);
    map.Read(kPhysicalAddressMaximum, &result, 1);
    CHECK(result == byte);
    CHECK(expectBusError([&] { map.Read(kPhysicalAddressLimit, &result, 1); }) ==
          PhysicalBusErrorCode::AddressOverflow);
    CHECK(expectBusError([&] { map.Read(0x200000000ULL, &result, 1); }) ==
          PhysicalBusErrorCode::Unmapped);

    Memory legacy(0x1000, false);
    legacy.Write(0x800, &byte, 1);
    legacy.Read(0x800, &result, 1);
    CHECK(result == byte);  // Flat-memory behavior remains the default.

    Memory strict(0x1000, false);
    PhysicalAddressMap& strictMap = strict.EnablePhysicalAddressMap();
    CHECK(strictMap.Regions().size() == 1);
    strict.Write(0x800, &byte, 1);
    strict.Read(0x800, &result, 1);
    CHECK(result == byte);
    CHECK(expectBusError([&] { strict.Read(0x1000, &result, 1); }) ==
          PhysicalBusErrorCode::Unmapped);
}

static uint64_t xkphys(uint8_t cca, uint64_t pa) {
    return 0x8000000000000000ULL | (uint64_t(cca & 7U) << 59) | pa;
}

static void testXkphysReachesMappedHighRamBothEndians() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        Memory memory(0x10000, false);
        PhysicalAddressMap& bus = memory.EnablePhysicalAddressMap();
        constexpr uint64_t codePa = 0x100000000ULL;
        constexpr uint64_t dataPa = 0x100001000ULL;
        bus.RegisterSparseRam(codePa, 0x2000, 0x2000);
        Mips64Cpu cpu(endian);
        std::vector<uint32_t> program;
        emitLoad64(program, 8, xkphys(2, dataPa));
        program.push_back(encI(0x19, 0, 9, 0x6B6B));
        program.push_back(encI(0x3F, 8, 9, 0));
        program.push_back(encI(0x37, 8, 10, 0));
        for (size_t i = 0; i < program.size(); ++i) {
            storeWord(memory, codePa + i * 4, program[i], endian == Mips64Endianness::Little);
        }
        CHECK(writeStatus(cpu, memory, kStatusBitBev | kStatusBitErl | kStatusBitKx));
        const uint64_t codeVa = xkphys(3, codePa);
        CHECK(cpu.probeAddress(codeVa, Mips64Access::Fetch).phys == codePa);
        cpu.setPc(codeVa);
        bool ran = true;
        for (size_t i = 0; i < program.size(); ++i) {
            if (cpu.step(memory) != Mips64StepResult::Continue) ran = false;
        }
        CHECK(ran);
        CHECK(cpu.readGpr(10) == 0x6B6B);
        uint8_t bytes[8] = {};
        memory.Read(dataPa, bytes, sizeof(bytes));
        CHECK(bytes[0] == (endian == Mips64Endianness::Little ? 0x6B : 0x00));
        CHECK(bytes[1] == (endian == Mips64Endianness::Little ? 0x6B : 0x00));
    }
}

static void testBusFaultsRemainDistinctFromAddressErrors() {
    Memory fetchMemory(0x1000, false);
    fetchMemory.EnablePhysicalAddressMap();
    Mips64Cpu fetchCpu;
    fetchCpu.setPc(kCKSeg0Base | 0x1000);
    CHECK(fetchCpu.step(fetchMemory) == Mips64StepResult::Fault);
    CHECK(((fetchCpu.cp0Cause() >> 2) & 0x1FULL) == static_cast<uint64_t>(Mips64ExcCode::IBE));
    CHECK(fetchCpu.fault() == Mips64Fault::MemoryFaultFetch);

    Memory dataMemory(0x1000, false);
    dataMemory.EnablePhysicalAddressMap();
    storeWord(dataMemory, 0x100, encI(0x0F, 0, 8, 1), true);  // $t0 = 0x10000, unmapped
    storeWord(dataMemory, 0x104, encI(0x23, 8, 9, 0), true);  // lw $t1, 0($t0)
    Mips64Cpu dataCpu;
    dataCpu.setPc(kCKSeg0Base | 0x100);
    CHECK(dataCpu.step(dataMemory) == Mips64StepResult::Continue);
    CHECK(dataCpu.step(dataMemory) == Mips64StepResult::Fault);
    CHECK(((dataCpu.cp0Cause() >> 2) & 0x1FULL) == static_cast<uint64_t>(Mips64ExcCode::DBE));
    CHECK(dataCpu.fault() == Mips64Fault::MemoryFaultLoad);

    Memory addressMemory(0x1000, false);
    addressMemory.EnablePhysicalAddressMap();
    storeWord(addressMemory, 0x100, encI(0x0D, 0, 8, 0x0101), true);
    storeWord(addressMemory, 0x104, encI(0x23, 8, 9, 0), true);  // unaligned lw
    Mips64Cpu addressCpu;
    addressCpu.setPc(kCKSeg0Base | 0x100);
    CHECK(addressCpu.step(addressMemory) == Mips64StepResult::Continue);
    CHECK(addressCpu.step(addressMemory) == Mips64StepResult::Fault);
    CHECK(((addressCpu.cp0Cause() >> 2) & 0x1FULL) == static_cast<uint64_t>(Mips64ExcCode::AdEL));
    CHECK(addressCpu.cp0BadVAddr() == 0x0101);
    CHECK(addressCpu.fault() == Mips64Fault::MisalignedLoad);
}

int main() {
    testRegionRegistrationAndTransactionRules();
    testHighPhysicalRangeAndStrictMemoryOptIn();
    testXkphysReachesMappedHighRamBothEndians();
    testBusFaultsRemainDistinctFromAddressErrors();
    std::cout << "test_mips64_physical_bus: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
