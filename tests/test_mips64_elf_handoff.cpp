#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64ElfLoader.h"
#include "mips64/Mips64MaltaProm.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

unsigned gPass = 0;
unsigned gFail = 0;
#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

constexpr uint64_t kRamBytes = 64ULL * 1024 * 1024;
constexpr uint32_t kSuccessWord = 0x47584F53;
constexpr uint32_t kFailureWord = 0x00000BAD;
constexpr uint64_t kEntryPoint = 0xFFFFFFFF80010000ULL;
constexpr char kGuestMessage[] = "guideXOS MIPS64 ELF handoff OK\r\n";

Mips64ISAPlugin* plugin(VirtualMachine& vm);
uint32_t readGuestWord(VirtualMachine& vm, uint64_t address, bool isLittle);
void readPhysicalBytes(VirtualMachine& vm, uint64_t address,
                       uint8_t* destination, size_t size);
void runToCompletion(VirtualMachine& vm, Mips64ISAPlugin& cpu, bool isLittle);

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool little(Mips64Endianness endian) {
    return endian == Mips64Endianness::Little;
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t at, bool isLittle) {
    return isLittle ? uint16_t(bytes[at]) | (uint16_t(bytes[at + 1]) << 8)
                    : (uint16_t(bytes[at]) << 8) | uint16_t(bytes[at + 1]);
}

uint32_t read32(const uint8_t* bytes, bool isLittle) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[i]) << (8 * (isLittle ? i : 3 - i));
    }
    return value;
}

uint64_t read64(const std::vector<uint8_t>& bytes, size_t at, bool isLittle) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= uint64_t(bytes[at + i]) << (8 * (isLittle ? i : 7 - i));
    }
    return value;
}

void write16(std::vector<uint8_t>& bytes, size_t at, uint16_t value, bool isLittle) {
    for (unsigned i = 0; i < 2; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 1 - i)));
    }
}

void write32(std::vector<uint8_t>& bytes, size_t at, uint32_t value, bool isLittle) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 3 - i)));
    }
}

void write64(std::vector<uint8_t>& bytes, size_t at, uint64_t value, bool isLittle) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 7 - i)));
    }
}

std::vector<uint8_t> loadFixture(bool isLittle) {
    const std::string name = isLittle ? "guidexos_mips64_le.elf"
                                      : "guidexos_mips64_be.elf";
    auto bytes = readFile(std::filesystem::path(MIPS64_ELF_FIXTURE_DIR) / name);
    if (bytes.empty()) throw std::runtime_error("missing independent MIPS64 ELF fixture: " + name);
    return bytes;
}

Mips64ElfLoadOptions options(Mips64Endianness endian) {
    Mips64ElfLoadOptions result;
    result.guestEndianness = endian;
    result.guestRamSize = kRamBytes;
    result.reservedPhysicalRanges = {
        {kMaltaPromDefaultPhysicalAddress, kMaltaPromImageBytes},
        {kMaltaElfCompletionPhysicalAddress, sizeof(uint32_t)}
    };
    return result;
}

bool rejected(const std::vector<uint8_t>& bytes, Mips64ElfLoadOptions opts) {
    Mips64ElfLoadPlan plan;
    return !Mips64ElfLoader::BuildPlan(bytes.data(), bytes.size(), opts, plan);
}

void testElfValidationBothEndians() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool isLittle = little(endian);
        const auto fixture = loadFixture(isLittle);
        Mips64ElfLoadPlan plan;
        std::string error;
        CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                         options(endian), plan, &error));
        CHECK(error.empty());
        CHECK(plan.endianness == endian);
        CHECK(plan.entryPoint == kEntryPoint);
        CHECK(plan.entryPhysicalAddress == 0x10000);
        CHECK(!plan.requiresKernelKx);
        CHECK((plan.machineFlags & 0xF0000000U) == 0x80000000U); // MIPS64r2.
        CHECK(plan.segments.size() == 3);
        CHECK(plan.segments[0].physicalAddress == 0x10000);
        CHECK(plan.segments[1].physicalAddress == 0x11000);
        CHECK(plan.segments[2].physicalAddress == 0x12000);
        CHECK((plan.segments[0].flags & 0x5U) == 0x5U &&
              (plan.segments[0].flags & 0x2U) == 0); // RX text.
        CHECK((plan.segments[1].flags & 0x4U) != 0 &&
              (plan.segments[1].flags & 0x2U) == 0); // read-only data.
        CHECK((plan.segments[2].flags & 0x2U) != 0); // writable data/BSS.
        CHECK(plan.segments[2].fileSize < plan.segments[2].memorySize);
        CHECK(plan.segments[2].image.size() == plan.segments[2].memorySize);
        CHECK(std::all_of(plan.segments[2].image.begin() +
                              static_cast<std::ptrdiff_t>(plan.segments[2].fileSize),
                          plan.segments[2].image.end(),
                          [](uint8_t byte) { return byte == 0; }));
        CHECK(read16(fixture, 18, isLittle) == 8); // EM_MIPS.

        auto directMapPaddrAliases = fixture;
        for (size_t index = 0; index < plan.segments.size(); ++index) {
            const size_t phdr = 64 + index * 56;
            const uint64_t virtualAddress = read64(directMapPaddrAliases,
                                                   phdr + 16, isLittle);
            write64(directMapPaddrAliases, phdr + 24, virtualAddress, isLittle);
        }
        Mips64ElfLoadPlan aliasPlan;
        CHECK(Mips64ElfLoader::BuildPlan(directMapPaddrAliases.data(),
                                         directMapPaddrAliases.size(),
                                         options(endian), aliasPlan));
        CHECK(aliasPlan.segments[0].physicalAddress == plan.segments[0].physicalAddress);

        CHECK(rejected(fixture, options(isLittle ? Mips64Endianness::Big
                                                 : Mips64Endianness::Little)));
        CHECK(!Mips64ElfLoader::BuildPlan(fixture.data(), 63, options(endian), plan));

        auto malformed = fixture;
        malformed[0] = 0;
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        malformed[4] = 1; // ELF32.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        malformed[5] = 0; // Invalid EI_DATA.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 18, 50, isLittle); // EM_IA_64.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 16, 3, isLittle); // ET_DYN.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 56, 0, isLittle); // no program headers.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 32, UINT64_MAX - 8, isLittle); // PHDR table overflow.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 48, 0x80800000U, isLittle); // unsupported ABI/ASE flags.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 24, 0x20000, isLittle); // conflicting p_paddr.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 32, UINT64_MAX, isLittle); // invalid first p_filesz.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 8, UINT64_MAX - 8, isLittle); // overflowing p_offset.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 64 + 56, 2, isLittle); // PT_DYNAMIC.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 64 + 56, 4, isLittle); // PT_NOTE must have a valid range.
        write64(malformed, 64 + 56 + 8, UINT64_MAX, isLittle);
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        const size_t secondPhdr = 64 + 56;
        write64(malformed, secondPhdr + 16, kEntryPoint, isLittle);
        write64(malformed, secondPhdr + 24, 0, isLittle);
        CHECK(rejected(malformed, options(endian))); // overlapping PT_LOADs.
        malformed = fixture;
        write64(malformed, 64 + 16, 0xFFFFFFFF88010000ULL, isLittle);
        write64(malformed, 64 + 24, 0, isLittle);
        CHECK(rejected(malformed, options(endian))); // resolved PA outside guest RAM.
        malformed = fixture;
        write64(malformed, 64 + 32, 0x1000, isLittle);
        write64(malformed, 64 + 40, 4, isLittle); // p_filesz > p_memsz.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 16, UINT64_MAX - 1, isLittle);
        CHECK(rejected(malformed, options(endian))); // virtual range overflow.
        malformed = fixture;
        write64(malformed, 24, kEntryPoint + 1, isLittle);
        CHECK(rejected(malformed, options(endian))); // unaligned entry.
        malformed = fixture;
        const uint64_t sectionOffset = read64(fixture, 40, isLittle);
        write32(malformed, static_cast<size_t>(sectionOffset + 64) + 4,
                4, isLittle); // SHT_RELA, even though the current fixture is static.
        CHECK(rejected(malformed, options(endian)));

        auto promCollisionOptions = options(endian);
        promCollisionOptions.reservedPhysicalRanges = {{0x10000, 0x1000}};
        CHECK(rejected(fixture, promCollisionOptions));
    }
}

class CountingMmio final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 4; }
    void Read(uint64_t, uint8_t* destination, size_t size) const override {
        std::fill(destination, destination + size, uint8_t{0});
    }
    void Write(uint64_t, const uint8_t*, size_t) override { ++writeCount; }
    void Reset() override {}
    std::map<std::string, uint64_t> InspectState() const override {
        return {{"writes", writeCount}};
    }
    uint64_t writeCount = 0;
};

void testLoadPlanRejectsRomAndMmioWithoutPartialRamWrites() {
    const auto fixture = loadFixture(true);
    Mips64ElfLoadPlan validPlan;
    CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                     options(Mips64Endianness::Little), validPlan));
    PhysicalAddressMap bus;
    bus.RegisterRam(0, 0x20000);
    const std::array<uint8_t, 4> romContents{{0xA5, 0x5A, 0xC3, 0x3C}};
    std::vector<uint8_t> rom(0x1000, 0);
    std::copy(romContents.begin(), romContents.end(), rom.begin());
    bus.RegisterRom(0x30000, rom);
    auto mmio = std::make_shared<CountingMmio>();
    bus.RegisterMmio(0x40000, 0x1000, mmio, {4});

    auto expectRejected = [&](uint64_t target) {
        Mips64ElfLoadPlan plan = validPlan;
        plan.segments[0].physicalAddress = target;
        std::string error;
        CHECK(!Mips64ElfLoader::CommitPlan(plan, bus, {}, &error));
        CHECK(!error.empty());
        uint8_t ramCheck[4]{0xFF, 0xFF, 0xFF, 0xFF};
        bus.Read(0x11000, ramCheck, sizeof(ramCheck));
        CHECK(std::all_of(std::begin(ramCheck), std::end(ramCheck),
                          [](uint8_t byte) { return byte == 0; }));
        uint8_t romCheck[4]{};
        bus.Read(0x30000, romCheck, sizeof(romCheck));
        CHECK(std::equal(romContents.begin(), romContents.end(), std::begin(romCheck)));
        CHECK(mmio->writeCount == 0);
    };
    expectRejected(0x30000);
    expectRejected(0x40000);
}

