#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

static unsigned gPass = 0;
static unsigned gFail = 0;
#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

namespace {

uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

std::vector<uint8_t> encodeWords(const std::vector<uint32_t>& words, bool little) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (uint32_t word : words) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            const unsigned shift = 8 * (little ? byte : 3 - byte);
            bytes.push_back(static_cast<uint8_t>(word >> shift));
        }
    }
    return bytes;
}

uint32_t decodeWord(const uint8_t* bytes, bool little) {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        const unsigned shift = 8 * (little ? byte : 3 - byte);
        value |= uint32_t(bytes[byte]) << shift;
    }
    return value;
}

Mips64ISAPlugin* plugin(VirtualMachine& vm, int index = 0) {
    CPUContext* context = vm.getCPUContext(index);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

MaltaBootstrapConfig configFor(Mips64Endianness endian) {
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.switches = 0x5A;
    config.jumpers = 0x03;
    return config;
}

void read32(PhysicalAddressMap& bus, uint64_t address, uint32_t& value, bool little) {
    uint8_t bytes[4]{};
    bus.Read(address, bytes, sizeof(bytes));
    value = decodeWord(bytes, little);
}

void write32(PhysicalAddressMap& bus, uint64_t address, uint32_t value, bool little) {
    uint8_t bytes[4]{};
    for (unsigned byte = 0; byte < 4; ++byte) {
        const unsigned shift = 8 * (little ? byte : 3 - byte);
        bytes[byte] = static_cast<uint8_t>(value >> shift);
    }
    bus.Write(address, bytes, sizeof(bytes));
}

bool hasRegion(const PhysicalAddressMap& bus, uint64_t base, uint64_t size,
               PhysicalRegionType type) {
    for (const PhysicalRegionInfo& region : bus.Regions()) {
        if (region.base == base && region.size == size && region.type == type) return true;
    }
    return false;
}

void testMapAndArchitecturalReset() {
    VirtualMachine defaultVm(2 * 1024 * 1024, "MIPS64");
    CHECK(dynamic_cast<const Memory*>(&defaultVm.getMemory())->GetPhysicalAddressMap() == nullptr);
    CHECK(defaultVm.init());
    CHECK(plugin(defaultVm) != nullptr);
    CHECK(plugin(defaultVm)->core().pc() == 0);

    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.getMips64MaltaBootstrapBoard() != nullptr);
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    CHECK(memory && memory->GetPhysicalAddressMap() != nullptr);
    if (!memory || !memory->GetPhysicalAddressMap()) return;
    PhysicalAddressMap& bus = *memory->GetPhysicalAddressMap();
    CHECK(hasRegion(bus, 0, 2 * 1024 * 1024, PhysicalRegionType::Ram));
    CHECK(hasRegion(bus, kMaltaFlashBase, kMaltaBootImageWindow, PhysicalRegionType::Rom));
    CHECK(hasRegion(bus, kMaltaFpgaBase, 0x900, PhysicalRegionType::Mmio));
    CHECK(hasRegion(bus, kMaltaUartBase, 0x40, PhysicalRegionType::Mmio));
    CHECK(hasRegion(bus, kMaltaResetRomBase, kMaltaBootImageWindow, PhysicalRegionType::Rom));
    CHECK(vm.init());

    Mips64ISAPlugin* core = plugin(vm);
    CHECK(core != nullptr);
    if (!core) return;
    CHECK(core->core().pc() == kMaltaResetVector);
    CHECK(core->core().cp0Status() == kStatusReset);
    CHECK(core->core().statusBev() && core->core().statusErl());
    CHECK(core->core().statusKsu() == 0);
    CHECK(core->core().cp0Count() == 0 && core->core().cp0Compare() == 0);
    for (unsigned index = 0; index < kTlbEntries; ++index) {
        Mips64TlbEntry entry;
        CHECK(core->core().tlbEntry(index, entry) && !entry.valid0 && !entry.valid1);
    }
    const Mips64Probe resetProbe = core->core().probeAddress(kMaltaResetVector, Mips64Access::Fetch);
    CHECK(resetProbe.ok && resetProbe.phys == kMaltaResetRomBase);
    const auto& image = vm.getMips64MaltaBootstrapBoard()->BootRomImage();
    CHECK(image.size() == kMaltaBootImageWindow);
    uint8_t resetBytes[16]{}, flashBytes[16]{};
    bus.Read(kMaltaResetRomBase, resetBytes, sizeof(resetBytes));
    bus.Read(kMaltaFlashBase, flashBytes, sizeof(flashBytes));
    CHECK(std::equal(std::begin(resetBytes), std::end(resetBytes), std::begin(flashBytes)));
    CHECK(std::equal(std::begin(resetBytes), std::end(resetBytes), image.begin()));

    bool romWriteRejected = false;
    const uint8_t changed = 0;
    try { bus.Write(kMaltaResetRomBase, &changed, 1); }
    catch (const PhysicalBusError& error) {
        romWriteRejected = error.code() == PhysicalBusErrorCode::PermissionDenied;
    }
    CHECK(romWriteRejected);
    CHECK(vm.step()); // Real fetch and execution of the first reset-ROM LUI.
    CHECK(core->core().pc() == kMaltaResetVector + 4);
    CHECK(core->core().readGpr(8) == 0xFFFFFFFFBF000000ULL);

    const Mips64Probe fpgaProbe = core->core().probeAddress(0xFFFFFFFFBF000208ULL,
                                                           Mips64Access::Load);
    const Mips64Probe uartProbe = core->core().probeAddress(0xFFFFFFFFBF000900ULL,
                                                           Mips64Access::Store);
    CHECK(fpgaProbe.ok && fpgaProbe.phys == kMaltaFpgaBase + 0x208);
    CHECK(uartProbe.ok && uartProbe.phys == kMaltaUartBase);
}

