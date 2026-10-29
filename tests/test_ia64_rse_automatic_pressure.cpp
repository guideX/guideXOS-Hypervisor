#include "IA64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ia64;

constexpr uint64_t kMain = 0x14000;
constexpr uint64_t kFunctionBase = 0x15000;
constexpr uint64_t kBacking = 0xA000;
constexpr uint64_t kVerification = 0xB000;
constexpr uint64_t kGuestRamSize = 1U << 20;
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

std::array<uint8_t, 16> makeBundle(uint8_t templateId,
                                  uint64_t slot0 = 0,
                                  uint64_t slot1 = 0,
                                  uint64_t slot2 = 0) {
    std::array<uint8_t, 16> bundle{};
    putBits(bundle, 0, 5, templateId);
    putBits(bundle, 5, 41, slot0 & kSlotMask);
    putBits(bundle, 46, 41, slot1 & kSlotMask);
    putBits(bundle, 87, 41, slot2 & kSlotMask);
    return bundle;
}

void writeBundle(IMemory& memory,
                 uint64_t ip,
                 uint8_t templateId,
                 uint64_t slot0 = 0,
                 uint64_t slot1 = 0,
                 uint64_t slot2 = 0) {
    const auto bundle = makeBundle(templateId, slot0, slot1, slot2);
    memory.Write(ip, bundle.data(), bundle.size());
}

void emitM(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMiiStop, instruction);
}

