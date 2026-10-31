#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "riscv_decoder.h"

namespace riscv {

// RISC-V has 32 general-purpose registers (x0-x31)
// x0 is hardwired to 0 (always reads zero, writes ignored)
// x1-x31 are general purpose
constexpr size_t NUM_GENERAL_REGISTERS = 32;

// RISC-V has 64-bit architecture
using reg_t = uint64_t;
using addr_t = uint64_t;

/**
 * PrivilegeMode - RV64 architectural privilege state (Phase RISCV-2).
 *
 * Encodings match the RISC-V privileged specification (U=0, S=1, M=3).
 * Encoding 2 is reserved and never reported by privilege().
 * Reset state is Machine mode.
 */
enum class PrivilegeMode : uint8_t {
    User = 0,
    Supervisor = 1,
    Machine = 3,
};

inline const char* PrivilegeModeToString(PrivilegeMode mode) {
    switch (mode) {
        case PrivilegeMode::User: return "U";
        case PrivilegeMode::Supervisor: return "S";
        case PrivilegeMode::Machine: return "M";
    }
    return "unknown";
}

/**
 * TrapCause - synchronous exception causes implemented in RISCV-2.
 * Values match the RISC-V privileged specification (mcause/scause code
 * field with the interrupt bit clear).
 */
enum class TrapCause : uint64_t {
    InstructionAddressMisaligned = 0,
    InstructionAccessFault = 1,
    IllegalInstruction = 2,
    Breakpoint = 3,
    LoadAddressMisaligned = 4,
    LoadAccessFault = 5,
    StoreAddressMisaligned = 6,
    StoreAccessFault = 7,
    EcallFromU = 8,
    EcallFromS = 9,
    EcallFromM = 11,
    // RISCV-3: Sv39 page-fault causes.
    InstructionPageFault = 12,
    LoadPageFault = 13,
    StorePageFault = 15,
};

inline const char* TrapCauseToString(TrapCause cause) {
    switch (cause) {
        case TrapCause::InstructionAddressMisaligned: return "instr_addr_misaligned";
        case TrapCause::InstructionAccessFault: return "instr_access_fault";
        case TrapCause::IllegalInstruction: return "illegal_insn";
        case TrapCause::Breakpoint: return "breakpoint";
        case TrapCause::LoadAddressMisaligned: return "load_addr_misaligned";
        case TrapCause::LoadAccessFault: return "load_access_fault";
        case TrapCause::StoreAddressMisaligned: return "store_addr_misaligned";
        case TrapCause::StoreAccessFault: return "store_access_fault";
        case TrapCause::EcallFromU: return "ecall_from_u";
        case TrapCause::EcallFromS: return "ecall_from_s";
        case TrapCause::EcallFromM: return "ecall_from_m";
        case TrapCause::InstructionPageFault: return "instruction_page_fault";
        case TrapCause::LoadPageFault: return "load_page_fault";
        case TrapCause::StorePageFault: return "store_page_fault";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// mstatus fields (RV64; subset implemented in RISCV-2, extended in RISCV-3).
// ---------------------------------------------------------------------------

constexpr uint64_t kMstatusSie = 1ULL << 1;
constexpr uint64_t kMstatusMie = 1ULL << 3;
constexpr uint64_t kMstatusSpie = 1ULL << 5;
constexpr uint64_t kMstatusMpie = 1ULL << 7;
constexpr uint64_t kMstatusSpp = 1ULL << 8;
constexpr uint64_t kMstatusMppMask = 3ULL << 11;
constexpr uint64_t kMstatusMprv = 1ULL << 17;
constexpr uint64_t kMstatusSum = 1ULL << 18;
constexpr uint64_t kMstatusMxr = 1ULL << 19;
constexpr uint64_t kMstatusTvm = 1ULL << 20;
constexpr uint64_t kMstatusWritableMask =
    kMstatusSie | kMstatusMie | kMstatusSpie | kMstatusMpie | kMstatusSpp |
    kMstatusMppMask | kMstatusMprv | kMstatusSum | kMstatusMxr;
constexpr uint64_t kSstatusMask =
    kMstatusSie | kMstatusSpie | kMstatusSpp | kMstatusSum | kMstatusMxr;

// ---------------------------------------------------------------------------
// Sv39 virtual-memory constants (RISCV-3).
// ---------------------------------------------------------------------------

constexpr uint64_t kPageSize4K = 0x1000ULL;
constexpr uint64_t kPageSize2M = 0x200000ULL;
constexpr uint64_t kPageSize1G = 0x40000000ULL;

constexpr unsigned kSv39VaddrBits = 39;
constexpr uint64_t kSv39VaddrMask = (1ULL << kSv39VaddrBits) - 1ULL;
constexpr unsigned kPageOffsetBits = 12;
constexpr uint64_t kPageOffsetMask = kPageSize4K - 1ULL;
constexpr unsigned kVpnBits = 9;
constexpr uint64_t kVpnMask = 0x1FFULL;
constexpr unsigned kPteSize = 8;
constexpr unsigned kPteShift = 3;

constexpr uint64_t kPteV   = 1ULL << 0;
constexpr uint64_t kPteR   = 1ULL << 1;
constexpr uint64_t kPteW   = 1ULL << 2;
constexpr uint64_t kPteX   = 1ULL << 3;
constexpr uint64_t kPteU   = 1ULL << 4;
constexpr uint64_t kPteG   = 1ULL << 5;
constexpr uint64_t kPteA   = 1ULL << 6;
constexpr uint64_t kPteD   = 1ULL << 7;
constexpr uint64_t kPtePPNMask = 0xFFFFFFFFFFFULL << 10; // PPN[43:0], bits 53:10.
constexpr uint64_t kPteReservedMask = 0x3FFULL << 54; // Unsupported extension/reserved bits 63:54.

constexpr uint64_t kSatpModeShift = 60;
constexpr uint64_t kSatpModeMask = 0xFULL << kSatpModeShift;
constexpr uint64_t kSatpModeBare = 0ULL;
constexpr uint64_t kSatpModeSv39 = 8ULL;
constexpr uint64_t kSatpPpnMask = 0xFFFFFFFFFFFULL; // Sv39 PPN[43:0].

constexpr uint32_t kSfenceVmaOpcode = 0x12000073U; // sfence.vma x0,x0.

enum class AccessType : uint8_t { Fetch, Load, Store };

struct Sv39Translation {
    bool ok = false;
    uint64_t paddr = 0;
    uint64_t pageSize = 0;
    TrapCause cause = TrapCause::InstructionAccessFault;
};

class Sv39Mmu;

/**
 * RV64CPUState - RISC-V CPU State
 *
 * Encapsulates all architectural registers and state for the RISC-V CPU:
 * 32 general-purpose registers (x0 hardwired to zero) and a 64-bit PC.
 * Deterministic reset zeroes all registers and PC.
 */
class RV64CPUState {
public:
    RV64CPUState();
    ~RV64CPUState() = default;

    // General register access (64-bit)
    reg_t GetGR(size_t index) const;
    void SetGR(size_t index, reg_t value);

    // Instruction pointer (PC)
    addr_t GetPC() const { return pc_; }
    void SetPC(addr_t value) { pc_ = value; }

    // Reset CPU to initial state
    void Reset();

    // Dump CPU state for debugging
    void Dump() const;

private:
    // General registers (64-bit)
    std::array<reg_t, NUM_GENERAL_REGISTERS> gr_;

    // Program counter
    addr_t pc_;
};

/**
 * StepOutcome - explicit result of one RV64CPU::step() call.
 *
 * RISCV-1 outcomes are preserved:
 * - Ok: instruction fetched, decoded with the production decoder, executed
 *   with correct semantics; registers, guest memory and PC updated.
 * - Halt: EBREAK (32-bit) or C.EBREAK breakpoint/stop marker when
 *   breakpoint-halt interception is enabled (default true, preserves
 *   RISCV-1 deterministic tests). PC points at the breakpoint instruction
 *   (not advanced).
 * - Unsupported: valid decoded instruction with no execution support in this
 *   phase (includes all compressed instructions except C.EBREAK, plus M/A/F/D/V
 *   and unimplemented privileged ops such as WFI). PC points at the
 *   unexecuted instruction (not advanced).
 * - Illegal / FetchFault / MemoryFault: retained for source compatibility.
 *   In architectural RISCV-2 execution these conditions are delivered as
 *   synchronous traps (outcome Trap, see below) whenever a guest image is
 *   loaded. The legacy outcomes are returned only when no trap can be
 *   delivered (e.g. step with no guest image loaded) or for the truncated
 *   corner reported before CSR state exists. New tests must expect Trap.
 * - Trap (new in RISCV-2): a synchronous exception was delivered
 *   architecturally. CSRs (xEPC/xCAUSE/xTVAL, status stack) are updated,
 *   privilege is updated, and PC points at the handler (mtvec/stvec BASE).
 *   result.trapTaken is true and trapCause/trapTarget describe delivery.
 *   This covers illegal instructions, breakpoints (when halt interception is
 *   off), misaligned addresses, access faults, and ECALLs (including the
 *   SBI interception path, which still performs full M-mode trap entry).
 */
enum class StepOutcome {
    Ok,
    Halt,
    Unsupported,
    Illegal,
    FetchFault,
    MemoryFault,
    Trap,
};

inline const char* StepOutcomeToString(StepOutcome outcome) {
    switch (outcome) {
        case StepOutcome::Ok: return "ok";
        case StepOutcome::Halt: return "halt";
        case StepOutcome::Unsupported: return "unsupported";
        case StepOutcome::Illegal: return "illegal";
        case StepOutcome::FetchFault: return "fetch_fault";
        case StepOutcome::MemoryFault: return "memory_fault";
        case StepOutcome::Trap: return "trap";
    }
    return "unknown";
}

/**
 * StepResult - details of one executed step.
 * pc is the entry PC; nextPc is the architectural PC after the step.
 * When outcome == Trap, trapTaken is true and trapCause/trapTarget/trapVector
 * describe delivery; sbiHandled is true when the attached SBI Base provider
 * supplied a0/a1 return values during M-mode trap entry for an S-mode ECALL.
 */
struct StepResult {
    StepOutcome outcome = StepOutcome::FetchFault;
    uint64_t pc = 0;
    uint64_t nextPc = 0;
    uint8_t length = 0;
    DecodedInstruction insn;
    bool trapTaken = false;
    TrapCause trapCause = TrapCause::IllegalInstruction;
    PrivilegeMode trapTarget = PrivilegeMode::Machine;
    uint64_t trapVector = 0;
    bool sbiHandled = false;
};

// SBI error codes (SBI Base, returned in a0 as signed 64-bit values).
constexpr uint64_t kSbiSuccess = 0ULL;
constexpr uint64_t kSbiErrFailed = static_cast<uint64_t>(-1LL);
constexpr uint64_t kSbiErrNotSupported = static_cast<uint64_t>(-2LL);
constexpr uint64_t kSbiErrInvalidParam = static_cast<uint64_t>(-3LL);

// SBI Base extension ID.
constexpr uint64_t kSbiBaseEid = 0x10ULL;

// Documented SBI version surface (SBI v2.0: major 2, minor 0).
constexpr uint64_t kSbiSpecVersion = (2ULL << 24) | 0ULL;  // 0x2000000
constexpr uint64_t kSbiImplId = 0x47584F53ULL;            // ASCII "GXOS"
constexpr uint64_t kSbiImplVersion = 2ULL;

struct SbiReturn {
    uint64_t error = kSbiErrNotSupported;
    uint64_t value = 0ULL;
};

/**
 * ISbiProvider - firmware interception boundary for S-mode ECALLs.
 *
 * The CPU performs full architectural trap entry to M-mode first (mepc,
 * mcause, mtval, privilege, PC=mtvec) and then, only when the trap targets
 * M-mode for an S-mode ECALL, invokes the attached provider to supply the
 * a0 (error) / a1 (value) return registers. Ordinary ECALL trap semantics
 * are never suppressed: with no provider attached the trap is still
 * delivered and guest registers are untouched.
 */
class ISbiProvider {
public:
    virtual ~ISbiProvider() = default;
    // Handle an S-mode ECALL trapped to M-mode. args holds guest a0-a5.
    virtual SbiReturn handle(uint64_t eid, uint64_t fid, const uint64_t args[6]) = 0;
    // True when eid is advertised by this provider.
    virtual bool supports(uint64_t eid) const = 0;
};

/**
 * SbiBaseProvider - deterministic test SBI Base extension (EID 0x10).
 *
 * Implements the SBI Base functions required by RISCV-2:
 *   FID 0 GetSpecVersion, 1 GetImplID, 2 GetImplVersion,
 *   3 ProbeExtension, 4 GetMVendorID, 5 GetMArchID, 6 GetMImpID.
 * Any other Base FID returns NotSupported. Any non-Base EID reports
 * unsupported (supports() false, handle() NotSupported) so tests can verify
 * that unsupported extensions are never advertised.
 */
class SbiBaseProvider : public ISbiProvider {
public:
    SbiReturn handle(uint64_t eid, uint64_t fid, const uint64_t args[6]) override;
    bool supports(uint64_t eid) const override;
};

/**
 * RV64CPU - standalone RISC-V RV64 CPU core (Phase RISCV-2).
 *
 * Compatibility decision (deliberate, documented):
 * - RV64CPU is standalone and does NOT inherit ia64::ICPU. The IA-64
 *   interface carries predicate registers, branch registers, frame markers
 *   (CFM), rotation bases and ia64::CPUState, none of which exist in RV64.
 *   No in-tree consumer uses RV64CPU through ia64::ICPU (verified by repo
 *   search: only riscv_cpu.* and the RISC-V test reference RV64CPU), so no
 *   adapter is retained. The IA-64 ICPU interface is intentionally untouched.
 * - Guest memory is a single bounded byte region [base, base+size) covering
 *   both instruction fetch and data access, little-endian, single-hart with
 *   no caches: FENCE/FENCE.I are documented no-ops that retire normally.
 * - Each step() fetches at the guest PC, determines length from the low two
 *   bits, decodes with the production riscv::Decoder, and executes the full
 *   RV64I integer set plus Zicsr CSR access and SYSTEM privileged
 *   instructions with trap delivery. M/A/F/D/V and compressed execution stay
 *   Unsupported (never Ok), except C.EBREAK which is Halt (halt interception
 *   on) or a breakpoint trap (halt interception off).
 * - Reset state: privilege Machine, PC = guest base (or 0 with no image),
 *   GPRs zero, CSRs to their reset values (mtvec/stvec 0 Direct, mstatus 0,
 *   medeleg/mideleg 0, mie/mip/sie/sip 0, mepc/sepc/mcause/scause/mtval/stval/
 *   mscratch/sscratch 0, misa fixed, mhartid 0, satp Bare 0).
 * - Guest proof programs enter lower privilege architecturally (mstatus.MPP
 *   + mepc + MRET, or SPP + sepc + SRET); the production API exposes no
 *   privilege setter. Focused unit tests may use the friend-only test helper.
 * - satp supports Bare (MODE 0) and Sv39 (MODE 8), with ASIDLEN=0; other
 *   MODE writes are WARL-ignored. Bare mode is a direct byte-region mapping,
 *   not paging or PMP isolation.
 */
class RV64CPU {
    friend class Sv39Mmu;
    friend class Sv39TestHelper;
public:
    RV64CPU();
    ~RV64CPU();

    // Deterministic reset: registers/PC zeroed, privilege Machine, CSRs to
    // reset values, counters cleared. Loaded guest memory is preserved across
    // reset and PC restarts at its base.
    void reset();

    // Load a bounded guest-memory image at [base, base+bytes.size()).
    // Replaces any previous image. Resets CPU state (privilege Machine) and
    // sets PC to base.
    void loadProgram(uint64_t base, const std::vector<uint8_t>& bytes);

    // Execute exactly one instruction at the current PC.
    StepResult step();

    // Register access (x0 hardwired to zero; out-of-range reads 0, writes ignored)
    uint64_t readGR(size_t index) const;
    void writeGR(size_t index, uint64_t value);

    // PC access. getIP/setIP are transitional aliases of getPC/setPC.
    uint64_t getPC() const;
    void setPC(uint64_t addr);
    uint64_t getIP() const { return getPC(); }
    void setIP(uint64_t addr) { setPC(addr); }

    // RISC-V architectural state access
    RV64CPUState& getState() { return state_; }
    const RV64CPUState& getState() const { return state_; }

    // Bounded guest-memory access (little-endian byte copies)
    uint64_t memoryBase() const { return memBase_; }
    size_t memorySize() const { return mem_.size(); }
    bool readMemory(uint64_t addr, uint8_t* dst, size_t size) const;
    bool readU64(uint64_t addr, uint64_t& value) const;

    // Sv39 translation (RISCV-3). Translate a virtual address to a guest
    // physical address; Bare mode is identity. Returns ok=false with a page
    // or access-fault cause on translation failure. Explicit physical-memory
    // bounds are checked by the CPU access after translation.
    Sv39Translation translateFetch(uint64_t vaddr, size_t size) const;
    Sv39Translation translateLoad(uint64_t vaddr, size_t size) const;
    Sv39Translation translateStore(uint64_t vaddr, size_t size) const;

    // Direct guest-physical read/write (bypasses translation). Used by the
    // page-table walker and by M-mode accesses when paging is bypassed.
    bool readPhysical(uint64_t paddr, uint8_t* dst, size_t size) const;
    bool writePhysical(uint64_t paddr, const uint8_t* src, size_t size);

    // Direct guest-physical write helper retained for test compatibility.
    // It bypasses translation; guest stores use RV64CPU::step().
    bool storeBytes(uint64_t addr, const uint8_t* src, size_t size) {
        return writePhysical(addr, src, size);
    }

    // MMU access and paging state.
    Sv39Mmu* mmu() { return mmu_; }
    const Sv39Mmu* mmu() const { return mmu_; }
    bool pagingEnabled() const;

    // Privilege and CSR access (architectural).
    PrivilegeMode privilege() const { return priv_; }
    bool readCsr(uint32_t csr, uint64_t& value) const;
    // Direct CSR write for harness setup verification only; enforces WARL
    // masks but not privilege checks. Prefer guest CSR instructions.
    void debugWriteCsr(uint32_t csr, uint64_t value);

    // Breakpoint-halt interception (default true for RISCV-1 compatibility).
    // When true, EBREAK/C.EBREAK report Halt. When false, they deliver a
    // breakpoint trap (cause 3) architecturally.
    bool breakpointHaltEnabled() const { return breakpointHalt_; }
    void setBreakpointHaltEnabled(bool enabled) { breakpointHalt_ = enabled; }

    // SBI firmware interception boundary (non-owning; may be null).
    void setSbiProvider(ISbiProvider* provider) { sbi_ = provider; }
    ISbiProvider* sbiProvider() const { return sbi_; }

    // Deterministic-harness counters. stepsTaken counts every step() call;
    // instructionsRetired counts only Ok outcomes (traps/halts/faults do not
    // retire).
    uint64_t stepsTaken() const { return stepsTaken_; }
    uint64_t instructionsRetired() const { return instructionsRetired_; }

    void dump() const;

private:
    bool fetchBytes(uint64_t addr, uint8_t* dst, size_t size);
    bool loadU64Internal(uint64_t addr, uint64_t& value) const;
    bool storeU64(uint64_t addr, uint64_t value);
    bool rangeInGuest(uint64_t addr, size_t size) const;

    // CSR helpers.
    bool csrSupported(uint32_t csr) const;
    bool csrRead(uint32_t csr, uint64_t& value) const;
    // Applies WARL masks; returns false for unknown CSRs.
    bool csrWrite(uint32_t csr, uint64_t value);
    static uint32_t csrPrivRequired(uint32_t csr);
    static bool csrIsReadOnly(uint32_t csr);

    // Deliver a synchronous trap: updates xEPC/xCAUSE/xTVAL, status stack,
    // privilege and PC. Fills result trap fields with outcome Trap.
    void deliverTrap(TrapCause cause, uint64_t tval, uint64_t faultPc, StepResult& result);

    RV64CPUState state_;
    Decoder decoder_;
    std::vector<uint8_t> mem_;
    uint64_t memBase_ = 0;
    bool memLoaded_ = false;
    uint64_t stepsTaken_ = 0;
    uint64_t instructionsRetired_ = 0;

    PrivilegeMode priv_ = PrivilegeMode::Machine;
    bool breakpointHalt_ = true;
    ISbiProvider* sbi_ = nullptr;
    Sv39Mmu* mmu_ = nullptr;

    // Machine state.
    uint64_t mstatus_ = 0;
    uint64_t medeleg_ = 0;
    uint64_t mideleg_ = 0;
    uint64_t mie_ = 0;
    uint64_t mip_ = 0;
    uint64_t mtvec_ = 0;
    uint64_t mscratch_ = 0;
    uint64_t mepc_ = 0;
    uint64_t mcause_ = 0;
    uint64_t mtval_ = 0;
    // Supervisor state (sie/sip are views of mie/mip, sstatus of mstatus).
    uint64_t stvec_ = 0;
    uint64_t sscratch_ = 0;
    uint64_t sepc_ = 0;
    uint64_t scause_ = 0;
    uint64_t stval_ = 0;
    uint64_t satp_ = 0;
};

}  // namespace riscv
