#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"

#include <set>

namespace {

constexpr uint64_t kTransitionBase = 0x80000000ULL;
constexpr uint64_t kTransitionRamSize = 0x04000000ULL;
constexpr uint64_t kTransitionData = 0x80010000ULL;
constexpr uint64_t kTransitionResult = 0x80002000ULL;
constexpr uint64_t kTransitionMagic = 0x4758504d50545231ULL;
constexpr uint64_t kTransitionWord = 0xFEEDCAFE11223344ULL;
constexpr uint64_t kTransitionComplete = 0x434F4D504C455445ULL;
constexpr uint64_t kTransitionBudget = 10000;

struct ExpectedDomainRegion {
    uint64_t base;
    uint64_t size;
    uint32_t permissions;
};

constexpr std::array<ExpectedDomainRegion, 11> kExpectedDomainRegions{{
    {0x02004000ULL, 8, 0x00},
    {0x0200BFF8ULL, 8, 0x00},
    {0x10000000ULL, 0x100, 0x1B},
    {0x1000A000ULL, 0x100, 0x1B},
    {0x83C00000ULL, 0x4000, 0x0B},
    {0x0C000000ULL, 0x400000, 0x1B},
    {0x80200000ULL, 0x200000, 0x3F},
    {0x80400000ULL, 0x400000, 0x3F},
    {0x80800000ULL, 0x800000, 0x3F},
    {0x81000000ULL, 0x1000000, 0x3F},
    {0x82000000ULL, 0x2000000, 0x3F},
}};

bool ReadBe32Prop(const FdtTree& tree, const std::string& path,
                  const std::string& name, uint32_t& value) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr || property->size() != 4) return false;
    value = Be32(*property, 0);
    return true;
}