void emitI(IMemory& memory, uint64_t ip, uint64_t instruction) {
    writeBundle(memory, ip, kMiiStop, 0, instruction);
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

uint64_t movGR(uint8_t destination, uint8_t source) {
    return (static_cast<uint64_t>(destination) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (0x2ULL << 27) |
           (0x3ULL << 29) |
           (0x8ULL << 37);
}

uint64_t movToAR(uint8_t applicationRegister, uint8_t source) {
    return (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(applicationRegister) << 20) |
           (0x2AULL << 27);
}

uint64_t store8(uint8_t base, uint8_t source, uint16_t offset = 8) {
    return (static_cast<uint64_t>(offset & 0x7FU) << 6) |
           (static_cast<uint64_t>(source) << 13) |
           (static_cast<uint64_t>(base) << 20) |
           (static_cast<uint64_t>((offset >> 7) & 1U) << 27) |
           (static_cast<uint64_t>(0x33) << 30) |
           (static_cast<uint64_t>((offset >> 8) & 1U) << 36) |
           (0x5ULL << 37);
}

uint64_t directBranch(uint8_t branchRegister, uint64_t ip, uint64_t target) {
    require((ip & 0xFULL) == 0 && (target & 0xFULL) == 0,
            "synthetic branch endpoints must be bundle aligned");
    const int64_t displacement =
        (static_cast<int64_t>(target) - static_cast<int64_t>(ip)) / 16;
    require(displacement >= -(1LL << 20) && displacement < (1LL << 20),
            "synthetic branch displacement is out of range");
    const uint64_t encoded = static_cast<uint64_t>(displacement) & 0x1FFFFFULL;
    return (static_cast<uint64_t>(branchRegister & 0x7U) << 6) |
           ((encoded & 0xFFFFFULL) << 13) |
           (((encoded >> 20) & 1ULL) << 36) |
           (5ULL << 37);
}

uint64_t indirectReturn(uint8_t source) {
    return (0x4ULL << 6) |
           (static_cast<uint64_t>(source & 0x7U) << 13) |
           (0x4ULL << 27) |
           (0x1ULL << 37);
}

void writeU64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t readU64(const IMemory& memory, uint64_t address) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

uint64_t backingRegisterAddress(uint64_t base, size_t registerIndex) {
    uint64_t address = base;
    for (size_t i = 0; i < registerIndex; ++i) {
        if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
        address += 8;
    }
    if (((address >> 3) & 0x3FULL) == 0x3FULL) address += 8;
    return address;
}

uint64_t rnatCollectionAddress(uint64_t base, size_t collectionIndex) {
    return base + (collectionIndex - 1) * 0x200 + 0x1F8;
}

size_t backingRegisterCount(uint64_t begin, uint64_t end) {
    size_t count = 0;
    for (uint64_t address = begin; address < end; address += 8) {
        if (((address >> 3) & 0x3FULL) != 0x3FULL) ++count;
    }
    return count;
}

size_t countPartition(const CPUState& cpu, RSERegisterPartition expected) {
    size_t count = 0;
    for (size_t i = 0; i < NUM_STACKED_GENERAL_REGISTERS; ++i) {
        if (cpu.GetRSEPhysicalPartition(i) == expected) ++count;
    }
    return count;
}

struct RunResult {
    uint64_t maxBspstore = 0;
    std::vector<uint64_t> bspstoreAfterAlloc;
    std::vector<uint64_t> fillReadAddresses;
    std::vector<uint64_t> frameVerifyValues;
    size_t instructions = 0;
};

RunResult runFrameChain(const std::vector<uint8_t>& frameSizes,
                        size_t repetitions,
                        bool watchRseReads) {
    require(frameSizes.size() >= 2 && frameSizes.size() <= 4,
            "pressure test requires a root and one to three nested frames");
    require(repetitions > 0, "pressure test needs at least one call chain");

    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "pressure VM initialization failed");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.getCPU().getISAPlugin());
    require(plugin != nullptr, "pressure VM did not select IA64ISAPlugin");
    plugin->reset();

    CPU& cpu = vm.getCPU();
    IMemory& memory = vm.getMemory();
    const uint8_t rootSof = frameSizes.front();
    const uint64_t rootCfm = rootSof | (static_cast<uint64_t>(rootSof) << 7);
    const uint64_t completionIP = kMain + 0x20;

    emitB(memory, kMain, directBranch(0, kMain, kFunctionBase));
    if (repetitions > 1) {
        emitB(memory, kMain + 0x10,
              directBranch(0, kMain + 0x10, kFunctionBase));
    } else {
        emitM(memory, kMain + 0x10, 0);
    }
    emitM(memory, kMain + 0x20, 0);

    for (size_t level = 1; level < frameSizes.size(); ++level) {
        const uint64_t start = kFunctionBase + (level - 1) * 0x100;
        const uint8_t sof = frameSizes[level];
        require(sof >= 32, "synthetic nested frames must contain the marker registers");
        const uint8_t lastRegister = static_cast<uint8_t>(31 + sof);
        emitM(memory, start, alloc(40, sof, sof));
        emitI(memory, start + 0x10, movGR(28, static_cast<uint8_t>(19 + level)));
        emitI(memory, start + 0x20, movGR(32, static_cast<uint8_t>(23 + level)));
        emitI(memory, start + 0x30, movGR(lastRegister,
                                          static_cast<uint8_t>(23 + level)));

        uint64_t cursor = start + 0x40;
        if (level + 1 < frameSizes.size()) {
            const uint64_t target = kFunctionBase + level * 0x100;
            emitB(memory, cursor,
                  directBranch(static_cast<uint8_t>(level), cursor, target));
            cursor = start + 0x50;
            emitI(memory, cursor, movGR(28, static_cast<uint8_t>(19 + level)));
            emitM(memory, cursor + 0x10, store8(28, 32));
            emitM(memory, cursor + 0x20, store8(28, lastRegister));
            emitM(memory, cursor + 0x30, movToAR(64, 40));
            emitB(memory, cursor + 0x40,
                  indirectReturn(static_cast<uint8_t>(level - 1)));
        } else {
            emitM(memory, cursor, movToAR(64, 40));
            emitB(memory, cursor + 0x10,
                  indirectReturn(static_cast<uint8_t>(level - 1)));
        }
    }

    for (uint64_t offset = 0; offset < 0x800; offset += 8) {
        writeU64(memory, kBacking + offset, 0xABABABABABABABABULL);
    }
    for (size_t level = 1; level < frameSizes.size(); ++level) {
        const uint64_t marker = 0xD000000000000000ULL | level;
        const uint64_t verify = kVerification + (level - 1) * 16;
        auto& state = cpu.getState();
        state.SetGR(static_cast<size_t>(23 + level), marker);
        state.SetGR(static_cast<size_t>(19 + level), verify);
    }

    CPUState& state = cpu.getState();
    state.SetCFM(rootCfm);
    state.SetRSC(0); // enforced-lazy mode: only mandatory transfers
    state.SetBSP(kBacking);
    state.SetBSPSTORE(kBacking);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(0);
    state.SetGR(28, kVerification + 0x100);
    state.SetGR(30, 0xFEEDFACECAFEBEEFULL);
    for (size_t i = 0; i < rootSof; ++i) {
        state.SetGR(32 + i, 0xC000000000000000ULL + i);
    }
    if (rootSof >= 64) {
        state.SetGRNaT(35, true); // register-stack NaT collection bit 3
        state.SetGRNaT(94, true); // bit 62, in the same 63-register collection
        state.SetGRNaT(95, true); // next RNAT collection, bit 0
    }
    cpu.setIP(kMain);

    RunResult result;
    result.maxBspstore = state.GetBSPSTORE();
    uint64_t previousBspstore = state.GetBSPSTORE();
    auto* ram = dynamic_cast<Memory*>(&memory);
    require(ram != nullptr, "pressure test requires the VM's MMU-aware Memory instance");
    bool watching = watchRseReads;
    const size_t readHook = ram->GetMMU().RegisterReadHook(
        [&](HookContext& context, uint8_t*) {
            if (watching && context.address >= kBacking &&
                context.address < kBacking + 0x800) {
                result.fillReadAddresses.push_back(context.address);
            }
        });

    constexpr size_t kInstructionLimit = 4000;
    while (result.instructions < kInstructionLimit && state.GetIP() != completionIP) {
        if (!vm.stepCPU(0)) {
            ram->GetMMU().UnregisterReadHook(readHook);
            throw std::runtime_error("guest stopped before its post-pressure continuation");
        }
        ++result.instructions;
        result.maxBspstore = std::max(result.maxBspstore, state.GetBSPSTORE());
        if (state.GetBSPSTORE() > previousBspstore) {
            result.bspstoreAfterAlloc.push_back(state.GetBSPSTORE());
        }
        previousBspstore = state.GetBSPSTORE();
    }
    watching = false;
    ram->GetMMU().UnregisterReadHook(readHook);

    require(state.GetIP() == completionIP,
            "guest did not return to the original caller continuation");
    require(state.GetCFM() == rootCfm, "return did not restore root CFM");
    require(state.GetPFS() == rootCfm, "return did not preserve the root frame marker in PFS");
    require(state.GetBSP() == kBacking && state.GetBSPSTORE() == kBacking,
            "unwind did not return BSP/BSPSTORE to the original caller boundary");
    require(state.GetRSELoadPointer() == kBacking,
            "automatic return fill did not return the internal load frontier to the caller boundary");
    require(state.GetRSEStoreReg() == state.GetRSEBOF() &&
                state.GetRSELoadReg() == state.GetRSEBOF(),
            "automatic return fill did not align ring transfer indices with the restored BOF");
    require(state.GetRSC() == 0, "pressure path changed enforced-lazy RSC mode");
    require(countPartition(state, RSERegisterPartition::CURRENT) >= rootSof,
            "unwind lost part of the current root frame partition");
    require(countPartition(state, RSERegisterPartition::DIRTY) == 0,
            "unwind left a dirty partition without a backing-store owner");

    for (size_t i = 0; i < rootSof; ++i) {
        require(state.GetGR(32 + i) == 0xC000000000000000ULL + i,
                "root caller register value was not restored after pressure");
    }
    if (rootSof >= 64) {
        require(state.GetGRNaT(35), "root register NaT bit 3 was lost");
        require(state.GetGRNaT(94), "root register NaT bit 62 was lost");
        require(state.GetGRNaT(95), "root register NaT bit 0 after a collection boundary was lost");
    }

    for (size_t level = 1; level + 1 < frameSizes.size(); ++level) {
        const uint64_t marker = 0xD000000000000000ULL | level;
        const uint64_t first = readU64(memory, kVerification + (level - 1) * 16);
        const uint64_t last = readU64(memory, kVerification + (level - 1) * 16 + 8);
        require(first == marker && last == marker,
                "nested frame " + std::to_string(level) +
                    " marker mismatch: first=" + std::to_string(first) +
                    " last=" + std::to_string(last) +
                    " expected=" + std::to_string(marker));
        result.frameVerifyValues.push_back(first);
        result.frameVerifyValues.push_back(last);
    }

    const size_t spilledRegisters = backingRegisterCount(kBacking, result.maxBspstore);
    std::vector<uint64_t> writtenWords;
    writtenWords.reserve(spilledRegisters + spilledRegisters / 63);
    for (size_t i = 0; i < spilledRegisters; ++i) {
        const uint64_t address = backingRegisterAddress(kBacking, i);
        writtenWords.push_back(address);
        const uint64_t value = readU64(memory, address);
        require(value != 0xABABABABABABABABULL,
                "mandatory spill left a sentinel in a transferred register word");
        if (i < rootSof) {
            require(value == 0xC000000000000000ULL + i,
                    "guest backing store does not contain the original root register value");
        }
    }
    for (size_t collection = 1; collection <= spilledRegisters / 63; ++collection) {
        const uint64_t address = rnatCollectionAddress(kBacking, collection);
        writtenWords.push_back(address);
        require(readU64(memory, address) != 0xABABABABABABABABULL,
                "mandatory spill did not materialize an RNAT collection boundary");
    }
    for (uint64_t address = kBacking; address < kBacking + 0x800; address += 8) {
        const bool expectedWrite = std::find(writtenWords.begin(), writtenWords.end(),
                                             address) != writtenWords.end();
        if (!expectedWrite) {
            require(readU64(memory, address) == 0xABABABABABABABABULL,
                    "mandatory spilling modified a backing-store sentinel outside the transfer range");
        }
    }

    if (rootSof >= 64) {
        require(readU64(memory, rnatCollectionAddress(kBacking, 1)) ==
                    ((1ULL << 3) | (1ULL << 62)),
                "first spilled RNAT collection lost nonzero bits before its boundary");
        require(state.GetRNAT() == ((1ULL << 3) | (1ULL << 62)),
                "return fill did not restore the root live RNAT collection");
        require(readU64(memory, backingRegisterAddress(kBacking, 63)) ==
                    0xC00000000000003FULL,
                "register after the first RNAT boundary was not stored at the correct address");
    }
    if (spilledRegisters >= 126) {
        require(readU64(memory, rnatCollectionAddress(kBacking, 2)) == 1,
                "second spilled RNAT collection lost its nonzero bit");
    }

    if (watchRseReads && spilledRegisters != 0) {
        require(std::find(result.fillReadAddresses.begin(), result.fillReadAddresses.end(),
                          backingRegisterAddress(kBacking, 0)) != result.fillReadAddresses.end(),
                "automatic return fill never read the spilled root register from guest memory");
    }

    return result;
}

