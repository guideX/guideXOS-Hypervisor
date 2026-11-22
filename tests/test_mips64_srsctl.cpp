// MIPS64-34 SRSCtl qualification and authentic OpenWrt continuation.

#include "IMemory.h"
#include "Mips64FullSnapshot.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_mmu_setup.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTestRamBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kSrsCtlPc = 0xFFFFFFFF8010C1E4ULL;
constexpr uint64_t kSrsCtlPa = 0x00010C1E4ULL;
constexpr uint32_t kSrsCtlWord = 0x40026002U;
constexpr uint64_t kLinuxRetiredBeforeSrsCtl = 158473;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr char kKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kPhase33Sha256[] =
    "d4d37456d4836aafcd31e4efab7d7d64bda05f6be46e132e0d5a7cdd7e0f1f28";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeCp0(unsigned rs, unsigned rt, unsigned rd, unsigned sel) {
    return 0x40000000U | ((rs & 31U) << 21) | ((rt & 31U) << 16) |
           ((rd & 31U) << 11) | (sel & 7U);
}

uint32_t encodeI(unsigned opcode, unsigned rs, unsigned rt, uint16_t imm) {
    return ((opcode & 63U) << 26) | ((rs & 31U) << 21) |
           ((rt & 31U) << 16) | imm;
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(word >> (8U * (little ? i : 3U - i)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open trace: " + path.string());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) lines.push_back(std::move(line));
    return lines;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open kernel ELF: " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::vector<std::string> linesOfType(const std::vector<std::string>& lines,
                                     const std::string& type) {
    std::vector<std::string> result;
    const std::string marker = "\"type\":\"" + type + "\"";
    for (const std::string& line : lines) {
        if (line.find(marker) != std::string::npos) result.push_back(line);
    }
    return result;
}

bool recordHas(const std::string& line, const std::string& key,
               const std::string& value) {
    return line.find("\"" + key + "\":\"" + value + "\"") != std::string::npos;
}

std::string hexValue(uint64_t value, unsigned width) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase << std::setw(width)
           << std::setfill('0') << value;
    return result.str();
}

using Gprs = std::array<uint64_t, kNumGpr>;

Gprs gprsOf(const Mips64Cpu& cpu) {
    Gprs result{};
    for (unsigned i = 0; i < result.size(); ++i) result[i] = cpu.readGpr(i);
    return result;
}

bool sameReplaySnapshot(const Mips64FullVmSnapshot& a,
                        const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity()) return false;
    Mips64FullVmSnapshot normalizedA = a;
    Mips64FullVmSnapshot normalizedB = b;
    // Console generation is host bookkeeping; guest-visible console state is
    // still included in the checksum comparison.
    normalizedA.console.generation = normalizedB.console.generation;
    return normalizedA.RefreshIntegrity() && normalizedB.RefreshIntegrity() &&
           normalizedA.integrityChecksum == normalizedB.integrityChecksum;
}

Mips64FullVmSnapshot capture(VirtualMachine& vm) {
    Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
    if (!snapshot.ValidateIntegrity()) throw std::runtime_error("invalid full-VM snapshot");
    return snapshot;
}

void testReadsAndReset(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    for (const unsigned rs : {0U, 1U}) {
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(2, 0xFEDCBA9876543210ULL);
        storeWord(memory, kTestPa, encodeCp0(rs, 2, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
        const std::vector<uint8_t> initialState = cpu.serialize();
        CHECK(initialState.size() == Mips64Cpu::kSerializedSize);
        CHECK(initialState.size() == 1001 && initialState[4] == 6);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(2) == 0);
        CHECK(cpu.pc() == kTestPc + 4 && cpu.cp0Count() == 1);
        CHECK(cpu.fault() == Mips64Fault::None && !cpu.delayArmed());
        CHECK(cpu.serialize().size() == initialState.size());

        cpu.reset();
        cpu.setPc(kTestPc);
        cpu.writeGpr(2, UINT64_MAX);
        CHECK(cpu.cp0Count() == 0 && cpu.cp0Status() == kStatusReset);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(2) == 0 && cpu.cp0Count() == 1);
    }
}

