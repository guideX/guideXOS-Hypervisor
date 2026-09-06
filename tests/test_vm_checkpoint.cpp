#include "VirtualMachine.h"
#include "VMCheckpoint.h"
#include "cpu.h"
#include "memory.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        ++failures;
        std::cerr << "[checkpoint-test] FAIL: " << description << "\n";
    }
}

bool sameRse(const ia64::RSEState& left, const ia64::RSEState& right) {
    return left.cfm == right.cfm && left.rsc == right.rsc && left.bsp == right.bsp &&
           left.bspstore == right.bspstore && left.rnat == right.rnat && left.pfs == right.pfs &&
           left.sof == right.sof && left.sol == right.sol && left.sor == right.sor;
}

} // namespace

int main() {
    ia64::VirtualMachine vm(1 * 1024 * 1024, 1, "IA-64");
    check(vm.init(), "minimal IA-64 VM initializes");
    ia64::CPUContext* context = vm.getCPUContext(0);
    check(context != nullptr && context->cpu != nullptr, "CPU context is available");
    if (context == nullptr || context->cpu == nullptr) return failures == 0 ? 1 : failures;

    ia64::CPUState& cpu = context->cpu->getState();
    cpu.SetGRPhysical(32, 0x1122334455667788ULL);
    cpu.SetGRNaTPhysical(32, true);
    cpu.SetPRPhysical(6, true);
    cpu.SetBR(0, 0x12340ULL);
    cpu.SetIP(0x240ULL);
    cpu.SetCFM(0x205ULL);
    ia64::RSEState rse = cpu.GetRSEState();
    rse.cfm = 0x205ULL;
    rse.rsc = 0xA5ULL;
    rse.bsp = 0x18000ULL;
    rse.bspstore = 0x1C000ULL;
    rse.rnat = 0x8000000000000040ULL;
    rse.pfs = 0x101ULL;
    rse.sof = 5;
    rse.sol = 3;
    rse.sor = 1;
    cpu.SetRSEStateForCheckpoint(rse);
    const ia64::CPURuntimeStateSnapshot cpuSnapshot = context->cpu->createSnapshot();
    cpu.SetGRPhysical(32, 0);
    cpu.SetGRNaTPhysical(32, false);
    cpu.SetPRPhysical(6, false);
    cpu.SetBR(0, 0);
    cpu.SetIP(0);
    context->cpu->restoreSnapshot(cpuSnapshot);
    check(cpu.GetGRPhysical(32) == 0x1122334455667788ULL, "general register survives CPU round trip");
    check(cpu.GetGRNaTPhysical(32), "NaT bit survives CPU round trip");
    check(cpu.GetPRPhysical(6) && cpu.GetBR(0) == 0x12340ULL, "predicate and branch state survive CPU round trip");
    check(cpu.GetIP() == 0x240ULL && sameRse(cpu.GetRSEState(), rse), "IP and RSE state survive CPU round trip");

    std::vector<uint8_t> expectedMemory(256);
    for (size_t i = 0; i < expectedMemory.size(); ++i) expectedMemory[i] = static_cast<uint8_t>(i ^ 0x5A);
    vm.getMemory().loadBuffer(0x4000, expectedMemory);
    const ia64::MemorySnapshot memorySnapshot = dynamic_cast<ia64::Memory&>(vm.getMemory()).CreateSnapshot();
    std::vector<uint8_t> zeroes(expectedMemory.size(), 0);
    vm.getMemory().loadBuffer(0x4000, zeroes);
    dynamic_cast<ia64::Memory&>(vm.getMemory()).RestoreSnapshot(memorySnapshot);
    std::vector<uint8_t> restoredMemory(expectedMemory.size(), 0);
    vm.getMemory().Read(0x4000, restoredMemory.data(), restoredMemory.size());
    check(restoredMemory == expectedMemory, "guest RAM survives byte-for-byte memory round trip");

    const std::filesystem::path malformed =
        std::filesystem::temp_directory_path() / "guidexos-checkpoint-truncated.gxcp";
    {
        std::ofstream output(malformed, std::ios::binary | std::ios::trunc);
        output.write("GXIA64VM", 8);
    }
    std::string error;
    check(!vm.readDiagnosticCheckpoint(malformed.string(), "test-identity", &error),
          "truncated checkpoint is rejected");
    check(!error.empty(), "truncated checkpoint reports an error");
    std::filesystem::remove(malformed);

    const std::filesystem::path unsupported =
        std::filesystem::temp_directory_path() / "guidexos-checkpoint-before-handoff.gxcp";
    error.clear();
    check(!vm.writeDiagnosticCheckpoint(unsupported.string(), "test-identity", &error),
          "checkpoint before the real EFI handoff is rejected");
    check(!error.empty(), "pre-handoff checkpoint rejection reports an error");
    std::filesystem::remove(unsupported);

    std::cout << (failures == 0 ? "checkpoint tests passed\n" : "checkpoint tests failed\n");
    return failures;
}