void testFpgaRegistersAndUartCoexistence() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
        vm.configureMips64MaltaBootstrap(configFor(endian));
        CHECK(vm.init());
        MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
        auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
        CHECK(board != nullptr && memory != nullptr);
        if (!board || !memory || !memory->GetPhysicalAddressMap()) continue;
        PhysicalAddressMap& bus = *memory->GetPhysicalAddressMap();

        uint32_t value = 0;
        read32(bus, kMaltaFpgaBase + 0x200, value, little);
        CHECK(value == 0x5A);
        read32(bus, kMaltaFpgaBase + 0x208, value, little);
        CHECK(value == (little ? 0x10U : 0x12U));
        read32(bus, kMaltaFpgaBase + 0x210, value, little);
        CHECK(value == 3);
        write32(bus, kMaltaFpgaBase + 0x200, 0xFFFFFFFF, little);
        read32(bus, kMaltaFpgaBase + 0x200, value, little);
        CHECK(value == 0x5A); // Read-only board input.

        write32(bus, kMaltaFpgaBase + 0x408, 0x123456A5, little);
        CHECK(board->Fpga()->LedBar() == 0xA5);
        read32(bus, kMaltaFpgaBase + 0x408, value, little);
        CHECK(value == 0xA5);

        write32(bus, kMaltaFpgaBase + 0x410, 0x41424344, little);
        CHECK(std::string(board->Fpga()->AsciiDisplay().begin(),
                          board->Fpga()->AsciiDisplay().end()) == "41424344");
        constexpr std::string_view display = "MaltaFP!";
        for (size_t index = 0; index < display.size(); ++index) {
            write32(bus, kMaltaFpgaBase + 0x418 + index * 8,
                    static_cast<uint8_t>(display[index]), little);
        }
        CHECK(std::string(board->Fpga()->AsciiDisplay().begin(),
                          board->Fpga()->AsciiDisplay().end()) == display);

        read32(bus, kMaltaFpgaBase + 0x508, value, little);
        CHECK(value == 0x0A);
        write32(bus, kMaltaFpgaBase + 0x508, 0x1234, little);
        CHECK(board->Fpga()->BreakReset() == 0x34);

        write32(bus, kMaltaFpgaBase + 0x500, 0x41, little);
        CHECK(!board->HasSoftwareResetRequest());
        CHECK(board->SoftwareResetRequestCount() == 0);

        bool badWidthRejected = false;
        try { bus.Write(kMaltaFpgaBase + 0x500, reinterpret_cast<const uint8_t*>(&value), 1); }
        catch (const PhysicalBusError& error) {
            badWidthRejected = error.code() == PhysicalBusErrorCode::InvalidWidth;
        }
        CHECK(badWidthRejected && !board->HasSoftwareResetRequest());

        bool unknownRejected = false;
        try { read32(bus, kMaltaFpgaBase + 0x600, value, little); }
        catch (const PhysicalBusError& error) {
            unknownRejected = error.code() == PhysicalBusErrorCode::Unmapped;
        }
        CHECK(unknownRejected);

        bool crossingRejected = false;
        uint8_t crossing[4]{};
        try { bus.Read(kMaltaFpgaBase + 0x8FE, crossing, sizeof(crossing)); }
        catch (const PhysicalBusError& error) {
            crossingRejected = error.code() == PhysicalBusErrorCode::BoundaryCrossing;
        }
        CHECK(crossingRejected);

        const uint8_t uartByte = 'U';
        bus.Write(kMaltaUartBase, &uartByte, 1);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>{'U'});
        CHECK(hasRegion(bus, kMaltaFpgaBase, 0x900, PhysicalRegionType::Mmio));
        CHECK(hasRegion(bus, kMaltaUartBase, 0x40, PhysicalRegionType::Mmio));
    }
}

