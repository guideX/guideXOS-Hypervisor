#include "IA64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ia64;

constexpr uint64_t kMain = 0x10000;
constexpr uint64_t kFunction = 0x11000;
constexpr uint64_t kBackingCrossBoundary = 0xAFE0;
constexpr uint64_t kBackingFirstTransfer = 0xB000;
constexpr uint64_t kPage0 = 0xA000;
constexpr uint64_t kPage1 = 0xB000;
constexpr uint64_t kVectorBase = 0x30000;
constexpr uint64_t kPageFaultHandler = kVectorBase + 0x5000;
constexpr uint64_t kDone = kMain + 0x20;
constexpr uint64_t kGuestRamSize = 1U << 20;
constexpr uint64_t kPageShift = 12;
constexpr uint64_t kPageSize = 1ULL << kPageShift;
constexpr uint64_t kPsrDt = 1ULL << 17;
constexpr uint64_t kPsrIc = 1ULL << 13;
constexpr uint64_t kIsrIr = 1ULL << 38;
constexpr uint64_t kIsrRs = 1ULL << 37;
constexpr uint64_t kIsrRead = 1ULL << 34;
constexpr uint64_t kIsrWrite = 1ULL << 33;
constexpr uint8_t kMiiStop = 0x01;
constexpr uint8_t kMibStop = 0x11;
constexpr uint64_t kSlotMask = (1ULL << 41) - 1;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void putBits(std::array<uint8_t, 16>& bundle,
             unsigned firstBit,
             unsigned bitCount,
             uint64_t value) {
    for (unsigned i = 0; i < bitCount; ++i) {
        if (((value >> i) & 1ULL) != 0) {
            const unsigned bit = firstBit + i;
            bundle[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
        }
    }
}

void writeBundle(IMemory& memory,
                 uint64_t ip,
                 uint8_t templateId,
                 uint64_t slot0 = 0,
                 uint64_t slot1 = 0,
                 uint64_t slot2 = 0) {
    std::array<uint8_t, 16> bundle{};
    putBits(bundle, 0, 5, templateId);
    putBits(bundle, 5, 41, slot0 & kSlotMask);
    putBits(bundle, 46, 41, slot1 & kSlotMask);
    putBits(bundle, 87, 41, slot2 & kSlotMask);
    memory.Write(ip, bundle.data(), bundle.size());
}

void emitM(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMiiStop, instruction);
}

void emitB(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMibStop, 0, 0, instruction);
}

uint64_t alloc(uint8_t destination, uint8_t sof, uint8_t sol) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(sof) << 13) |
           (static_cast<uint64_t>(sol) << 20) |
           (0x6ULL << 33) |
           (0x1ULL << 37);
}

uint64_t directCall(uint64_t ip, uint64_t target) {
    require((ip & 0xFULL) == 0 && (target & 0xFULL) == 0,
            "synthetic call endpoints must be bundle aligned");
    const int64_t displacement =
        (static_cast<int64_t>(target) - static_cast<int64_t>(ip)) / 16;
    require(displacement >= -(1LL << 20) && displacement < (1LL << 20),
            "synthetic call displacement is out of range");
    const uint64_t encoded = static_cast<uint64_t>(displacement) & 0x1FFFFFULL;
    return ((encoded & 0xFFFFFULL) << 13) |
           (((encoded >> 20) & 1ULL) << 36) |
           (5ULL << 37);
}

uint64_t indirectReturn(uint8_t source) {
    return (0x4ULL << 6) |
           (static_cast<uint64_t>(source & 0x7U) << 13) |
           (0x4ULL << 27) |
           (0x1ULL << 37);
}

uint64_t itcD(uint8_t source) {
    return (static_cast<uint64_t>(source & 0x7FU) << 13) |
           (0x2EULL << 27) |
           (0x1ULL << 37);
}

uint64_t ptcL(uint8_t virtualAddressSource, uint8_t pageSizeSource) {
    return (static_cast<uint64_t>(virtualAddressSource & 0x7FU) << 20) |
           (static_cast<uint64_t>(pageSizeSource & 0x7FU) << 13) |
           (0x09ULL << 27) |
           (0x1ULL << 37);
}

