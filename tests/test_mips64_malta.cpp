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
#include <string_view>
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

    VirtualMachine failedVm(2 * 1024 * 1024, "MIPS64");
    MaltaBootstrapConfig missingRom;
    missingRom.useBuiltInDemoRom = false;
    bool missingRomRejected = false;
    try { failedVm.configureMips64MaltaBootstrap(missingRom); }
    catch (const std::invalid_argument&) { missingRomRejected = true; }
    auto* failedMemory = dynamic_cast<Memory*>(&failedVm.getMemory());
    CHECK(missingRomRejected);
    CHECK(failedMemory != nullptr && failedMemory->GetPhysicalAddressMap() == nullptr);

    VirtualMachine conflictingVm(2 * 1024 * 1024, "MIPS64");
    PhysicalAddressMap& conflictingBus = conflictingVm.enableMips64PhysicalAddressMap();
    conflictingBus.RegisterRam(kMaltaFpgaBase + 0x400, 4, {1, 2, 4});
    const size_t regionsBeforeConflict = conflictingBus.Regions().size();
    bool overlapRejected = false;
    try { conflictingVm.configureMips64MaltaBootstrap(); }
    catch (const PhysicalBusError& error) {
        overlapRejected = error.code() == PhysicalBusErrorCode::Overlap;
    }
    CHECK(overlapRejected);
    CHECK(conflictingBus.Regions().size() == regionsBeforeConflict);

    VirtualMachine invalidRomVm(2 * 1024 * 1024, "MIPS64");
    MaltaBootstrapConfig invalidRom;
    invalidRom.endianness = Mips64Endianness::Big;
    invalidRom.bootRom = {0xC4, 0x00, 0x00, 0x00}; // unsupported LWC1 encoding.
    invalidRomVm.configureMips64MaltaBootstrap(invalidRom);
    CHECK(invalidRomVm.init());
    CHECK(!invalidRomVm.step());
    CHECK(invalidRomVm.getState() == VMState::ERROR);
    CHECK(invalidRomVm.hasKernelPanic());

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
    CHECK(vm.getMips64MaltaBootstrapBoard()->Config().mapFlashMirror);
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
        CHECK(core->core().tlbEntry(index, entry) && !entry.v0 && !entry.v1);
    }
    const Mips64Probe resetProbe = core->core().probeAddress(kMaltaResetVector, Mips64Access::Fetch);
    CHECK(resetProbe.ok && resetProbe.phys == kMaltaResetRomBase);
    const auto& image = vm.getMips64MaltaBootstrapBoard()->BootRomImage();
    CHECK(image.size() == kMaltaBootImageWindow);
    uint8_t resetBytes[8]{}, flashBytes[8]{};
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

    VirtualMachine noFlashVm(2 * 1024 * 1024, "MIPS64");
    MaltaBootstrapConfig noFlash;
    noFlash.mapFlashMirror = false;
    noFlashVm.configureMips64MaltaBootstrap(noFlash);
    auto* noFlashMemory = dynamic_cast<Memory*>(&noFlashVm.getMemory());
    CHECK(noFlashMemory && noFlashMemory->GetPhysicalAddressMap());
    if (noFlashMemory && noFlashMemory->GetPhysicalAddressMap()) {
        bool unmappedFlashRejected = false;
        uint8_t byte = 0;
        try { noFlashMemory->GetPhysicalAddressMap()->Read(kMaltaFlashBase, &byte, 1); }
        catch (const PhysicalBusError& error) {
            unmappedFlashRejected = error.code() == PhysicalBusErrorCode::Unmapped;
        }
        CHECK(unmappedFlashRejected);
    }
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
    const std::array<char, 8> blankDisplay{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}};
    CHECK(board->Fpga()->AsciiDisplay() == blankDisplay);
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
        std::cout << "GUEST UART OUTPUT (" << (little ? "little" : "big")
                  << " endian): " << std::string(outputBytes.begin(), outputBytes.end()) << "\n";
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
    write32(*firstMemory->GetPhysicalAddressMap(), kMaltaFpgaBase + 0x408,
            0x11, true);
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

    std::weak_ptr<Ns16550Uart> uartLifetime;
    std::weak_ptr<MaltaFpgaRegisters> fpgaLifetime;
    {
        VirtualMachine scoped(2 * 1024 * 1024, "MIPS64");
        scoped.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
        MaltaBootstrapBoard* scopedBoard = scoped.getMips64MaltaBootstrapBoard();
        CHECK(scopedBoard != nullptr);
        if (scopedBoard) {
            uartLifetime = scopedBoard->Uart();
            fpgaLifetime = scopedBoard->Fpga();
        }
    }
    CHECK(uartLifetime.expired() && fpgaLifetime.expired());
}

