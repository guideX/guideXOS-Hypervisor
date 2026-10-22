#pragma once

#include <cstdint>
#include "riscv_cpu.h"

namespace riscv {

// Sv39 page sizes and constants are declared in riscv_cpu.h.

// satp PPN mask is same as satpPpnMask in riscv_cpu.h.

// SFENCE.VMA base encoding (rs1=0, rs2=0 = flush all).

// Sv39 address translator. Uncached page-table walker: re-reads guest physical
// PTEs on every translation. Single-hart, deterministic, fixed max depth (3).
class Sv39Mmu {
public:
    explicit Sv39Mmu(RV64CPU* cpu);

    Sv39Translation translate(uint64_t vaddr, size_t accessSize, AccessType type,
                              PrivilegeMode priv, bool mprv) const;

    bool pagingEnabled() const;

    void sfenceVma(uint64_t rs1, uint64_t rs2) const;

private:
    RV64CPU* cpu_;

    static uint64_t vpn(uint64_t vaddr, unsigned level);
    bool readPte(uint64_t paddr, uint64_t& pte) const;
    bool writePte(uint64_t paddr, uint64_t pte) const;

    bool walk(uint64_t vaddr, uint64_t& pte, uint64_t& pteAddr, uint64_t& pageSize) const;

    bool validatePte(uint64_t pte, bool atFinalLevel, TrapCause& cause) const;

    bool checkPermissions(uint64_t pte, AccessType type, PrivilegeMode priv,
                          bool mxr, bool sum, TrapCause& cause) const;

    static PrivilegeMode effectivePriv(PrivilegeMode priv, uint64_t mstatus, bool mprv);

    bool updateADBits(uint64_t pteAddr, uint64_t pte, bool setA, bool setD) const;
};

} // namespace riscv