uint64_t movToPfs(uint8_t source) {
    return (static_cast<uint64_t>(source & 0x7FU) << 13) |
           (64ULL << 20) |
           (0x2AULL << 27);
}

uint64_t movGR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination & 0x7FU) << 6) |
           (static_cast<uint64_t>(source & 0x7FU) << 13) |
           (0x2ULL << 27) |
           (0x3ULL << 29) |
           (0x8ULL << 37);
}

uint64_t makeTte(uint64_t physicalPage) {
    // Present, PL=3, read/write access rights, 4 KiB page.
    return (physicalPage & ~(kPageSize - 1)) | (3ULL << 7) |
           (3ULL << 9) | 1ULL;
}

void writeU64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t advanceBackingPointer(uint64_t address, size_t registers) {
    while (registers-- != 0) {
        if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
        address += 8;
    }
    if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
    return address;
}

struct FaultRecord {
    uint64_t iip = 0;
    uint64_t isr = 0;
    uint64_t ifa = 0;
    uint64_t itir = 0;
    uint64_t bsp = 0;
    uint64_t bspstore = 0;
    uint64_t bspLoad = 0;
    uint8_t bof = 0;
    uint8_t storeReg = 0;
    uint8_t loadReg = 0;
    uint64_t cfm = 0;
    uint64_t pfs = 0;
    uint64_t psr = 0;
    uint64_t ifs = 0;
    uint64_t rsc = 0;
    uint64_t rnat = 0;
    std::array<RSERegisterPartition, NUM_STACKED_GENERAL_REGISTERS> partitions{};
};

struct RunResult {
    CPUState cpu;
    std::vector<uint8_t> ram;
    std::vector<FaultRecord> faults;
    size_t handlerEntries = 0;
};