bool sameReplayState(const Mips64FullVmSnapshot& left,
                     const Mips64FullVmSnapshot& right) {
    if (left.cpuCount != right.cpuCount || left.totalVirtualCycles != right.totalVirtualCycles ||
        left.activeCpuIndex != right.activeCpuIndex || left.cpus.size() != right.cpus.size() ||
        left.scheduler.lastCPUIndex != right.scheduler.lastCPUIndex ||
        left.scheduler.numCPUs != right.scheduler.numCPUs ||
        left.scheduler.quantumSize != right.scheduler.quantumSize ||
        left.scheduler.cpuBundleCounters != right.scheduler.cpuBundleCounters ||
        left.fpga.ledBar != right.fpga.ledBar ||
        left.fpga.asciiDisplay != right.fpga.asciiDisplay ||
        left.fpga.breakReset != right.fpga.breakReset ||
        left.fpga.resetPending != right.fpga.resetPending ||
        left.fpga.resetRequestCount != right.fpga.resetRequestCount ||
        left.uart.ier != right.uart.ier || left.uart.fcrLastWrite != right.uart.fcrLastWrite ||
        left.uart.lcr != right.uart.lcr || left.uart.mcr != right.uart.mcr ||
        left.uart.scratch != right.uart.scratch ||
        left.uart.divisorLatchLow != right.uart.divisorLatchLow ||
        left.uart.divisorLatchHigh != right.uart.divisorLatchHigh ||
        left.uart.transmitCount != right.uart.transmitCount ||
        left.console.bytes != right.console.bytes || left.console.sequence != right.console.sequence ||
        left.physicalMemory.regions.size() != right.physicalMemory.regions.size()) return false;
    for (size_t i = 0; i < left.cpus.size(); ++i) {
        const auto& a = left.cpus[i];
        const auto& b = right.cpus[i];
        if (a.cpuId != b.cpuId || a.architecturalState != b.architecturalState ||
            a.executionState != b.executionState || a.cyclesExecuted != b.cyclesExecuted ||
            a.instructionsExecuted != b.instructionsExecuted || a.idleCycles != b.idleCycles ||
            a.enabled != b.enabled || a.lastActivationTime != b.lastActivationTime) return false;
    }
    for (size_t i = 0; i < left.physicalMemory.regions.size(); ++i) {
        const auto& a = left.physicalMemory.regions[i];
        const auto& b = right.physicalMemory.regions[i];
        if (a.info.base != b.info.base || a.info.size != b.info.size ||
            a.info.type != b.info.type || a.info.readable != b.info.readable ||
            a.info.writable != b.info.writable || a.info.accessWidths != b.info.accessWidths ||
            a.backendKind != b.backendKind || a.backingId != b.backingId ||
            a.sparseCapacity != b.sparseCapacity || a.denseBytes != b.denseBytes ||
            a.sparseBytes != b.sparseBytes) return false;
    }
    return true;
}