bool ValidateDomainFdt(const std::vector<uint8_t>& bytes) {
    const FdtTree tree = ParseFdt(bytes);
    Expect(tree.valid, "RISCV-16 source FDT is structurally valid");
    if (!tree.valid) return false;
    Expect(HasString(Property(tree, "/chosen/opensbi-domains", "compatible"),
                     "opensbi,domain,config"),
           "FDT uses OpenSBI's domain configuration binding");

    constexpr char domainPath[] = "/chosen/opensbi-domains/supervisor-domain";
    Expect(HasString(Property(tree, domainPath, "compatible"), "opensbi,domain,instance"),
           "FDT contains a supervisor domain instance");
    uint32_t domainPhandle = 0;
    uint32_t cpuPhandle = 0;
    const bool domainPhandleValid = ReadBe32Prop(tree, domainPath, "phandle", domainPhandle);
    const bool cpuPhandleValid = ReadBe32Prop(tree, "/cpus/cpu@0", "phandle", cpuPhandle);
    Expect(domainPhandleValid && cpuPhandleValid,
           "DTS compiler assigned domain and boot-hart phandles");
    const auto* assignment = Property(tree, "/cpus/cpu@0", "opensbi-domain");
    Expect(assignment != nullptr && assignment->size() == 4 &&
           Be32(*assignment, 0) == domainPhandle,
           "hart 0 is assigned to the intended supervisor domain");
    const auto* possibleHarts = Property(tree, domainPath, "possible-harts");
    const auto* bootHart = Property(tree, domainPath, "boot-hart");
    Expect(cpuPhandleValid && possibleHarts != nullptr && possibleHarts->size() == 4 &&
           bootHart != nullptr && bootHart->size() == 4 &&
           Be32(*possibleHarts, 0) == cpuPhandle && Be32(*bootHart, 0) == cpuPhandle,
           "domain's possible-hart and boot-hart properties both select hart 0");
    uint32_t nextMode = UINT32_MAX;
    uint64_t nextAddress = 0;
    uint64_t nextFdt = 0;
    const auto* nextAddressProp = Property(tree, domainPath, "next-addr");
    const auto* nextFdtProp = Property(tree, domainPath, "next-arg1");
    Expect(ReadBe32Prop(tree, domainPath, "next-mode", nextMode) && nextMode == 1 &&
           nextAddressProp != nullptr && nextAddressProp->size() == 8 &&
           nextFdtProp != nullptr && nextFdtProp->size() == 8,
           "domain selects a valid S-mode handoff with 64-bit addresses");
    if (nextAddressProp != nullptr && nextAddressProp->size() == 8)
        nextAddress = Be64Pair(*nextAddressProp, 0);
    if (nextFdtProp != nullptr && nextFdtProp->size() == 8)
        nextFdt = Be64Pair(*nextFdtProp, 0);
    Expect(nextAddress == 0x80200000ULL && nextFdt == 0x82200000ULL,
           "domain handoff points at the guest entry and platform FDT");

    const auto* regions = Property(tree, domainPath, "regions");
    Expect(regions != nullptr && regions->size() == kExpectedDomainRegions.size() * 8,
           "domain has exactly eleven explicit region/permission pairs");
    std::map<uint32_t, ExpectedDomainRegion> byPhandle;
    for (const auto& [path, node] : tree.nodes) {
        if (path.rfind("/chosen/opensbi-domains/", 0) != 0 || path == domainPath) continue;
        uint32_t phandle = 0;
        const auto* base = Property(tree, path, "base");
        uint32_t order = UINT32_MAX;
        if (ReadBe32Prop(tree, path, "phandle", phandle) &&
            base != nullptr && base->size() == 8 &&
            ReadBe32Prop(tree, path, "order", order) && order >= 3 && order <= 56) {
            const uint64_t address = Be64Pair(*base, 0);
            byPhandle.emplace(phandle, ExpectedDomainRegion{address, 1ULL << order, 0});
        }
    }
    bool pairsValid = regions != nullptr && regions->size() == kExpectedDomainRegions.size() * 8;
    std::set<uint64_t> observedBase;
    if (pairsValid) {
        for (size_t offset = 0; offset < regions->size(); offset += 8) {
            const uint32_t phandle = Be32(*regions, offset);
            const uint32_t permissions = Be32(*regions, offset + 4);
            const auto found = byPhandle.find(phandle);
            if (found == byPhandle.end()) {
                pairsValid = false;
                continue;
            }
            const auto expected = std::find_if(kExpectedDomainRegions.begin(),
                kExpectedDomainRegions.end(), [&found, permissions](const ExpectedDomainRegion& region) {
                    return region.base == found->second.base && region.size == found->second.size &&
                           region.permissions == permissions;
                });
            if (expected == kExpectedDomainRegions.end() ||
                !observedBase.insert(found->second.base).second) pairsValid = false;
            found->second.permissions = permissions;
        }
    }
    pairsValid = pairsValid && observedBase.size() == kExpectedDomainRegions.size();
    Expect(pairsValid,
           "FDT region list matches the bounded RAM, firmware-adjacent, MMIO, and timer policy");

    const auto* rootMemory = Property(tree, "/memory@80000000", "reg");
    Expect(rootMemory != nullptr && rootMemory->size() == 16 &&
           Be64Pair(*rootMemory, 0) == 0x80000000ULL &&
           Be64Pair(*rootMemory, 8) == 0x04000000ULL,
           "FDT preserves the existing 64 MiB RAM topology");
    Expect(HasString(Property(tree, "/aclint-mtimer@200bff8", "compatible"),
                     "riscv,aclint-mtimer") &&
           HasString(Property(tree, "/soc/serial@10000000", "compatible"), "ns16550") &&
           HasString(Property(tree, "/soc/serial@1000a000", "compatible"), "ns16550") &&
           HasString(Property(tree, "/soc/interrupt-controller@c000000", "compatible"),
                     "riscv,plic0"),
           "FDT retains ACLINT, UART0, UART1, and PLIC device topology");
    const auto* fdtReservation = Property(tree, "/reserved-memory/firmware-fdt@82200000", "reg");
    Expect(fdtReservation != nullptr && fdtReservation->size() == 16 &&
           Be64Pair(*fdtReservation, 0) == 0x82200000ULL &&
           Be64Pair(*fdtReservation, 8) == 0x3000,
           "FDT reserves 12 KiB for OpenSBI's expanded supervisor-visible FDT");
    return pairsValid;
}

