#include "IA64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "decoder.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace ia64;

constexpr uint64_t kRoot = 0x10000;
constexpr uint64_t kOuter = 0x11000;
constexpr uint64_t kInner = 0x12000;
constexpr uint64_t kInterruptVectorBase = 0x1000;
constexpr uint64_t kInterruptHandler = 0x4000;
constexpr uint64_t kInterruptHelper = 0x5000;
constexpr uint64_t kBackingStore = 0x8000;
constexpr uint64_t kInitialCfm = 6ULL | (4ULL << 7);
constexpr uint64_t kOuterCfm = 8ULL | (6ULL << 7);
constexpr uint64_t kInnerCfm = 5ULL | (3ULL << 7);
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
    for (unsigned bit = 0; bit < bitCount; ++bit) {
        if (((value >> bit) & 1ULL) != 0) {
            const unsigned destination = firstBit + bit;
            bundle[destination / 8] |= static_cast<uint8_t>(1U << (destination % 8));
        }
    }
}

std::array<uint8_t, 16> makeBundle(uint8_t templateId,
                                  uint64_t slot0 = 0,
                                  uint64_t slot1 = 0,
                                  uint64_t slot2 = 0) {
    std::array<uint8_t, 16> bytes{};
    putBits(bytes, 0, 5, templateId);
    putBits(bytes, 5, 41, slot0 & kSlotMask);
    putBits(bytes, 46, 41, slot1 & kSlotMask);
    putBits(bytes, 87, 41, slot2 & kSlotMask);
    return bytes;
}

void writeBundle(IMemory& memory,
                 uint64_t ip,
                 uint8_t templateId,
                 uint64_t slot0 = 0,
                 uint64_t slot1 = 0,
                 uint64_t slot2 = 0) {
    const auto bytes = makeBundle(templateId, slot0, slot1, slot2);
    memory.Write(ip, bytes.data(), bytes.size());
}

uint64_t alloc(uint8_t destination, uint8_t sof, uint8_t sol) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(sof) << 13) |
           (static_cast<uint64_t>(sol) << 20) |
           (0x6ULL << 33) | (0x1ULL << 37);
}

uint64_t movFromBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) | (0x31ULL << 27);
}

uint64_t movToBR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) | (0x7ULL << 33);
}

uint64_t movToPfs(uint8_t source) {
    return (static_cast<uint64_t>(source) << 13) |
           (64ULL << 20) | (0x2AULL << 27);
}

uint64_t directCall(uint64_t ip, uint64_t target) {
    const int64_t displacement =
        (static_cast<int64_t>(target) - static_cast<int64_t>(ip)) / 16;
    const uint64_t encoded = static_cast<uint64_t>(displacement) & 0x1FFFFFULL;
    return ((encoded & 0xFFFFFULL) << 13) |
           (((encoded >> 20) & 1ULL) << 36) | (5ULL << 37);
}