void testSnapshotFormatAndCpuV5Restore() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    bool createdStateRejected = false;
    try { (void)vm.captureMips64FullSnapshot(); }
    catch (const std::logic_error&) { createdStateRejected = true; }
    CHECK(createdStateRejected);
    CHECK(vm.init());
    CHECK(vm.run(12) == 12);
    const Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
    CHECK(snapshot.ValidateIntegrity());
    CHECK(snapshot.formatVersion == Mips64FullVmSnapshot::kFormatVersion);
    CHECK(snapshot.cpuCount == 1 && snapshot.cpus.size() == 1);
    CHECK(snapshot.cpuComponentVersion == 5);
    CHECK(snapshot.cpus[0].architecturalState.size() == Mips64Cpu::kSerializedSize);
    CHECK(snapshot.cpus[0].architecturalState[4] == 5);
    CHECK(snapshot.romSize == kMaltaBootImageWindow && snapshot.romContentHash != 0);

    uint8_t changedRam = 0xA7;
    vm.getMemory().Write(0x200, &changedRam, 1);
    plugin(vm)->core().writeGpr(5, 0x12345678);
    write32(*dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap(),
            kMaltaFpgaBase + 0x408, 0x55, true);
    const Mips64FullVmSnapshot beforeFailure = vm.captureMips64FullSnapshot();

    Mips64FullVmSnapshot badVersion = snapshot;
    badVersion.formatVersion = 77;
    CHECK(badVersion.RefreshIntegrity());
    std::string error;
    CHECK(!vm.restoreMips64FullSnapshot(badVersion, &error));
    CHECK(!error.empty());
    CHECK(vm.captureMips64FullSnapshot().integrityChecksum == beforeFailure.integrityChecksum);

    CHECK(vm.restoreMips64FullSnapshot(snapshot, &error));
    CHECK(vm.getState() == VMState::PAUSED);
    CHECK(plugin(vm)->core().pc() == kMaltaResetVector + 12 * 4);
    CHECK(plugin(vm)->core().readGpr(5) == 0);
    uint8_t restoredRam = 0xFF;
    vm.getMemory().Read(0x200, &restoredRam, 1);
    CHECK(restoredRam == 0);
    CHECK(vm.getMips64MaltaBootstrapBoard()->Fpga()->LedBar() == snapshot.fpga.ledBar);

    Mips64FullVmSnapshot badLength = snapshot;
    ++badLength.cpuComponentLength;
    CHECK(!badLength.ValidateIntegrity());
}

void testSnapshotDenseSparseAndHighMemoryRestore() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.enableMips64PhysicalAddressMap().IsEmpty() == false);
    const uint64_t sparseBase = kPhysicalAddressLimit - 0x1000;
    vm.enableMips64PhysicalAddressMap().RegisterSparseRam(sparseBase, 0x1000, 4);
    CHECK(vm.init());
    uint8_t denseValue = 0x19;
    vm.getMemory().Write(0x100, &denseValue, 1);
    const uint8_t sparseA = 0xA5;
    const uint8_t sparseB = 0x5A;
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (!memory) return;
    memory->GetPhysicalAddressMap()->Write(sparseBase + 3, &sparseA, 1);
    memory->GetPhysicalAddressMap()->Write(kPhysicalAddressLimit - 1, &sparseB, 1);
    const Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
    const auto sparse = std::find_if(snapshot.physicalMemory.regions.begin(),
                                     snapshot.physicalMemory.regions.end(),
        [sparseBase](const PhysicalRegionSnapshot& region) {
            return region.info.base == sparseBase;
        });
    CHECK(sparse != snapshot.physicalMemory.regions.end());
    if (sparse != snapshot.physicalMemory.regions.end()) {
        CHECK(sparse->backendKind == PhysicalBackendKind::SparseRam);
        CHECK(sparse->sparseCapacity == 4 && sparse->sparseBytes.size() == 2);
    }

    uint8_t replacement = 0x44;
    vm.getMemory().Write(0x100, &replacement, 1);
    memory->GetPhysicalAddressMap()->Write(sparseBase + 3, &replacement, 1);
    memory->GetPhysicalAddressMap()->Write(kPhysicalAddressLimit - 1, &replacement, 1);
    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(snapshot, &error));
    uint8_t value = 0;
    vm.getMemory().Read(0x100, &value, 1);
    CHECK(value == denseValue);
    memory->GetPhysicalAddressMap()->Read(sparseBase + 3, &value, 1);
    CHECK(value == sparseA);
    memory->GetPhysicalAddressMap()->Read(kPhysicalAddressLimit - 1, &value, 1);
    CHECK(value == sparseB);
    const Mips64FullVmSnapshot restoredSnapshot = vm.captureMips64FullSnapshot();
    const auto restoredSparse = std::find_if(restoredSnapshot.physicalMemory.regions.begin(),
                                              restoredSnapshot.physicalMemory.regions.end(),
        [sparseBase](const PhysicalRegionSnapshot& region) {
            return region.info.base == sparseBase;
        });
    CHECK(restoredSparse != restoredSnapshot.physicalMemory.regions.end());
    if (restoredSparse != restoredSnapshot.physicalMemory.regions.end()) {
        CHECK(restoredSparse->sparseCapacity == sparse->sparseCapacity);
        CHECK(restoredSparse->sparseBytes == sparse->sparseBytes);
    }

    Mips64FullVmSnapshot badCapacity = snapshot;
    auto badSparse = std::find_if(badCapacity.physicalMemory.regions.begin(),
                                  badCapacity.physicalMemory.regions.end(),
        [sparseBase](const PhysicalRegionSnapshot& region) {
            return region.info.base == sparseBase;
        });
    if (badSparse != badCapacity.physicalMemory.regions.end()) ++badSparse->sparseCapacity;
    CHECK(badCapacity.RefreshIntegrity());
    const Mips64FullVmSnapshot beforeFailure = vm.captureMips64FullSnapshot();
    CHECK(!vm.restoreMips64FullSnapshot(badCapacity, &error));
    CHECK(vm.captureMips64FullSnapshot().integrityChecksum == beforeFailure.integrityChecksum);
}