bool TransitionGuest(const std::vector<uint8_t>& binary) {
    Expect(!binary.empty() && binary.size() < 0x2000,
           "independent M/S transition binary fits its 8 KiB executable PMP region");
    if (binary.empty() || binary.size() >= 0x2000) return false;

    std::vector<uint8_t> ram(static_cast<size_t>(kTransitionRamSize), 0);
    std::copy(binary.begin(), binary.end(), ram.begin());
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    cpu.loadProgram(kTransitionBase, ram);
    const std::array<uint8_t, 8> data{{0x44, 0x33, 0x22, 0x11, 0xFE, 0xCA, 0xED, 0xFE}};
    Expect(cpu.writePhysical(kTransitionData, data.data(), data.size()),
           "transition test initializes the isolated target word before guest execution");
    Expect(cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
           cpu.pmpImplementedEntries() == 16,
           "transition test selects the unchanged StandardPmp16 profile");

    uint64_t complete = 0;
    uint64_t steps = 0;
    uint64_t deliveredTraps = 0;
    std::deque<std::string> recentTrace;
    for (; steps < kTransitionBudget; ++steps) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        std::ostringstream traceLine;
        traceLine << "pc=0x" << std::hex << pc << " next=0x" << step.nextPc
                  << " before=" << PrivilegeModeToString(before)
                  << " after=" << PrivilegeModeToString(cpu.privilege())
                  << " outcome=" << StepOutcomeToString(step.outcome);
        if (step.outcome == StepOutcome::Trap)
            traceLine << " trap=0x" << static_cast<uint64_t>(step.trapCause);
        recentTrace.push_back(traceLine.str());
        if (recentTrace.size() > 80) recentTrace.pop_front();
        if (step.outcome == StepOutcome::Trap) ++deliveredTraps;
        if (step.outcome != StepOutcome::Ok && step.outcome != StepOutcome::Trap &&
            step.outcome != StepOutcome::Waiting) {
            std::cerr << "transition guest stopped at pc=0x" << std::hex << step.pc
                      << " outcome=" << StepOutcomeToString(step.outcome) << std::dec << '\n';
            break;
        }
        if (ReadWord(cpu, kTransitionResult + 120, complete) &&
            complete == kTransitionComplete) {
            ++steps;
            break;
        }
        if (step.outcome == StepOutcome::Waiting) break;
    }
    if (complete != kTransitionComplete) {
        uint64_t mcause = 0, mepc = 0, scause = 0, sepc = 0, stval = 0;
        (void)cpu.readCsr(0x342, mcause);
        (void)cpu.readCsr(0x341, mepc);
        (void)cpu.readCsr(0x142, scause);
        (void)cpu.readCsr(0x141, sepc);
        (void)cpu.readCsr(0x143, stval);
        std::cerr << "transition stop pc=0x" << std::hex << cpu.getPC()
                  << " mcause=0x" << mcause << " mepc=0x" << mepc
                  << " scause=0x" << scause << " sepc=0x" << sepc
                  << " stval=0x" << stval << std::dec << '\n';
        for (const std::string& line : recentTrace) std::cerr << line << '\n';
        for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
            std::cerr << "denied pc=0x" << std::hex << event.pc << " pa=0x"
                      << event.physicalAddress << " entry=" << std::dec
                      << static_cast<int>(event.matchingEntry) << " cfg=0x" << std::hex
                      << static_cast<unsigned>(event.config) << '\n';
        }
    }
    Expect(complete == kTransitionComplete && steps <= kTransitionBudget,
           "guest completes restrictive, grant, successful access, revoke, and denial sequence");
    Expect(deliveredTraps >= 2,
           "expected S-mode access faults are delivered architecturally to the guest trap handler");

    uint64_t magic = 0, version = 0, error = UINT64_MAX, faultCount = 0;
    uint64_t loadedWord = 0, transitionCount = 0;
    Expect(ReadWord(cpu, kTransitionResult + 0, magic) && magic == kTransitionMagic &&
           ReadWord(cpu, kTransitionResult + 8, version) && version == 1 &&
           ReadWord(cpu, kTransitionResult + 16, error) && error == 0 &&
           ReadWord(cpu, kTransitionResult + 24, faultCount) && faultCount == 2 &&
           ReadWord(cpu, kTransitionResult + 32, loadedWord) && loadedWord == kTransitionWord &&
           ReadWord(cpu, kTransitionResult + 48, transitionCount) && transitionCount == 2,
           "guest-authored signature proves S-mode grant succeeded and deny was restored");

    uint64_t firstCause = 0, firstAddress = 0, firstPc = 0;
    uint64_t secondCause = 0, secondAddress = 0, secondPc = 0;
    const bool guestFaultLog = ReadWord(cpu, kTransitionResult + 64, firstCause) &&
        ReadWord(cpu, kTransitionResult + 72, firstAddress) &&
        ReadWord(cpu, kTransitionResult + 80, firstPc) &&
        ReadWord(cpu, kTransitionResult + 88, secondCause) &&
        ReadWord(cpu, kTransitionResult + 96, secondAddress) &&
        ReadWord(cpu, kTransitionResult + 104, secondPc);
    Expect(guestFaultLog && firstCause == 5 && secondCause == 5 &&
           firstAddress == kTransitionData && secondAddress == kTransitionData &&
           firstPc != 0 && secondPc != 0 && firstPc != secondPc,
           "S-mode trap logs preserve both load access causes, addresses, and distinct PCs");

    size_t deniedLoads = 0;
    bool firstDeniedPc = false;
    bool secondDeniedPc = false;
    for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
        if (event.privilege == PrivilegeMode::Supervisor && event.access == AccessType::Load &&
            event.physicalAddress == kTransitionData && event.matchingEntry == 1 &&
            event.config == 0x18 && !event.allowed) {
            ++deniedLoads;
            firstDeniedPc = firstDeniedPc || event.pc == firstPc;
            secondDeniedPc = secondDeniedPc || event.pc == secondPc;
        }
    }
    Expect(deniedLoads == 2 && firstDeniedPc && secondDeniedPc,
           "PMP traces attribute both S-mode denials to the exact unlocked NAPOT entry");

    std::vector<uint64_t> pmpcfgWrites;
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
        if (event.kind == PmpTraceKind::CsrWrite && event.csr == 0x3A0)
            pmpcfgWrites.push_back(event.csrValue);
    }
    Expect(pmpcfgWrites.size() == 3 && pmpcfgWrites[0] == 0x001B181D &&
           pmpcfgWrites[1] == 0x001B191D && pmpcfgWrites[2] == 0x001B181D,
           "guest M-mode CSR trace records initial deny, exact read grant, and restored deny");
    Expect(cpu.sfenceVmaTrace().size() == 3 &&
           std::all_of(cpu.sfenceVmaTrace().begin(), cpu.sfenceVmaTrace().end(),
               [](const SfenceVmaTraceEvent& event) {
                   return event.privilege == PrivilegeMode::Machine && event.rs1 == 0 &&
                          event.rs2 == 0;
               }),
           "initial policy, grant, and revoke each execute actual M-mode SFENCE.VMA");
    for (const SfenceVmaTraceEvent& event : cpu.sfenceVmaTrace()) {
        std::cout << "SFENCE.VMA M pc=0x" << std::hex << event.pc
                  << " rs1=0x" << event.rs1 << " rs2=0x" << event.rs2 << std::dec << '\n';
    }

    PmpEntryView code, target, result;
    const bool finalPolicy = cpu.pmpEntry(0, code) && code.config == 0x1D &&
           code.lowerBound == 0x80000000ULL && code.upperBound == 0x80002000ULL &&
           cpu.pmpEntry(1, target) && target.config == 0x18 &&
           target.lowerBound == kTransitionData && target.upperBound == kTransitionData + 8 &&
           cpu.pmpEntry(2, result) && result.config == 0x1B &&
           result.lowerBound == kTransitionResult && result.upperBound == kTransitionResult + 0x1000;
    if (!finalPolicy) {
        for (size_t index = 0; index < 3; ++index) {
            PmpEntryView entry;
            (void)cpu.pmpEntry(index, entry);
            std::cerr << "final PMP index=" << index << " cfg=0x" << std::hex
                      << static_cast<unsigned>(entry.config) << " range=[0x"
                      << entry.lowerBound << ",0x" << entry.upperBound << ")\n";
        }
    }
    Expect(finalPolicy,
           "final effective PMP state restores the restrictive target without changing other rules");
    bool unusedEntriesOff = true;
    for (size_t index = 3; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        unusedEntriesOff = unusedEntriesOff && cpu.pmpEntry(index, entry) && !entry.active;
    }
    Expect(unusedEntriesOff, "permission transition leaves all unrelated PMP entries OFF");

    std::cout << "M/S transition steps=" << steps << " delivered traps=" << deliveredTraps
              << " denied PMP events=" << deniedLoads << " final privilege="
              << PrivilegeModeToString(cpu.privilege()) << '\n';
    cpu.reset();
    PmpEntryView resetEntry;
    Expect(cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
           cpu.pmpEntry(1, resetEntry) && !resetEntry.active && resetEntry.config == 0 &&
           cpu.sfenceVmaTrace().empty(),
           "reset preserves PMP profile, clears unlocked rule state, and clears SFENCE trace");
    return complete == kTransitionComplete && magic == kTransitionMagic && error == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: test_riscv_pmp_domain_policy platform.dtb transition.bin\n";
        return 2;
    }
    const std::vector<uint8_t> fdt = ReadFile(argv[1]);
    const std::vector<uint8_t> transition = ReadFile(argv[2]);
    (void)ValidateDomainFdt(fdt);
    (void)TransitionGuest(transition);
    std::cout << "RISCV-16 PMP domain policy checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