RunResult runGuest(uint64_t backingStart,
                   bool injectFaults,
                   bool remapBeforeReturn) {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "RSE restart VM initialization failed");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.getCPU().getISAPlugin());
    require(plugin != nullptr, "RSE restart VM did not select IA64ISAPlugin");
    plugin->reset();
    plugin->setInterruptVectorBase(kVectorBase);

    IMemory& memory = vm.getMemory();
    const uint64_t returnLink = kMain + 0x10;
    emitB(memory, kMain, directCall(kMain, kFunction));
    emitM(memory, returnLink, movGR(18, 16)); // caller continuation normalizes the test TTE register
    emitM(memory, kDone, 0);
    emitM(memory, kFunction, alloc(40, 64, 64));
    emitM(memory, kFunction + 0x10, ptcL(19, 20));
    emitM(memory, kFunction + 0x20, itcD(18));
    emitM(memory, kFunction + 0x30, movToPfs(40));
    emitB(memory, kFunction + 0x40, indirectReturn(0));

    // The page-fault vector performs a guest-visible DTLB insertion and then
    // returns architecturally. It uses only static GRs and does not alter the
    // interrupted RSE frame or RNAT state.
    emitM(memory, kPageFaultHandler, ptcL(19, 20));
    emitM(memory, kPageFaultHandler + 0x10, itcD(16));
    emitB(memory, kPageFaultHandler + 0x20, 0x40000000ULL); // rfi

    CPUState& state = plugin->getCPUState();
    const uint64_t rr = (kPageShift << 2);
    const uint64_t itir = (kPageShift << 2);
    state.SetRR(0, rr);
    state.SetPSR(kPsrDt | kPsrIc);
    state.SetCR(20, kPage1);
    state.SetCR(21, itir);
    state.SetCR(23, 1ULL << 63); // valid interruption frame marker before entry
    state.SetCFM(64ULL | (64ULL << 7));
    state.SetPFS(0);
    state.SetRSC(0);
    state.SetBSP(backingStart);
    state.SetBSPSTORE(backingStart);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(0);
    state.SetRSEStoreRegForCheckpoint(0);
    state.SetRSELoadRegForCheckpoint(0);

    const uint64_t tte0 = makeTte(kPage0);
    const uint64_t tte1 = makeTte(kPage1);
    state.SetGR(16, tte1); // fault handler's guest map operation
    state.SetGR(18, remapBeforeReturn ? tte1 : 0);
    state.SetGR(19, kPage1);
    state.SetGR(20, itir);
    for (size_t i = 0; i < 64; ++i) {
        state.SetGR(32 + i, 0xD100000000000000ULL + i * 0x101ULL);
        state.SetGRNaT(32 + i, i == 2 || i == 3 || i == 62);
    }

    for (uint64_t address = kPage0; address < kPage1 + kPageSize; address += 8) {
        writeU64(memory, address, 0xABABABABABABABABULL);
    }

    TranslationRegisterState page0;
    page0.physicalAddress = tte0;
    page0.virtualAddress = kPage0;
    page0.itir = itir;
    page0.regionValue = rr;
    page0.valid = true;
    state.SetDTLBStateForCheckpoint(0, page0);

    TranslationRegisterState page1;
    page1.physicalAddress = injectFaults ? 0 : tte1;
    page1.virtualAddress = kPage1;
    page1.itir = itir;
    page1.regionValue = rr;
    page1.valid = true;
    state.SetDTLBStateForCheckpoint(1, page1);
    vm.getCPU().setIP(kMain);

    RunResult result;
    size_t steps = 0;
    while (state.GetIP() != kDone && steps < 1000) {
        const uint64_t beforeIP = state.GetIP();
        if (!vm.stepCPU(0)) {
            throw std::runtime_error(
                "RSE restart guest stopped at IP=0x" +
                std::to_string(state.GetIP()) + " VM state=" +
                std::to_string(static_cast<unsigned>(vm.getState())));
        }
        ++steps;
        if (beforeIP != kPageFaultHandler && state.GetIP() == kPageFaultHandler) {
            ++result.handlerEntries;
            FaultRecord fault;
            fault.iip = state.GetCR(19);
            fault.isr = state.GetCR(17);
            fault.ifa = state.GetCR(20);
            fault.itir = state.GetCR(21);
            fault.bsp = state.GetBSP();
            fault.bspstore = state.GetBSPSTORE();
            fault.bspLoad = state.GetRSELoadPointer();
            fault.bof = state.GetRSEBOF();
            fault.storeReg = state.GetRSEStoreReg();
            fault.loadReg = state.GetRSELoadReg();
            fault.cfm = state.GetCFM();
            fault.pfs = state.GetPFS();
            fault.psr = state.GetCR(16);
            fault.ifs = state.GetCR(23);
            fault.rsc = state.GetRSC();
            fault.rnat = state.GetRNAT();
            for (size_t i = 0; i < fault.partitions.size(); ++i) {
                fault.partitions[i] = state.GetRSEPhysicalPartition(i);
            }
            result.faults.push_back(fault);
        }
    }
    require(steps < 1000, "RSE restart guest did not reach its caller continuation");
    require(state.GetIP() == kDone, "RSE restart guest missed its completion bundle");

    result.cpu = state;
    result.ram.resize(memory.GetTotalSize());
    memory.Read(0, result.ram.data(), result.ram.size());
    return result;
}