void testXkphysKernelEntryAndKxSetup() {
    constexpr uint64_t xkphysEntry = 0x9800000000010000ULL;
    auto fixture = loadFixture(true);
    write64(fixture, 24, xkphysEntry, true);
    write64(fixture, 64 + 16, xkphysEntry, true);
    Mips64ElfLoadPlan plan;
    CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                     options(Mips64Endianness::Little), plan));
    CHECK(plan.entryPoint == xkphysEntry);
    CHECK(plan.entryPhysicalAddress == 0x10000);
    CHECK(plan.requiresKernelKx);

    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    std::string error;
    CHECK(vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
    CHECK(vm.init());
    Mips64ISAPlugin* cpu = plugin(vm);
    CHECK(cpu != nullptr);
    if (!cpu) return;
    for (unsigned i = 0; i < 64 && cpu->core().pc() != xkphysEntry; ++i) {
        CHECK(vm.run(1) == 1);
    }
    CHECK(cpu->core().pc() == xkphysEntry);
    CHECK((cpu->core().cp0Status() & kStatusBitKx) != 0);
    runToCompletion(vm, *cpu, true);
    CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    const auto& output = vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(output == std::vector<uint8_t>(kGuestMessage,
                                         kGuestMessage + sizeof(kGuestMessage) - 1));
}

void testPromImageBoundsAndEncoding() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        MaltaPromConfig config;
        MaltaPromImage image;
        std::string error;
        CHECK(Mips64MaltaProm::BuildImage(config, endian, kRamBytes, image, &error));
        CHECK(image.argumentCount == 2);
        CHECK(image.argumentTablePointer32 == 0x80002000U);
        CHECK(image.environmentTablePointer32 == 0x80002008U);
        CHECK(image.argumentTablePointer == 0xFFFFFFFF80002000ULL);
        CHECK(image.environmentTablePointer == 0xFFFFFFFF80002008ULL);
        CHECK(image.bytes.size() == kMaltaPromImageBytes);
        CHECK(read32(image.bytes.data(), little(endian)) == 0x80002040U);
        CHECK(read32(image.bytes.data() + 4, little(endian)) == 0x80002140U);
        CHECK(read32(image.bytes.data() + 8, little(endian)) == 0x80002240U);
        CHECK(read32(image.bytes.data() + 32, little(endian)) == 0);
        CHECK(std::string(reinterpret_cast<const char*>(image.bytes.data() + 64),
                          std::strlen("guideXOS-mips64-elf")) == "guideXOS-mips64-elf");
        CHECK(!Mips64MaltaProm::BuildImage(config, endian, 0x3000, image, &error));
        config.kernelCommandLine.assign(kMaltaPromStringCapacity, 'x');
        CHECK(!Mips64MaltaProm::BuildImage(config, endian, kRamBytes, image, &error));
    }
}

Mips64ISAPlugin* plugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

bool sameReplayState(const Mips64FullVmSnapshot& left,
                     const Mips64FullVmSnapshot& right) {
    if (left.cpuCount != right.cpuCount ||
        left.totalVirtualCycles != right.totalVirtualCycles ||
        left.activeCpuIndex != right.activeCpuIndex ||
        left.cpus.size() != right.cpus.size() ||
        left.scheduler.lastCPUIndex != right.scheduler.lastCPUIndex ||
        left.scheduler.numCPUs != right.scheduler.numCPUs ||
        left.scheduler.quantumSize != right.scheduler.quantumSize ||
        left.scheduler.cpuBundleCounters != right.scheduler.cpuBundleCounters ||
        left.uart.ier != right.uart.ier ||
        left.uart.fcrLastWrite != right.uart.fcrLastWrite ||
        left.uart.lcr != right.uart.lcr || left.uart.mcr != right.uart.mcr ||
        left.uart.scratch != right.uart.scratch ||
        left.uart.divisorLatchLow != right.uart.divisorLatchLow ||
        left.uart.divisorLatchHigh != right.uart.divisorLatchHigh ||
        left.uart.transmitCount != right.uart.transmitCount ||
        left.uart.fifoEnabled != right.uart.fifoEnabled ||
        left.uart.receiveTriggerLevel != right.uart.receiveTriggerLevel ||
        left.uart.receiveOverrun != right.uart.receiveOverrun ||
        left.uart.receiveFifo != right.uart.receiveFifo ||
        left.uart.pendingHostInput != right.uart.pendingHostInput ||
        left.console.bytes != right.console.bytes ||
        left.console.sequence != right.console.sequence ||
        left.physicalMemory.regions.size() != right.physicalMemory.regions.size()) {
        return false;
    }
    for (size_t i = 0; i < left.cpus.size(); ++i) {
        if (left.cpus[i].architecturalState != right.cpus[i].architecturalState ||
            left.cpus[i].cyclesExecuted != right.cpus[i].cyclesExecuted ||
            left.cpus[i].instructionsExecuted != right.cpus[i].instructionsExecuted ||
            left.cpus[i].executionState != right.cpus[i].executionState) return false;
    }
    for (size_t i = 0; i < left.physicalMemory.regions.size(); ++i) {
        const auto& a = left.physicalMemory.regions[i];
        const auto& b = right.physicalMemory.regions[i];
        if (a.info.base != b.info.base || a.info.size != b.info.size ||
            a.backendKind != b.backendKind || a.denseBytes != b.denseBytes ||
            a.sparseBytes != b.sparseBytes) return false;
    }
    return true;
}

uint32_t readGuestWord(VirtualMachine& vm, uint64_t address, bool isLittle) {
    uint8_t bytes[4]{};
    vm.getMemory().Read(address, bytes, sizeof(bytes));
    return read32(bytes, isLittle);
}

void readPhysicalBytes(VirtualMachine& vm, uint64_t address,
                       uint8_t* destination, size_t size) {
    PhysicalAddressMap* map = dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap();
    if (!map) throw std::runtime_error("expected MIPS64 Malta physical memory map");
    size_t done = 0;
    while (done < size) {
        const size_t remaining = size - done;
        const size_t width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        map->Read(address + done, destination + done, width);
        done += width;
    }
}

bool executeOneAndTrace(VirtualMachine& vm, Mips64ISAPlugin& cpu,
                        std::ostream* trace) {
    const uint64_t pc = cpu.core().pc();
    const uint64_t cycle = vm.getCyclesExecuted();
    const Mips64Probe probe = cpu.core().probeAddress(pc, Mips64Access::Fetch);
    uint32_t word = 0;
    if (probe.ok) {
        uint8_t bytes[4]{};
        vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
        word = read32(bytes, cpu.core().isLittleEndian());
    }
    CPUContext* context = vm.getCPUContext(0);
    if (!context) throw std::runtime_error("MIPS64 probe lost CPU context 0");
    const uint64_t instructionsBefore = context->instructionsExecuted;
    const uint64_t cyclesRun = vm.run(1);
    const bool vmStepSucceeded = cyclesRun == 1;
    const bool retired = context->instructionsExecuted == instructionsBefore + 1;
    if (trace && *trace) {
        *trace << "{\"type\":\"instruction\",\"cycle\":" << cycle
               << ",\"pc_va\":\"0x" << std::hex << std::setw(16)
               << std::setfill('0') << pc << "\",\"fetch_ok\":"
               << (probe.ok ? "true" : "false") << ",\"pc_pa\":\"0x"
               << std::setw(9) << (probe.ok ? probe.phys : 0)
               << "\",\"instruction\":\"0x" << std::setw(8) << word
               << "\",\"outcome\":\"" << (retired ? "retired" : "fault")
               << "\",\"exc_code\":" << cpu.core().causeExcCode()
               << ",\"epc\":\"0x" << std::setw(16) << cpu.core().cp0Epc()
               << "\",\"cause\":\"0x" << std::setw(16) << cpu.core().cp0Cause()
               << "\",\"badvaddr\":\"0x" << std::setw(16)
               << cpu.core().cp0BadVAddr() << "\",\"mips_fault_code\":"
               << std::dec << static_cast<uint32_t>(cpu.core().fault())
               << ",\"mips_fault_name\":\""
               << Mips64Cpu::faultName(cpu.core().fault()) << "\"}\n";
    }
    return vmStepSucceeded && retired;
}

void runToEntry(VirtualMachine& vm, Mips64ISAPlugin& cpu, uint64_t entry,
                std::ofstream* trace) {
    for (unsigned i = 0; i < 256 && cpu.core().pc() != entry; ++i) {
        if (!executeOneAndTrace(vm, cpu, trace)) {
            throw std::runtime_error("VM stopped before entering the ELF executable");
        }
    }
    if (cpu.core().pc() != entry) {
        throw std::runtime_error("ROM-to-ELF transfer exceeded the 256-instruction trace budget");
    }
    if (trace && *trace) {
        *trace << "{\"type\":\"stop\",\"reason\":\"elf_entry\",\"pc_va\":\"0x"
               << std::hex << std::setw(16) << std::setfill('0') << entry
               << "\",\"cycle\":" << std::dec << vm.getCyclesExecuted() << "}\n";
    }
}

