// Production VirtualMachine integration proof for the registered MIPS64 ISA.
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "IA64ISAPlugin.h"
#include "ISAPluginRegistry.h"
#include "Mips64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "decoder.h"
#include "mips64_interrupt_test_support.h"

using namespace ia64;
using namespace mips64;
using namespace mips64_interrupt_test;

static constexpr uint64_t kTimerMask = kCauseBitTi | kCauseIp7Bit;

static unsigned g_pass = 0;
static unsigned g_fail = 0;

#define CHECK(expr) do { \
    if (expr) { ++g_pass; } \
    else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

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

static void emitLoad32(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(encI(0x0F, 0, static_cast<uint8_t>(reg),
                         static_cast<uint16_t>(value >> 16)));
    words.push_back(encI(0x0D, static_cast<uint8_t>(reg), static_cast<uint8_t>(reg),
                         static_cast<uint16_t>(value)));
}

static Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm, int index = 0) {
    CPUContext* context = vm.getCPUContext(index);
    if (!context || !context->isaPlugin) {
        return nullptr;
    }
    return dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get());
}

static bool initLoadAndEnter(VirtualMachine& vm, const std::vector<uint32_t>& words,
                             uint64_t physicalLoad = 0x1000) {
    if (!vm.init()) return false;
    const std::vector<uint8_t> image = encodeWords(words);
    if (!vm.loadProgram(image.data(), image.size(), physicalLoad)) return false;
    vm.setEntryPoint(kKernelBase + physicalLoad);
    return vm.getIP() == kKernelBase + physicalLoad;
}

static void testRegistryAndProductionVMConstruction() {
    CHECK(ISAPluginRegistry::instance().isRegistered("MIPS64"));
    Mips64FactoryData mipsFactory;
    std::unique_ptr<IISA> created = ISAPluginRegistry::instance().createISA("MIPS64", &mipsFactory);
    CHECK(created != nullptr);
    CHECK(created && created->getName() == "MIPS64");

    InstructionDecoder decoder;
    IA64FactoryData ia64Factory(&decoder);
    std::unique_ptr<IISA> ia64 = ISAPluginRegistry::instance().createISA("IA-64", &ia64Factory);
    CHECK(ia64 != nullptr);
    CHECK(ia64 && ia64->getName() == "IA-64");

    bool unknownRejected = false;
    try {
        VirtualMachine invalid(1024 * 1024, "NO-SUCH-ISA");
    } catch (const std::invalid_argument&) {
        unknownRejected = true;
    } catch (const std::exception&) {
        unknownRejected = true;
    }
    CHECK(unknownRejected);

    const std::string failingName = "MIPS64_TEST_FACTORY_FAILURE";
    CHECK(ISAPluginRegistry::instance().registerISA(
        failingName, [](void*) -> std::unique_ptr<IISA> {
            throw std::runtime_error("intentional plugin construction failure");
        }));
    bool factoryFailureReported = false;
    try {
        VirtualMachine failed(1024 * 1024, failingName);
    } catch (const std::runtime_error&) {
        factoryFailureReported = true;
    }
    CHECK(factoryFailureReported);
    CHECK(ISAPluginRegistry::instance().unregisterISA(failingName));

    VirtualMachine vm(16 * 1024 * 1024, "MIPS64");
    CHECK(vm.getCPUCount() == 1);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    CHECK(vm.getCPUContext(0)->cpu != nullptr);
    if (plugin) {
        CHECK(plugin->core().pc() == kResetPc);
        CHECK(plugin->core().cp0Status() == kStatusReset);
        CHECK(plugin->core().cp0Count() == 0);
    }
    CHECK(vm.reset());
    if (plugin) {
        CHECK(plugin->core().pc() == kResetPc);
        CHECK(plugin->core().cp0Status() == kStatusReset);
        CHECK(plugin->core().cp0Count() == 0);
    }

    bool vmStateRejected = false;
    try { (void)vm.getCPUState(); }
    catch (const std::runtime_error&) { vmStateRejected = true; }
    CHECK(vmStateRejected);
    bool indexedStateRejected = false;
    try { (void)vm.getCPUState(0); }
    catch (const std::runtime_error&) { indexedStateRejected = true; }
    CHECK(indexedStateRejected);
    bool wrapperStateRejected = false;
    try { (void)vm.getCPU().getState(); }
    catch (const std::runtime_error&) { wrapperStateRejected = true; }
    CHECK(wrapperStateRejected);
    bool predicateReadRejected = false;
    try { (void)vm.getCPU().readPR(1); }
    catch (const std::runtime_error&) { predicateReadRejected = true; }
    CHECK(predicateReadRejected);
    bool rotationReadRejected = false;
    try { (void)vm.getCPU().getGRRotationBase(); }
    catch (const std::runtime_error&) { rotationReadRejected = true; }
    CHECK(rotationReadRejected);
    bool snapshotRejected = false;
    try { (void)vm.getCPU().createSnapshot(); }
    catch (const std::runtime_error&) { snapshotRejected = true; }
    CHECK(snapshotRejected);
    CHECK(vm.createFullSnapshot("mips64-test", "must not omit plugin state").empty());
}