RunResult runFlushrsGuest(bool injectFaults) {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "flushrs restart VM initialization failed");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.getCPU().getISAPlugin());
    require(plugin != nullptr, "flushrs restart VM did not select IA64ISAPlugin");
    plugin->reset();
    plugin->setInterruptVectorBase(kVectorBase);

    IMemory& memory = vm.getMemory();
    constexpr uint64_t kFlushrs = 0x60000000ULL;
    emitM(memory, kMain, kFlushrs);
    emitM(memory, kMain + 0x10, 0);
    emitM(memory, kDone, 0);
    emitM(memory, kPageFaultHandler, ptcL(19, 20));
    emitM(memory, kPageFaultHandler + 0x10, itcD(16));
    emitB(memory, kPageFaultHandler + 0x20, 0x40000000ULL); // rfi

    CPUState& state = plugin->getCPUState();
    const uint64_t rr = (kPageShift << 2);
    const uint64_t itir = (kPageShift << 2);
    const uint64_t backingStart = kBackingCrossBoundary;
    const uint64_t backingEnd = advanceBackingPointer(backingStart, 64);
    state.SetRR(0, rr);
    state.SetPSR(kPsrDt | kPsrIc);
    state.SetCR(20, kPage1);
    state.SetCR(21, itir);
    state.SetCR(23, 1ULL << 63);
    state.SetCFM(64ULL | (64ULL << 7));
    state.SetPFS(0);
    state.SetRSC(0);
    state.SetBSP(backingEnd);
    state.SetBSPSTORE(backingStart);
    state.SetRSELoadPointer(backingStart);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(0);
    state.SetRSEStoreRegForCheckpoint(0);
    state.SetRSELoadRegForCheckpoint(0);
    const uint64_t tte0 = makeTte(kPage0);
    const uint64_t tte1 = makeTte(kPage1);
    state.SetGR(16, tte1);
    state.SetGR(19, kPage1);
    state.SetGR(20, itir);
    for (size_t i = 0; i < 64; ++i) {
        state.SetGR(32 + i, 0xD100000000000000ULL + i * 0x101ULL);
        state.SetGRNaT(32 + i, i == 2 || i == 3 || i == 62);
    }
    for (uint64_t address = kPage0; address < kPage1 + kPageSize; address += 8) {
        writeU64(memory, address, 0xABABABABABABABABULL);
    }

    TranslationRegisterState page0;
    page0.physicalAddress = tte0;
    page0.virtualAddress = kPage0;
    page0.itir = itir;
    page0.regionValue = rr;
    page0.valid = true;
    state.SetDTLBStateForCheckpoint(0, page0);
    TranslationRegisterState page1;
    page1.physicalAddress = injectFaults ? 0 : tte1;
    page1.virtualAddress = kPage1;
    page1.itir = itir;
    page1.regionValue = rr;
    page1.valid = true;
    state.SetDTLBStateForCheckpoint(1, page1);
    vm.getCPU().setIP(kMain);

    RunResult result;
    size_t steps = 0;
    while (state.GetIP() != kDone && steps < 1000) {
        const uint64_t beforeIP = state.GetIP();
        if (!vm.stepCPU(0)) {
            throw std::runtime_error("flushrs restart guest stopped at IP=0x" +
                                     std::to_string(state.GetIP()));
        }
        ++steps;
        if (beforeIP != kPageFaultHandler && state.GetIP() == kPageFaultHandler) {
            ++result.handlerEntries;
            FaultRecord fault;
            fault.iip = state.GetCR(19);
            fault.isr = state.GetCR(17);
            fault.ifa = state.GetCR(20);
            fault.itir = state.GetCR(21);
            fault.bsp = state.GetBSP();
            fault.bspstore = state.GetBSPSTORE();
            fault.bspLoad = state.GetRSELoadPointer();
            fault.bof = state.GetRSEBOF();
            fault.storeReg = state.GetRSEStoreReg();
            fault.loadReg = state.GetRSELoadReg();
            fault.cfm = state.GetCFM();
            fault.pfs = state.GetPFS();
            fault.psr = state.GetCR(16);
            fault.ifs = state.GetCR(23);
            fault.rsc = state.GetRSC();
            fault.rnat = state.GetRNAT();
            for (size_t i = 0; i < fault.partitions.size(); ++i) {
                fault.partitions[i] = state.GetRSEPhysicalPartition(i);
            }
            result.faults.push_back(fault);
        }
    }
    require(state.GetIP() == kDone, "flushrs restart guest missed its completion bundle");
    result.cpu = state;
    result.ram.resize(memory.GetTotalSize());
    memory.Read(0, result.ram.data(), result.ram.size());
    return result;
}

