#include "VirtualMachine.h"
#include "VMCheckpoint.h"
#include "IA64ISAPlugin.h"
#include "cpu.h"
#include "decoder.h"
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
    constexpr uint64_t testRid = 5;
    constexpr uint64_t testRr = 0x539ULL;
    constexpr uint64_t testItir = (testRid << 8) | (14ULL << 2);
    cpu.SetRR(5, testRr);
    cpu.InsertITLB(0x1000000003c661ULL, 0xA0007FFFFFC80000ULL,
                   testItir, testRr);
    cpu.InsertDTLB(0x1000000003c661ULL, 0xA0007FFFFFC80000ULL,
                   testItir, testRr);
    cpu.SetITLBReplacementIndexForCheckpoint(17);
    cpu.SetDTLBReplacementIndexForCheckpoint(23);
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
    cpu.PurgeITLB(0xA0007FFFFFC80000ULL, 14, testRid);
    cpu.PurgeDTLB(0xA0007FFFFFC80000ULL, 14, testRid);
    cpu.SetITLBReplacementIndexForCheckpoint(0);
    cpu.SetDTLBReplacementIndexForCheckpoint(0);
    context->cpu->restoreSnapshot(cpuSnapshot);
    check(cpu.GetGRPhysical(32) == 0x1122334455667788ULL, "general register survives CPU round trip");
    check(cpu.GetGRNaTPhysical(32), "NaT bit survives CPU round trip");
    check(cpu.GetPRPhysical(6) && cpu.GetBR(0) == 0x12340ULL, "predicate and branch state survive CPU round trip");
    check(cpu.GetIP() == 0x240ULL && sameRse(cpu.GetRSEState(), rse), "IP and RSE state survive CPU round trip");
    check(cpu.GetITLB(0).valid && cpu.GetDTLB(0).valid,
          "instruction and data TLB entries survive CPU round trip");
    check(cpu.GetITLBReplacementIndex() == 17 && cpu.GetDTLBReplacementIndex() == 23,
          "TLB replacement state survives CPU round trip");

    // A nested DTLB miss injects the guest vector without collecting the
    // ordinary IIP/PSR/IFA/ITIR/IHA frame (KVM nested_dtlb semantics).
    {
        constexpr uint64_t rr5 = 0x539ULL;
        constexpr uint64_t region5 = 5ULL << 61;
        const uint8_t frontierBundle[16] = {
            0x03, 0xD0, 0x00, 0x48, 0x18, 0x10, 0x60, 0x03,
            0x90, 0x00, 0x42, 0x20, 0xE3, 0xD2, 0x30, 0x80
        };
        ia64::InstructionDecoder decoder;
        ia64::IA64ISAPlugin plugin(decoder);
        ia64::IA64ISAState& isaState =
            static_cast<ia64::IA64ISAState&>(plugin.getState());
        ia64::CPUState& faultCpu = isaState.getCPUState();
        ia64::Memory faultMemory(0x20000, false);
        faultMemory.Write(0x1000, frontierBundle, sizeof(frontierBundle));

        constexpr uint64_t oldIpsr = 0x12345678ULL;
        constexpr uint64_t oldIsr = (1ULL << 38) | (1ULL << 34) | (1ULL << 7);
        constexpr uint64_t oldIip = 0x2340ULL;
        constexpr uint64_t oldIfa = 0x5678000ULL;
        constexpr uint64_t oldItir = 0x9cULL;
        constexpr uint64_t oldIha = 0x12345000ULL;
        faultCpu.SetIP(0x1000);
        faultCpu.SetPSR(1ULL << 17);
        faultCpu.SetRR(5, rr5);
        faultCpu.SetCR(2, 0x4000000);
        faultCpu.SetCR(8, 0);
        faultCpu.SetCR(16, oldIpsr);
        faultCpu.SetCR(17, oldIsr);
        faultCpu.SetCR(19, oldIip);
        faultCpu.SetCR(20, oldIfa);
        faultCpu.SetCR(21, oldItir);
        faultCpu.SetCR(25, oldIha);
        faultCpu.SetGR(36, region5 + 0x1234);

        const ia64::ISADecodeResult decoded = plugin.decode(faultMemory);
        check(decoded.valid, "nested-miss LD8 fixture decodes");
        if (decoded.valid) {
            check(plugin.execute(faultMemory, decoded) == ia64::ISAExecutionResult::CONTINUE,
                  "nested translation fault dispatches to the guest vector");
        }
        check(faultCpu.GetCR(19) == 0x4001400,
              "nested DTLB vector becomes the current IIP");
        check(faultCpu.GetCR(16) == oldIpsr,
              "nested DTLB preserves the prior IPSR");
        check(faultCpu.GetCR(17) == (oldIsr & ~(1ULL << 38)),
              "nested DTLB clears only ISR.IR");
        check(faultCpu.GetCR(20) == oldIfa, "nested DTLB preserves IFA");
        check(faultCpu.GetCR(21) == oldItir, "nested DTLB preserves ITIR");
        check(faultCpu.GetCR(25) == oldIha, "nested DTLB preserves IHA");
    }

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