static void testProductionRunCountersAndScheduler() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    const std::vector<uint32_t> program = {
        encI(0x19, 0, 8, 11),
        encI(0x19, 0, 9, 22),
        encR(8, 9, 10, 0, 0x2D),
    };
    CHECK(initLoadAndEnter(vm, program));
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    CHECK(vm.getActiveCPUIndex() == 0);
    const uint64_t cycles = vm.run(3);
    CHECK(cycles == 3);
    CHECK(vm.getCyclesExecuted() == 3);
    CHECK(vm.getState() == VMState::STOPPED);
    CHECK(vm.getActiveCPUIndex() == 0);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(context->cyclesExecuted == 3);
    CHECK(context->instructionsExecuted == 3);
    if (plugin) {
        CHECK(plugin->core().readGpr(8) == 11);
        CHECK(plugin->core().readGpr(9) == 22);
        CHECK(plugin->core().readGpr(10) == 33);
        CHECK(plugin->core().pc() == kKernelBase + 0x100C);
    }
}

static void testRoundRobinSchedulerSelectsIndependentMips64Cpus() {
    VirtualMachine vm(2 * 1024 * 1024, 2, "MIPS64");
    CHECK(vm.init());
    const std::vector<uint32_t> program = {
        encI(0x19, 0, 8, 11),
        encI(0x19, 0, 9, 22),
    };
    const std::vector<uint8_t> image = encodeWords(program);
    CHECK(vm.loadProgram(image.data(), image.size(), 0x1000));
    const uint64_t entry = kKernelBase + 0x1000;
    vm.setEntryPoint(entry);

    CPUContext* cpu0 = vm.getCPUContext(0);
    CPUContext* cpu1 = vm.getCPUContext(1);
    CHECK(cpu0 != nullptr && cpu1 != nullptr);
    if (!cpu0 || !cpu1) return;
    Mips64ISAPlugin* plugin0 = getMipsPlugin(vm, 0);
    Mips64ISAPlugin* plugin1 = getMipsPlugin(vm, 1);
    CHECK(plugin0 != nullptr && plugin1 != nullptr);
    if (!plugin0 || !plugin1) return;

    plugin1->core().setPc(entry);
    cpu1->state = CPUExecutionState::RUNNING;
    cpu1->enabled = true;

    CHECK(vm.step()); // Initially selects CPU 0.
    CHECK(vm.getActiveCPUIndex() == 0);
    CHECK(plugin0->core().readGpr(8) == 11);
    CHECK(plugin1->core().readGpr(8) == 0);

    CHECK(vm.step()); // Round-robin advances to CPU 1.
    CHECK(vm.getActiveCPUIndex() == 1);
    CHECK(plugin1->core().readGpr(8) == 11);
    CHECK(cpu0->cyclesExecuted == 1 && cpu1->cyclesExecuted == 1);

    CHECK(vm.step()); // Then returns to CPU 0.
    CHECK(vm.getActiveCPUIndex() == 0);
    CHECK(plugin0->core().readGpr(9) == 22);
    CHECK(cpu0->instructionsExecuted == 2 && cpu1->instructionsExecuted == 1);
    CHECK(vm.getCyclesExecuted() == 3);
    vm.halt();
}

