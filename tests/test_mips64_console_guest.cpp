#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Mips64ISAPlugin.h"
#include "Ns16550Uart.h"
#include "VirtualMachine.h"
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

static std::vector<uint8_t> encodeWords(const std::vector<uint32_t>& words,
                                       bool littleEndian) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (uint32_t word : words) {
        for (unsigned i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<uint8_t>(word >> (8 * (littleEndian ? i : 3 - i))));
        }
    }
    return bytes;
}

static Mips64ISAPlugin* getPlugin(VirtualMachine& vm, int index = 0) {
    CPUContext* context = vm.getCPUContext(index);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

static void patchBranch(std::vector<uint32_t>& words, size_t branch,
                        size_t target, uint8_t rs, uint8_t rt) {
    const int64_t offset = static_cast<int64_t>(target) - static_cast<int64_t>(branch + 1);
    words[branch] = encI(0x04, rs, rt, static_cast<uint16_t>(offset));
}

static std::vector<uint32_t> guestConsoleProgram() {
    constexpr uint8_t t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13;
    std::vector<uint32_t> words = {
        encI(0x0F, 0, t0, 0xBF00),                 // lui $t0, 0xBF00 -> CKSEG1 base
        encI(0x0D, t0, t0, 0x0900),                // ori $t0, $t0, 0x0900
        encI(0x0F, 0, t3, 0x8000),                 // CKSEG0 message pointer
        encI(0x0D, t3, t3, 0x2000),
    };
    const size_t sendLoop = words.size();
    words.push_back(encI(0x24, t3, t1, 0));        // lbu $t1, 0($t3)
    const size_t doneBranch = words.size();
    words.push_back(0);                            // beq $t1, $zero, done
    words.push_back(0);                            // branch delay slot
    const size_t pollLoop = words.size();
    words.push_back(encI(0x24, t0, t2, 40));       // lbu $t2, LSR($t0)
    words.push_back(encI(0x0C, t2, t2, 0x20));     // andi $t2, $t2, THRE
    const size_t pollBranch = words.size();
    words.push_back(0);                            // beq $t2, $zero, poll
    words.push_back(0);
    words.push_back(encI(0x28, t0, t1, 0));        // sb $t1, THR($t0)
    words.push_back(encI(0x19, t3, t3, 1));        // advance message pointer
    const size_t sendBranch = words.size();
    words.push_back(0);                            // b sendLoop
    words.push_back(0);

    const size_t done = words.size();
    words.push_back(encI(0x0F, 0, t4, 0x8000));
    words.push_back(encI(0x0D, t4, t4, 0x3000));
    words.push_back(encI(0x19, 0, t5, 0x1234));
    words.push_back(encI(0x2B, t4, t5, 0));        // publish completion marker
    const size_t haltLoop = words.size();
    words.push_back(0);                            // deterministic terminal loop
    words.push_back(0);

    patchBranch(words, doneBranch, done, t1, 0);
    patchBranch(words, pollBranch, pollLoop, t2, 0);
    patchBranch(words, sendBranch, sendLoop, 0, 0);
    patchBranch(words, haltLoop, haltLoop, 0, 0);
    return words;
}

static std::vector<uint32_t> oneByteWriter(uint8_t byte) {
    constexpr uint8_t t0 = 8, t1 = 9;
    std::vector<uint32_t> words = {
        encI(0x0F, 0, t0, 0xBF00),
        encI(0x0D, t0, t0, 0x0900),
        encI(0x19, 0, t1, byte),
        encI(0x28, t0, t1, 0),
    };
    const size_t loop = words.size();
    words.push_back(0);
    words.push_back(0);
    patchBranch(words, loop, loop, 0, 0);
    return words;
}

static std::vector<uint8_t> readConsoleString() {
    const std::string message = "Hello from guideXOS MIPS64!";
    return std::vector<uint8_t>(message.begin(), message.end());
}

static void testProductionVmGuestPrintsThroughCkseg1BothEndians() {
    const std::string expected = "Hello from guideXOS MIPS64!";
    const Ns16550Mapping mapping = Ns16550Mapping::MaltaOriented();
    VirtualMachine flatVm(1024 * 1024, "MIPS64");
    auto* flatMemory = dynamic_cast<Memory*>(&flatVm.getMemory());
    CHECK(flatMemory != nullptr && flatMemory->GetPhysicalAddressMap() == nullptr);
    VirtualMachine ia64Vm(1024 * 1024, "IA-64");
    bool unrelatedVmRejected = false;
    try { (void)ia64Vm.enableMips64PhysicalAddressMap(); }
    catch (const std::logic_error&) { unrelatedVmRejected = true; }
    CHECK(unrelatedVmRejected);
    CHECK(dynamic_cast<Memory*>(&ia64Vm.getMemory())->GetPhysicalAddressMap() == nullptr);

    for (bool littleEndian : {true, false}) {
        VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
        Mips64ISAPlugin* plugin = getPlugin(vm);
        CHECK(plugin != nullptr);
        if (!plugin) continue;
        plugin->configureEndianness(littleEndian);
        PhysicalAddressMap& bus = vm.enableMips64PhysicalAddressMap();
        const size_t originalRegions = bus.Regions().size();
        auto rejectedSink = std::make_shared<GuestConsoleSink>(8);
        auto rejectedDevice = std::make_shared<Ns16550Uart>(rejectedSink, mapping.registerShift);
        std::weak_ptr<Ns16550Uart> rejectedWeak = rejectedDevice;
        bool overlapRejected = false;
        try { bus.RegisterMmio(0x1000, mapping.RegionSize(), rejectedDevice, {1}); }
        catch (const PhysicalBusError& error) {
            overlapRejected = error.code() == PhysicalBusErrorCode::Overlap;
        }
        rejectedDevice.reset();
        CHECK(overlapRejected && bus.Regions().size() == originalRegions);
        CHECK(rejectedWeak.expired());
        auto sink = std::make_shared<GuestConsoleSink>(128);
        auto uart = std::make_shared<Ns16550Uart>(sink, mapping.registerShift);
        bus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(), uart, {1});
        CHECK(vm.init());

        const std::vector<uint32_t> words = guestConsoleProgram();
        const std::vector<uint8_t> code = encodeWords(words, littleEndian);
        const std::vector<uint8_t> message = readConsoleString();
        std::vector<uint8_t> guestMessage = message;
        guestMessage.push_back(0);
        CHECK(vm.loadProgram(code.data(), code.size(), 0x1000));
        CHECK(vm.loadProgram(guestMessage.data(), guestMessage.size(), 0x2000));
        vm.setEntryPoint(kCKSeg0Base | 0x1000);
        CHECK(vm.getIP() == (kCKSeg0Base | 0x1000));
        const uint64_t budget = 1024;
        CHECK(vm.run(budget) == budget);

        std::vector<uint8_t> expectedBytes(expected.begin(), expected.end());
        CHECK(sink->Bytes() == expectedBytes);
        CHECK(plugin->core().isLittleEndian() == littleEndian);
        const uint64_t uartVa = 0xFFFFFFFFBF000900ULL;
        const Mips64Probe translated = plugin->core().probeAddress(uartVa, Mips64Access::Store);
        CHECK(translated.ok && translated.phys == mapping.physicalBase);
        uint8_t marker[4] = {};
        vm.getMemory().Read(0x3000, marker, sizeof(marker));
        const uint32_t markerValue = littleEndian
            ? uint32_t(marker[0]) | (uint32_t(marker[1]) << 8) |
                (uint32_t(marker[2]) << 16) | (uint32_t(marker[3]) << 24)
            : (uint32_t(marker[0]) << 24) | (uint32_t(marker[1]) << 16) |
                (uint32_t(marker[2]) << 8) | uint32_t(marker[3]);
        CHECK(markerValue == 0x1234);
        std::cout << "GUEST UART OUTPUT (" << (littleEndian ? "little" : "big")
                  << " endian): " << std::string(sink->Bytes().begin(), sink->Bytes().end()) << "\n";

        vm.halt();
        CHECK(sink->Bytes() == expectedBytes);  // Stop preserves device output.
        plugin->reset();
        CHECK(sink->Bytes() == expectedBytes);  // CPU reset does not reset devices.
        CHECK(vm.reset());                      // VM reset resets owned MMIO devices.
        CHECK(sink->Bytes().empty());
    }
}

