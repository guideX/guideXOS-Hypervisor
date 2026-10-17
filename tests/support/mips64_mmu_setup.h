// guideXOS Hypervisor — MIPS64-3 test support (NOT production code).
//
// Genuine-MMU guest setup helpers shared by the MIPS64 focused tests.
// Everything here goes through real encoded MIPS64r2 instructions executed
// by the production decoder/executor; there is no host-side TLB injection.
//
// Address discipline (MIPS64-3): the repository IMemory abstraction is
// PHYSICAL memory. Test fixtures write instruction/data bytes at physical
// addresses; guest virtual addresses reach them via CPU translation:
//   - kuseg VAs covered by setupKusegIdentityMap() translate VA == PA.
//   - ckseg0/ckseg1 VAs translate as PA = VA & 0x1FFFFFFF, so fixtures must
//     store handler/vector bytes at the masked physical address
//     (storeGuestWord() does this).

#pragma once

#include <cstdint>
#include <vector>

#include "IMemory.h"
#include "mips64/Mips64Cpu.h"

namespace mips64_test {

inline uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | funct;
}

inline uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | imm;
}

inline uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel) {
    return (static_cast<uint32_t>(0x10) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | (static_cast<uint32_t>(rd) << 11) | sel;
}

// Raw physical store of one instruction word.
inline void storeWordPhys(ia64::IMemory& mem, uint64_t phys, uint32_t word,
                           bool littleEndian) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[littleEndian ? i : 3 - i] = static_cast<uint8_t>((word >> (8 * i)) & 0xFFU);
    }
    mem.Write(phys, bytes, 4);
}

inline void fillPhysZeros(ia64::IMemory& mem, uint64_t phys, unsigned size) {
    std::vector<uint8_t> zeros(size, 0);
    mem.Write(phys, zeros.data(), zeros.size());
}

// Guest-VA-aware store: resolves the physical address for directly
// writable segments (kuseg identity assumed by the caller after
// setupKusegIdentityMap(); ckseg0/ckseg1 via PA = VA & 0x1FFFFFFF).
// Returns false for VAs with no direct physical image (mapped kernel
// segments, unsupported 64-bit regions).
inline bool guestPhysAddr(uint64_t va, uint64_t& phys) {
    if (va <= mips64::kKusegEnd) {
        phys = va;  // caller-installed identity mapping
        return true;
    }
    if (va >= mips64::kCKSeg0Base && va <= mips64::kCKSeg1End) {
        phys = va & mips64::kDirectMapMask;
        return true;
    }
    return false;
}

inline bool storeGuestWord(ia64::IMemory& mem, uint64_t va, uint32_t word,
                            bool littleEndian) {
    uint64_t phys = 0;
    if (!guestPhysAddr(va, phys)) {
        return false;
    }
    storeWordPhys(mem, phys, word, littleEndian);
    return true;
}

inline bool fillGuestZeros(ia64::IMemory& mem, uint64_t va, unsigned size) {
    uint64_t phys = 0;
    if (!guestPhysAddr(va, phys)) {
        return false;
    }
    fillPhysZeros(mem, phys, size);
    return true;
}

// Install TLB entry 0 as a 16 MiB paired-page identity mapping covering
// VA == PA over 0x00000000..0x01FFFFFF (kuseg), using genuine encoded
// instructions (DMTC0/MTC0/TLBWI) executed from a CKSEG0 scratch window.
// Requires effective kernel mode (true after reset). Preserves PC and all
// GPRs ($k0/$26 is used transiently and restored); Status/Cause/EPC are
// untouched; Count ticks once per prologue instruction (10 steps).
// scratchPhys selects the 36-byte CKSEG0 scratch window (must be mapped
// physical memory, 4-byte aligned, clear of guest code/data).
    // Returns false if any prologue instruction fails to retire.
    inline bool setupKusegIdentityMap(mips64::Mips64Cpu& cpu, ia64::IMemory& mem,
                                      uint64_t scratchPhys = 0x6000ULL) {
    using namespace mips64;
    const bool le = cpu.isLittleEndian();
    // CKSEG0 scratch: VA 0xFFFFFFFF80000000|P aliases physical P (direct map).
    const uint64_t kScratchPhys = scratchPhys;
    const uint64_t kScratchVirt = 0xFFFFFFFF80000000ULL | (scratchPhys & 0x1FFFFFFFULL);
    const uint32_t prologue[10] = {
        encCp0(5, 0, 10, 0),          // dmtc0 $0, EntryHi (VPN2=0, ASID=0)
        (0x0FU << 26) | (26U << 16) | 0x1FFU,  // lui $k0, 0x1FF
        (0x0DU << 26) | (26U << 21) | (26U << 16) | 0xE000U,  // ori $k0,$k0,0xE000
        encCp0(5, 26, 5, 0),          // dmtc0 $k0, PageMask (16 MiB pages)
        (0x19U << 26) | (26U << 16) | 7U,  // daddiu $k0, $0, 7 (pfn 0, D|V|G)
        encCp0(5, 26, 2, 0),          // dmtc0 $k0, EntryLo0
        (0x19U << 26) | (26U << 16) | 7U,  // daddiu $k0, $0, 7 (pfn 0, D|V|G)
        encCp0(5, 26, 3, 0),          // dmtc0 $k0, EntryLo1
        encCp0(5, 0, 0, 0),           // dmtc0 $0, Index (entry 0)
        0x42000002U,                  // tlbwi
    };
    for (size_t i = 0; i < 10; ++i) {
        storeWordPhys(mem, kScratchPhys + static_cast<uint64_t>(i) * 4, prologue[i], le);
    }
    const uint64_t savedPc = cpu.pc();
    const uint64_t savedK0 = cpu.readGpr(26);
    cpu.setPc(kScratchVirt);
    bool ok = true;
    for (size_t i = 0; i < 10; ++i) {
        if (cpu.step(mem) != mips64::Mips64StepResult::Continue) {
            ok = false;
            break;
        }
    }
    cpu.writeGpr(26, savedK0);
    cpu.setPc(savedPc);
    return ok;
}

}  // namespace mips64_test