void testSafeGuestSoftwareResetAndColdReset() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.init());
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    Mips64ISAPlugin* core = plugin(vm);
    CHECK(board != nullptr && core != nullptr);
    if (!board || !core) return;

    CHECK(vm.coldResetMips64Malta());
    CHECK(core->core().pc() == kMaltaResetVector);
    CHECK(core->core().cp0Status() == kStatusReset);
    CHECK(board->Fpga()->LedBar() == 0 && board->Fpga()->BreakReset() == 0x0A);
    CHECK(board->Fpga()->AsciiDisplay() == std::array<char, 8>{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}});
    CHECK(board->ConsoleSink()->Bytes().empty());

    // The guest performs one SW to SOFTRES; the device only records the
    // request. The VM applies reset after that instruction retires.
    const std::vector<uint32_t> resetProgram{
        encI(0x0F, 0, 8, 0xBF00),
        encI(0x0D, 8, 8, 0x0500),
        encI(0x09, 0, 9, 0x42),
        encI(0x2B, 8, 9, 0),
    };
    const auto resetImage = encodeWords(resetProgram, true);
    CHECK(vm.loadProgram(resetImage.data(), resetImage.size(), 0x1000));
    core->core().setPc(0xFFFFFFFF80001000ULL);
    CHECK(vm.run(4) == 4);
    CHECK(board->SoftwareResetRequestCount() == 1);
    CHECK(!board->HasSoftwareResetRequest());
    CHECK(vm.getIP() == kMaltaResetVector);
    CHECK(core->core().cp0Status() == kStatusReset);
    CHECK(core->core().cp0Count() == 0 && core->core().cp0Compare() == 0);
    CHECK(board->ConsoleSink()->Bytes().empty());

    // A bounded second run starts fetching the ROM again, proving reset was
    // deferred to a VM boundary and re-entered the reset mapping.
    CHECK(vm.run(8) == 8);
    CHECK(core->core().pc() == kMaltaResetVector + 32);
}