void compareArchitecturalResult(const RunResult& expected,
                                const RunResult& actual,
                                const std::string& label) {
    require(expected.ram == actual.ram, label + " guest RAM differs from fault-free execution");
    require(expected.cpu.GetIP() == actual.cpu.GetIP(), label + " final IIP differs");
    require(expected.cpu.GetPSR() == actual.cpu.GetPSR(), label + " final PSR differs");
    require(expected.cpu.GetCFM() == actual.cpu.GetCFM(), label + " final CFM differs");
    require(expected.cpu.GetPFS() == actual.cpu.GetPFS(), label + " final PFS differs");
    require(expected.cpu.GetRSC() == actual.cpu.GetRSC(), label + " final RSC differs");
    require(expected.cpu.GetBSP() == actual.cpu.GetBSP(), label + " final BSP differs");
    require(expected.cpu.GetBSPSTORE() == actual.cpu.GetBSPSTORE(),
            label + " final BSPSTORE differs");
    require(expected.cpu.GetRNAT() == actual.cpu.GetRNAT(), label + " final RNAT differs");
    const RSEState& expectedRse = expected.cpu.GetRSEState();
    const RSEState& actualRse = actual.cpu.GetRSEState();
    require(expectedRse.bspLoad == actualRse.bspLoad &&
                expectedRse.bof == actualRse.bof &&
                expectedRse.storeReg == actualRse.storeReg &&
                expectedRse.loadReg == actualRse.loadReg &&
                expectedRse.physicalPartition == actualRse.physicalPartition,
            label + " final RSE transfer state differs");
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        require(expected.cpu.GetGRPhysical(i) == actual.cpu.GetGRPhysical(i),
                label + " physical GR differs at index " + std::to_string(i));
        require(expected.cpu.GetGRNaTPhysical(i) == actual.cpu.GetGRNaTPhysical(i),
                label + " physical NaT differs at index " + std::to_string(i));
    }
}

void verifyFaultMatrix(const RunResult& result,
                       uint64_t backingStart,
                       bool expectPartialPrefix) {
    require(result.faults.size() == 2,
            "guest must recover from one spill fault and one return-fill fault");
    const FaultRecord& spill = result.faults[0];
    const FaultRecord& fill = result.faults[1];
    require(spill.iip == kFunction,
            "mandatory alloc spill fault must be attributed to the issuing instruction");
    require((spill.isr & (kIsrRs | kIsrWrite)) == (kIsrRs | kIsrWrite) &&
                (spill.isr & kIsrIr) == 0,
            "alloc spill ISR must identify a mandatory write without incomplete frame");
    require(spill.ifa == kPage1 && spill.itir == (kPageShift << 2),
            "alloc spill fault must preserve IFA and ITIR");
    require((spill.psr & (3ULL << 41)) == 0,
            "alloc spill must save the issuing slot in IPSR");
    require((spill.cfm & 0x7FULL) == 0 && spill.pfs == (64ULL | (64ULL << 7)),
            "alloc spill must fault before committing the new frame");
    require((spill.bspstore == kPage1 && spill.storeReg == 3) == expectPartialPrefix,
            "alloc spill did not preserve the expected tear point and ring cursor");
    require(spill.ifs == 0 && spill.rsc == 0,
            "alloc spill interruption must leave IFS invalid and RSC unchanged");
    const size_t completedSpillRegisters = expectPartialPrefix ? 3 : 0;
    for (size_t i = 0; i < NUM_STACKED_GENERAL_REGISTERS; ++i) {
        const auto expected = i < completedSpillRegisters
            ? RSERegisterPartition::CLEAN
            : (i < 64 ? RSERegisterPartition::DIRTY
                      : RSERegisterPartition::INVALID);
        require(spill.partitions[i] == expected,
                "alloc spill partition mismatch at physical register " +
                    std::to_string(i));
    }
    require(spill.rnat == 0,
            "alloc spill fault must retain the live RNAT collection for its pending region");

    require(fill.iip == kMain + 0x10,
            "mandatory br.ret fill fault must be attributed to its target instruction");
    require((fill.isr & (kIsrRs | kIsrIr | kIsrRead)) ==
                (kIsrRs | kIsrIr | kIsrRead) && (fill.isr & kIsrWrite) == 0,
            "br.ret fill ISR must mark an incomplete-frame mandatory read");
    require(fill.ifa == kPage1 && fill.itir == (kPageShift << 2),
            "br.ret fill fault must preserve its backing-store IFA and ITIR");
    require(fill.bsp == backingStart && fill.bspstore > fill.bsp,
            "br.ret fill must expose the incomplete-frame BSP/BSPSTORE state");
    require(fill.cfm == (64ULL | (64ULL << 7)),
            "br.ret fill interruption must see the returned-to caller CFM");
    require((fill.psr & (3ULL << 41)) == 0,
            "target instruction interruption must save target slot zero");
    require(fill.bspLoad == kPage1,
            "br.ret fill did not preserve the faulting transfer address");
    require(fill.loadReg == (expectPartialPrefix ? 3 : 0),
            "br.ret fill did not preserve the expected ring cursor at the fault");
    require(fill.ifs == 0 && fill.rsc == 0,
            "br.ret fill interruption must leave IFS invalid and RSC unchanged");
    const size_t completedFillRegisters = expectPartialPrefix ? 3 : 0;
    for (size_t i = 0; i < NUM_STACKED_GENERAL_REGISTERS; ++i) {
        const auto expected = i < completedFillRegisters
            ? RSERegisterPartition::CLEAN
            : (i < 64 ? RSERegisterPartition::CURRENT
                      : RSERegisterPartition::INVALID);
        require(fill.partitions[i] == expected,
                "br.ret fill partition mismatch at physical register " +
                    std::to_string(i));
    }
}