void runToCompletion(VirtualMachine& vm, Mips64ISAPlugin& cpu, bool isLittle) {
    for (unsigned i = 0; i < 2048; ++i) {
        if (!executeOneAndTrace(vm, cpu, nullptr)) {
            throw std::runtime_error("guest VM stopped before publishing completion");
        }
        const uint32_t marker = readGuestWord(
            vm, kMaltaElfCompletionPhysicalAddress, isLittle);
        if (marker == kSuccessWord) return;
        if (marker == kFailureWord) {
            throw std::runtime_error("independent ELF guest published its failure marker");
        }
    }
    throw std::runtime_error("ELF guest exceeded the 2048-instruction execution budget");
}

void testTransactionalVmAndSnapshotReplay(const std::filesystem::path& traceDirectory) {
    std::filesystem::create_directories(traceDirectory);
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool isLittle = little(endian);
        const auto fixture = loadFixture(isLittle);
        VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
        uint8_t sentinel[4]{0xA1, 0xB2, 0xC3, 0xD4};
        CHECK(vm.loadProgram(sentinel, sizeof(sentinel), 0x800));
        const uint64_t originalIp = vm.getIP();
        std::string error;
        MaltaBootstrapConfig badConfig;
        badConfig.endianness = endian;
        CHECK(!vm.configureMips64MaltaElfHandoff(
            fixture.data(), fixture.size() - 1, badConfig, &error));
        CHECK(!error.empty());
        CHECK(vm.getMips64MaltaBootstrapBoard() == nullptr);
        CHECK(vm.getIP() == originalIp);
        uint8_t afterFailure[4]{};
        vm.getMemory().Read(0x800, afterFailure, sizeof(afterFailure));
        CHECK(std::equal(std::begin(sentinel), std::end(sentinel), std::begin(afterFailure)));
        CHECK(dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap() == nullptr);

        MaltaBootstrapConfig config;
        config.endianness = endian;
        config.prom.kernelImageName = isLittle ? "guidexos_mips64_le" : "guidexos_mips64_be";
        CHECK(vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
        CHECK(error.empty());
        MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
        CHECK(board != nullptr);
        CHECK(board && board->Config().bootMode == MaltaBootMode::ElfKernelHandoff);
        CHECK(board && board->Config().kernelEntryPoint == kEntryPoint);
        CHECK(board && board->Config().guestRamSize == kRamBytes);
        CHECK(vm.init());
        Mips64ISAPlugin* cpu = plugin(vm);
        CHECK(cpu != nullptr);
        if (!cpu || !board) continue;
        CHECK(cpu->core().pc() == kMaltaResetVector);
        CHECK(cpu->core().isLittleEndian() == isLittle);

        MaltaPromImage expectedProm;
        CHECK(Mips64MaltaProm::BuildImage(config.prom, endian, kRamBytes,
                                          expectedProm, &error));
        std::vector<uint8_t> actualProm(expectedProm.bytes.size());
        readPhysicalBytes(vm, expectedProm.physicalAddress,
                          actualProm.data(), actualProm.size());
        CHECK(actualProm == expectedProm.bytes);

        uint8_t text[16]{};
        readPhysicalBytes(vm, 0x10000, text, sizeof(text));
        CHECK(std::equal(text, text + sizeof(text), fixture.begin() + 0x10000));
        uint8_t zeroBss[4]{1, 1, 1, 1};
        vm.getMemory().Read(0x12020, zeroBss, sizeof(zeroBss));
        CHECK(std::all_of(std::begin(zeroBss), std::end(zeroBss),
                          [](uint8_t byte) { return byte == 0; }));

        std::ofstream trace(traceDirectory /
            (isLittle ? "mips64_elf_handoff_le.jsonl" : "mips64_elf_handoff_be.jsonl"),
            std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(trace));
        for (unsigned i = 0; i < 8; ++i) CHECK(executeOneAndTrace(vm, *cpu, &trace));
        const Mips64FullVmSnapshot preHandoff = vm.captureMips64FullSnapshot();
        runToEntry(vm, *cpu, kEntryPoint, &trace);
        trace.close();

        CHECK(cpu->core().readGpr(4) == 2);
        CHECK(cpu->core().readGpr(5) == 0xFFFFFFFF80002000ULL);
        CHECK(cpu->core().readGpr(6) == 0xFFFFFFFF80002008ULL);
        CHECK(cpu->core().readGpr(7) == kRamBytes);
        CHECK((cpu->core().cp0Status() & kStatusBitKx) == 0);
        CHECK(cpu->core().delayArmed() == false);
        const Mips64FullVmSnapshot postHandoff = vm.captureMips64FullSnapshot();

        runToCompletion(vm, *cpu, isLittle);
        CHECK(vm.getState() != VMState::ERROR);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(readGuestWord(vm, 0x12020, isLittle) == kSuccessWord);
        CHECK(readGuestWord(vm, 0x12000, isLittle) == 0x66778899U);
        CHECK(board->Uart()->InspectState().at("transmit_count") ==
              sizeof(kGuestMessage) - 1);
        const Mips64FullVmSnapshot finalState = vm.captureMips64FullSnapshot();

        CHECK(vm.restoreMips64FullSnapshot(preHandoff, &error));
        runToCompletion(vm, *cpu, isLittle);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(sameReplayState(finalState, vm.captureMips64FullSnapshot()));

        CHECK(vm.restoreMips64FullSnapshot(postHandoff, &error));
        CHECK(cpu->core().pc() == kEntryPoint);
        runToCompletion(vm, *cpu, isLittle);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(sameReplayState(finalState, vm.captureMips64FullSnapshot()));
    }
}

void testIndependentVmImageIsolation() {
    const auto fixture = loadFixture(true);
    VirtualMachine first(static_cast<size_t>(kRamBytes), "MIPS64");
    VirtualMachine second(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig firstConfig;
    MaltaBootstrapConfig secondConfig;
    secondConfig.prom.kernelImageName = "isolated-second-guest";
    std::string error;
    CHECK(first.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), firstConfig, &error));
    CHECK(second.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), secondConfig, &error));
    CHECK(first.init());
    CHECK(second.init());
    Mips64ISAPlugin* firstCpu = plugin(first);
    Mips64ISAPlugin* secondCpu = plugin(second);
    CHECK(firstCpu != nullptr && secondCpu != nullptr);
    if (!firstCpu || !secondCpu) return;

    CHECK(firstCpu->core().pc() == kMaltaResetVector);
    CHECK(secondCpu->core().pc() == kMaltaResetVector);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == 0);
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());
    runToEntry(first, *firstCpu, kEntryPoint, nullptr);
    runToCompletion(first, *firstCpu, true);

    CHECK(readGuestWord(first, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == 0);
    CHECK(readGuestWord(first, 0x12020, true) == kSuccessWord);
    CHECK(readGuestWord(second, 0x12020, true) == 0);
    CHECK(first.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
          std::vector<uint8_t>(kGuestMessage,
                               kGuestMessage + sizeof(kGuestMessage) - 1));
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());

    runToEntry(second, *secondCpu, kEntryPoint, nullptr);
    runToCompletion(second, *secondCpu, true);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
          std::vector<uint8_t>(kGuestMessage,
                               kGuestMessage + sizeof(kGuestMessage) - 1));
}

void testPromCollisionFailsWithoutBoardOrRamMutation() {
    const auto fixture = loadFixture(true);
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    const uint32_t sentinel = 0xA5A55A5A;
    CHECK(vm.loadProgram(reinterpret_cast<const uint8_t*>(&sentinel), sizeof(sentinel), 0x1800));
    MaltaBootstrapConfig config;
    config.prom.physicalAddress = 0x10000; // conflicts with the first PT_LOAD.
    std::string error;
    CHECK(!vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
    CHECK(!error.empty());
    CHECK(vm.getMips64MaltaBootstrapBoard() == nullptr);
    uint32_t after = 0;
    vm.getMemory().Read(0x1800, reinterpret_cast<uint8_t*>(&after), sizeof(after));
    CHECK(after == sentinel);
    CHECK(dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap() == nullptr);

    VirtualMachine completionVm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig completionCollision;
    completionCollision.prom.physicalAddress = 0x3040; // table would cover 0x4000.
    CHECK(!completionVm.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), completionCollision, &error));
    CHECK(error.find("completion") != std::string::npos);
    CHECK(completionVm.getMips64MaltaBootstrapBoard() == nullptr);
}

void testMismatchedElfPromSnapshotRejected() {
    const auto fixture = loadFixture(true);
    std::string error;
    VirtualMachine source(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig sourceConfig;
    CHECK(source.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), sourceConfig, &error));
    CHECK(source.init());
    const Mips64FullVmSnapshot sourceSnapshot = source.captureMips64FullSnapshot();

    VirtualMachine destination(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig destinationConfig;
    destinationConfig.prom.kernelImageName = "different-kernel-name";
    CHECK(destination.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), destinationConfig, &error));
    CHECK(destination.init());
    const uint64_t originalPc = destination.getIP();
    std::array<uint8_t, 20> originalName{};
    readPhysicalBytes(destination, 0x2040, originalName.data(), originalName.size());
    CHECK(!destination.restoreMips64FullSnapshot(sourceSnapshot, &error));
    CHECK(error.find("profile") != std::string::npos);
    CHECK(destination.getIP() == originalPc);
    std::array<uint8_t, 20> afterName{};
    readPhysicalBytes(destination, 0x2040, afterName.data(), afterName.size());
    CHECK(afterName == originalName);

    auto changedImage = fixture;
    changedImage[0x12000] ^= 0x01; // Keep the ELF valid, change its initialized data.
    VirtualMachine imageDestination(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig imageDestinationConfig;
    CHECK(imageDestination.configureMips64MaltaElfHandoff(
        changedImage.data(), changedImage.size(), imageDestinationConfig, &error));
    CHECK(imageDestination.init());
    const uint64_t imageDestinationPc = imageDestination.getIP();
    const uint32_t changedDataBefore = readGuestWord(imageDestination, 0x12000, true);
    CHECK(changedDataBefore != 0x11223344U);
    CHECK(!imageDestination.restoreMips64FullSnapshot(sourceSnapshot, &error));
    CHECK(error.find("profile") != std::string::npos);
    CHECK(imageDestination.getIP() == imageDestinationPc);
    CHECK(readGuestWord(imageDestination, 0x12000, true) == changedDataBefore);
}