void testCapacityBoundaries() {
    const RunResult noPressure = runFrameChain({32, 32}, 1, false);
    require(noPressure.maxBspstore == kBacking,
            "no-pressure baseline performed an unnecessary eager spill");
    const RunResult exactCapacity = runFrameChain({32, 64}, 1, false);
    require(exactCapacity.maxBspstore == kBacking,
            "an exact 96-register resident stack spilled unnecessarily");

    const RunResult firstOverflow = runFrameChain({33, 64}, 1, true);
    require(firstOverflow.maxBspstore == kBacking + 8,
            "first frame beyond capacity did not spill exactly one register");
    require(firstOverflow.bspstoreAfterAlloc == std::vector<uint64_t>{kBacking + 8},
            "first capacity overflow did not produce exactly one mandatory spill wave");
    require(firstOverflow.fillReadAddresses.size() == 1 &&
                firstOverflow.fillReadAddresses.front() == kBacking,
            "one-register return fill did not read the spilled caller word from guest memory");
}

void testMultipleAutomaticSpillAndFillWaves() {
    const RunResult result = runFrameChain({64, 64, 64, 64}, 2, true);
    const std::vector<uint64_t> expectedAllocFrontiers = {
        kBacking + 0x100,
        kBacking + 0x308,
        kBacking + 0x510,
        kBacking + 0x100,
        kBacking + 0x308,
        kBacking + 0x510,
    };
    require(result.bspstoreAfterAlloc == expectedAllocFrontiers,
            "nested allocs did not produce the expected repeated spill waves");
    require(result.maxBspstore == kBacking + 0x510,
            "multiple spill waves did not advance over two RNAT collection slots");
    require(result.fillReadAddresses.size() > 100,
            "nested pressure returns did not perform guest backing-store reads");
    require(std::find(result.fillReadAddresses.begin(), result.fillReadAddresses.end(),
                      backingRegisterAddress(kBacking, 64)) != result.fillReadAddresses.end(),
            "first fill wave did not restore the displaced first nested caller frame");
    require(std::find(result.fillReadAddresses.begin(), result.fillReadAddresses.end(),
                      backingRegisterAddress(kBacking, 128)) != result.fillReadAddresses.end(),
            "second fill wave did not restore the displaced second nested caller frame");
}