void testSnapshotDeviceAndConsoleState() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.init());
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (!memory) return;
    PhysicalAddressMap& bus = *memory->GetPhysicalAddressMap();
    write32(bus, kMaltaFpgaBase + 0x408, 0xC3, true);
    write32(bus, kMaltaFpgaBase + 0x418, static_cast<uint8_t>('Q'), true);
    write32(bus, kMaltaFpgaBase + 0x508, 0x17, true);
    const uint8_t lcrDlab = 0x83;
    const uint8_t dll = 0x12;
    const uint8_t dlm = 0x34;
    const uint8_t lcr = 0x03;
    const uint8_t ier = 0x05;
    const uint8_t fcr = 0xC1;
    const uint8_t mcr = 0x1B;
    const uint8_t scratch = 0x7E;
    bus.Write(kMaltaUartBase + 0x18, &lcrDlab, 1);
    bus.Write(kMaltaUartBase, &dll, 1);
    bus.Write(kMaltaUartBase + 0x08, &dlm, 1);
    bus.Write(kMaltaUartBase + 0x18, &lcr, 1);
    bus.Write(kMaltaUartBase + 0x08, &ier, 1);
    bus.Write(kMaltaUartBase + 0x10, &fcr, 1);
    bus.Write(kMaltaUartBase + 0x20, &mcr, 1);
    bus.Write(kMaltaUartBase + 0x38, &scratch, 1);
    const uint8_t binary[] = {'A', 0, 0xFF};
    for (uint8_t byte : binary) bus.Write(kMaltaUartBase, &byte, 1);
    const Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
    CHECK(snapshot.console.bytes == std::vector<uint8_t>({'A', 0, 0xFF}));
    CHECK(snapshot.console.sequence == 3 && snapshot.console.capacityBytes == 64 * 1024);
    CHECK(snapshot.uart.dlab == false && snapshot.uart.transmitterReady);
    CHECK(snapshot.uart.divisorLatchLow == dll && snapshot.uart.divisorLatchHigh == dlm);
    CHECK(snapshot.uart.ier == ier && snapshot.uart.fcrLastWrite == fcr);
    CHECK(snapshot.uart.mcr == mcr && snapshot.uart.scratch == scratch);

    const uint64_t capturedEpoch = snapshot.console.generation;
    const uint8_t other = 'X';
    bus.Write(kMaltaUartBase, &other, 1);
    write32(bus, kMaltaFpgaBase + 0x408, 0, true);
    write32(bus, kMaltaFpgaBase + 0x500, 0x42, true);
    CHECK(vm.getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(snapshot, &error));
    const auto& board = *vm.getMips64MaltaBootstrapBoard();
    CHECK(board.ConsoleSink()->Bytes() == std::vector<uint8_t>({'A', 0, 0xFF}));
    CHECK(board.ConsoleSink()->Generation() > capturedEpoch);
    CHECK(board.Fpga()->LedBar() == 0xC3);
    CHECK(board.Fpga()->AsciiDisplay()[0] == 'Q');
    CHECK(board.Fpga()->BreakReset() == 0x17);
    const auto uartState = board.Uart()->InspectState();
    CHECK(uartState.at("DLL") == dll && uartState.at("DLM") == dlm);
    CHECK(uartState.at("IER") == ier && uartState.at("FCR_last_write") == fcr);
    CHECK(uartState.at("MCR") == mcr && uartState.at("SCR") == scratch);
    CHECK(uartState.at("transmit_count") == 3);
    CHECK(!board.HasSoftwareResetRequest() && board.SoftwareResetRequestCount() ==
          snapshot.fpga.resetRequestCount);
}