void testPartialBoundaryRecovery() {
    const RunResult reference = runGuest(kBackingCrossBoundary, false, true);
    const RunResult recovered = runGuest(kBackingCrossBoundary, true, false);
    verifyFaultMatrix(recovered, kBackingCrossBoundary, true);
    require(recovered.handlerEntries == 2,
            "both guest page-fault interruptions must enter the real guest handler");
    compareArchitecturalResult(reference, recovered, "partial RNAT-boundary recovery");

    // The first collection contains NaT for the third spilled value. The
    // live RNAT collection covers the following region and carries the fourth.
    require((recovered.cpu.GetGRNaTPhysical(34) &&
             recovered.cpu.GetGRNaTPhysical(35)),
            "recovered caller NaT values were not restored across RNAT regions");
}

void testFirstTransferRecovery() {
    const RunResult reference = runGuest(kBackingFirstTransfer, false, true);
    const RunResult recovered = runGuest(kBackingFirstTransfer, true, false);
    verifyFaultMatrix(recovered, kBackingFirstTransfer, false);
    require(recovered.handlerEntries == 2,
            "first-transfer faults must enter the guest handler for spill and fill");
    compareArchitecturalResult(reference, recovered, "first-transfer recovery");
}

void testPartialFlushrsRecovery() {
    const RunResult reference = runFlushrsGuest(false);
    const RunResult recovered = runFlushrsGuest(true);
    require(recovered.faults.size() == 1 && recovered.handlerEntries == 1,
            "flushrs must enter the guest handler exactly once");
    const FaultRecord& fault = recovered.faults.front();
    require(fault.iip == kMain,
            "flushrs mandatory store fault must be attributed to the issuing instruction");
    require((fault.isr & (kIsrRs | kIsrWrite)) == (kIsrRs | kIsrWrite) &&
                (fault.isr & kIsrIr) == 0,
            "flushrs fault ISR must identify a mandatory write without an incomplete frame");
    require(fault.ifa == kPage1 && fault.itir == (kPageShift << 2),
            "flushrs fault must preserve IFA and ITIR");
    require(fault.bspstore == kPage1 && fault.storeReg == 3 && fault.rnat == 0,
            "flushrs fault did not preserve the post-RNAT-boundary frontier");
    for (size_t i = 0; i < NUM_STACKED_GENERAL_REGISTERS; ++i) {
        const auto expected = i < 3
            ? RSERegisterPartition::CLEAN
            : (i < 64 ? RSERegisterPartition::DIRTY
                      : RSERegisterPartition::INVALID);
        require(fault.partitions[i] == expected,
                "flushrs partition mismatch at physical register " + std::to_string(i));
    }
    compareArchitecturalResult(reference, recovered, "partial flushrs recovery");
}

} // namespace

int main() {
    try {
        testPartialBoundaryRecovery();
        testFirstTransferRecovery();
        testPartialFlushrsRecovery();
        std::cout << "IA-64 RSE fault/restart tests passed: alloc spill recovery, "
                     "flushrs restart, br.ret target fill restart, partial progress, "
                     "and RNAT state\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IA-64 RSE fault/restart test failed: " << error.what() << '\n';
        return 1;
    }
}