static void testTwoVmsAreIsolatedAndCpusShareOneBus() {
    const Ns16550Mapping mapping = Ns16550Mapping::MaltaOriented();
    VirtualMachine first(1024 * 1024, "MIPS64");
    VirtualMachine second(1024 * 1024, "MIPS64");
    PhysicalAddressMap& firstBus = first.enableMips64PhysicalAddressMap();
    PhysicalAddressMap& secondBus = second.enableMips64PhysicalAddressMap();
    auto firstSink = std::make_shared<GuestConsoleSink>(16);
    auto secondSink = std::make_shared<GuestConsoleSink>(16);
    firstBus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(),
                          std::make_shared<Ns16550Uart>(firstSink, mapping.registerShift), {1});
    secondBus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(),
                           std::make_shared<Ns16550Uart>(secondSink, mapping.registerShift), {1});
    CHECK(first.init() && second.init());
    const auto firstCode = encodeWords(oneByteWriter('A'), true);
    const auto secondCode = encodeWords(oneByteWriter('B'), true);
    CHECK(first.loadProgram(firstCode.data(), firstCode.size(), 0x1000));
    CHECK(second.loadProgram(secondCode.data(), secondCode.size(), 0x1000));
    getPlugin(first)->core().setPc(kCKSeg0Base | 0x1000);
    getPlugin(second)->core().setPc(kCKSeg0Base | 0x1000);
    CHECK(first.run(12) == 12 && second.run(12) == 12);
    CHECK(firstSink->Bytes() == std::vector<uint8_t>({'A'}));
    CHECK(secondSink->Bytes() == std::vector<uint8_t>({'B'}));

    VirtualMachine shared(1024 * 1024, 2, "MIPS64");
    PhysicalAddressMap& sharedBus = shared.enableMips64PhysicalAddressMap();
    auto sharedSink = std::make_shared<GuestConsoleSink>(16);
    sharedBus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(),
                           std::make_shared<Ns16550Uart>(sharedSink, mapping.registerShift), {1});
    CHECK(shared.init());
    const auto cpu0Code = encodeWords(oneByteWriter('X'), true);
    const auto cpu1Code = encodeWords(oneByteWriter('Y'), true);
    CHECK(shared.loadProgram(cpu0Code.data(), cpu0Code.size(), 0x1000));
    CHECK(shared.loadProgram(cpu1Code.data(), cpu1Code.size(), 0x1100));
    CPUContext* cpu0 = shared.getCPUContext(0);
    CPUContext* cpu1 = shared.getCPUContext(1);
    CHECK(cpu0 && cpu1);
    if (cpu0 && cpu1) {
        getPlugin(shared, 0)->core().setPc(kCKSeg0Base | 0x1000);
        getPlugin(shared, 1)->core().setPc(kCKSeg0Base | 0x1100);
        cpu1->state = CPUExecutionState::RUNNING;
        cpu1->enabled = true;
        CHECK(shared.run(20) == 20);
    }
    CHECK(sharedSink->Bytes().size() == 2);
    CHECK(std::find(sharedSink->Bytes().begin(), sharedSink->Bytes().end(), 'X') !=
          sharedSink->Bytes().end());
    CHECK(std::find(sharedSink->Bytes().begin(), sharedSink->Bytes().end(), 'Y') !=
          sharedSink->Bytes().end());
}

static void testVmDestructionReleasesMappedDeviceOwnership() {
    std::weak_ptr<Ns16550Uart> weakDevice;
    {
        VirtualMachine vm(64 * 1024, "MIPS64");
        PhysicalAddressMap& bus = vm.enableMips64PhysicalAddressMap();
        auto sink = std::make_shared<GuestConsoleSink>(8);
        auto device = std::make_shared<Ns16550Uart>(sink);
        weakDevice = device;
        bus.RegisterMmio(0x20000, device->RegionSize(), device, {1});
        device.reset();
        CHECK(!weakDevice.expired());
    }
    CHECK(weakDevice.expired());
}

int main() {
    testProductionVmGuestPrintsThroughCkseg1BothEndians();
    testTwoVmsAreIsolatedAndCpusShareOneBus();
    testVmDestructionReleasesMappedDeviceOwnership();
    std::cout << "test_mips64_console_guest: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