void testSnapshotTransactionalFailureInjectionAndResetMailbox() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.enableMips64PhysicalAddressMap().IsEmpty() == false);
    const uint64_t sparseBase = kPhysicalAddressLimit - 0x2000;
    vm.enableMips64PhysicalAddressMap().RegisterSparseRam(sparseBase, 0x1000, 8);
    CHECK(vm.init());
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (!memory) return;
    uint8_t sparseValue = 0x81;
    memory->GetPhysicalAddressMap()->Write(sparseBase + 7, &sparseValue, 1);
    const Mips64FullVmSnapshot base = vm.captureMips64FullSnapshot();

    unsigned scenario = 0;
    auto expectRejectedWithoutMutation = [&](const Mips64FullVmSnapshot& invalid,
                                             Mips64SnapshotFailurePoint injection) {
        ++scenario;
        const uint8_t changed = static_cast<uint8_t>(0x20 + scenario);
        vm.getMemory().Write(0x300 + scenario, &changed, 1);
        plugin(vm)->core().writeGpr(6, scenario * 101);
        write32(*memory->GetPhysicalAddressMap(), kMaltaFpgaBase + 0x408,
                static_cast<uint8_t>(scenario), true);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        vm.setMips64SnapshotFailurePointForTesting(injection);
        std::string error;
        CHECK(!vm.restoreMips64FullSnapshot(invalid, &error));
        CHECK(!error.empty());
        const Mips64FullVmSnapshot after = vm.captureMips64FullSnapshot();
        CHECK(after.integrityChecksum == before.integrityChecksum);
    };

    Mips64FullVmSnapshot invalidCpu = base;
    invalidCpu.cpus[0].architecturalState[7] = 1;
    CHECK(invalidCpu.RefreshIntegrity());
    expectRejectedWithoutMutation(invalidCpu, Mips64SnapshotFailurePoint::None);

    Mips64FullVmSnapshot corruptedChecksum = base;
    corruptedChecksum.integrityChecksum ^= 1;
    expectRejectedWithoutMutation(corruptedChecksum, Mips64SnapshotFailurePoint::None);

    Mips64FullVmSnapshot invalidLength = base;
    ++invalidLength.memoryComponentLength;
    expectRejectedWithoutMutation(invalidLength, Mips64SnapshotFailurePoint::None);
    expectRejectedWithoutMutation(base, Mips64SnapshotFailurePoint::CpuStateValidation);

    expectRejectedWithoutMutation(base, Mips64SnapshotFailurePoint::SparseMemoryPreparation);
    expectRejectedWithoutMutation(base, Mips64SnapshotFailurePoint::DeviceStatePreparation);
    expectRejectedWithoutMutation(base, Mips64SnapshotFailurePoint::RegionManifestValidation);
    expectRejectedWithoutMutation(base, Mips64SnapshotFailurePoint::RestoreCommitPreparation);

    Mips64FullVmSnapshot invalidManifest = base;
    invalidManifest.physicalMemory.regions[0].info.base = invalidManifest.physicalMemory.regions[0].info.base + 1;
    CHECK(invalidManifest.RefreshIntegrity());
    expectRejectedWithoutMutation(invalidManifest, Mips64SnapshotFailurePoint::None);

    Mips64FullVmSnapshot overlappingManifest = base;
    overlappingManifest.physicalMemory.regions[1].info.base =
        overlappingManifest.physicalMemory.regions[0].info.base;
    CHECK(overlappingManifest.RefreshIntegrity());
    expectRejectedWithoutMutation(overlappingManifest, Mips64SnapshotFailurePoint::None);

    Mips64FullVmSnapshot invalidDevice = base;
    ++invalidDevice.console.capacityBytes;
    CHECK(invalidDevice.RefreshIntegrity());
    expectRejectedWithoutMutation(invalidDevice, Mips64SnapshotFailurePoint::None);

    // Snapshot A: before reset request. Snapshot B: request queued but not
    // consumed. The VM processes B at its next scheduler boundary.
    const Mips64FullVmSnapshot beforeRequest = vm.captureMips64FullSnapshot();
    write32(*memory->GetPhysicalAddressMap(), kMaltaFpgaBase + 0x500, 0x42, true);
    const Mips64FullVmSnapshot pendingRequest = vm.captureMips64FullSnapshot();
    CHECK(pendingRequest.pendingDeferredBoardReset && pendingRequest.fpga.resetRequestCount == 1);
    CHECK(vm.step());
    CHECK(!vm.getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
    CHECK(plugin(vm)->core().pc() == kMaltaResetVector);
    CHECK(vm.restoreMips64FullSnapshot(pendingRequest));
    CHECK(vm.getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
    CHECK(vm.step());
    CHECK(!vm.getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
    CHECK(vm.getMips64MaltaBootstrapBoard()->SoftwareResetRequestCount() == 1);
    CHECK(plugin(vm)->core().pc() == kMaltaResetVector);
    const Mips64FullVmSnapshot completedReset = vm.captureMips64FullSnapshot();
    plugin(vm)->core().setPc(0xFFFFFFFF80001234ULL);
    CHECK(vm.restoreMips64FullSnapshot(completedReset));
    CHECK(plugin(vm)->core().pc() == kMaltaResetVector);
    CHECK(vm.restoreMips64FullSnapshot(beforeRequest));
    CHECK(!vm.getMips64MaltaBootstrapBoard()->HasSoftwareResetRequest());
}

void testProductionVmDeterministicReplayAndTimerRestore() {
    const std::string expected =
        "guideXOS MIPS64 Malta bootstrap OKMIPS64 RAM payload running";
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    vm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(vm.init());
    const auto payload = MaltaBootstrapBoard::BuildDemoRamPayload(Mips64Endianness::Little);
    CHECK(vm.loadProgram(payload.data(), payload.size(), kMaltaRamPayloadPhysicalAddress));
    const Mips64FullVmSnapshot beforeOutput = vm.captureMips64FullSnapshot();
    CHECK(vm.run(60) == 60);
    const Mips64FullVmSnapshot midOutput = vm.captureMips64FullSnapshot();
    CHECK(plugin(vm)->core().pc() >= kMaltaResetVector &&
          plugin(vm)->core().pc() < kMaltaResetVector + kMaltaBootImageWindow);
    const auto& prefix = vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(std::string(prefix.begin(), prefix.end()) == "guideX");
    CHECK(vm.run(512) == 512);
    const Mips64FullVmSnapshot completed = vm.captureMips64FullSnapshot();
    const auto& full = vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(std::string(full.begin(), full.end()) == expected);
    uint8_t completion[4]{};
    vm.getMemory().Read(0x2000, completion, sizeof(completion));
    CHECK(decodeWord(completion, true) == 0x47584F53);

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(midOutput, &error));
    CHECK(vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == midOutput.console.bytes);
    CHECK(sameReplayState(midOutput, vm.captureMips64FullSnapshot()));
    CHECK(vm.run(512) == 512);
    const Mips64FullVmSnapshot replayed = vm.captureMips64FullSnapshot();
    CHECK(sameReplayState(completed, replayed));
    const auto& replayOutput = vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(std::string(replayOutput.begin(), replayOutput.end()) == expected);

    CHECK(vm.restoreMips64FullSnapshot(beforeOutput, &error));
    CHECK(vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());
    CHECK(vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Generation() > beforeOutput.console.generation);

    CHECK(vm.restoreMips64FullSnapshot(completed, &error));
    CHECK(vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() == completed.console.bytes);
    CHECK(vm.getMips64MaltaBootstrapBoard()->Uart()->InspectState().at("transmit_count") ==
          completed.uart.transmitCount);

    // Capture the pending branch transfer at the real ROM JR boundary.
    CHECK(vm.restoreMips64FullSnapshot(beforeOutput, &error));
    CHECK(vm.run(119) == 119);
    CHECK(plugin(vm)->core().delayArmed());
    const Mips64FullVmSnapshot delaySlot = vm.captureMips64FullSnapshot();
    CHECK(vm.run(1) == 1);
    CHECK(vm.restoreMips64FullSnapshot(delaySlot, &error));
    CHECK(plugin(vm)->core().delayArmed());
    CHECK(vm.run(1) == 1);
    CHECK(plugin(vm)->core().pc() == kMaltaRamPayloadVirtualAddress);

    // A two-CPU Malta VM proves the production round-robin position is part
    // of the restored decision state.
    VirtualMachine twoCpu(2 * 1024 * 1024, 2, "MIPS64");
    twoCpu.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(twoCpu.init());
    CHECK(twoCpu.run(4) == 4);
    const Mips64FullVmSnapshot schedulerPoint = twoCpu.captureMips64FullSnapshot();
    std::vector<int> firstOrder;
    for (int i = 0; i < 8; ++i) {
        CHECK(twoCpu.run(1) == 1);
        firstOrder.push_back(twoCpu.getActiveCPUIndex());
    }
    const Mips64FullVmSnapshot firstEnd = twoCpu.captureMips64FullSnapshot();
    CHECK(twoCpu.restoreMips64FullSnapshot(schedulerPoint, &error));
    std::vector<int> replayOrder;
    for (int i = 0; i < 8; ++i) {
        CHECK(twoCpu.run(1) == 1);
        replayOrder.push_back(twoCpu.getActiveCPUIndex());
    }
    const Mips64FullVmSnapshot replayEnd = twoCpu.captureMips64FullSnapshot();
    CHECK(firstOrder == replayOrder);
    CHECK(sameReplayState(firstEnd, replayEnd));

    // Count/Compare reaches its compare value while interrupts are masked.
    // The snapshot preserves the latched TI/IP7 request, then replays delivery,
    // handler acknowledgement and ERET through VirtualMachine scheduling.
    VirtualMachine timerVm(2 * 1024 * 1024, "MIPS64");
    timerVm.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(timerVm.init());
    const auto mtc0 = [](uint8_t rt, uint8_t rd) {
        return (0x10U << 26) | (4U << 21) | (static_cast<uint32_t>(rt) << 16) |
               (static_cast<uint32_t>(rd) << 11);
    };
    std::vector<uint32_t> timerProgram{
        encI(0x09, 0, 8, 16), mtc0(8, 11)
    };
    timerProgram.insert(timerProgram.end(), 14, 0);
    timerProgram.push_back(encI(0x0D, 0, 8, 0x8001));
    timerProgram.push_back(mtc0(8, 12));
    timerProgram.push_back(0);
    const auto timerImage = encodeWords(timerProgram, true);
    CHECK(timerVm.loadProgram(timerImage.data(), timerImage.size(), 0x1000));
    const std::vector<uint32_t> timerHandler{
        encI(0x09, 0, 8, 0), mtc0(8, 11), encI(0x09, 0, 9, 0x55),
        (0x0FU << 26) | (10U << 16) | 0x8000U,
        encI(0x0D, 10, 10, 0x0300), encI(0x2B, 10, 9, 0), 0x42000018U
    };
    const auto handlerImage = encodeWords(timerHandler, true);
    CHECK(timerVm.loadProgram(handlerImage.data(), handlerImage.size(), 0x180));
    plugin(timerVm)->core().setPc(kMaltaRamPayloadVirtualAddress);
    CHECK(timerVm.run(16) == 16);
    CHECK(plugin(timerVm)->core().cp0Count() == 16);
    CHECK((plugin(timerVm)->core().cp0Cause() & (kCauseBitTi | kCauseIp7Bit)) ==
          (kCauseBitTi | kCauseIp7Bit));
    const Mips64FullVmSnapshot pendingTimer = timerVm.captureMips64FullSnapshot();
    CHECK(timerVm.run(3) == 3);
    CHECK(plugin(timerVm)->core().pc() == 0xFFFFFFFF80000180ULL);
    CHECK(plugin(timerVm)->core().statusExl());
    const Mips64FullVmSnapshot inHandler = timerVm.captureMips64FullSnapshot();
    CHECK(inHandler.cpus[0].architecturalState.size() == Mips64Cpu::kSerializedSize);
    CHECK(timerVm.run(7) == 7);
    CHECK(!plugin(timerVm)->core().statusExl());
    CHECK((plugin(timerVm)->core().cp0Cause() & (kCauseBitTi | kCauseIp7Bit)) == 0);
    const Mips64FullVmSnapshot timerCompleted = timerVm.captureMips64FullSnapshot();
    uint8_t timerMarker = 0;
    timerVm.getMemory().Read(0x300, &timerMarker, 1);
    CHECK(timerMarker == 0x55);
    CHECK(timerVm.restoreMips64FullSnapshot(pendingTimer, &error));
    CHECK(timerVm.run(3) == 3);
    CHECK(timerVm.captureMips64FullSnapshot().cpus[0].architecturalState ==
          inHandler.cpus[0].architecturalState);
    CHECK(timerVm.run(7) == 7);
    const Mips64FullVmSnapshot timerReplay = timerVm.captureMips64FullSnapshot();
    CHECK(sameReplayState(timerCompleted, timerReplay));
    CHECK(timerVm.restoreMips64FullSnapshot(inHandler, &error));
    CHECK(timerVm.run(7) == 7);
    CHECK(sameReplayState(timerCompleted, timerVm.captureMips64FullSnapshot()));

    // Cross-VM restore is accepted only for the same profile and identities.
    VirtualMachine independent(2 * 1024 * 1024, 2, "MIPS64");
    independent.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(independent.init());
    const bool independentRestored = independent.restoreMips64FullSnapshot(schedulerPoint, &error);
    CHECK(independentRestored);
    CHECK(plugin(independent)->core().serialize() == schedulerPoint.cpus[0].architecturalState);
}

void testSnapshotCompatibilityRejections() {
    VirtualMachine source(2 * 1024 * 1024, "MIPS64");
    source.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(source.init());
    const Mips64FullVmSnapshot snapshot = source.captureMips64FullSnapshot();

    auto expectRejectedWithoutMutation = [&](VirtualMachine& destination) {
        const Mips64FullVmSnapshot before = destination.captureMips64FullSnapshot();
        std::string error;
        CHECK(!destination.restoreMips64FullSnapshot(snapshot, &error));
        CHECK(!error.empty());
        CHECK(destination.captureMips64FullSnapshot().integrityChecksum == before.integrityChecksum);
    };

    VirtualMachine differentEndian(2 * 1024 * 1024, "MIPS64");
    differentEndian.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Big));
    CHECK(differentEndian.init());
    expectRejectedWithoutMutation(differentEndian);

    VirtualMachine differentCpuCount(2 * 1024 * 1024, 2, "MIPS64");
    differentCpuCount.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(differentCpuCount.init());
    expectRejectedWithoutMutation(differentCpuCount);

    VirtualMachine differentRamSize(1 * 1024 * 1024, "MIPS64");
    differentRamSize.configureMips64MaltaBootstrap(configFor(Mips64Endianness::Little));
    CHECK(differentRamSize.init());
    expectRejectedWithoutMutation(differentRamSize);

    MaltaBootstrapConfig differentRomConfig = configFor(Mips64Endianness::Little);
    differentRomConfig.useBuiltInDemoRom = false;
    differentRomConfig.bootRom = MaltaBootstrapBoard::BuildDemoBootRom(Mips64Endianness::Little);
    differentRomConfig.bootRom[0] ^= 0x01;
    VirtualMachine differentRom(2 * 1024 * 1024, "MIPS64");
    differentRom.configureMips64MaltaBootstrap(differentRomConfig);
    CHECK(differentRom.init());
    expectRejectedWithoutMutation(differentRom);

    MaltaBootstrapConfig differentMapConfig = configFor(Mips64Endianness::Little);
    differentMapConfig.mapFlashMirror = false;
    VirtualMachine differentMap(2 * 1024 * 1024, "MIPS64");
    differentMap.configureMips64MaltaBootstrap(differentMapConfig);
    CHECK(differentMap.init());
    expectRejectedWithoutMutation(differentMap);

    VirtualMachine unsupportedProfile(2 * 1024 * 1024, "MIPS64");
    CHECK(unsupportedProfile.init());
    const VMState stateBefore = unsupportedProfile.getState();
    const uint64_t pcBefore = unsupportedProfile.getIP(0);
    std::string error;
    CHECK(!unsupportedProfile.restoreMips64FullSnapshot(snapshot, &error));
    CHECK(!error.empty());
    CHECK(unsupportedProfile.getState() == stateBefore);
    CHECK(unsupportedProfile.getIP(0) == pcBefore);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_mips64_malta <map|fpga|reset|boot|ownership|snapshot_format|snapshot_memory|snapshot_devices|snapshot_restore|snapshot_vm|snapshot_compat>\n";
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "map") testMapAndArchitecturalReset();
    else if (mode == "fpga") testFpgaRegistersAndUartCoexistence();
    else if (mode == "reset") testSafeGuestSoftwareResetAndColdReset();
    else if (mode == "boot") testColdBootGuestAndPayloadBothEndians();
    else if (mode == "ownership") testTwoVmsAndTwoCpusAreBoardScoped();
    else if (mode == "snapshot_format") testSnapshotFormatAndCpuV5Restore();
    else if (mode == "snapshot_memory") testSnapshotDenseSparseAndHighMemoryRestore();
    else if (mode == "snapshot_devices") testSnapshotDeviceAndConsoleState();
    else if (mode == "snapshot_restore") testSnapshotTransactionalFailureInjectionAndResetMailbox();
    else if (mode == "snapshot_vm") testProductionVmDeterministicReplayAndTimerRestore();
    else if (mode == "snapshot_compat") testSnapshotCompatibilityRejections();
    else return 2;
    std::cout << "test_mips64_malta[" << mode << "]: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