void testPartialSpillFault() {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "fault-path VM initialization failed");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.getCPU().getISAPlugin());
    require(plugin != nullptr, "fault-path VM did not select IA64ISAPlugin");
    plugin->reset();

    CPU& cpu = vm.getCPU();
    IMemory& memory = vm.getMemory();
    constexpr uint64_t kFaultingAlloc = kMain;
    constexpr uint64_t kLastRegisterWord = kGuestRamSize - 16;
    constexpr uint64_t kLastRnatWord = kGuestRamSize - 8;
    const uint64_t dirtyBsp = backingRegisterAddress(kLastRegisterWord, 64);
    require(backingRegisterCount(kLastRegisterWord, dirtyBsp) == 64,
            "fault-path setup did not create a 64-register dirty interval");
    emitM(memory, kFaultingAlloc, alloc(40, 64, 64));

    CPUState& state = cpu.getState();
    state.SetCFM(64ULL | (64ULL << 7));
    state.SetRSC(0);
    state.SetBSP(dirtyBsp);
    state.SetBSPSTORE(kLastRegisterWord);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(0);
    for (size_t i = 0; i < 64; ++i) {
        state.SetGR(32 + i, 0xE000000000000000ULL + i);
    }
    writeU64(memory, kLastRegisterWord, 0xABABABABABABABABULL);
    writeU64(memory, kLastRnatWord, 0xABABABABABABABABULL);
    cpu.setIP(kMain);
    const uint64_t oldCfm = state.GetCFM();
    const uint64_t oldPfs = state.GetPFS();
    const uint64_t oldBsp = state.GetBSP();

    bool interrupted = false;
    try {
        interrupted = !vm.stepCPU(0);
    } catch (const std::exception&) {
        interrupted = true;
    }
    require(interrupted,
            "invalid backing-store access did not stop mandatory alloc spilling (IP=" +
                std::to_string(state.GetIP()) + ", BSP=0x" +
                std::to_string(state.GetBSP()) + ", BSPSTORE=0x" +
                std::to_string(state.GetBSPSTORE()) + ", dirtyBSP=0x" +
                std::to_string(dirtyBsp) + ")");
    require(state.GetIP() == kFaultingAlloc,
            "failed alloc advanced past the faulting guest instruction");
    require(state.GetCFM() == oldCfm && state.GetPFS() == oldPfs &&
                state.GetBSP() == oldBsp,
            "failed mandatory spill partially committed the alloc frame transition");
    require(state.GetBSPSTORE() == kGuestRamSize,
            "partial spill did not leave BSPSTORE at the first failed word");
    require(readU64(memory, kLastRegisterWord) == 0xE000000000000000ULL,
            "partial mandatory spill did not commit its first successful backing word");
    require(readU64(memory, kLastRnatWord) == 0,
            "partial mandatory spill did not commit the RNAT word at its boundary");
    require(state.GetRSEStoreReg() == 1,
            "partial mandatory spill did not advance its physical store position");
}