void testColdBootGuestAndPayloadBothEndians() {
    const std::string expected =
        "guideXOS MIPS64 Malta bootstrap OKMIPS64 RAM payload running";
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
        vm.configureMips64MaltaBootstrap(configFor(endian));
        CHECK(vm.init());
        MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
        Mips64ISAPlugin* core = plugin(vm);
        CHECK(board != nullptr && core != nullptr);
        if (!board || !core) continue;
        const auto payload = MaltaBootstrapBoard::BuildDemoRamPayload(endian);
        CHECK(vm.loadProgram(payload.data(), payload.size(),
                             kMaltaRamPayloadPhysicalAddress));
        CHECK(core->core().pc() == kMaltaResetVector);
        CHECK(core->core().probeAddress(kMaltaResetVector, Mips64Access::Fetch).phys ==
              kMaltaResetRomBase);
        CHECK(core->core().probeAddress(kMaltaRamPayloadVirtualAddress,
                                        Mips64Access::Fetch).phys ==
              kMaltaRamPayloadPhysicalAddress);
        CHECK(core->core().probeAddress(0xFFFFFFFFBF000408ULL,
                                        Mips64Access::Store).phys == kMaltaFpgaBase + 0x408);
        CHECK(core->core().probeAddress(0xFFFFFFFFBF000900ULL,
                                        Mips64Access::Store).phys == kMaltaUartBase);
        CHECK(core->isLittleEndian() == little);
        CHECK(board->ConsoleSink()->Bytes().empty());

        CHECK(vm.run(512) == 512);
        const auto& outputBytes = board->ConsoleSink()->Bytes();
        CHECK(std::string(outputBytes.begin(), outputBytes.end()) == expected);
        CHECK(core->core().readGpr(16) == (little ? 0x10U : 0x12U));
        CHECK(board->Fpga()->LedBar() == 0xA5);
        CHECK(std::string(board->Fpga()->AsciiDisplay().begin(),
                          board->Fpga()->AsciiDisplay().end()) == "guideXOS");
        const auto uartState = board->Uart()->InspectState();
        CHECK(uartState.at("LCR") == 3);
        CHECK(uartState.at("DLL") == 1 && uartState.at("DLM") == 0);

        uint8_t completion[4]{};
        vm.getMemory().Read(0x2000, completion, sizeof(completion));
        CHECK(decodeWord(completion, little) == 0x47584F53);
        const Mips64Probe finalPc = core->core().probeAddress(core->core().pc(),
                                                              Mips64Access::Fetch);
        CHECK(finalPc.ok && finalPc.phys >= kMaltaRamPayloadPhysicalAddress &&
              finalPc.phys < kMaltaRamPayloadPhysicalAddress + payload.size());
    }
}

void testTwoVmsAndTwoCpusAreBoardScoped() {
    VirtualMachine first(2 * 1024 * 1024, "MIPS64");
    VirtualMachine second(2 * 1024 * 1024, "MIPS64");
    first.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    second.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(first.init() && second.init());
    MaltaBootstrapBoard* firstBoard = first.getMips64MaltaBootstrapBoard();
    MaltaBootstrapBoard* secondBoard = second.getMips64MaltaBootstrapBoard();
    CHECK(firstBoard && secondBoard && firstBoard != secondBoard);
    auto* firstMemory = dynamic_cast<Memory*>(&first.getMemory());
    auto* secondMemory = dynamic_cast<Memory*>(&second.getMemory());
    CHECK(firstMemory && secondMemory);
    if (!firstBoard || !secondBoard || !firstMemory || !secondMemory) return;
    uint8_t led = 0x11;
    firstMemory->GetPhysicalAddressMap()->Write(kMaltaFpgaBase + 0x408, &led, 4);
    CHECK(firstBoard->Fpga()->LedBar() == 0x11);
    CHECK(secondBoard->Fpga()->LedBar() == 0);

    VirtualMachine shared(2 * 1024 * 1024, 2, "MIPS64");
    shared.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(shared.init());
    CHECK(plugin(shared, 0) && plugin(shared, 1));
    CHECK(plugin(shared, 0)->core().pc() == kMaltaResetVector);
    CHECK(plugin(shared, 1)->core().pc() == kMaltaResetVector);
    CHECK(shared.getMips64MaltaBootstrapBoard() != nullptr);
    auto* sharedMemory = dynamic_cast<Memory*>(&shared.getMemory());
    CHECK(sharedMemory && sharedMemory->GetPhysicalAddressMap() != nullptr);
    if (sharedMemory && sharedMemory->GetPhysicalAddressMap()) {
        const uint8_t uartByte = 'C';
        sharedMemory->GetPhysicalAddressMap()->Write(kMaltaUartBase, &uartByte, 1);
        CHECK(shared.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
              std::vector<uint8_t>{'C'});
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_mips64_malta <map|fpga|reset|boot|ownership>\n";
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "map") testMapAndArchitecturalReset();
    else if (mode == "fpga") testFpgaRegistersAndUartCoexistence();
    else if (mode == "reset") testSafeGuestSoftwareResetAndColdReset();
    else if (mode == "boot") testColdBootGuestAndPayloadBothEndians();
    else if (mode == "ownership") testTwoVmsAndTwoCpusAreBoardScoped();
    else return 2;
    std::cout << "test_mips64_malta[" << mode << "]: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