int runBoundedKernelProbe(const std::filesystem::path& imagePath,
                          const std::filesystem::path& tracePath,
                          unsigned instructionBudget = 200000,
                          bool continuePastDsrl32Frontier = false,
                          bool continuePastLldFrontier = false) {
    constexpr uint64_t probeRamBytes = 256ULL * 1024 * 1024;
    constexpr unsigned traceInstructionLimit = 256;
    constexpr unsigned continuationTraceLimit = 512;
    constexpr uint64_t formerFrontierPc = 0xFFFFFFFF80917700ULL;
    constexpr uint32_t formerFrontierWord = 0x000C643EU;
    constexpr uint64_t sltiuFrontierPc = 0xFFFFFFFF80B4F0B4ULL;
    constexpr uint32_t sltiuFrontierWord = 0x2C420021U;
    constexpr uint64_t lldFrontierPc = 0xFFFFFFFF8091A338ULL;
    constexpr uint32_t lldFrontierWord = 0xD0850000U;
    const auto startTime = std::chrono::steady_clock::now();
    const auto image = readFile(imagePath);
    if (image.empty()) throw std::runtime_error("kernel image is empty or unreadable");

    Mips64ElfLoadOptions loadOptions;
    loadOptions.guestEndianness = Mips64Endianness::Big;
    loadOptions.guestRamSize = probeRamBytes;
    loadOptions.reservedPhysicalRanges = {
        {kMaltaPromDefaultPhysicalAddress, kMaltaPromImageBytes},
        {kMaltaElfCompletionPhysicalAddress, sizeof(uint32_t)}
    };
    Mips64ElfLoadPlan plan;
    std::string error;
    if (!Mips64ElfLoader::BuildPlan(image.data(), image.size(), loadOptions,
                                    plan, &error)) {
        throw std::runtime_error("kernel ELF rejected: " + error);
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    VirtualMachine vm(static_cast<size_t>(probeRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = probeRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("kernel Malta handoff setup failed: " + error);
    }
    if (!vm.init()) throw std::runtime_error("production VM initialization failed");
    Mips64ISAPlugin* cpu = plugin(vm);
    if (!cpu) throw std::runtime_error("production VM has no MIPS64 CPU plugin");

    std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create bounded kernel trace");

    std::set<uint64_t> distinctPcs;
    unsigned attempts = 0;
    unsigned retired = 0;
    bool entryReached = false;
    unsigned retiredBeforeEntry = 0;
    bool formerFrontierVerified = false;
    bool sltiuFrontierVerified = false;
    bool lldFrontierVerified = false;
    bool lldReplayVerified = false;
    unsigned retiredAfterLld = 0;
    Mips64FullVmSnapshot lldActiveSnapshot;
    std::string formerFrontierRecord;
    std::string sltiuFrontierRecord;
    std::string lldFrontierRecord;
    std::string lldScdRecord;
    std::string lldReplayRecord;
    std::string lldPreInstructionRecord;
    std::ostringstream frontierInstructionTrace;
    std::ostringstream preSltiuContinuationTrace;
    std::ostringstream postSltiuContinuationTrace;
    unsigned continuationTraceRecords = 0;
    std::string stopReason = "instruction_budget_exhausted";
    uint64_t stopPc = cpu->core().pc();
    uint64_t stopPhysical = 0;
    uint32_t stopWord = 0;
    bool stopFetchOk = false;
    for (; attempts < instructionBudget; ++attempts) {
        stopPc = cpu->core().pc();
        distinctPcs.insert(stopPc);
        if (stopPc == plan.entryPoint && !entryReached) {
            entryReached = true;
            retiredBeforeEntry = retired;
        }
        const Mips64Probe fetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = fetch.ok;
        stopPhysical = fetch.ok ? fetch.phys : 0;
        stopWord = 0;
        if (fetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(fetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }
        if (stopPc == lldFrontierPc && stopFetchOk &&
            stopWord == lldFrontierWord && lldPreInstructionRecord.empty()) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const uint64_t base = cpu->core().readGpr(decoded.rs);
            const uint64_t effective = base +
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(decoded.imm)));
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Load);
            if (!dataProbe.ok) throw std::runtime_error("authenticated LLD data address did not translate");
            uint8_t dataBytes[8]{};
            vm.getMemory().Read(dataProbe.phys, dataBytes, sizeof(dataBytes));
            uint64_t loaded = 0;
            for (uint8_t byte : dataBytes) loaded = (loaded << 8) | byte;
            std::ostringstream event;
            event << "{\"type\":\"lld_pre_instruction_auth\",\"retired\":" << retired
                  << ",\"attempts\":" << attempts << ",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"decoded_valid\":" << (decoded.valid ? "true" : "false")
                  << ",\"decode_fault\":\"" << Mips64Cpu::faultName(decoded.fault)
                  << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3fU)
                  << ",\"rs\":" << unsigned(decoded.rs) << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"signed_offset\":" << static_cast<int>(static_cast<int16_t>(decoded.imm))
                  << ",\"base\":\"0x" << std::hex << std::setw(16) << base
                  << "\",\"effective_va\":\"0x" << std::setw(16) << effective
                  << "\",\"translated_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"bytes_be\":\"";
            for (uint8_t byte : dataBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"loaded_be64\":\"0x" << std::hex << std::setw(16) << loaded
                  << "\",\"gpr4\":\"0x" << std::setw(16) << cpu->core().readGpr(4)
                  << "\",\"gpr5\":\"0x" << std::setw(16) << cpu->core().readGpr(5)
                  << "\",\"status\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                  << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                  << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
                  << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
                  << "\",\"cp0_count\":" << std::dec << cpu->core().cp0Count()
                  << ",\"vm_cycles\":" << vm.getCyclesExecuted()
                  << ",\"architectural_exception\":false,\"uart_bytes\":"
                  << (vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size()) << "}";
            lldPreInstructionRecord = event.str();
        }
        const bool recordingPrefix = attempts < traceInstructionLimit;
        bool didRetire = false;
        if (continuePastDsrl32Frontier && stopPc == formerFrontierPc) {
            if (retired != 150041 || vm.getCyclesExecuted() != 150041 ||
                !stopFetchOk || stopWord != formerFrontierWord) {
                throw std::runtime_error("authentic Phase 12 prefix/frontier did not reproduce");
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            if (!decoded.valid || decoded.op != Mips64Op::Dsrl32 ||
                decoded.rs != 0 || decoded.rt != 12 || decoded.rd != 12 || decoded.sa != 16) {
                throw std::runtime_error("former Linux frontier did not decode as the recorded DSRL32");
            }
            const uint64_t source = cpu->core().readGpr(decoded.rt);
            const uint64_t expectedResult = source >> (static_cast<unsigned>(decoded.sa) + 32U);
            const uint64_t beforeStatus = cpu->core().cp0Status();
            const uint64_t beforeCause = cpu->core().cp0Cause();
            const uint64_t beforeEpc = cpu->core().cp0Epc();
            const uint64_t beforeBadVAddr = cpu->core().cp0BadVAddr();
            const uint64_t beforeCount = cpu->core().cp0Count();
            const uint64_t beforeHi = cpu->core().hi();
            const uint64_t beforeLo = cpu->core().lo();
            const uint64_t beforeDest = cpu->core().readGpr(decoded.rd);
            if (source != 0 || expectedResult != 0 || beforeCount != 150041 ||
                beforeStatus != 0x00400080ULL || beforeCause != 0 || beforeEpc != 0 ||
                beforeBadVAddr != 0 || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("architectural state before the old frontier differs from Phase 12");
            }
            const Mips64FullVmSnapshot frontierSnapshot = vm.captureMips64FullSnapshot();
            if (!frontierSnapshot.ValidateIntegrity() || frontierSnapshot.formatVersion != 3 ||
                frontierSnapshot.uartComponentVersion != 4 ||
                frontierSnapshot.irqRoutingComponentVersion != 1 ||
                frontierSnapshot.cpuComponentVersion != 6) {
                throw std::runtime_error("authentic frontier snapshot failed integrity/version checks");
            }
            CPUContext* frontierContext = vm.getCPUContext(0);
            const auto* frontierBoard = vm.getMips64MaltaBootstrapBoard();
            if (!frontierContext || !frontierBoard || !frontierBoard->ConsoleSink() ||
                !frontierBoard->Uart()) {
                throw std::runtime_error("authentic frontier snapshot lost Malta CPU/device state");
            }
            didRetire = executeOneAndTrace(vm, *cpu, &frontierInstructionTrace);
            if (!didRetire || cpu->core().pc() != formerFrontierPc + 4 ||
                cpu->core().readGpr(decoded.rd) != expectedResult ||
                cpu->core().cp0Status() != beforeStatus || cpu->core().cp0Cause() != beforeCause ||
                cpu->core().cp0Epc() != beforeEpc || cpu->core().cp0BadVAddr() != beforeBadVAddr ||
                cpu->core().cp0Count() != beforeCount + 1 || cpu->core().hi() != beforeHi ||
                cpu->core().lo() != beforeLo || cpu->core().fault() != Mips64Fault::None ||
                vm.getState() == VMState::ERROR) {
                throw std::runtime_error("production VM did not retire DSRL32 with the expected state");
            }
            const std::vector<uint8_t> firstCpuState = cpu->core().serialize();
            const std::vector<uint8_t> firstConsoleBytes = frontierBoard->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstUartState = frontierBoard->Uart()->CaptureSnapshot();
            const uint64_t firstVmCycles = vm.getCyclesExecuted();
            const uint64_t firstRetiredCount = frontierContext->instructionsExecuted;
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(frontierSnapshot, &restoreError) ||
                !restoreError.empty()) {
                throw std::runtime_error("authentic frontier snapshot restore failed: " + restoreError);
            }
            const bool replayRetired = executeOneAndTrace(vm, *cpu, nullptr);
            const Ns16550Uart::Snapshot replayUartState = frontierBoard->Uart()->CaptureSnapshot();
            uint8_t replayInstructionBytes[4]{};
            vm.getMemory().Read(stopPhysical, replayInstructionBytes,
                                sizeof(replayInstructionBytes));
            if (!replayRetired || cpu->core().serialize() != firstCpuState ||
                frontierContext->instructionsExecuted != firstRetiredCount ||
                vm.getCyclesExecuted() != firstVmCycles ||
                frontierBoard->ConsoleSink()->Bytes() != firstConsoleBytes ||
                replayUartState.transmitCount != firstUartState.transmitCount ||
                replayUartState.virtualTimeCycle != firstUartState.virtualTimeCycle ||
                read32(replayInstructionBytes, false) != formerFrontierWord) {
                throw std::runtime_error("authentic frontier snapshot replay diverged after DSRL32");
            }
            std::ostringstream event;
            event << "{\"type\":\"dsrl32_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << formerFrontierPc
                  << "\",\"instruction\":\"0x" << std::setw(8) << formerFrontierWord
                  << "\",\"rs\":" << std::dec << unsigned(decoded.rs)
                  << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"rd\":" << unsigned(decoded.rd)
                  << ",\"sa\":" << unsigned(decoded.sa)
                  << ",\"shift\":" << (unsigned(decoded.sa) + 32U)
                  << ",\"source\":\"0x" << std::hex << std::setw(16) << source
                  << "\",\"destination_before\":\"0x" << std::setw(16) << beforeDest
                  << "\",\"expected\":\"0x" << std::setw(16) << expectedResult
                  << "\",\"destination_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rd)
                  << "\",\"pc_after\":\"0x" << std::setw(16) << cpu->core().pc()
                  << "\",\"status_before\":\"0x" << std::setw(16) << beforeStatus
                  << "\",\"status_after\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                  << "\",\"cause_before\":\"0x" << std::setw(16) << beforeCause
                  << "\",\"cause_after\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                  << "\",\"count_before\":" << std::dec << beforeCount
                  << ",\"count_after\":" << cpu->core().cp0Count()
                  << ",\"hi_before\":\"0x" << std::hex << std::setw(16) << beforeHi
                  << "\",\"hi_after\":\"0x" << std::setw(16) << cpu->core().hi()
                  << "\",\"lo_before\":\"0x" << std::setw(16) << beforeLo
                  << "\",\"lo_after\":\"0x" << std::setw(16) << cpu->core().lo()
                  << "\",\"vm_cycle_after\":" << std::dec << vm.getCyclesExecuted()
                  << ",\"snapshot_replay_verified\":true,\"snapshot_format_version\":"
                  << frontierSnapshot.formatVersion
                  << ",\"snapshot_uart_version\":" << frontierSnapshot.uartComponentVersion
                  << ",\"snapshot_irq_version\":" << frontierSnapshot.irqRoutingComponentVersion
                  << ",\"snapshot_cpu_version\":" << frontierSnapshot.cpuComponentVersion
                  << "}";
            formerFrontierRecord = event.str();
            formerFrontierVerified = true;
        } else if (continuePastDsrl32Frontier && formerFrontierVerified &&
                   stopPc == sltiuFrontierPc) {
            if (!stopFetchOk || stopPhysical != 0x00B4F0B4ULL ||
                stopWord != sltiuFrontierWord || retired != 150287U ||
                vm.getCyclesExecuted() != 150287U || cpu->core().readGpr(2) != 6ULL ||
                cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Phase 13 SLTIU frontier did not reproduce");
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            if (!decoded.valid || decoded.op != Mips64Op::Sltiu || decoded.rs != 2 ||
                decoded.rt != 2 || decoded.imm != 33) {
                throw std::runtime_error("authentic Linux instruction did not decode as SLTIU $2,$2,33");
            }
            const uint64_t source = cpu->core().readGpr(decoded.rs);
            const uint64_t immediate = static_cast<uint64_t>(
                static_cast<int64_t>(static_cast<int16_t>(decoded.imm)));
            const uint64_t expectedResult = source < immediate ? 1ULL : 0ULL;
            const uint64_t beforeStatus = cpu->core().cp0Status();
            const uint64_t beforeCause = cpu->core().cp0Cause();
            const uint64_t beforeEpc = cpu->core().cp0Epc();
            const uint64_t beforeBadVAddr = cpu->core().cp0BadVAddr();
            const uint64_t beforeCount = cpu->core().cp0Count();
            const uint64_t beforeHi = cpu->core().hi();
            const uint64_t beforeLo = cpu->core().lo();
            const uint64_t beforeDest = cpu->core().readGpr(decoded.rt);
            if (source != 6ULL || immediate != 33ULL || expectedResult != 1ULL ||
                beforeDest != 6ULL || beforeCount != 150287U ||
                beforeStatus != 0x00400080ULL || beforeCause != 0 || beforeEpc != 0 ||
                beforeBadVAddr != 0) {
                throw std::runtime_error("architectural state before authentic SLTIU differs from Phase 13");
            }

            const Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
            if (!snapshot.ValidateIntegrity() || snapshot.formatVersion != 3 ||
                snapshot.uartComponentVersion != 4 || snapshot.irqRoutingComponentVersion != 1 ||
                snapshot.cpuComponentVersion != 6) {
                throw std::runtime_error("authentic SLTIU snapshot failed integrity/version checks");
            }
            CPUContext* context = vm.getCPUContext(0);
            const auto* board = vm.getMips64MaltaBootstrapBoard();
            if (!context || !board || !board->ConsoleSink() || !board->Uart()) {
                throw std::runtime_error("authentic SLTIU snapshot lost Malta CPU/device state");
            }
            const uint64_t beforeCycles = vm.getCyclesExecuted();
            const uint64_t beforeRetired = context->instructionsExecuted;
            const std::vector<uint8_t> beforeConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot beforeUart = board->Uart()->CaptureSnapshot();
            didRetire = executeOneAndTrace(vm, *cpu, &preSltiuContinuationTrace);
            ++continuationTraceRecords;
            if (!didRetire || cpu->core().pc() != sltiuFrontierPc + 4 ||
                cpu->core().readGpr(decoded.rt) != expectedResult ||
                cpu->core().cp0Status() != beforeStatus || cpu->core().cp0Cause() != beforeCause ||
                cpu->core().cp0Epc() != beforeEpc || cpu->core().cp0BadVAddr() != beforeBadVAddr ||
                cpu->core().cp0Count() != beforeCount + 1 || cpu->core().hi() != beforeHi ||
                cpu->core().lo() != beforeLo || cpu->core().fault() != Mips64Fault::None ||
                vm.getState() == VMState::ERROR || context->instructionsExecuted != beforeRetired + 1 ||
                vm.getCyclesExecuted() != beforeCycles + 1) {
                throw std::runtime_error("production VM did not retire authentic SLTIU with the expected state");
            }
            const std::vector<uint8_t> firstCpu = cpu->core().serialize();
            const std::vector<uint8_t> firstConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstUart = board->Uart()->CaptureSnapshot();
            const uint64_t firstCycles = vm.getCyclesExecuted();
            const uint64_t firstRetired = context->instructionsExecuted;
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(snapshot, &restoreError) || !restoreError.empty()) {
                throw std::runtime_error("authentic SLTIU snapshot restore failed: " + restoreError);
            }
            uint8_t restoredWordBytes[4]{};
            vm.getMemory().Read(stopPhysical, restoredWordBytes, sizeof(restoredWordBytes));
            const Ns16550Uart::Snapshot restoredUart = board->Uart()->CaptureSnapshot();
            if (cpu->core().pc() != sltiuFrontierPc ||
                cpu->core().serialize() != snapshot.cpus[0].architecturalState ||
                context->instructionsExecuted != beforeRetired ||
                vm.getCyclesExecuted() != beforeCycles || board->ConsoleSink()->Bytes() != beforeConsole ||
                restoredUart.transmitCount != beforeUart.transmitCount ||
                restoredUart.virtualTimeCycle != beforeUart.virtualTimeCycle ||
                read32(restoredWordBytes, false) != sltiuFrontierWord) {
                throw std::runtime_error("authentic SLTIU restore changed CPU, RAM or UART before reexecution");
            }
            const bool replayRetired = executeOneAndTrace(vm, *cpu, nullptr);
            const Ns16550Uart::Snapshot replayUart = board->Uart()->CaptureSnapshot();
            if (!replayRetired || cpu->core().serialize() != firstCpu ||
                context->instructionsExecuted != firstRetired || vm.getCyclesExecuted() != firstCycles ||
                board->ConsoleSink()->Bytes() != firstConsole ||
                replayUart.transmitCount != firstUart.transmitCount ||
                replayUart.virtualTimeCycle != firstUart.virtualTimeCycle ||
                cpu->core().readGpr(decoded.rt) != expectedResult ||
                cpu->core().pc() != sltiuFrontierPc + 4) {
                throw std::runtime_error("authentic SLTIU snapshot replay diverged");
            }
            std::ostringstream event;
            event << "{\"type\":\"sltiu_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << sltiuFrontierPc
                  << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3FU)
                  << ",\"rs\":" << unsigned(decoded.rs) << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"immediate\":" << static_cast<int>(static_cast<int16_t>(decoded.imm))
                  << ",\"source\":\"0x" << std::hex << std::setw(16) << source
                  << "\",\"sign_extended_immediate\":\"0x" << std::setw(16) << immediate
                  << "\",\"destination_before\":\"0x" << std::setw(16) << beforeDest
                  << "\",\"expected\":\"0x" << std::setw(16) << expectedResult
                  << "\",\"destination_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rt)
                  << "\",\"pc_after\":\"0x" << std::setw(16) << cpu->core().pc()
                  << "\",\"status_before\":\"0x" << std::setw(16) << beforeStatus
                  << "\",\"cause_before\":\"0x" << std::setw(16) << beforeCause
                  << "\",\"epc_before\":\"0x" << std::setw(16) << beforeEpc
                  << "\",\"badvaddr_before\":\"0x" << std::setw(16) << beforeBadVAddr
                  << "\",\"count_before\":" << std::dec << beforeCount
                  << ",\"count_after\":" << cpu->core().cp0Count()
                  << ",\"retired_before\":" << beforeRetired
                  << ",\"retired_after\":" << context->instructionsExecuted
                  << ",\"vm_cycles_before\":" << beforeCycles
                  << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                  << ",\"snapshot_replay_verified\":true,\"snapshot_format_version\":"
                  << snapshot.formatVersion << ",\"snapshot_uart_version\":" << snapshot.uartComponentVersion
                  << ",\"snapshot_irq_version\":" << snapshot.irqRoutingComponentVersion
                  << ",\"snapshot_cpu_version\":" << snapshot.cpuComponentVersion << "}";
            sltiuFrontierRecord = event.str();
            sltiuFrontierVerified = true;
        } else if (continuePastLldFrontier && stopPc == lldFrontierPc) {
            if (!formerFrontierVerified || !sltiuFrontierVerified || !stopFetchOk ||
                stopPhysical != 0x00091A338ULL || stopWord != lldFrontierWord ||
                retired != 150313U || vm.getCyclesExecuted() != 150313U ||
                cpu->core().cp0Count() != 150313U || cpu->core().readGpr(4) != 0xFFFFFFFF80B0D078ULL ||
                cpu->core().readGpr(5) != 1ULL || cpu->core().cp0Status() != 0x00400080ULL ||
                cpu->core().cp0Cause() != 0 || cpu->core().cp0Epc() != 0 ||
                cpu->core().cp0BadVAddr() != 0 || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Phase 15 LLD prestate did not reproduce");
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const Mips64Probe dataProbe = cpu->core().probeAddress(
                cpu->core().readGpr(decoded.rs) +
                    static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(decoded.imm))),
                Mips64Access::Load);
            uint8_t beforeBytes[8]{};
            if (!decoded.valid || decoded.op != Mips64Op::Lld || decoded.rs != 4 ||
                decoded.rt != 5 || !dataProbe.ok || dataProbe.phys != 0x000B0D078ULL ||
                dataProbe.cacheAttribute != 3) {
                throw std::runtime_error("authenticated Linux LLD decode/translation changed");
            }
            vm.getMemory().Read(dataProbe.phys, beforeBytes, sizeof(beforeBytes));
            uint64_t loaded = 0;
            for (uint8_t byte : beforeBytes) loaded = (loaded << 8) | byte;
            if (loaded != 0) throw std::runtime_error("authenticated Linux LLD backing data changed");

            {
                const Mips64FullVmSnapshot beforeLld = vm.captureMips64FullSnapshot();
                CPUContext* beforeContext = vm.getCPUContext(0);
                if (!beforeLld.ValidateIntegrity() || beforeLld.formatVersion != 3 ||
                    beforeLld.cpuComponentVersion != 6 || beforeLld.memoryComponentVersion != 2 ||
                    beforeLld.cpus.size() != 1 || !beforeContext ||
                    beforeLld.cpus[0].architecturalState != cpu->core().serialize() ||
                    cpu->core().reservationState().valid) {
                    throw std::runtime_error("pre-LLD full snapshot did not capture the authenticated CPU state");
                }
                const uint64_t beforeInstructions = beforeContext->instructionsExecuted;
                const uint64_t beforeCycles = vm.getCyclesExecuted();

                // Execute once, preserve the architectural result, then restore the
                // exact pre-LLD VM and replay the encoded instruction.
                if (!executeOneAndTrace(vm, *cpu, nullptr) ||
                    cpu->core().pc() != lldFrontierPc + 4 ||
                    cpu->core().readGpr(5) != loaded || cpu->core().fault() != Mips64Fault::None ||
                    !cpu->core().reservationState().valid ||
                    vm.getState() == VMState::ERROR) {
                    throw std::runtime_error("first authentic LLD execution did not establish a valid reservation");
                }
                const std::vector<uint8_t> firstLldCpuState = cpu->core().serialize();
                const Mips64ReservationState firstReservation = cpu->core().reservationState();
                const uint64_t firstInstructions = vm.getCPUContext(0)->instructionsExecuted;
                const uint64_t firstCycles = vm.getCyclesExecuted();
                uint8_t firstLldBytes[8]{};
                vm.getMemory().Read(dataProbe.phys, firstLldBytes, sizeof(firstLldBytes));

                std::string restoreError;
                if (!vm.restoreMips64FullSnapshot(beforeLld, &restoreError) || !restoreError.empty() ||
                    cpu->core().serialize() != beforeLld.cpus[0].architecturalState ||
                    vm.getCPUContext(0)->instructionsExecuted != beforeInstructions ||
                    vm.getCyclesExecuted() != beforeCycles ||
                    cpu->core().reservationState().valid) {
                    throw std::runtime_error("pre-LLD full snapshot restore changed CPU, reservation, or counters: " + restoreError);
                }

                didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
                const Mips64ReservationState replayReservation = cpu->core().reservationState();
                uint8_t replayLldBytes[8]{};
                vm.getMemory().Read(dataProbe.phys, replayLldBytes, sizeof(replayLldBytes));
                CPUContext* replayContext = vm.getCPUContext(0);
                if (!didRetire || !replayContext ||
                    cpu->core().serialize() != firstLldCpuState ||
                    replayReservation.valid != firstReservation.valid ||
                    replayReservation.size != firstReservation.size ||
                    replayReservation.cacheAttribute != firstReservation.cacheAttribute ||
                    replayReservation.virtualAddress != firstReservation.virtualAddress ||
                    replayReservation.physicalAddress != firstReservation.physicalAddress ||
                    replayReservation.backingId != firstReservation.backingId ||
                    replayReservation.granuleOffset != firstReservation.granuleOffset ||
                    replayReservation.generation != firstReservation.generation ||
                    replayContext->instructionsExecuted != firstInstructions ||
                    vm.getCyclesExecuted() != firstCycles ||
                    !std::equal(std::begin(firstLldBytes), std::end(firstLldBytes),
                                std::begin(replayLldBytes)) ||
                    !std::equal(std::begin(beforeBytes), std::end(beforeBytes),
                                std::begin(replayLldBytes))) {
                    throw std::runtime_error("pre-LLD snapshot replay diverged in result, reservation, memory, or counters");
                }
            }

            if (!didRetire || cpu->core().pc() != lldFrontierPc + 4 ||
                cpu->core().readGpr(5) != loaded || cpu->core().fault() != Mips64Fault::None ||
                !cpu->core().reservationState().valid ||
                cpu->core().reservationState().virtualAddress != 0xFFFFFFFF80B0D078ULL ||
                cpu->core().reservationState().physicalAddress != 0x000B0D078ULL ||
                cpu->core().reservationState().cacheAttribute != dataProbe.cacheAttribute ||
                vm.getState() == VMState::ERROR) {
                throw std::runtime_error("production VM did not retire authentic LLD with an active reservation");
            }
            lldActiveSnapshot = vm.captureMips64FullSnapshot();
            if (!lldActiveSnapshot.ValidateIntegrity() || lldActiveSnapshot.formatVersion != 3 ||
                lldActiveSnapshot.cpuComponentVersion != 6 ||
                lldActiveSnapshot.memoryComponentVersion != 2 ||
                lldActiveSnapshot.physicalMemory.reservationGenerations.empty()) {
                throw std::runtime_error("active LLD snapshot failed integrity/version checks");
            }
            std::ostringstream event;
            event << "{\"type\":\"lld_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << lldFrontierPc
                  << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"base_va\":\"0x" << std::setw(16) << cpu->core().reservationState().virtualAddress
                  << "\",\"data_pa\":\"0x" << std::setw(9) << cpu->core().reservationState().physicalAddress
                  << "\",\"bytes_be\":\"";
            for (uint8_t byte : beforeBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"loaded_be64\":\"0x" << std::hex << std::setw(16) << loaded
                  << "\",\"destination_before\":\"0x0000000000000001\",\"destination_after\":\"0x"
                  << std::setw(16) << cpu->core().readGpr(5)
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"reservation_generation\":" << std::dec
                  << cpu->core().reservationState().generation
                  << ",\"backing_id\":" << cpu->core().reservationState().backingId
                  << ",\"granule_offset\":\"0x" << std::hex << std::setw(9)
                  << cpu->core().reservationState().granuleOffset
                  << "\",\"cp0_count_after\":" << std::dec << cpu->core().cp0Count()
                  << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                  << ",\"pre_lld_snapshot_replay_verified\":true"
                  << ",\"active_reservation_snapshot\":true"
                  << ",\"snapshot_format_version\":" << lldActiveSnapshot.formatVersion
                  << ",\"snapshot_cpu_version\":" << lldActiveSnapshot.cpuComponentVersion
                  << ",\"snapshot_memory_version\":" << lldActiveSnapshot.memoryComponentVersion
                  << "}";
            lldFrontierRecord = event.str();
            lldFrontierVerified = true;
        } else if (continuePastLldFrontier && lldFrontierVerified &&
                   stopPc == lldFrontierPc + 0x10 && lldScdRecord.empty()) {
            const Mips64Decoded scd = decodeInstruction(stopWord);
            const Mips64ReservationState reservation = cpu->core().reservationState();
            const uint64_t source = cpu->core().readGpr(1);
            const uint64_t effective = cpu->core().readGpr(4) +
                static_cast<uint64_t>(static_cast<int64_t>(
                    static_cast<int16_t>(scd.imm)));
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Store);
            uint8_t beforeBytes[8]{};
            if (!scd.valid || scd.op != Mips64Op::Scd || scd.rs != 4 || scd.rt != 1 ||
                !reservation.valid || reservation.cacheAttribute != 3 ||
                !dataProbe.ok || dataProbe.phys != 0x000B0D078ULL ||
                reservation.virtualAddress != effective ||
                reservation.physicalAddress != dataProbe.phys ||
                reservation.cacheAttribute != dataProbe.cacheAttribute) {
                throw std::runtime_error("authenticated SCD no longer matches the active LLD reservation");
            }
            vm.getMemory().Read(dataProbe.phys, beforeBytes, sizeof(beforeBytes));
            didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
            uint8_t afterBytes[8]{};
            vm.getMemory().Read(dataProbe.phys, afterBytes, sizeof(afterBytes));
            uint64_t stored = 0;
            for (uint8_t byte : afterBytes) stored = (stored << 8) | byte;
            if (!didRetire || cpu->core().readGpr(1) != 1 || stored != source ||
                cpu->core().reservationState().valid || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Linux SCD did not commit its reserved doubleword");
            }
            std::ostringstream event;
            event << "{\"type\":\"scd_frontier_execution\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"base_va\":\"0x" << std::setw(16) << effective
                  << "\",\"data_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"source_before\":\"0x" << std::setw(16) << source
                  << "\",\"source_register_result\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(1) << "\",\"bytes_before_be\":\"";
            for (uint8_t byte : beforeBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"bytes_after_be\":\"";
            for (uint8_t byte : afterBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"stored_be64\":\"0x" << std::hex << std::setw(16) << stored
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"reservation_generation_before\":" << reservation.generation
                  << ",\"reservation_consumed\":true,\"scd_success\":true}";
            lldScdRecord = event.str();
        } else if (continuePastLldFrontier && lldFrontierVerified && stopFetchOk) {
            const Mips64Decoded candidate = decodeInstruction(stopWord);
            if (!candidate.valid && candidate.fault == Mips64Fault::UnsupportedInstruction) {
                stopReason = "unsupported_instruction";
                ++attempts;
                break;
            }
            didRetire = executeOneAndTrace(vm, *cpu,
                continuationTraceRecords < continuationTraceLimit
                    ? static_cast<std::ostream*>(&postSltiuContinuationTrace) : nullptr);
            if (continuationTraceRecords < continuationTraceLimit) ++continuationTraceRecords;
        } else {
            std::ostream* stepTrace = recordingPrefix ? &trace : nullptr;
            if (continuePastDsrl32Frontier && formerFrontierVerified &&
                continuationTraceRecords < continuationTraceLimit) {
                stepTrace = sltiuFrontierVerified
                    ? static_cast<std::ostream*>(&postSltiuContinuationTrace)
                    : static_cast<std::ostream*>(&preSltiuContinuationTrace);
                ++continuationTraceRecords;
            }
            didRetire = executeOneAndTrace(
                vm, *cpu, stepTrace);
        }
        if (didRetire) {
            ++retired;
            if (lldFrontierVerified && stopPc != lldFrontierPc) ++retiredAfterLld;
        } else {
            const Mips64Fault fault = cpu->core().fault();
            stopReason = fault == Mips64Fault::UnsupportedInstruction
                ? "unsupported_instruction"
                : fault == Mips64Fault::None ? "vm_run_returned_zero"
                : Mips64Cpu::faultEntersException(fault) ? "architectural_exception"
                : "mips_fault";
            ++attempts;
            break;
        }
    }

    if (continuePastLldFrontier && lldFrontierVerified &&
        stopReason == "unsupported_instruction") {
        const Mips64Decoded next = decodeInstruction(stopWord);
        if (!stopFetchOk || next.valid || next.fault != Mips64Fault::UnsupportedInstruction ||
            !lldActiveSnapshot.ValidateIntegrity()) {
            throw std::runtime_error("Phase 15 did not stop at a genuine unsupported guest frontier");
        }
        const Mips64FullVmSnapshot firstFinal = vm.captureMips64FullSnapshot();
        CPUContext* context = vm.getCPUContext(0);
        if (!context) throw std::runtime_error("Phase 15 replay lost CPU context 0");
        const uint64_t firstInstructions = context->instructionsExecuted;
        const uint64_t firstCycles = vm.getCyclesExecuted();
        const std::vector<uint8_t> firstConsole =
            vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
        const Ns16550Uart::Snapshot firstUart =
            vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
        std::string restoreError;
        if (!vm.restoreMips64FullSnapshot(lldActiveSnapshot, &restoreError) ||
            !restoreError.empty()) {
            throw std::runtime_error("active LLD reservation snapshot restore failed: " + restoreError);
        }
        for (unsigned index = 0; index < retiredAfterLld; ++index) {
            if (!executeOneAndTrace(vm, *cpu, nullptr)) {
                throw std::runtime_error("active LLD replay stopped before the recorded next frontier");
            }
        }
        const Mips64Probe replayFetch = cpu->core().probeAddress(cpu->core().pc(), Mips64Access::Fetch);
        uint8_t replayBytes[4]{};
        if (!replayFetch.ok) throw std::runtime_error("replayed next frontier did not translate");
        vm.getMemory().Read(replayFetch.phys, replayBytes, sizeof(replayBytes));
        const uint32_t replayWord = read32(replayBytes, false);
        const Mips64FullVmSnapshot replayFinal = vm.captureMips64FullSnapshot();
        const Ns16550Uart::Snapshot replayUart =
            vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
        if (!replayFinal.ValidateIntegrity() ||
            !sameReplayState(firstFinal, replayFinal) ||
            replayFinal.physicalMemory.reservationGenerations !=
                firstFinal.physicalMemory.reservationGenerations ||
            replayFinal.physicalMemory.nextReservationGeneration !=
                firstFinal.physicalMemory.nextReservationGeneration ||
            cpu->core().pc() != stopPc || replayFetch.phys != stopPhysical ||
            replayWord != stopWord || context->instructionsExecuted != firstInstructions ||
            vm.getCyclesExecuted() != firstCycles ||
            vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() != firstConsole ||
            replayUart.transmitCount != firstUart.transmitCount ||
            replayUart.virtualTimeCycle != firstUart.virtualTimeCycle) {
            throw std::runtime_error("active LLD full-snapshot replay diverged before the next frontier");
        }
        lldReplayVerified = true;
        std::ostringstream event;
        event << "{\"type\":\"lld_snapshot_replay\",\"checkpoint_pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << lldFrontierPc + 4
              << "\",\"active_checkpoint_generation\":" << std::dec
              << lldActiveSnapshot.physicalMemory.reservationGenerations.front().generation
              << ",\"retired_after_lld\":" << retiredAfterLld
              << ",\"next_frontier_pc_va\":\"0x" << std::hex << std::setw(16) << stopPc
              << "\",\"next_frontier_instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"reservation_generations_replayed\":"
              << replayFinal.physicalMemory.reservationGenerations.size()
              << ",\"first_console_generation\":" << firstFinal.console.generation
              << ",\"replay_console_generation\":" << replayFinal.console.generation
              << ",\"snapshot_replay_verified\":true}";
        lldReplayRecord = event.str();
        lldFrontierRecord.pop_back();
        lldFrontierRecord += ",\"snapshot_replay_verified\":true}";
    }

    if (stopReason == "instruction_budget_exhausted") {
        // The budget ended after a successful final step; this is the next
        // PC that would have been attempted if the deterministic budget grew.
        stopPc = cpu->core().pc();
        const Mips64Probe nextFetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = nextFetch.ok;
        stopPhysical = nextFetch.ok ? nextFetch.phys : 0;
        stopWord = 0;
        if (nextFetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(nextFetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }
    }

    if (continuePastDsrl32Frontier && formerFrontierVerified) {
        const unsigned skippedBeforeFrontier = 150041U > traceInstructionLimit
            ? 150041U - traceInstructionLimit : 0;
        if (skippedBeforeFrontier != 0) {
            trace << "{\"type\":\"gap\",\"region\":\"pre_dsrl32_frontier\",\"skipped_instructions\":"
                  << skippedBeforeFrontier << "}\n";
        }
    } else if (attempts > traceInstructionLimit) {
        trace << "{\"type\":\"gap\",\"skipped_instructions\":"
              << (attempts - traceInstructionLimit) << "}\n";
    }
    trace << frontierInstructionTrace.str();
    if (!formerFrontierRecord.empty()) trace << formerFrontierRecord << "\n";
    trace << preSltiuContinuationTrace.str();
    if (!sltiuFrontierRecord.empty()) trace << sltiuFrontierRecord << "\n";
    if (!lldFrontierRecord.empty()) trace << lldFrontierRecord << "\n";
    trace << postSltiuContinuationTrace.str();
    if (!lldScdRecord.empty()) trace << lldScdRecord << "\n";
    if (!lldReplayRecord.empty()) trace << lldReplayRecord << "\n";
    if (continuePastDsrl32Frontier && formerFrontierVerified && attempts > 150042U) {
        const unsigned attemptedAfterFrontier = attempts - 150042U;
        if (attemptedAfterFrontier > continuationTraceRecords) {
            trace << "{\"type\":\"continuation_gap\",\"skipped_attempts\":"
                  << (attemptedAfterFrontier - continuationTraceRecords) << "}\n";
        }
    }
    unsigned validTlbEntries = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu->core().tlbEntry(index, entry) && (entry.v0 || entry.v1)) {
            ++validTlbEntries;
        }
    }
    trace << "{\"type\":\"stop\",\"reason\":\"" << stopReason
          << "\",\"attempt_pc_va\":\"0x" << std::hex << std::setw(16)
          << std::setfill('0') << stopPc << "\",\"attempt_fetch_ok\":"
          << (stopFetchOk ? "true" : "false") << ",\"attempt_pc_pa\":\"0x"
          << std::setw(9) << stopPhysical << "\",\"attempt_instruction\":\"0x"
          << std::setw(8) << stopWord << "\",\"pc_va_after\":\"0x"
          << std::setw(16) << cpu->core().pc() << "\",\"exc_code\":"
          << std::dec << cpu->core().causeExcCode() << ",\"epc\":\"0x"
          << std::hex << std::setw(16) << cpu->core().cp0Epc()
          << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
          << "\",\"badvaddr\":\"0x" << std::setw(16)
          << cpu->core().cp0BadVAddr() << "\",\"status\":\"0x"
          << std::setw(16) << cpu->core().cp0Status() << "\",\"mips_fault_code\":"
          << std::dec << static_cast<uint32_t>(cpu->core().fault())
          << ",\"mips_fault_name\":\""
          << Mips64Cpu::faultName(cpu->core().fault()) << "\",\"cycles\":"
          << std::dec << vm.getCyclesExecuted() << ",\"cp0_count\":"
          << cpu->core().cp0Count() << ",\"entryhi\":\"0x"
          << std::hex << std::setw(16) << cpu->core().cp0EntryHi()
          << "\",\"entrylo0\":\"0x" << std::setw(16)
          << cpu->core().cp0EntryLo0() << "\",\"entrylo1\":\"0x"
          << std::setw(16) << cpu->core().cp0EntryLo1()
          << "\",\"gpr2\":\"0x" << std::setw(16) << cpu->core().readGpr(2)
          << "\",\"gpr12\":\"0x" << std::setw(16) << cpu->core().readGpr(12)
          << "\",\"valid_tlb_entries\":" << std::dec << validTlbEntries
          << ",\"attempted_steps\":" << attempts
          << ",\"retired_from_reset\":" << retired
          << ",\"retired_from_linux_entry\":"
          << (entryReached && retired >= retiredBeforeEntry ? retired - retiredBeforeEntry : 0)
          << ",\"retired_beyond_old_frontier\":"
          << (formerFrontierVerified && retired >= 150041 ? retired - 150041 : 0)
          << ",\"sltiu_frontier_verified\":" << (sltiuFrontierVerified ? "true" : "false")
          << ",\"retired_beyond_sltiu_frontier\":"
          << (sltiuFrontierVerified && retired >= 150287 ? retired - 150287 : 0)
          << ",\"frontier_verified\":" << (formerFrontierVerified ? "true" : "false")
          << ",\"lld_frontier_verified\":" << (lldFrontierVerified ? "true" : "false")
          << ",\"lld_snapshot_replay_verified\":" << (lldReplayVerified ? "true" : "false")
          << ",\"retired_after_lld\":" << retiredAfterLld
          << ",\"entry_retired_before_linux\":" << retiredBeforeEntry
          << ",\"unique_guest_pcs\":" << distinctPcs.size()
          << ",\"uart_bytes\":"
          << (vm.getMips64MaltaBootstrapBoard() &&
              vm.getMips64MaltaBootstrapBoard()->ConsoleSink()
                  ? vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size() : 0)
          << ",\"vm_state\":" << static_cast<unsigned>(vm.getState())
          << "}\n";
    if (sltiuFrontierVerified && stopReason == "unsupported_instruction") {
        const Mips64Decoded next = decodeInstruction(stopWord);
        trace << "{\"type\":\"next_frontier\",\"pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << stopPc
              << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
              << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3FU)
              << ",\"rs\":" << unsigned(next.rs) << ",\"rt\":" << unsigned(next.rt)
              << ",\"rd\":" << unsigned(next.rd) << ",\"sa\":" << unsigned(next.sa)
              << ",\"immediate\":" << static_cast<int>(static_cast<int16_t>(next.imm))
              << ",\"decoded_valid\":" << (next.valid ? "true" : "false")
              << ",\"decode_fault\":\"" << Mips64Cpu::faultName(next.fault) << "\""
              << ",\"gpr_rs\":\"0x" << std::hex << std::setw(16)
              << cpu->core().readGpr(next.rs) << "\",\"gpr_rt\":\"0x" << std::setw(16)
              << cpu->core().readGpr(next.rt) << "\",\"status\":\"0x"
              << std::setw(16) << cpu->core().cp0Status()
              << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
              << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
              << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
              << "\",\"valid_tlb_entries\":" << std::dec << validTlbEntries << "}\n";
    }
    if (!lldPreInstructionRecord.empty()) trace << lldPreInstructionRecord << "\n";
    trace.close();

    const auto* board = vm.getMips64MaltaBootstrapBoard();
    const size_t uartBytes = board && board->ConsoleSink()
        ? board->ConsoleSink()->Bytes().size() : 0;
    std::string uartHex;
    if (board && board->ConsoleSink()) {
        static constexpr char digits[] = "0123456789abcdef";
        for (uint8_t byte : board->ConsoleSink()->Bytes()) {
            uartHex.push_back(digits[byte >> 4]);
            uartHex.push_back(digits[byte & 0x0FU]);
        }
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    const Mips64Fault fault = cpu->core().fault();
    std::cout << "MIPS64_KERNEL_PROBE image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << plan.entryPoint
              << " ram_bytes=" << std::dec << probeRamBytes
              << " segments=" << plan.segments.size()
              << " instructions=" << attempts << " retired=" << retired
              << " distinct_pcs=" << distinctPcs.size()
              << " entry_reached=" << (entryReached ? "true" : "false")
              << " linux_instructions="
              << (entryReached && retired >= retiredBeforeEntry ? retired - retiredBeforeEntry : 0)
              << " retired_beyond_old_frontier="
              << (formerFrontierVerified && retired >= 150041 ? retired - 150041 : 0)
              << " sltiu_frontier_verified=" << (sltiuFrontierVerified ? "true" : "false")
              << " retired_beyond_sltiu_frontier="
              << (sltiuFrontierVerified && retired >= 150287 ? retired - 150287 : 0)
              << " frontier_verified=" << (formerFrontierVerified ? "true" : "false")
              << " lld_frontier_verified=" << (lldFrontierVerified ? "true" : "false")
              << " lld_snapshot_replay_verified=" << (lldReplayVerified ? "true" : "false")
              << " retired_after_lld=" << retiredAfterLld
              << " stop_reason=" << stopReason
              << " stop_pc=0x" << std::hex << stopPc
              << " stop_pa=0x" << stopPhysical
              << " stop_word=0x" << stopWord
              << " mips_fault=" << Mips64Cpu::faultName(fault)
              << " mips_fault_code=" << std::dec << static_cast<uint32_t>(fault)
              << " detail=" << cpu->core().lastError()
              << " exc_code=" << std::dec << cpu->core().causeExcCode()
              << " epc=0x" << std::hex << cpu->core().cp0Epc()
              << " cause=0x" << cpu->core().cp0Cause()
              << " badvaddr=0x" << cpu->core().cp0BadVAddr()
              << " status=0x" << cpu->core().cp0Status()
              << " entryhi=0x" << cpu->core().cp0EntryHi()
              << " entrylo0=0x" << cpu->core().cp0EntryLo0()
              << " entrylo1=0x" << cpu->core().cp0EntryLo1()
              << " gpr2=0x" << cpu->core().readGpr(2)
              << " gpr12=0x" << cpu->core().readGpr(12)
              << " cp0_count=" << std::dec << cpu->core().cp0Count()
              << " valid_tlb_entries=" << std::dec << validTlbEntries
              << " cycles=" << std::dec << vm.getCyclesExecuted()
              << " uart_bytes=" << uartBytes
              << " uart_hex=" << uartHex
              << " elapsed_ms=" << elapsedMs
              << " instruction_budget=" << instructionBudget
              << " vm_state=" << static_cast<unsigned>(vm.getState())
              << " trace=" << tracePath.string() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--probe-kernel") {
            return runBoundedKernelProbe(argv[2], argv[3]);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-kernel") {
            constexpr unsigned totalAttemptBudget = 1150041;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-sltiu-kernel") {
            constexpr unsigned totalAttemptBudget = 1150288;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-lld-kernel") {
            constexpr unsigned totalAttemptBudget = 350000;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true, true);
        }
        testElfValidationBothEndians();
        testLoadPlanRejectsRomAndMmioWithoutPartialRamWrites();
        testPromImageBoundsAndEncoding();
        testXkphysKernelEntryAndKxSetup();
        testPromCollisionFailsWithoutBoardOrRamMutation();
        testMismatchedElfPromSnapshotRejected();
        testIndependentVmImageIsolation();
        if (argc > 1) testTransactionalVmAndSnapshotReplay(argv[1]);
    } catch (const std::exception& error) {
        ++gFail;
        std::cout << "UNCAUGHT: " << error.what() << "\n";
    }
    std::cout << "test_mips64_elf_handoff: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