void testPartialReturnFillFault() {
    VirtualMachine vm(kGuestRamSize, 1, "IA-64");
    require(vm.init(), "return-fill fault VM initialization failed");
    auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.getCPU().getISAPlugin());
    require(plugin != nullptr, "return-fill fault VM did not select IA64ISAPlugin");
    plugin->reset();

    CPU& cpu = vm.getCPU();
    IMemory& memory = vm.getMemory();
    emitB(memory, kMain, indirectReturn(0));

    constexpr uint64_t kLastRegisterWord = kGuestRamSize - 16;
    constexpr uint64_t kLastRnatWord = kGuestRamSize - 8;
    const uint64_t fillEnd = backingRegisterAddress(kLastRegisterWord, 2);
    require(backingRegisterCount(kLastRegisterWord, fillEnd) == 2,
            "return-fill fault setup did not create a two-register range");
    writeU64(memory, kLastRegisterWord, 0xF1F2F3F4F5F6F7F8ULL);
    writeU64(memory, kLastRnatWord, 1ULL << 62);

    CPUState& state = cpu.getState();
    state.SetCFM(1ULL | (1ULL << 7));
    state.SetPFS(2ULL | (2ULL << 7));
    state.SetRSC(0);
    state.SetBSP(fillEnd);
    state.SetBSPSTORE(fillEnd);
    state.SetRNAT(0);
    state.SetRSEBOFForCheckpoint(2);
    state.SetBR(0, kMain + 0x10);
    cpu.setIP(kMain);

    const uint64_t oldCfm = state.GetCFM();
    const uint64_t oldPfs = state.GetPFS();
    const uint64_t oldBsp = state.GetBSP();
    const uint64_t oldBspstore = state.GetBSPSTORE();
    bool stoppedAtFillFault = false;
    for (size_t slot = 0; slot < 3; ++slot) {
        if (!vm.stepCPU(0)) {
            stoppedAtFillFault = true;
            break;
        }
    }
    require(stoppedAtFillFault,
            "invalid caller register backing read did not stop mandatory br.ret fill");
    require(state.GetIP() == kMain,
            "failed mandatory br.ret fill advanced to its return target");
    require(state.GetCFM() == oldCfm && state.GetPFS() == oldPfs &&
                state.GetBSP() == oldBsp && state.GetBSPSTORE() == oldBspstore,
            "failed mandatory br.ret fill committed the frame or pointer transition");
    require(state.GetGRPhysical(NUM_STATIC_GR) == 0xF1F2F3F4F5F6F7F8ULL &&
                state.GetGRNaTPhysical(NUM_STATIC_GR),
            "partial return fill did not preserve the successful value and RNAT read before the fault");
}

} // namespace

int main() {
    try {
        testCapacityBoundaries();
        testMultipleAutomaticSpillAndFillWaves();
        testPartialSpillFault();
        testPartialReturnFillFault();
        std::cout << "automatic RSE pressure tests passed: capacity boundaries, repeated spill/fill waves, RNAT, and fault tear point\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "automatic RSE pressure test failed: " << error.what() << '\n';
        return 1;
    }
}