static void testProductionVmExecutesGuestTimerHandlers() {
    VirtualMachine vm(16 * 1024 * 1024, "MIPS64");

    std::vector<uint32_t> main;
    emitLoad32(main, 8, 0x00008001U);            // IE + IM7, clear BEV/ERL
    main.push_back(encCp0(4, 8, 12));             // mtc0 $t0, Status
    emitLoad32(main, 9, 0);
    main.push_back(encCp0(5, 9, 9));              // dmtc0 $t1, Count
    emitLoad32(main, 10, 25);
    main.push_back(encCp0(5, 10, 11));            // dmtc0 $t2, Compare
    main.push_back(encI(0x19, 0, 16, 7));         // daddiu $s0,$zero,7
    main.push_back(encI(0x19, 0, 17, 5));         // daddiu $s1,$zero,5
    main.push_back(encR(16, 17, 18, 0, 0x2D));   // daddu $s2,$s0,$s1
    main.push_back((0x0FU << 26) | (26U << 16) | 0x8000U); // lui $k0,0x8000
    main.push_back(encI(0x0D, 26, 26, 0x3010));  // ori $k0,$k0,0x3010
    main.push_back(encI(0x3F, 26, 18, 0));        // sd $s2,0($k0)
    main.push_back(encI(0x37, 26, 19, 0));        // ld $s3,0($k0)
    main.push_back(encI(0x19, 0, 20, 1));         // daddiu $s4,$zero,1
    main.push_back(encI(0x19, 21, 21, 1));        // loop: daddiu $s5,$s5,1
    main.push_back(encI(0x04, 0, 0, 0xFFFE));     // beq $zero,$zero,loop
    main.push_back(0);                             // delay-slot nop

    const std::vector<uint8_t> image = encodeWords(main);
    CHECK(vm.init());
    CHECK(vm.loadProgram(image.data(), image.size(), 0x1000));
    const uint64_t entry = kKernelBase + 0x1000;
    vm.setEntryPoint(entry);
    CHECK(vm.getIP() == entry);
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    if (!plugin) return;

    // Confirm this exact virtual entry and exception vector go through the
    // MIPS64-3 production CKSEG0 translator to physical 0x1000 and 0x180.
    const Mips64Probe entryProbe = plugin->core().probeAddress(entry, Mips64Access::Fetch);
    const Mips64Probe vectorProbe = plugin->core().probeAddress(kGeneralVectorBev0,
                                                                  Mips64Access::Fetch);
    CHECK(entryProbe.ok && entryProbe.phys == 0x1000);
    CHECK(vectorProbe.ok && vectorProbe.phys == 0x180);

    GuestProgram handler(vm.getMemory(), kGeneralVectorBev0);
    handler.emit(encCp0(1, 27, 13));              // dmfc0 $k1, Cause before ack
    handler.emit(encCp0(1, 26, 9));               // dmfc0 $k0, Count
    handler.emit(encI(0x19, 26, 26, 32));         // daddiu $k0,$k0,32
    handler.emit(encCp0(5, 26, 11));              // dmtc0 $k0, Compare (ack + rearm)
    handler.emit((0x0FU << 26) | (24U << 16) | 0x8000U); // lui $t8,0x8000
    handler.emit(encI(0x0D, 24, 24, 0x3000));     // ori $t8,$t8,0x3000
    handler.emit(encI(0x3F, 24, 27, 0));          // sd $k1,0($t8), record Cause
    handler.emit(encI(0x37, 24, 26, 8));          // ld $k0,8($t8), guest counter
    handler.emit(encI(0x19, 26, 26, 1));          // daddiu $k0,$k0,1
    handler.emit(encI(0x3F, 24, 26, 8));          // sd $k0,8($t8)
    handler.emit(0x42000018U);                     // eret
    const uint8_t zeros[24]{};
    vm.getMemory().Write(0x3000, zeros, sizeof(zeros));

    unsigned steps = 0;
    unsigned interruptEntries = 0;
    bool completed = false;
    while (steps < 160) {
        CHECK(vm.step());
        ++steps;
        if (plugin->core().pc() == kGeneralVectorBev0) {
            ++interruptEntries;
        }
        uint8_t countBytes[8]{};
        vm.getMemory().Read(0x3008, countBytes, sizeof(countBytes));
        uint64_t guestInterruptCount = 0;
        for (unsigned i = 0; i < 8; ++i) {
            guestInterruptCount |= static_cast<uint64_t>(countBytes[i]) << (8 * i);
        }
        if (guestInterruptCount == 2 && !plugin->core().statusExl() &&
            plugin->core().pc() != kGeneralVectorBev0) {
            completed = true;
            break;
        }
    }

    CHECK(completed);
    CHECK(interruptEntries == 2);
    CHECK(plugin->core().readGpr(16) == 7);
    CHECK(plugin->core().readGpr(17) == 5);
    CHECK(plugin->core().readGpr(18) == 12);
    CHECK(plugin->core().readGpr(19) == 12);
    CHECK(plugin->core().readGpr(20) == 1);
    CHECK(plugin->core().readGpr(21) > 0);
    uint8_t countBytes[8]{};
    uint8_t causeBytes[8]{};
    vm.getMemory().Read(0x3008, countBytes, sizeof(countBytes));
    vm.getMemory().Read(0x3000, causeBytes, sizeof(causeBytes));
    uint64_t guestInterruptCount = 0;
    uint64_t guestCause = 0;
    for (unsigned i = 0; i < 8; ++i) {
        guestInterruptCount |= static_cast<uint64_t>(countBytes[i]) << (8 * i);
        guestCause |= static_cast<uint64_t>(causeBytes[i]) << (8 * i);
    }
    CHECK(guestInterruptCount == 2);
    CHECK((guestCause & (kCauseBitTi | kCauseIp7Bit)) ==
          (kCauseBitTi | kCauseIp7Bit));
    CHECK((guestCause & kCauseExcCodeMask) == 0);
    CHECK(!plugin->core().statusExl() && !plugin->core().statusErl());
    CHECK((plugin->core().cp0Cause() & kTimerMask) == 0);
    CHECK(plugin->core().cp0Compare() > plugin->core().cp0Count());

    CPUContext* context = vm.getCPUContext(0);
    CHECK(context->cyclesExecuted == vm.getCyclesExecuted());
    CHECK(context->cyclesExecuted == steps);
    CHECK(context->instructionsExecuted + interruptEntries == context->cyclesExecuted);
    CHECK(vm.getActiveCPUIndex() == 0);
    vm.halt();
    CHECK(vm.getState() == VMState::STOPPED);
}