uint64_t indirectReturn() {
    return (0x4ULL << 6) | (0x4ULL << 27) | (0x1ULL << 37);
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

void emitStepTrace(const VirtualMachine& vm,
                   const IA64ISAPlugin& plugin,
                   const InstructionDecoder& decoder,
                   const uint8_t* rawMemory,
                   size_t ordinal) {
    const CPUState& cpu = vm.getCPUState(0);
    const uint64_t ip = cpu.GetIP();
    size_t slot = plugin.getCurrentSlot();
    const Bundle bundle = decoder.DecodeBundleAt(rawMemory + ip, ip);
    if (slot >= bundle.instructions.size()) slot = 0;
    const InstructionEx& instruction = bundle.instructions.at(slot);
    uint64_t rawBundleLo = 0;
    uint64_t rawBundleHi = 0;
    for (size_t byte = 0; byte < 8; ++byte) {
        rawBundleLo |= static_cast<uint64_t>(rawMemory[ip + byte]) << (byte * 8);
        rawBundleHi |= static_cast<uint64_t>(rawMemory[ip + 8 + byte]) << (byte * 8);
    }
    const RSEState& rse = cpu.GetRSEState();
    size_t currentCount = 0;
    size_t cleanCount = 0;
    size_t dirtyCount = 0;
    size_t invalidCount = 0;
    for (const RSERegisterPartition partition : rse.physicalPartition) {
        switch (partition) {
            case RSERegisterPartition::CURRENT: ++currentCount; break;
            case RSERegisterPartition::CLEAN: ++cleanCount; break;
            case RSERegisterPartition::DIRTY: ++dirtyCount; break;
            case RSERegisterPartition::INVALID: ++invalidCount; break;
        }
    }
    std::cout << "step=" << ordinal
              << " ip=" << hex(ip)
              << " slot=" << slot
              << " raw41=" << hex(instruction.GetRawBits())
              << " raw_bundle_hi=" << hex(rawBundleHi)
              << " raw_bundle_lo=" << hex(rawBundleLo)
              << " instruction=\"" << instruction.GetDisassembly() << '\"'
              << " cfm=" << hex(cpu.GetCFM())
              << " pfs=" << hex(cpu.GetPFS())
              << " rsc=" << hex(cpu.GetRSC())
              << " bsp=" << hex(cpu.GetBSP())
              << " bspstore=" << hex(cpu.GetBSPSTORE())
              << " bspLoad=" << hex(cpu.GetRSELoadPointer())
              << " rnat=" << hex(cpu.GetRNAT())
              << " bof=" << static_cast<unsigned>(cpu.GetRSEBOF())
              << " capacity=" << NUM_STACKED_GENERAL_REGISTERS
              << " partitions={current:" << currentCount << ",clean:" << cleanCount
              << ",dirty:" << dirtyCount << ",invalid:" << invalidCount << '}'
              << " r32-r39=[";
    for (size_t reg = 32; reg <= 39; ++reg) {
        if (reg != 32) std::cout << ',';
        std::cout << hex(cpu.GetGR(reg)) << (cpu.GetGRNaT(reg) ? "N" : "");
    }
    std::cout << "] b0=" << hex(cpu.GetBR(0)) << '\n';
}

void runNestedCallReturnRegression() {
    VirtualMachine vm(1U << 20, 1, "IA-64");
    require(vm.init(), "call-frame VM initialization failed");
    CPU& processor = vm.getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(processor.getISAPlugin());
    require(plugin != nullptr, "call-frame VM did not select IA64ISAPlugin");
    plugin->reset();

    CPUState& cpu = plugin->getCPUState();
    cpu.SetIP(kRoot);
    cpu.SetCFM(kInitialCfm);
    cpu.SetPFS(0xA5A5A5A5A5A5A5A5ULL);
    cpu.SetBSP(kBackingStore);
    cpu.SetBSPSTORE(kBackingStore);
    cpu.SetRSC(0);
    cpu.SetGR(36, 0x1122334455667788ULL);
    cpu.SetGRNaT(36, true);
    cpu.SetGR(37, 0x8877665544332211ULL);

    IMemory& memory = vm.getMemory();
    writeBundle(memory, kRoot, kMibStop, 0, 0, directCall(kRoot, kOuter));
    writeBundle(memory, kRoot + 0x10, kMiiStop);
    writeBundle(memory, kOuter, kMiiStop, alloc(39, 8, 6));
    writeBundle(memory, kOuter + 0x10, kMiiStop, 0, movFromBR(38, 0));
    writeBundle(memory, kOuter + 0x20, kMibStop, 0, 0,
                directCall(kOuter + 0x20, kInner));
    writeBundle(memory, kOuter + 0x30, kMibStop,
                movToPfs(39), movToBR(0, 38), indirectReturn());
    writeBundle(memory, kInner, kMibStop,
                alloc(36, 5, 3), movToPfs(36), indirectReturn());

    InstructionDecoder decoder;
    const uint8_t* rawMemory = vm.getMemory().GetRawData();
    for (size_t step = 0; step < 18; ++step) {
        emitStepTrace(vm, *plugin, decoder, rawMemory, step);
        const uint64_t ip = cpu.GetIP();
        size_t slot = plugin->getCurrentSlot();
        if (slot >= 3) slot = 0;
        require(vm.stepCPU(0), "production IA-64 call-frame guest stopped early");

        if (ip == kRoot && slot == 2) {
            require(cpu.GetCFM() == 2 && cpu.GetPFS() == kInitialCfm,
                    "root br.call did not establish the first architectural call frame");
            require(cpu.GetBSP() == kBackingStore + 4 * 8,
                    "root br.call advanced BSP by the wrong caller SOL");
            require(cpu.GetGR(32) == 0x1122334455667788ULL && cpu.GetGRNaT(32),
                    "root output argument value or NaT was not renamed to callee r32");
        }
        if (ip == kOuter + 0x20 && slot == 2) {
            require(cpu.GetCFM() == 2 && cpu.GetPFS() == kOuterCfm,
                    "nested br.call did not save the outer CFM in AR.PFS");
            require(cpu.GetBSP() == kBackingStore + 10 * 8,
                    "nested br.call advanced BSP by the wrong SOL");
            require(cpu.GetGR(32) == kRoot + 0x10,
                    "outer return link was not passed through the nested output register");
        }
        if (ip == kInner && slot == 2) {
            require(cpu.GetIP() == kOuter + 0x30 && cpu.GetCFM() == kOuterCfm,
                    "inner br.ret did not restore the outer frame from AR.PFS");
            require(cpu.GetBSP() == kBackingStore + 4 * 8,
                    "inner br.ret did not rewind BSP to the outer frame");
            require(cpu.GetGR(38) == kRoot + 0x10,
                    "return did not restore the outer saved return-link register");
        }
        if (ip == kOuter + 0x30 && slot == 2) {
            require(cpu.GetIP() == kRoot + 0x10 && cpu.GetCFM() == kInitialCfm,
                    "outer br.ret did not restore the entry frame from AR.PFS");
            require(cpu.GetPFS() == kInitialCfm,
                    "outer return left AR.PFS inconsistent with its entry frame");
            require(cpu.GetBSP() == kBackingStore &&
                        cpu.GetBSPSTORE() == kBackingStore,
                    "outer br.ret did not restore the entry backing-store boundary");
            require(cpu.GetGR(36) == 0x1122334455667788ULL && cpu.GetGRNaT(36),
                    "outer br.ret failed to restore the caller stacked argument and NaT");
        }
    }

    require(cpu.GetIP() == kRoot + 0x10,
            "nested call-return guest did not reach the root continuation");
    std::cout << "production IA-64 call-frame regression passed: two nested calls, "
                 "architectural AR.PFS restore, stacked arguments, NaT, and BSP unwind\n";
}

void runInterruptedNestedCallRegression() {
    VirtualMachine vm(1U << 20, 1, "IA-64");
    require(vm.init(), "interrupted call-frame VM initialization failed");
    CPU& processor = vm.getCPU();
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(processor.getISAPlugin());
    require(plugin != nullptr,
            "interrupted call-frame VM did not select IA64ISAPlugin");
    plugin->reset();

    CPUState& cpu = plugin->getCPUState();
    cpu.SetIP(kRoot);
    cpu.SetCFM(kInitialCfm);
    cpu.SetPFS(0xA5A5A5A5A5A5A5A5ULL);
    cpu.SetBSP(kBackingStore);
    cpu.SetBSPSTORE(kBackingStore);
    cpu.SetRSC(0);
    cpu.SetPSR(IA64_PSR_IC | IA64_PSR_I);
    cpu.SetCR(IA64_CR_IVA, kInterruptVectorBase);
    plugin->setInterruptsEnabled(true);
    cpu.SetGR(36, 0x1122334455667788ULL);
    cpu.SetGRNaT(36, true);

    IMemory& memory = vm.getMemory();
    writeBundle(memory, kRoot, kMibStop, 0, 0,
                directCall(kRoot, kOuter));
    writeBundle(memory, kRoot + 0x10, kMiiStop);
    writeBundle(memory, kOuter, kMiiStop, alloc(39, 8, 6));
    writeBundle(memory, kOuter + 0x10, kMiiStop, 0, movFromBR(38, 0));
    writeBundle(memory, kOuter + 0x20, kMibStop, 0, 0,
                directCall(kOuter + 0x20, kInner));
    writeBundle(memory, kOuter + 0x30, kMibStop,
                movToPfs(39), movToBR(0, 38), indirectReturn());
    writeBundle(memory, kInner, kMibStop,
                alloc(36, 5, 3), movToPfs(36), indirectReturn());

    // External interrupt entry targets IVA+0x3000. The handler covers the
    // interrupted frame, makes a nested call, restores AR.PFS and b0, then
    // executes the real decoded rfi instruction from a guest bundle.
    writeBundle(memory, kInterruptHandler, kMibStop, 0, 0, 0x10000000ULL);
    writeBundle(memory, kInterruptHandler + 0x10, kMiiStop,
                alloc(38, 8, 6));
    writeBundle(memory, kInterruptHandler + 0x20, kMiiStop,
                0, movFromBR(39, 0));
    writeBundle(memory, kInterruptHandler + 0x30, kMibStop,
                0, 0, directCall(kInterruptHandler + 0x30,
                                  kInterruptHelper));
    writeBundle(memory, kInterruptHandler + 0x40, kMibStop,
                movToPfs(38), movToBR(0, 39), 0x40000000ULL);
    writeBundle(memory, kInterruptHelper, kMibStop,
                alloc(35, 5, 3), movToPfs(35), indirectReturn());

    constexpr uint64_t interruptedCfm = 2;
    constexpr uint64_t interruptedPfs = kOuterCfm;
    constexpr uint64_t interruptedB0 = kOuter + 0x30;
    constexpr uint64_t interruptedBsp = kBackingStore + 10 * 8;
    bool interruptInjected = false;
    bool interruptReturned = false;
    bool helperReturned = false;
    for (size_t step = 0; step < 64 && cpu.GetIP() != kRoot + 0x10; ++step) {
        const uint64_t ip = cpu.GetIP();
        size_t slot = plugin->getCurrentSlot();
        if (slot >= 3) slot = 0;
        if (!interruptInjected && ip == kInner && slot == 0) {
            require(cpu.GetCFM() == interruptedCfm &&
                        cpu.GetPFS() == interruptedPfs &&
                        cpu.GetBSP() == interruptedBsp &&
                        cpu.GetBR(0) == interruptedB0,
                    "nested-call interruption begins with the expected inner frame");
            plugin->queueInterrupt(0xEF);
            require(plugin->tryDeliverPendingInterrupt(),
                    "nested-call interruption is delivered at the inner entry");
            require(cpu.GetIP() == kInterruptHandler &&
                        cpu.GetCR(IA64_CR_IIP) == kInner &&
                        cpu.GetCR(IA64_CR_IPSR) == (IA64_PSR_IC | IA64_PSR_I),
                    "interrupt entry captures the nested call resume boundary");
            interruptInjected = true;
            continue;
        }

        require(vm.stepCPU(0), "interrupted nested-call guest stopped early");

        if (ip == kInterruptHandler && slot == 2) {
            require(cpu.GetCR(23) == ((1ULL << 63) | interruptedCfm),
                    "handler cover saves the inner frame in architectural IFS");
        }
        if (ip == kInterruptHandler + 0x30 && slot == 2) {
            require(cpu.GetCFM() == 2 &&
                        cpu.GetPFS() == (8ULL | (6ULL << 7)),
                    "handler nested call establishes its own architectural frame");
        }
        if (ip == kInterruptHelper && slot == 2) {
            helperReturned = true;
        }
        if (ip == kInterruptHandler + 0x40 && slot == 2) {
            require(cpu.GetPFS() == interruptedPfs &&
                        cpu.GetBR(0) == interruptedB0,
                    "handler restores interrupted PFS and b0 before rfi");
        }
        if (interruptInjected && !interruptReturned && cpu.GetIP() == kInner &&
            cpu.GetCFM() == interruptedCfm) {
            require(cpu.GetPFS() == interruptedPfs &&
                        cpu.GetBSP() == interruptedBsp &&
                        cpu.GetBR(0) == interruptedB0,
                    "rfi restores the interrupted inner frame and return link");
            interruptReturned = true;
        }
    }

    require(interruptInjected && interruptReturned && helperReturned,
            "interrupted nested call completed the handler call and rfi path");
    require(cpu.GetIP() == kRoot + 0x10,
            "interrupted nested guest calls return to the root continuation");
    require(cpu.GetCFM() == kInitialCfm,
            "interrupted nested guest calls restore the root CFM");
    require(cpu.GetBSP() == kBackingStore &&
                cpu.GetBSPSTORE() == kBackingStore,
            "interrupted nested guest calls unwind root BSP/BSPSTORE");
    require(cpu.GetGR(36) == 0x1122334455667788ULL && cpu.GetGRNaT(36),
            "interrupted nested guest calls preserve the caller value and NaT");

    std::cout << "production IA-64 interrupted call-frame regression passed: "
                 "nested handler call, PFS/b0 save, rfi, and guest return chain\n";
}

} // namespace

int main() {
    try {
        runNestedCallReturnRegression();
        runInterruptedNestedCallRegression();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "production IA-64 call-frame regression failed: "
                  << error.what() << '\n';
        return 1;
    }
}