void testSelectorsAndWritePolicy(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    storeWord(memory, kTestPa, encodeCp0(0, 2, kCp0RegStatus, kCp0SelStatus), little);
    storeWord(memory, kTestPa + 4, encodeCp0(0, 3, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    cpu.writeGpr(2, UINT64_MAX);
    cpu.writeGpr(3, UINT64_MAX);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(2) == (kStatusReset & kStatusWritableMask));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(3) == 0);
    CHECK(cpu.cp0Status() == kStatusReset);

    for (const unsigned sel : {1U, 3U, 4U, 7U}) {
        Mips64Cpu unsupported(endian);
        unsupported.setPc(kTestPc);
        storeWord(memory, kTestPa, encodeCp0(0, 2, kCp0RegStatus, sel), little);
        CHECK(unsupported.step(memory) == Mips64StepResult::Fault);
        CHECK(unsupported.fault() == Mips64Fault::Cp0Violation);
        CHECK(!Mips64Cpu::faultEntersException(unsupported.fault()));
        CHECK(unsupported.cp0Status() == kStatusReset);
    }

    Mips64Cpu writes(endian);
    writes.setPc(kTestPc);
    writes.writeGpr(8, 0);
    writes.writeGpr(9, 1);
    storeWord(memory, kTestPa, encodeCp0(4, 8, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    storeWord(memory, kTestPa + 4, encodeCp0(5, 8, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    storeWord(memory, kTestPa + 8, encodeCp0(4, 9, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    CHECK(writes.step(memory) == Mips64StepResult::Continue); // legal MTC0 zero
    CHECK(writes.step(memory) == Mips64StepResult::Continue); // legal DMTC0 zero
    CHECK(writes.step(memory) == Mips64StepResult::Fault);    // shadow set 1 is not implemented
    CHECK(writes.fault() == Mips64Fault::Cp0Violation);
    CHECK(!Mips64Cpu::faultEntersException(writes.fault()));
    writes.reset();
    writes.setPc(kTestPc + 12);
    writes.writeGpr(9, 1);
    storeWord(memory, kTestPa + 12,
              encodeCp0(5, 9, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    CHECK(writes.step(memory) == Mips64StepResult::Fault); // DMTC0 cannot select set 1 either
    CHECK(writes.fault() == Mips64Fault::Cp0Violation);
    writes.setPc(kTestPc);
    writes.writeGpr(2, UINT64_MAX);
    storeWord(memory, kTestPa, encodeCp0(0, 2, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    CHECK(writes.step(memory) == Mips64StepResult::Continue);
    CHECK(writes.readGpr(2) == 0); // rejected write did not create shadow state
}

void testExceptionEretAndSingleBank(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    constexpr uint64_t exceptionVector = 0xFFFFFFFF80000180ULL;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.writeGpr(8, 0);                       // clear BEV/ERL through MTC0 Status
    cpu.writeGpr(4, kTestPc + 8);              // handler return address
    for (unsigned i = 1; i < kNumGpr; ++i) {
        if (i != 4 && i != 8) cpu.writeGpr(i, 0xA5A5000000000000ULL | i);
    }
    storeWord(memory, kTestPa, encodeCp0(4, 8, kCp0RegStatus, kCp0SelStatus), little);
    storeWord(memory, kTestPa + 4, 0x0000000CU, little); // SYSCALL
    storeWord(memory, kTestPa + 8, encodeCp0(0, 2, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    storeWord(memory, 0x180, encodeCp0(0, 3, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    storeWord(memory, 0x184, encodeCp0(4, 4, kCp0RegEpc, 0), little);
    storeWord(memory, 0x188, 0x42000018U, little); // ERET

    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.cp0Status() == 0);
    const Gprs beforeException = gprsOf(cpu);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::SyscallTrap &&
          Mips64Cpu::faultEntersException(cpu.fault()));
    CHECK(cpu.pc() == exceptionVector && !cpu.delayArmed());
    CHECK((cpu.cp0Status() & kStatusBitExl) != 0);
    CHECK(cpu.cp0Epc() == kTestPc + 4);
    CHECK(((cpu.cp0Cause() >> kCauseExcCodeShift) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::Sys));
    CHECK(gprsOf(cpu) == beforeException); // exception entry did not select another GPR set

    const Gprs handlerEntry = gprsOf(cpu);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(3) == 0 && cpu.pc() == exceptionVector + 4);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.cp0Epc() == kTestPc + 8 && cpu.pc() == exceptionVector + 8);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue); // ERET
    CHECK(cpu.pc() == kTestPc + 8 && !cpu.delayArmed());
    CHECK((cpu.cp0Status() & kStatusBitExl) == 0);
    CHECK(cpu.readGpr(1) == handlerEntry[1] && cpu.readGpr(5) == handlerEntry[5]);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(2) == 0 && cpu.pc() == kTestPc + 12);
}

void testCp0PrivilegePreserved(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    CHECK(mips64_test::setupKusegIdentityMap(cpu, memory));
    CHECK(mips64_test::clearErlFromGuest(cpu, memory));
    const uint32_t program[] = {
        (0x0FU << 26) | (8U << 16) | 0x40U, // lui $t0, 0x40 (BEV)
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x10U, // add User KSU
        encodeCp0(4, 8, kCp0RegStatus, kCp0SelStatus),
        encodeCp0(0, 2, kCp0RegSRSCtl, kCp0SelSRSCtl),
    };
    for (unsigned i = 0; i < 4; ++i) {
        mips64_test::storeWordPhys(memory, 0x5000 + i * 4, program[i], little);
    }
    cpu.writeGpr(2, 0x123456789ABCDEF0ULL);
    cpu.setPc(0x5000);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.isUserMode());
    CHECK(cpu.cp0Status() == (kStatusBitBev | 0x10ULL));
    const uint64_t oldV0 = cpu.readGpr(2);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::PrivilegeViolation &&
          Mips64Cpu::faultEntersException(cpu.fault()));
    CHECK(cpu.readGpr(2) == oldV0);
    CHECK(cpu.cp0Epc() == 0x500CU && cpu.pc() == 0xFFFFFFFFBFC00380ULL);
    CHECK(((cpu.cp0Cause() >> kCauseExcCodeShift) & 0x1FULL) ==
          static_cast<uint64_t>(Mips64ExcCode::RI));
}

Mips64ISAPlugin* initializeVm(VirtualMachine& vm, Mips64Endianness endian) {
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    if (!vm.init()) return nullptr;
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

void testDelaySlotSnapshotReplay(Mips64Endianness endian) {
    const bool little = endian == Mips64Endianness::Little;
    VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
    Mips64ISAPlugin* plugin = initializeVm(vm, endian);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr && context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();
    storeWord(vm.getMemory(), kTestPa, encodeI(0x04, 0, 0, 2), little);
    storeWord(vm.getMemory(), kTestPa + 4,
              encodeCp0(0, 2, kCp0RegSRSCtl, kCp0SelSRSCtl), little);
    storeWord(vm.getMemory(), kTestPa + 8, 0, little);
    storeWord(vm.getMemory(), kTestPa + 12, 0, little);
    cpu.writeGpr(2, UINT64_MAX);
    vm.setEntryPoint(kTestPc);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.pc() == kTestPc + 4 && cpu.delayArmed());
    const Mips64FullVmSnapshot beforeSlot = capture(vm);
    CHECK(beforeSlot.formatVersion == 3 && beforeSlot.cpuComponentVersion == 6);
    CHECK(beforeSlot.cpus[0].architecturalState.size() == 1001);
    CHECK(beforeSlot.memoryComponentVersion == 2 && beforeSlot.uartComponentVersion == 4 &&
          beforeSlot.irqRoutingComponentVersion == 1);
    CHECK(beforeSlot.cpus[0].architecturalState == cpu.serialize());

    Mips64FullVmSnapshot malformed = beforeSlot;
    malformed.integrityChecksum ^= 1ULL;
    const auto cpuBeforeBadRestore = cpu.serialize();
    const uint64_t cyclesBeforeBadRestore = vm.getCyclesExecuted();
    const uint64_t retiredBeforeBadRestore = context->instructionsExecuted;
    std::string restoreError;
    CHECK(!vm.restoreMips64FullSnapshot(malformed, &restoreError));
    CHECK(!restoreError.empty() && cpu.serialize() == cpuBeforeBadRestore &&
          vm.getCyclesExecuted() == cyclesBeforeBadRestore &&
          context->instructionsExecuted == retiredBeforeBadRestore);

    CHECK(vm.run(1) == 1);
    const Mips64FullVmSnapshot afterSlot = capture(vm);
    CHECK(cpu.readGpr(2) == 0 && cpu.pc() == kTestPc + 12 && !cpu.delayArmed());
    CHECK(cpu.cp0Count() == 2 && vm.getCyclesExecuted() == 2 &&
          context->instructionsExecuted == 2);
    CHECK(cpu.cp0Status() == kStatusReset && cpu.cp0Cause() == 0);
    CHECK(vm.restoreMips64FullSnapshot(beforeSlot, &restoreError) && restoreError.empty());
    CHECK(cpu.pc() == kTestPc + 4 && cpu.delayArmed());
    CHECK(vm.run(1) == 1);
    const Mips64FullVmSnapshot replay = capture(vm);
    CHECK(cpu.readGpr(2) == 0 && cpu.pc() == kTestPc + 12 && !cpu.delayArmed());
    CHECK(sameReplaySnapshot(replay, afterSlot));
}

void runFocusedTests() {
    for (const Mips64Endianness endian : {Mips64Endianness::Little,
                                           Mips64Endianness::Big}) {
        testReadsAndReset(endian);
        testSelectorsAndWritePolicy(endian);
        testExceptionEretAndSingleBank(endian);
        testCp0PrivilegePreserved(endian);
        testDelaySlotSnapshotReplay(endian);
    }
}

struct Continuation {
    std::string reason;
    std::string detail;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    std::array<uint8_t, 4> bytes{};
    uint64_t attempts = 0;
    uint64_t retired = 0;
    uint64_t attemptedCount = 0;
    uint64_t cycles = 0;
    uint64_t totalRetired = 0;
    std::vector<std::string> instructions;
    Mips64FullVmSnapshot terminal;
};

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t& physical, std::array<uint8_t, 4>& bytes) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("cannot fetch guest instruction at " +
                                            hexValue(address, 16));
    physical = probe.phys;
    vm.getMemory().Read(probe.phys, bytes.data(), bytes.size());
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | bytes[3];
}

Continuation continueLinux(VirtualMachine& vm, Mips64Cpu& cpu, CPUContext& context,
                           uint64_t budget) {
    Continuation stop;
    stop.reason = "instruction_budget_exhausted";
    for (uint64_t index = 0; index < budget; ++index) {
        stop.pc = cpu.pc();
        const Mips64Probe probe = cpu.probeAddress(stop.pc, Mips64Access::Fetch);
        if (!probe.ok) {
            stop.reason = Mips64Cpu::faultName(probe.fault);
            stop.physical = probe.phys;
            stop.attemptedCount = cpu.cp0Count();
            stop.cycles = vm.getCyclesExecuted();
            stop.totalRetired = context.instructionsExecuted;
            stop.terminal = capture(vm);
            return stop;
        }
        stop.physical = probe.phys;
        stop.word = fetchWord(vm, cpu, stop.pc, stop.physical, stop.bytes);
        const Mips64Decoded decoded = decodeInstruction(stop.word);
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            stop.detail = decoded.error;
            stop.attemptedCount = cpu.cp0Count();
            stop.cycles = vm.getCyclesExecuted();
            stop.totalRetired = context.instructionsExecuted;
            stop.terminal = capture(vm);
            return stop;
        }
        if (decoded.valid && (decoded.op == Mips64Op::Mfc0 ||
                              decoded.op == Mips64Op::Dmfc0 ||
                              decoded.op == Mips64Op::Mtc0 ||
                              decoded.op == Mips64Op::Dmtc0)) {
            Mips64Cpu probeCpu = cpu;
            if (probeCpu.step(vm.getMemory()) == Mips64StepResult::Fault &&
                probeCpu.fault() == Mips64Fault::Cp0Violation) {
                stop.reason = "unsupported_cp0_access";
                stop.detail = probeCpu.lastError();
                stop.attemptedCount = cpu.cp0Count();
                stop.cycles = vm.getCyclesExecuted();
                stop.totalRetired = context.instructionsExecuted;
                stop.terminal = capture(vm);
                return stop;
            }
        }
        const Gprs before = gprsOf(cpu);
        const uint64_t countBefore = cpu.cp0Count();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted == retiredBefore + 1) {
            ++stop.retired;
            if (stop.instructions.size() < 128) {
                std::ostringstream line;
                line << "{\"type\":\"linux_continuation_instruction\",\"pc\":\""
                     << hexValue(stop.pc, 16) << "\",\"pa\":\""
                     << hexValue(stop.physical, 9) << "\",\"word\":\""
                     << hexValue(stop.word, 8) << "\",\"disassembly\":\""
                     << cpu.disassemble(decoded) << "\",\"v0_before\":\""
                     << hexValue(before[2], 16) << "\",\"v0_after\":\""
                     << hexValue(cpu.readGpr(2), 16) << "\",\"count_before\":"
                     << countBefore << ",\"count_after\":" << cpu.cp0Count()
                     << ",\"retired_after\":" << context.instructionsExecuted << "}";
                if (decoded.op == Mips64Op::Sw) {
                    const uint64_t storeAddress = before[decoded.rs] +
                        static_cast<uint64_t>(static_cast<int64_t>(
                            static_cast<int16_t>(decoded.imm)));
                    const Mips64Probe storeProbe = cpu.probeAddress(
                        storeAddress, Mips64Access::Load);
                    if (storeProbe.ok) {
                        std::array<uint8_t, 4> stored{};
                        vm.getMemory().Read(storeProbe.phys, stored.data(), stored.size());
                        const uint32_t observed = (uint32_t(stored[0]) << 24) |
                            (uint32_t(stored[1]) << 16) | (uint32_t(stored[2]) << 8) |
                            stored[3];
                        std::string body = line.str();
                        body.pop_back();
                        line.str("");
                        line.clear();
                        line << body << ",\"store_va\":\""
                             << hexValue(storeAddress, 16) << "\",\"store_pa\":\""
                             << hexValue(storeProbe.phys, 9) << "\",\"store_value\":\""
                             << hexValue(before[decoded.rt], 16)
                             << "\",\"stored_word\":\"" << hexValue(observed, 8) << "\"}";
                    }
                }
                stop.instructions.push_back(line.str());
            }
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cyclesBefore + 1) {
            stop.reason = "architectural_exception";
            stop.detail = Mips64Cpu::faultName(cpu.fault());
        } else if (vm.getState() == VMState::ERROR) {
            stop.reason = "vm_execution_error";
            stop.detail = cpu.lastError();
        } else {
            stop.reason = "stopped_without_retirement";
            stop.detail = Mips64Cpu::faultName(cpu.fault());
        }
        stop.attemptedCount = cpu.cp0Count();
        stop.cycles = vm.getCyclesExecuted();
        stop.totalRetired = context.instructionsExecuted;
        stop.terminal = capture(vm);
        return stop;
    }
    stop.pc = cpu.pc();
    stop.attemptedCount = cpu.cp0Count();
    stop.cycles = vm.getCyclesExecuted();
    stop.totalRetired = context.instructionsExecuted;
    stop.terminal = capture(vm);
    return stop;
}

bool sameContinuation(const Continuation& a, const Continuation& b) {
    return a.reason == b.reason && a.detail == b.detail && a.pc == b.pc &&
           a.physical == b.physical && a.word == b.word && a.bytes == b.bytes &&
           a.attempts == b.attempts && a.retired == b.retired &&
           a.attemptedCount == b.attemptedCount && a.cycles == b.cycles &&
           a.totalRetired == b.totalRetired && a.instructions == b.instructions &&
           sameReplaySnapshot(a.terminal, b.terminal);
}

int runAuthenticLinux(const std::filesystem::path& imagePath,
                      const std::filesystem::path& phase29Path,
                      const std::filesystem::path& phase33Path,
                      const std::filesystem::path& outputPath) {
    if (std::filesystem::exists(outputPath)) {
        throw std::runtime_error("refusing to overwrite trace: " + outputPath.string());
    }
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected OpenWrt ELF size");
    const auto phase29Lines = readLines(phase29Path);
    const auto initial = linesOfType(phase29Lines, "instruction");
    const auto phase33Lines = readLines(phase33Path);
    if (initial.size() != 256 || phase33Lines.size() != 4 ||
        phase33Lines[2].find("\"next_frontier\":\"0xffffffff8010c1e4\"") ==
            std::string::npos ||
        phase33Lines[3].find("\"instruction\":\"0x40026002\"") ==
            std::string::npos) {
        throw std::runtime_error("accepted Phase 29/33 trace chain changed");
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("Malta ELF handoff failed: " + error);
    }
    if (!vm.init()) throw std::runtime_error("production Malta VM initialization failed");
    CPUContext* context = vm.getCPUContext(0);
    auto* plugin = context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    if (!context || !plugin || !board || !board->Uart() || !board->ConsoleSink()) {
        throw std::runtime_error("production Malta VM components are unavailable");
    }
    Mips64Cpu& cpu = plugin->core();
    if (cpu.pc() != kLinuxEntry && cpu.pc() != 0xFFFFFFFFBFC00000ULL) {
        throw std::runtime_error("unexpected Malta handoff PC");
    }
    for (uint64_t cycle = 0; cycle < initial.size(); ++cycle) {
        uint64_t physical = 0;
        std::array<uint8_t, 4> bytes{};
        const uint32_t word = fetchWord(vm, cpu, cpu.pc(), physical, bytes);
        const std::string& accepted = initial[static_cast<size_t>(cycle)];
        if (!recordHas(accepted, "pc_va", hexValue(cpu.pc(), 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) ||
            vm.run(1) != 1) {
            throw std::runtime_error("Phase 29 authenticated startup prefix diverged");
        }
    }
    const uint64_t remaining = kLinuxRetiredBeforeSrsCtl - initial.size();
    if (vm.run(remaining) != remaining || cpu.pc() != kSrsCtlPc ||
        cpu.cp0Count() != kLinuxRetiredBeforeSrsCtl ||
        context->instructionsExecuted != kLinuxRetiredBeforeSrsCtl ||
        vm.getCyclesExecuted() != kLinuxRetiredBeforeSrsCtl) {
        throw std::runtime_error("authentic Linux prefix did not reach the Phase 33 frontier");
    }
    uint64_t physical = 0;
    std::array<uint8_t, 4> bytes{};
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), physical, bytes);
    const Mips64Decoded decoded = decodeInstruction(word);
    if (word != kSrsCtlWord || physical != kSrsCtlPa ||
        bytes != std::array<uint8_t, 4>{{0x40, 0x02, 0x60, 0x02}} ||
        !decoded.valid || decoded.op != Mips64Op::Mfc0 || decoded.rs != 0 ||
        decoded.rt != 2 || decoded.rd != 12 || decoded.sel != 2 || decoded.sa != 0 ||
        ((word >> 3) & 0xFFU) != 0) {
        throw std::runtime_error("authentic MFC0 SRSCtl encoding or fetch bytes differ");
    }
    const uint64_t previousV0 = cpu.readGpr(2);
    const uint64_t statusBefore = cpu.cp0Status();
    const uint64_t causeBefore = cpu.cp0Cause();
    const uint64_t epcBefore = cpu.cp0Epc();
    const uint64_t badVaddrBefore = cpu.cp0BadVAddr();
    const uint64_t ebaseBefore = cpu.cp0EBase();
    const uint64_t countBefore = cpu.cp0Count();
    const Gprs gprsBefore = gprsOf(cpu);
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const bool delayBefore = cpu.delayArmed();
    if (delayBefore || statusBefore != 0x00400080ULL || causeBefore != 0 ||
        epcBefore != 0 || badVaddrBefore != 0 || countBefore != 158473) {
        throw std::runtime_error("authentic SRSCtl prestate differs from accepted trace");
    }
    const Mips64FullVmSnapshot before = capture(vm);
    if (before.formatVersion != 3 || before.cpuComponentVersion != 6 ||
        before.cpus[0].architecturalState.size() != 1001 ||
        before.memoryComponentVersion != 2 || before.uartComponentVersion != 4 ||
        before.irqRoutingComponentVersion != 1 || before.romSize != 4194304) {
        throw std::runtime_error("authentic pre-SRSCtl full-VM snapshot metadata differs");
    }

    const uint64_t countStart = cpu.cp0Count();
    const uint64_t cyclesStart = vm.getCyclesExecuted();
    const uint64_t retiredStart = context->instructionsExecuted;
    if (vm.run(1) != 1 || cpu.readGpr(2) != 0 || cpu.pc() != kSrsCtlPc + 4 ||
        cpu.delayArmed() || cpu.cp0Count() != countStart + 1 ||
        vm.getCyclesExecuted() != cyclesStart + 1 ||
        context->instructionsExecuted != retiredStart + 1 ||
        cpu.cp0Status() != statusBefore || cpu.cp0Cause() != causeBefore ||
        cpu.cp0Epc() != epcBefore || cpu.cp0BadVAddr() != badVaddrBefore ||
        cpu.cp0EBase() != ebaseBefore || cpu.fault() != Mips64Fault::None ||
        Mips64Cpu::faultEntersException(cpu.fault()) ||
        cpu.reservationState().valid != reservationBefore.valid) {
        throw std::runtime_error("production MFC0 SRSCtl did not retire as modeled");
    }
    const Mips64FullVmSnapshot afterRead = capture(vm);
    const uint64_t afterReadV0 = cpu.readGpr(2);
    const uint64_t afterReadPc = cpu.pc();
    const uint64_t afterReadCount = cpu.cp0Count();
    const uint64_t afterReadCycles = vm.getCyclesExecuted();
    const uint64_t afterReadRetired = context->instructionsExecuted;

    constexpr uint64_t budget = 1000000;
    const Continuation first = continueLinux(vm, cpu, *context, budget);
    const uint64_t totalRetired = context->instructionsExecuted;
    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(before, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kSrsCtlPc || cpu.readGpr(2) != previousV0 ||
        cpu.cp0Count() != countBefore || vm.getCyclesExecuted() != cyclesStart ||
        context->instructionsExecuted != retiredStart || vm.run(1) != 1 ||
        cpu.readGpr(2) != afterReadV0 || cpu.pc() != afterReadPc ||
        cpu.cp0Count() != afterReadCount || vm.getCyclesExecuted() != afterReadCycles ||
        context->instructionsExecuted != afterReadRetired) {
        throw std::runtime_error("authentic pre-SRSCtl snapshot restore or read replay diverged: " +
                                 restoreError);
    }
    const Continuation replay = continueLinux(vm, cpu, *context, budget);
    if (!sameContinuation(first, replay) || context->instructionsExecuted != totalRetired) {
        throw std::runtime_error("authentic Linux continuation replay diverged");
    }

    const auto uart = board->Uart()->CaptureSnapshot();
    const auto console = board->ConsoleSink()->CaptureSnapshot();
    std::ofstream trace(outputPath, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!trace) throw std::runtime_error("cannot create Phase 34 trace: " + outputPath.string());
    trace << "{\"type\":\"authentic_srsctl_pre_instruction\","
          << "\"kernel_sha256\":\"" << kKernelSha256 << "\","
          << "\"phase33_trace_sha256\":\"" << kPhase33Sha256 << "\","
          << "\"pc_va\":\"" << hexValue(kSrsCtlPc, 16) << "\","
          << "\"fetch_pa\":\"" << hexValue(physical, 9) << "\","
          << "\"raw_bytes_be\":\"40026002\",\"word\":\"0x40026002\","
          << "\"opcode\":16,\"rs\":0,\"rt\":2,\"rd\":12,\"sel\":2,"
          << "\"reserved_bits_10_3\":0,\"disassembly\":\"" << cpu.disassemble(decoded)
          << "\",\"previous_v0\":\"" << hexValue(previousV0, 16)
          << "\",\"status\":\"" << hexValue(statusBefore, 16)
          << "\",\"cause\":\"" << hexValue(causeBefore, 16)
          << "\",\"epc\":\"" << hexValue(epcBefore, 16)
          << "\",\"badvaddr\":\"" << hexValue(badVaddrBefore, 16)
          << "\",\"ebase\":\"" << hexValue(ebaseBefore, 16)
          << "\",\"count\":" << countBefore << ",\"cycles\":" << cyclesStart
          << ",\"retired\":" << retiredStart << ",\"privilege\":\"kernel\""
          << ",\"delay_armed\":false,\"gprs\":[";
    for (size_t i = 0; i < gprsBefore.size(); ++i) {
        if (i) trace << ',';
        trace << '"' << hexValue(gprsBefore[i], 16) << '"';
    }
    trace << "],\"reservation_valid\":"
          << (reservationBefore.valid ? "true" : "false")
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1"
          << ",\"rom_base\":\"" << hexValue(before.romBase, 16)
          << "\",\"rom_size\":" << before.romSize
          << ",\"rom_content_hash\":\"" << hexValue(before.romContentHash, 16)
          << "\",\"snapshot_checksum\":\""
          << hexValue(before.integrityChecksum, 16) << "\"}\n";
    trace << "{\"type\":\"authentic_srsctl_retirement\",\"expected_value\":\"0x0000000000000000\""
          << ",\"observed_v0\":\"" << hexValue(afterReadV0, 16)
          << "\",\"hss\":0,\"eicss\":0,\"ess\":0,\"pss\":0,\"css\":0"
          << ",\"linux_srsets_expected\":1,\"pc_after\":\""
          << hexValue(afterReadPc, 16) << "\",\"count_after\":" << afterReadCount
          << ",\"cycles_after\":" << afterReadCycles
          << ",\"retired_after\":" << afterReadRetired
          << ",\"no_exception\":true,\"authentic_pre_instruction_snapshot_replay\":true}\n";
    for (const std::string& line : first.instructions) trace << line << '\n';
    trace << "{\"type\":\"authentic_srsctl_continuation_summary\","
          << "\"total_retired\":" << totalRetired
          << ",\"retired_since_linux_entry\":" << (totalRetired - 17)
          << ",\"retirements_beyond_former_srsctl_frontier\":"
          << (totalRetired - retiredStart) << ",\"continuation_retired\":" << first.retired
          << ",\"continuation_attempts\":" << first.attempts
          << ",\"next_frontier\":\"" << hexValue(first.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(first.physical, 9)
          << "\",\"next_word\":\"" << hexValue(first.word, 8)
          << "\",\"stop_reason\":\"" << first.reason << "\",\"stop_detail\":\""
          << first.detail << "\",\"pre_stop_count\":" << first.attemptedCount
          << ",\"pre_stop_cycles\":" << first.cycles
          << ",\"pre_stop_retired\":" << first.totalRetired
          << ",\"terminal_snapshot_replay_verified\":true"
          << ",\"uart_tx_bytes\":" << uart.transmitCount
          << ",\"uart_rx_fifo_bytes\":" << uart.receiveFifo.size()
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"console_sequence\":" << console.sequence
          << ",\"mmio_observation\":\"not instrumented\",\"budget\":" << budget
          << "}\n";
    const Mips64Decoded nextDecoded = decodeInstruction(first.word);
    trace << "{\"type\":\"authentic_srsctl_next_frontier\",\"pc_va\":\""
          << hexValue(first.pc, 16) << "\",\"fetch_pa\":\""
          << hexValue(first.physical, 9) << "\",\"raw_bytes_be\":\"";
    for (uint8_t byte : first.bytes) trace << std::hex << std::setw(2)
                                           << std::setfill('0') << unsigned(byte);
    trace << std::dec << "\",\"word\":\"" << hexValue(first.word, 8)
          << "\",\"disassembly\":\"" << cpu.disassemble(nextDecoded)
          << "\",\"decode_valid\":" << (nextDecoded.valid ? "true" : "false")
          << ",\"decode_fault\":\"" << Mips64Cpu::faultName(nextDecoded.fault)
          << "\",\"decode_detail\":\"" << nextDecoded.error
          << "\",\"architectural_exception\":"
          << (first.reason == "architectural_exception" ? "true" : "false")
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16) << "\"}\n";
    trace.close();
    if (!trace) throw std::runtime_error("failed writing Phase 34 trace");
    std::cout << "SRSCtl: PC=" << hexValue(kSrsCtlPc, 16)
              << " PA=" << hexValue(physical, 9)
              << " previous_v0=" << hexValue(previousV0, 16)
              << " result=" << hexValue(afterReadV0, 16)
              << " total_retired=" << totalRetired
              << " next=" << hexValue(first.pc, 16)
              << " reason=" << first.reason << " replay=true\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticLinux(argv[2], argv[3], argv[4], argv[5]);
        }
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_srsctl: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        std::cerr << "Usage: test_mips64_srsctl [--continue-authentic-kernel <ELF> "
                     "<phase29-trace> <phase33-trace> <output-jsonl>]\n";
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "test_mips64_srsctl: " << exception.what() << "\n";
        return 1;
    }
}