static void testInvalidEntryAndUnmappedGuestAccessAreArchitectural() {
    {
        VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
        CHECK(vm.init());
        const uint64_t invalid = 0x0000000080001000ULL;
        vm.setEntryPoint(invalid);
        CHECK(vm.step());
        Mips64ISAPlugin* plugin = getMipsPlugin(vm);
        CHECK(plugin != nullptr);
        if (plugin) {
            CHECK(plugin->core().cp0BadVAddr() == invalid);
            CHECK(plugin->core().causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::AdEL));
            CHECK(plugin->core().statusExl());
        }
        CHECK(vm.getState() == VMState::RUNNING);
        CHECK(!vm.hasKernelPanic());
        vm.halt();
        CHECK(vm.getState() == VMState::STOPPED);
    }

    {
        VirtualMachine vm(16 * 1024 * 1024, "MIPS64");
        const std::vector<uint32_t> guest = {
            (0x0FU << 26) | (8U << 16) | 0x8200U, // full CKSEG0 VA maps to PA 32 MiB
            encI(0x37, 8, 9, 0),                   // ld $t1,0($t0) => bus error
        };
        CHECK(initLoadAndEnter(vm, guest));
        CHECK(vm.step());
        CHECK(vm.step());
        Mips64ISAPlugin* plugin = getMipsPlugin(vm);
        CHECK(plugin != nullptr);
        if (plugin) {
            CHECK(plugin->core().causeExcCode() == static_cast<uint8_t>(Mips64ExcCode::DBE));
            CHECK(plugin->core().statusExl());
            CHECK(plugin->core().pc() == kGeneralVectorBev1);
        }
        CHECK(vm.getState() == VMState::RUNNING);
        CHECK(!vm.hasKernelPanic());
        vm.halt();
    }
}

