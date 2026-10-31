#include "riscv/riscv_mmu.h"

#include <cstring>
#include <iostream>

// Sv39 constants are declared in riscv_cpu.h.

namespace riscv {

Sv39Mmu::Sv39Mmu(RV64CPU* cpu) : cpu_(cpu) {}

uint64_t Sv39Mmu::vpn(uint64_t vaddr, unsigned level) {
    // level 0 -> VPN[0] = vaddr[20:12], level 1 -> VPN[1] = vaddr[29:21],
    // level 2 -> VPN[2] = vaddr[38:30].
    return (vaddr >> (12U + 9U * level)) & kVpnMask;
}

bool Sv39Mmu::readPte(uint64_t paddr, uint64_t& pte) const {
    uint8_t bytes[8] = {0};
    if (!cpu_->readMemory(paddr, bytes, sizeof(bytes))) {
        return false;
    }
    pte = static_cast<uint64_t>(bytes[0]) |
          (static_cast<uint64_t>(bytes[1]) << 8) |
          (static_cast<uint64_t>(bytes[2]) << 16) |
          (static_cast<uint64_t>(bytes[3]) << 24) |
          (static_cast<uint64_t>(bytes[4]) << 32) |
          (static_cast<uint64_t>(bytes[5]) << 40) |
          (static_cast<uint64_t>(bytes[6]) << 48) |
          (static_cast<uint64_t>(bytes[7]) << 56);
    return true;
}

bool Sv39Mmu::writePte(uint64_t paddr, uint64_t pte) const {
    uint8_t bytes[8];
    bytes[0] = static_cast<uint8_t>(pte & 0xFFU);
    bytes[1] = static_cast<uint8_t>((pte >> 8) & 0xFFU);
    bytes[2] = static_cast<uint8_t>((pte >> 16) & 0xFFU);
    bytes[3] = static_cast<uint8_t>((pte >> 24) & 0xFFU);
    bytes[4] = static_cast<uint8_t>((pte >> 32) & 0xFFU);
    bytes[5] = static_cast<uint8_t>((pte >> 40) & 0xFFU);
    bytes[6] = static_cast<uint8_t>((pte >> 48) & 0xFFU);
    bytes[7] = static_cast<uint8_t>((pte >> 56) & 0xFFU);
    return cpu_->storeBytes(paddr, bytes, sizeof(bytes));
}

bool Sv39Mmu::validatePte(uint64_t pte, bool atFinalLevel, TrapCause& cause) const {
    // V must be set.
    if ((pte & kPteV) == 0ULL) {
        cause = TrapCause::InstructionPageFault;
        return false;
    }
    // W without R is a reserved encoding.
    if (((pte & kPteW) != 0ULL) && ((pte & kPteR) == 0ULL)) {
        cause = TrapCause::InstructionPageFault;
        return false;
    }
    // Bits 63:54 include unsupported Svnapot/Svpbmt and reserved fields.
    if ((pte & kPteReservedMask) != 0ULL) {
        cause = TrapCause::InstructionPageFault;
        return false;
    }
    if (atFinalLevel) {
        // A leaf has at least one of R/W/X set. W without R is reserved.
        if ((pte & (kPteR | kPteW | kPteX)) == 0ULL) {
            cause = TrapCause::InstructionPageFault;
            return false;
        }
    } else {
        // U, A, and D are reserved in non-leaf PTEs; G and RSW are allowed.
        if ((pte & (kPteU | kPteA | kPteD)) != 0ULL) {
            cause = TrapCause::InstructionPageFault;
            return false;
        }
    }
    cause = TrapCause::IllegalInstruction; // sentinel: ok
    return true;
}

bool Sv39Mmu::checkPermissions(uint64_t pte, AccessType type, PrivilegeMode priv,
                               bool mxr, bool sum, TrapCause& cause) const {
    // M-mode bypasses all permission checks.
    if (priv == PrivilegeMode::Machine) {
        cause = TrapCause::IllegalInstruction; // ok
        return true;
    }

    const bool userPage = (pte & kPteU) != 0ULL;
    const bool isUser = (priv == PrivilegeMode::User);

    // U-mode can only access U pages.
    if (isUser && !userPage) {
        cause = TrapCause::InstructionPageFault;
        return false;
    }

    // S-mode may load/store a U page when SUM=1, but may never fetch from it.
    if (userPage && !isUser && (type == AccessType::Fetch || !sum)) {
        cause = TrapCause::InstructionPageFault;
        return false;
    }

    const bool pteR = (pte & kPteR) != 0ULL;
    const bool pteW = (pte & kPteW) != 0ULL;
    const bool pteX = (pte & kPteX) != 0ULL;

    // MXR permits loads from execute-only pages at either S or U privilege.
    const bool canRead = pteR || (mxr && pteX);
    const bool canWrite = pteW;
    const bool canExecute = pteX;

    switch (type) {
        case AccessType::Fetch:
            if (!canExecute) {
                cause = TrapCause::InstructionPageFault;
                return false;
            }
            break;
        case AccessType::Load:
            if (!canRead) {
                cause = TrapCause::InstructionPageFault;
                return false;
            }
            break;
        case AccessType::Store:
            if (!canWrite) {
                cause = TrapCause::InstructionPageFault;
                return false;
            }
            break;
    }
    cause = TrapCause::IllegalInstruction; // ok
    return true;
}

PrivilegeMode Sv39Mmu::effectivePriv(PrivilegeMode priv, uint64_t mstatus, bool mprv) {
    if (mprv && priv == PrivilegeMode::Machine) {
        const uint64_t mpp = (mstatus >> 11) & 0x3ULL;
        if (mpp == 3ULL) return PrivilegeMode::Machine;
        if (mpp == 1ULL) return PrivilegeMode::Supervisor;
        return PrivilegeMode::User; // MPP==2 reserved -> U
    }
    return priv;
}

bool Sv39Mmu::updateADBits(uint64_t pteAddr, uint64_t pte, bool setA, bool setD) const {
    uint64_t newPte = pte;
    if (setA) {
        newPte |= kPteA;
    }
    if (setD) {
        newPte |= kPteD;
    }
    if (newPte != pte) {
        return writePte(pteAddr, newPte);
    }
    return true;
}

bool Sv39Mmu::pagingEnabled() const {
    uint64_t satp = 0;
    if (!cpu_->readCsr(0x180U, satp)) {
        return false;
    }
    return ((satp >> kSatpModeShift) & 0xFULL) == kSatpModeSv39;
}

void Sv39Mmu::sfenceVma(uint64_t /*rs1*/, uint64_t /*rs2*/) const {
    // Uncached walker: no translations to invalidate. This is recognized as
    // a synchronization point; the next walk re-reads guest physical PTEs,
    // satisfying the single-hart SFENCE.VMA contract.
}

Sv39Translation Sv39Mmu::translate(uint64_t vaddr, size_t accessSize, AccessType type,
                                   PrivilegeMode priv, bool mprv) const {
    Sv39Translation result;
    // The CPU's physical read/write boundary checks the complete access size
    // after translation and reports an access fault for an inaccessible PA.
    (void)accessSize;
    const auto faultCause = [type](bool accessFault) {
        if (type == AccessType::Fetch) {
            return accessFault ? TrapCause::InstructionAccessFault
                               : TrapCause::InstructionPageFault;
        }
        if (type == AccessType::Store) {
            return accessFault ? TrapCause::StoreAccessFault
                               : TrapCause::StorePageFault;
        }
        return accessFault ? TrapCause::LoadAccessFault : TrapCause::LoadPageFault;
    };

    uint64_t mstatus = 0;
    cpu_->readCsr(0x300U, mstatus);
    const bool mxr = (mstatus & kMstatusMxr) != 0ULL;
    const bool sum = (mstatus & kMstatusSum) != 0ULL;
    const PrivilegeMode effPriv = effectivePriv(priv, mstatus, mprv);

    // Bare mode and effective M-mode accesses bypass S-stage translation.
    // In particular, MPRV=1 with MPP=M is still a physical M-mode access.
    if (!pagingEnabled() || effPriv == PrivilegeMode::Machine) {
        result.ok = true;
        result.paddr = vaddr;
        result.pageSize = kPageSize4K;
        return result;
    }

    // Canonical address check: VA[63:39] must equal VA[38] (sign extension).
    const uint64_t signBit = (vaddr >> 38ULL) & 1ULL;
    const uint64_t signExt = signBit ? ~kSv39VaddrMask : 0ULL;
    if ((vaddr & ~kSv39VaddrMask) != signExt) {
        result.cause = (type == AccessType::Fetch) ? TrapCause::InstructionPageFault :
                         (type == AccessType::Store) ? TrapCause::StorePageFault :
                                                       TrapCause::LoadPageFault;
        return result;
    }

    // Walk the page table.
    uint64_t pte = 0;
    uint64_t pteAddr = 0;
    uint64_t pageSize = 0;
    const WalkResult walkResult = walk(vaddr, pte, pteAddr, pageSize);
    if (walkResult != WalkResult::Success) {
        result.cause = faultCause(walkResult == WalkResult::AccessFault);
        return result;
    }

    // Check permissions.
    TrapCause permCause = TrapCause::InstructionPageFault;
    if (!checkPermissions(pte, type, effPriv, mxr, sum, permCause)) {
        result.cause = (type == AccessType::Fetch) ? TrapCause::InstructionPageFault :
                         (type == AccessType::Store) ? TrapCause::StorePageFault :
                                                       TrapCause::LoadPageFault;
        return result;
    }

    // Build physical address: PA = (PPN << 12) | (VA & page_mask).
    const uint64_t ppn = (pte & kPtePPNMask) >> 10;
    const uint64_t pageMask = pageSize - 1ULL;
    const uint64_t offset = vaddr & pageMask;
    result.paddr = (ppn << 12ULL) | offset;
    result.pageSize = pageSize;

    // Update A/D bits: A on any qualifying access, D on qualifying store.
    bool setA = true;
    bool setD = (type == AccessType::Store);
    if ((setA || setD) && !updateADBits(pteAddr, pte, setA, setD)) {
        result.cause = faultCause(true);
        return result;
    }

    result.ok = true;
    result.cause = TrapCause::InstructionAccessFault; // sentinel: no fault
    return result;
}

Sv39Mmu::WalkResult Sv39Mmu::walk(uint64_t vaddr, uint64_t& pte,
                                  uint64_t& pteAddr, uint64_t& pageSize) const {
    pageSize = 0;

    // Read satp to get root PPN and mode.
    uint64_t satp = 0;
    if (!cpu_->readCsr(0x180U, satp)) {
        return WalkResult::PageFault;
    }
    const uint64_t mode = (satp >> kSatpModeShift) & 0xFULL;
    if (mode != kSatpModeSv39) {
        return WalkResult::PageFault;
    }
    const uint64_t rootPPN = satp & kSatpPpnMask;

    // Fixed maximum depth: levels 2, 1, 0.
    uint64_t ppn = rootPPN;
    for (int level = 2; level >= 0; --level) {
        const uint64_t vpnBits = vpn(vaddr, static_cast<unsigned>(level));

        // Overflow guard: ppn << 12 must not exceed 64 bits.
        if (ppn > (0xFFFFFFFFFFFFFFFFULL >> 12)) {
            return WalkResult::AccessFault;
        }
        const uint64_t pteAddrCandidate = (ppn << 12ULL) | (vpnBits << kPteShift);

        // PTE address must be within guest memory for 8 bytes.
        if (!cpu_->rangeInGuest(pteAddrCandidate, kPteSize)) {
            return WalkResult::AccessFault;
        }

        if (!readPte(pteAddrCandidate, pte)) {
            return WalkResult::AccessFault;
        }
        pteAddr = pteAddrCandidate;

        // A leaf has any of R/W/X set. Validate every PTE, including non-leaves.
        const bool isLeaf = (pte & (kPteR | kPteW | kPteX)) != 0ULL;
        TrapCause validationCause = TrapCause::InstructionPageFault;
        if (!validatePte(pte, isLeaf, validationCause)) {
            return WalkResult::PageFault;
        }

        if (isLeaf) {
            // Superpage alignment checks.
            const uint64_t ppnVal = (pte & kPtePPNMask) >> 10;
            if (level == 2) {
                // 1 GiB page: PPN[1:0] comprises the low 18 PPN bits.
                if ((ppnVal & 0x3FFFFULL) != 0ULL) {
                    return WalkResult::PageFault;
                }
                pageSize = kPageSize1G;
            } else if (level == 1) {
                // 2 MiB page: all nine PPN[0] bits must be 0.
                if ((ppnVal & 0x1FFULL) != 0ULL) {
                    return WalkResult::PageFault;
                }
                pageSize = kPageSize2M;
            } else {
                pageSize = kPageSize4K;
            }
            return WalkResult::Success;
        }

        // Non-leaf: V=1, R=0, W=0. Extract PPN for next-level table base.
        ppn = (pte & kPtePPNMask) >> 10;

        // At level 0, a non-leaf valid PTE has no next level: invalid.
        if (level == 0) {
            return WalkResult::PageFault;
        }
    }

    return WalkResult::PageFault;
}

} // namespace riscv