static void testUnsupportedInstructionBecomesIsaNeutralVmError() {
    VirtualMachine vm(2 * 1024 * 1024, "MIPS64");
    CHECK(initLoadAndEnter(vm, {0xC4000000U})); // valid but unsupported LWC1
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    CHECK(!vm.step());
    CHECK(vm.getState() == VMState::ERROR);
    CHECK(vm.hasKernelPanic());
    const KernelPanic* panic = vm.getLastPanic();
    CHECK(panic != nullptr);
    if (panic) {
        CHECK(!panic->ia64StateValid);
        CHECK(panic->isaName == "MIPS64");
        CHECK(!panic->isaStateDump.empty());
        CHECK(!panic->bundleValid);
    }
    const std::string report = vm.getLastPanicReport();
    CHECK(report.find("ISA STATE AT PANIC (MIPS64)") != std::string::npos);
    CHECK(report.find("LAST EXECUTED BUNDLE") == std::string::npos);
    CHECK(report.find("IA-64 register dump, bundle and stack trace do not apply") !=
          std::string::npos);
    CHECK(vm.getCPUContext(0)->state == CPUExecutionState::ERROR);
}

static void testTwoIndependentProductionVmInstances() {
    VirtualMachine first(2 * 1024 * 1024, "MIPS64");
    VirtualMachine second(2 * 1024 * 1024, "MIPS64");
    CHECK(initLoadAndEnter(first, {encI(0x19, 0, 8, 101)}));
    CHECK(initLoadAndEnter(second, {encI(0x19, 0, 8, 202)}));
    Mips64ISAPlugin* firstPlugin = getMipsPlugin(first);
    Mips64ISAPlugin* secondPlugin = getMipsPlugin(second);
    CHECK(firstPlugin && secondPlugin);
    CHECK(first.step());
    CHECK(first.step());
    CHECK(second.step());
    CHECK(firstPlugin->core().readGpr(8) == 101);
    CHECK(secondPlugin->core().readGpr(8) == 202);
    CHECK(firstPlugin->core().cp0Count() == 2);
    CHECK(secondPlugin->core().cp0Count() == 1);
    CHECK(first.getCyclesExecuted() == 2);
    CHECK(second.getCyclesExecuted() == 1);
    first.halt();
    second.halt();
}

int main() {
    testRegistryAndProductionVMConstruction();
    testProductionRunCountersAndScheduler();
    testRoundRobinSchedulerSelectsIndependentMips64Cpus();
    testProductionVmExecutesGuestTimerHandlers();
    testInvalidEntryAndUnmappedGuestAccessAreArchitectural();
    testUnsupportedInstructionBecomesIsaNeutralVmError();
    testTwoIndependentProductionVmInstances();
    std::cout << "MIPS64 production VM integration: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
