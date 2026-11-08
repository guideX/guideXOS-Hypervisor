#pragma once

// AArch64 (ARMv8-A, A64 instruction set) CPU architectural state,
// instruction fetch/decode and execution for guideXOS Hypervisor.
//
// SCOPE (Phase AARCH64-1):
//   - General-purpose registers X0-X30, banked SP_EL0/SP_EL1, PC and a
//     modeled EL0/EL1 PSTATE including NZCV, DAIF, EL and SPSel.
//   - Register 31 is context-sensitive (SP vs XZR/WZR) and is NEVER modelled
//     as one ordinary register.
//   - A64 fixed 32-bit little-endian fetch, deterministic reset/entry profile,
//     bounded stepping, and explicit faults for everything unsupported.
// SCOPE (Phase AARCH64-3, additive):
//   - ADR / ADRP (PC-relative address generation, Rd==31 is XZR).
//   - LDR (literal, immediate) 32/64-bit general-purpose forms through
//     IMemory (Rt==31 is WZR/XZR; failed loads preserve dest and PC).
//   - MOVZ/MOVK unchanged; full-width synthesis validated by tests.
//   - Nested BL/RET programs using SUB/ADD SP + STR/LDR unsigned-offset
//     stack frames (no STP/LDP required).
// SCOPE (Phase AARCH64-4, additive):
//   - STP/LDP register pairs: signed-offset, pre-index and post-index
//     integer W/X forms, single contiguous pair transfer, documented
//     CONSTRAINED-UNPREDICTABLE rejection policy.
//   - LDUR/STUR unscaled signed-offset accesses (imm9, no writeback).
//   - LDR/STR pre-index and post-index (imm9 signed, writeback on success).
//   - Logical (immediate) AND/ORR/EOR/ANDS plus the TST and MOV aliases,
//     with the ARM DecodeBitMasks algorithm and reserved-encoding rejection.
//   - CSEL/CSINC conditional selection (and the CSET/CINC aliases).
//   - A three-level nested compiler-style call chain (stp/ldp frames).
// SCOPE (Phase AARCH64-5/6, additive): EL0/EL1 synchronous exceptions,
//   selected system registers, SVC/BRK, VBAR_EL1 vector dispatch, ERET and a
//   bounded guest-controlled EL0/EL1 Stage-1 4 KiB MMU. With SCTLR_EL1.M=0,
//   the established flat-memory execution profile remains active.
// OUT OF SCOPE: Stage-2, EL2/EL3, GICv3 SMP/SGIs/ITS/LPIs/virtualization,
//   FIQ, virtio, JIT, SIMD/FP, and the rest of the A64 ISA. The bounded
//   single-PE GICv3, timer, PL011, and IRQ profile is in docs/aarch64/.
//
// AARCH64-2 memory profile (explicit and bounded):
//   - Little-endian guest data access through the generic IMemory contract.
//   - Normal memory, no Device mapping, no SCTLR_ELx.A alignment control
//     (deferred); Stage-1 translation is an additive AARCH64-6 profile.
//   - Architecturally permitted unaligned ordinary LDR/STR accesses are
//     ALLOWED in this profile (ARM permits them for Normal memory when
//     alignment checking is disabled).
//   - Instruction fetch still requires 4-byte alignment (UnalignedPC).
//   - SP-alignment checking (CheckSPAlignment) is deferred, as in AARCH64-1.
//
// Encoding tables used: ARM Architecture Reference Manual ARMv8-A (ARM DDI 0487),
//   sections C6.6 (branches), C6.2 (add/sub immediate, load/store immediate),
//   C6.2 (move wide immediate), C6.2.10/11 (ADR/ADRP), C6.2 (load literal),
//   C6.2 (logical immediate), C6.2 (conditional select), C6.2 (load/store
//   register pair) and C6.2 (HINT/NOP = 0xD503201F).
// Cross-checked against the machine-readable A64 ISA definitions mirrored at
//   https://arm.jonpalmisc.com/latest_aarch64/ (RET operation target = X(n);
//   LDR/STR unsigned-offset offset = LSL(ZeroExtend(imm12), scale);
//   B.cond decode offset = SignExtend(imm19:'00')) and the shared
//   ConditionHolds() pseudocode (cond 0b1110 AL and 0b1111 NV both always
//   hold; the bit-0 inversion explicitly excludes 0b1111).
// AARCH64-4 encodings were cross-checked against the field tables and
//   shared pseudocode of the ARM ARM (arm.jonpalmisc.com, version 2026.06)
//   and against well-known reference words recorded in the phase test suite
//   (no AArch64 assembler is installed in this environment; see
//   docs/aarch64/AARCH64_4_COMPILER_STACK.md).
// This header is intentionally decoupled from IA-64 state (cpu_state.h),
// the IA-64 decoder and IA-64 firmware. It only depends on the generic
// IMemory contract (forward declared) for instruction fetch.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aarch64/AArch64MMU.h"
#include "aarch64/AArch64TLB.h"

namespace ia64 {
class IMemory; // generic memory contract; no IA-64 specifics required
}

namespace aarch64 {

class AArch64ExecutionSnapshot;
class AArch64DeviceSnapshot;
class AArch64ISAPlugin;

// Optional per-machine architectural system-register bank. The CPU holds only
// a weak reference so removing a device cannot leave a dangling CPU callback.
// A guest instruction step leases the owner across execute + retirement, which
// also makes device snapshot boundaries atomic with respect to virtual time.
class AArch64SystemRegisterHandler {
public:
    virtual ~AArch64SystemRegisterHandler() = default;
    virtual bool handlesSystemRegister(uint16_t key) const noexcept = 0;
    virtual bool el0AccessAllowed(uint16_t key, bool isRead) const noexcept = 0;
    virtual bool el0AccessIsUndefined(uint16_t key,
                                      bool isRead) const noexcept = 0;
    virtual bool writeIsUndefined(uint16_t key) const noexcept = 0;
    virtual uint64_t readSystemRegister(uint16_t key) const noexcept = 0;
    virtual bool writeSystemRegister(uint16_t key, uint64_t value,
                                     std::string* error) noexcept = 0;
    virtual uint8_t instructionStepOrder() const noexcept { return 0; }
    // Optional wired IRQ output. The CPU samples this only at an instruction
    // boundary; acknowledging the interrupt remains a guest ICC_IAR operation.
    virtual bool irqOutputAsserted() const noexcept { return false; }
    virtual bool beginInstructionStep() noexcept = 0;
    virtual void finishInstructionStep(bool retired) noexcept = 0;
};

// Fixed A64 instruction size in bytes.
inline constexpr std::size_t kInstructionSize = 4;

// PSTATE condition-flag bit positions (NZCV lives in PSTATE[31:28]).
inline constexpr uint32_t kPstateN = 1u << 31;
inline constexpr uint32_t kPstateZ = 1u << 30;
inline constexpr uint32_t kPstateC = 1u << 29;
inline constexpr uint32_t kPstateV = 1u << 28;
inline constexpr uint32_t kPstateD = 1u << 9;
inline constexpr uint32_t kPstateA = 1u << 8;
inline constexpr uint32_t kPstateI = 1u << 7;
inline constexpr uint32_t kPstateF = 1u << 6;
inline constexpr uint32_t kPstateDAIF = kPstateD | kPstateA | kPstateI | kPstateF;
inline constexpr uint32_t kPstateSpsel = 1u;
inline constexpr uint32_t kPstateElMask = 3u << 2;
inline constexpr uint32_t kPstateModeMask = 0x1Fu;

enum class ExceptionLevel : uint8_t { EL0 = 0, EL1 = 1 };

// Canonical NOP encoding (HINT #0): 1101010100 0000011001 000000 11111 11111.
inline constexpr uint32_t kNopEncoding = 0xD503201Fu;

// Deterministic fault taxonomy. Every unsupported, malformed or
// non-fetchable instruction maps to exactly one of these; unsupported
// instructions are NEVER silently treated as NOPs.
enum class Fault {
    None = 0,
    UnalignedPC,         // PC not 4-byte aligned at fetch time
    InvalidFetch,        // instruction bytes not readable from guest memory
    UnallocatedEncoding, // bit pattern is unallocated/malformed in A64
    UnsupportedFeature,  // allocated A64 encoding outside the subset
    MemoryFault,         // guest data access failed or its address range wrapped
    TranslationFault,    // guest-controlled Stage-1 translation/access abort
    SnapshotBoundary,    // guest execution rejected while a complete snapshot is coordinated
};

// Implemented A64 operation set.
enum class Op {
    Invalid = 0,
    Nop,
    Movz,
    Movk,
    Rev,      // REV (byte-reverse within a 32- or 64-bit GPR)
    AddImm,
    SubImm,
    B,
    Bl,
    Ret,
    LdrImm,   // LDR (immediate, unsigned offset), 32/64-bit via sf64
    StrImm,   // STR (immediate, unsigned offset), 32/64-bit via sf64
    Cbz,      // Compare and branch on zero
    Cbnz,     // Compare and branch on non-zero
    AddsImm,  // ADDS (immediate): flag-setting add
    SubsImm,  // SUBS (immediate): flag-setting subtract
    CmpImm,   // CMP (immediate): SUBS with XZR/WZR destination (alias)
    BCond,    // B.<cond>: conditional branch on NZCV
    Adr,      // ADR: Xd = PC + SEx(imm21) (Rd==31 is XZR, never SP)
    Adrp,     // ADRP: Xd = (PC & ~0xFFF) + SEx(imm21)*4096 (Rd==31 is XZR)
    LdrLit,   // LDR (literal): Xt/Wt = Mem[PC + SEx(imm19)*4] via sf64
    Stp,      // STP (register pair): signed-offset / pre / post-index
    Ldp,      // LDP (register pair): signed-offset / pre / post-index
    Ldur,     // LDUR (unscaled, imm9 signed): Wt/Xt = Mem[Rn + SEx(imm9)]
    Stur,     // STUR (unscaled, imm9 signed): Mem[Rn + SEx(imm9)] = Wt/Xt
    LdrPre,   // LDR (immediate, pre-index): writeback base = base + SEx(imm9)
    StrPre,   // STR (immediate, pre-index): writeback base = base + SEx(imm9)
    LdrPost,  // LDR (immediate, post-index): writeback base = base + SEx(imm9)
    StrPost,  // STR (immediate, post-index): writeback base = base + SEx(imm9)
    AndImm,   // AND (immediate): bitmask immediate
    OrrImm,   // ORR (immediate): bitmask immediate (MOV alias when Rn==31)
    EorImm,   // EOR (immediate): bitmask immediate
    AndsImm,  // ANDS (immediate): bitmask immediate (TST alias when Rd==31)
    Csel,     // CSEL: conditional select
    Csinc,    // CSINC: conditional select increment (CINC/CSET aliases)
    Mrs,      // MRS Xt, system register
    Msr,      // MSR system register, Xt
    MsrPstateImm, // MSR SPSel/DAIFSet/DAIFClr, #imm
    Svc,      // SVC #imm16: synchronous exception to EL1
    Brk,      // BRK #imm16: breakpoint instruction exception
    Eret,     // ERET: restore PSTATE from SPSR_EL1 and PC from ELR_EL1
    Dsb,      // DSB: ordered single-CPU memory operation
    Dmb,      // DMB: ordered single-CPU memory operation
    Isb,      // ISB: instruction context synchronization event
    Tlbi,     // Supported EL1&0 stage-1 TLBI system instruction
};

enum class TlbiOperation : uint8_t {
    Vmalle1, Vmalle1Is, Vae1, Vae1Is, Aside1, Aside1Is, Vaae1, Vaae1Is
};

// Result of decoding one 32-bit A64 instruction word.
struct Decoded {
    Op op = Op::Invalid;
    bool valid = false;
    bool sf64 = true;          // true = 64-bit (X) form, false = 32-bit (W) form
    uint8_t rd = 0;            // destination register number 0..31
    uint8_t rn = 0;            // source register number 0..31
    uint8_t rt2 = 0;           // second transfer register (LDP/STP Rt2 field)
    uint8_t rm = 0;            // second source register (CSEL/CSINC Rm field)
    uint8_t hw = 0;            // MOVZ/MOVK halfword selector 0..3
    uint16_t imm16 = 0;        // MOVZ/MOVK 16-bit immediate
    uint16_t imm12 = 0;        // ADD/SUB/LDR/STR 12-bit immediate
    uint8_t shift = 0;         // ADD/SUB shift amount (0 or 12); MOVZ/MOVK pos = hw*16
    int64_t branchOffset = 0;  // B/BL/CBZ/CBNZ/B.cond signed byte offset (scaled by 4)
    uint64_t memOffset = 0;    // LDR/STR scaled unsigned byte offset (imm12 << 2 or 3)
    uint8_t accessBytes = 4;   // byte width for scalar GPR LDR/STR transfers
    // Addressing mode for LDP/STP and LDR/STR indexed forms:
    //   0 = offset (signed-offset pair, or unsigned-offset single; no writeback)
    //   1 = pre-index  (address = base + offset, then writeback)
    //   2 = post-index (address = base, then writeback = base + offset)
    uint8_t addrMode = 0;
    // Signed byte displacement, already scaled for LDP/STP (imm7 << 2/3) and
    // unscaled for LDUR/STUR/LDR/STR indexed (SEx(imm9)). Used by all
    // non-unsigned-offset memory forms.
    int64_t signedOffset = 0;
    // Logical (immediate) bitmask fields.
    uint32_t immr = 0;         // rotate right amount (6 bits)
    uint32_t imms = 0;         // mask length/element selector (6 bits)
    uint32_t immN = 0;         // N bit (64-bit element selector)
    uint64_t logicalImm = 0;   // decoded bitmask immediate
    bool setsFlags = false;    // ANDS/TST (sets N,Z; clears C,V)
    int64_t pcOffset = 0;      // ADR/ADRP/LDR-literal signed byte displacement
                               // from the current instruction address, fully
                               // scaled (ADR x1, ADRP x4096, LDR literal x4).
                               // ADR/ADRP/LDR-literal only; branches use
                               // branchOffset, LDR/STR-imm use memOffset.
    uint8_t cond = 0;          // B.cond 4-bit condition code (bits [3:0])
    uint8_t sysOp0 = 0;        // System-register encoding fields
    uint8_t sysOp1 = 0;
    uint8_t sysCrn = 0;
    uint8_t sysCrm = 0;
    uint8_t sysOp2 = 0;
    TlbiOperation tlbiOperation = TlbiOperation::Vmalle1;
    uint64_t address = 0;      // address the word was fetched from
    uint32_t word = 0;         // raw instruction word
    Fault fault = Fault::None;
    bool hasMemoryAbort = false;
    bool memoryAbortInstruction = false;
    bool memoryAbortWrite = false;
    bool memoryAbortWalk = false;
    uint8_t memoryAbortStatus = 0;
    std::string error;         // human-readable fault detail (empty when valid)
    std::string disassembly;   // valid only
};

// Bounded-run outcome.
struct RunOutcome {
    uint64_t stepsExecuted = 0;
    bool stoppedOnFault = false;
    Fault fault = Fault::None;
    std::string error;
};

// AArch64 CPU architectural state + fetch/decode/execute.
//
// Reset/entry convention (deterministic emulator profile, NOT the physical
// Arm reset architecture): reset(entry) enters EL0 using SP_EL0 by default.
// Tests/embedders may explicitly select EL0 or EL1 at reset. EL1 entry uses
// EL1h/SP_EL1. A real guest may transition levels only through ERET.
class Cpu {
public:
    explicit Cpu(uint64_t entryPoint = 0,
                 ExceptionLevel initialLevel = ExceptionLevel::EL0) {
        reset(entryPoint, initialLevel);
    }
    Cpu(const Cpu& other);
    Cpu& operator=(const Cpu& other);
    Cpu(Cpu&& other);
    Cpu& operator=(Cpu&& other);

    // -- reset ------------------------------------------------------------
    void reset(uint64_t entryPoint = 0,
               ExceptionLevel initialLevel = ExceptionLevel::EL0);

    // Bind a machine-owned system-register device (for example the Generic
    // Timer). The weak ownership is intentionally excluded from CPU snapshots
    // and CPU copies; device snapshots own the device state.
    bool bindSystemRegisterHandler(
        const std::shared_ptr<AArch64SystemRegisterHandler>& handler);

    // -- X registers ------------------------------------------------------
    // Only indices 0..30 are addressable here; index 31 is rejected because
    // register 31 is SP-or-zero depending on context (see read/write helpers).
    uint64_t readX(unsigned index) const;
    void writeX(unsigned index, uint64_t value);
    uint32_t readW(unsigned index) const;   // low 32 bits of Xn
    void writeW(unsigned index, uint32_t value); // zero-extends into Xn

    // Context-sensitive register-31 access.
    // withZero=true  -> XZR/WZR view (reads 0, writes discarded).
    // withZero=false -> SP view (reads/writes the real stack pointer).
    uint64_t readReg31(bool is64, bool withZero) const;
    void writeReg31(bool is64, bool withZero, uint64_t value);

    // -- SP / PC / PSTATE --------------------------------------------------
    uint64_t sp() const;
    void setSp(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        activeSpRef() = value;
    }
    uint64_t spEl0() const { return spEl0_; }
    uint64_t spEl1() const { return spEl1_; }
    void setSpEl0(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        spEl0_ = value;
    }
    void setSpEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        spEl1_ = value;
    }
    uint64_t pc() const { return pc_; }
    void setPc(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        pc_ = value;
    }
    uint32_t pstate() const { return pstate_; }
    void setPstate(uint32_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        pstate_ = value;
    }
    ExceptionLevel exceptionLevel() const {
        return static_cast<ExceptionLevel>((pstate_ & kPstateElMask) >> 2);
    }
    bool usesSpEl0() const {
        return exceptionLevel() == ExceptionLevel::EL0 ||
               (pstate_ & kPstateSpsel) == 0;
    }
    uint64_t elrEl1() const { return elrEl1_; }
    void setElrEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        elrEl1_ = value;
    }
    uint32_t spsrEl1() const { return spsrEl1_; }
    void setSpsrEl1(uint32_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        spsrEl1_ = value;
    }
    uint64_t vbarEl1() const { return vbarEl1_; }
    void setVbarEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        vbarEl1_ = value & ~uint64_t{0x7FF};
    }
    uint32_t esrEl1() const { return esrEl1_; }
    void setEsrEl1(uint32_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        esrEl1_ = value;
    }
    uint64_t farEl1() const { return farEl1_; }
    void setFarEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        farEl1_ = value;
    }
    uint64_t sctlrEl1() const { return sctlrEl1_; }
    uint64_t tcrEl1() const { return tcrEl1_; }
    uint64_t ttbr0El1() const { return ttbr0El1_; }
    uint64_t ttbr1El1() const { return ttbr1El1_; }
    uint64_t mairEl1() const { return mairEl1_; }
    uint64_t effectiveSctlrEl1() const { return effectiveSctlrEl1_; }
    uint64_t effectiveTcrEl1() const { return effectiveTcrEl1_; }
    uint64_t effectiveTtbr0El1() const { return effectiveTtbr0El1_; }
    uint64_t effectiveTtbr1El1() const { return effectiveTtbr1El1_; }
    uint64_t effectiveMairEl1() const { return effectiveMairEl1_; }
    const AArch64TLB& tlb() const { return tlb_; }
    void setSctlrEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        sctlrEl1_ = effectiveSctlrEl1_ = value;
    }
    void setTcrEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        tcrEl1_ = effectiveTcrEl1_ = value;
    }
    void setTtbr0El1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        ttbr0El1_ = effectiveTtbr0El1_ = value;
    }
    void setTtbr1El1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        ttbr1El1_ = effectiveTtbr1El1_ = value;
    }
    void setMairEl1(uint64_t value) {
        std::lock_guard<std::recursive_mutex> lock(executionMutex_);
        if (snapshotBoundaryActive_) return;
        mairEl1_ = effectiveMairEl1_ = value;
    }
    bool flagN() const { return (pstate_ & kPstateN) != 0; }
    bool flagZ() const { return (pstate_ & kPstateZ) != 0; }
    bool flagC() const { return (pstate_ & kPstateC) != 0; }
    bool flagV() const { return (pstate_ & kPstateV) != 0; }
    void setNzcv(bool n, bool z, bool c, bool v);

    // -- fetch / decode / execute ------------------------------------------
    // Pure decode of one raw word (no memory access, no state change).
    static Decoded decodeWord(uint32_t word, uint64_t address);
    // Fetch 4 bytes little-endian at PC and decode.
    Decoded fetchDecode(ia64::IMemory& memory) const;
    Decoded fetchDecodeAt(ia64::IMemory& memory, uint64_t address) const;
    // Execute a previously decoded instruction. Executing an invalid
    // Decoded returns its fault and changes no architectural state.
    // All implemented loads/stores, including pair, unscaled and indexed
    // forms, access guest memory through `memory`. Failed loads preserve
    // destinations, writeback bases and PC; failed stores do not write back
    // or advance PC. Pair stores use one contiguous Write call, but an
    // arbitrary IMemory backend may partially mutate bytes before throwing.
    Fault executeDecoded(const Decoded& decoded, ia64::IMemory& memory);
    // Legacy single-argument form kept for AARCH64-1 call-site
    // compatibility: behaves identically for non-memory instructions; data
    // accesses report Fault::MemoryFault because no memory is bound.
    Fault executeDecoded(const Decoded& decoded);
    // Combined fetch+decode+execute of the instruction at PC.
    Fault step(ia64::IMemory& memory);
    // Bounded stepping: executes at most maxSteps instructions, stopping
    // early on the first fault. Cannot run indefinitely.
    RunOutcome run(ia64::IMemory& memory, uint64_t maxSteps);

    Fault lastFault() const { return lastFault_; }
    const std::string& lastError() const { return lastError_; }

    std::string dumpState() const;

private:
    friend class AArch64State;
    friend class AArch64ExecutionSnapshot;
    friend class AArch64DeviceSnapshot;
    friend class AArch64ISAPlugin;
    Fault executeSystemRegister(const Decoded& decoded);
    Fault executeTlbi(const Decoded& decoded);
    Fault executePstateImmediate(const Decoded& decoded);
    bool acceptPendingIrq();
    void takeIrqException();
    void takeSynchronousException(uint8_t exceptionClass, uint32_t iss,
                                  uint64_t preferredReturnAddress);
    void synchronizeTranslationContext();
    void takeMemoryAbort(uint64_t faultAddress, MMUAccess access,
                         uint8_t faultStatus, bool duringTableWalk);
    bool readGuestMemory(const ia64::IMemory& memory, uint64_t virtualAddress,
                         uint8_t* destination, size_t size, MMUAccess access,
                         Decoded* fetchFault, uint8_t* status = nullptr,
                         bool* duringWalk = nullptr,
                         uint64_t* faultAddress = nullptr) const;
    bool writeGuestMemory(ia64::IMemory& memory, uint64_t virtualAddress,
                          const uint8_t* source, size_t size,
                          uint8_t* status = nullptr,
                          bool* duringWalk = nullptr,
                          uint64_t* faultAddress = nullptr);
    MMUResult translateGuestAddress(const ia64::IMemory& memory,
                                    uint64_t virtualAddress,
                                    MMUAccess access) const;
    Fault executeEret(const Decoded& decoded);
    uint64_t& activeSpRef();
    const uint64_t& activeSpRef() const;

    uint64_t x_[31] = {}; // X0..X30. There is deliberately no x_[31].
    uint64_t spEl0_ = 0;
    uint64_t spEl1_ = 0;
    uint64_t pc_ = 0;     // Program counter (byte address, 4-aligned).
    uint32_t pstate_ = 0; // NZCV, DAIF, EL and SPSel; unsupported fields RES0.
    uint64_t elrEl1_ = 0;
    uint32_t spsrEl1_ = 0;
    uint64_t vbarEl1_ = 0;
    uint32_t esrEl1_ = 0;
    uint64_t farEl1_ = 0;
    uint64_t sctlrEl1_ = 0;
    uint64_t tcrEl1_ = 0;
    uint64_t ttbr0El1_ = 0;
    uint64_t ttbr1El1_ = 0;
    uint64_t mairEl1_ = 0;
    uint64_t effectiveSctlrEl1_ = 0;
    uint64_t effectiveTcrEl1_ = 0;
    uint64_t effectiveTtbr0El1_ = 0;
    uint64_t effectiveTtbr1El1_ = 0;
    uint64_t effectiveMairEl1_ = 0;
    // True until the first instruction at the latest exception vector has
    // been fetched and dispatched. A failed fetch in this window is fatal,
    // preventing an unmapped vector from repeatedly aborting into itself.
    bool exceptionVectorPending_ = false;
    std::vector<std::weak_ptr<AArch64SystemRegisterHandler>> systemRegisterHandlers_;
    mutable AArch64TLB tlb_;
    // Serializes complete instruction execution with snapshot management.
    // This mutex is host coordination state and is never serialized.
    mutable std::recursive_mutex executionMutex_;
    // The device-aware coordinator owns the full machine boundary between
    // quiescence and commit. Guest stepping and host mutators reject while it
    // is active; only the coordinator thread may invoke the nested v1 codec.
    mutable bool snapshotBoundaryActive_ = false;
    mutable bool allowCoordinatedSnapshotCodec_ = false;
    mutable std::thread::id snapshotBoundaryOwner_{};
    Fault lastFault_ = Fault::None;
    std::string lastError_;
};

// Canonical 64-bit sign extension of an N-bit field value.
int64_t signExtend(uint32_t value, unsigned bits);

// Authoritative A64 condition evaluation (ARM ConditionHolds pseudocode):
// cond[3:1] selects EQ/NE(Z), CS/CC(C), MI/PL(N), VS/VC(V), HI/LS(C&~Z),
// GE/LT(N==V), GT/LE(N==V&~Z), or 111->TRUE(AL); bit 0 inverts the result
// EXCEPT for cond 0b1111 (NV), which — like 0b1110 (AL) — always holds.
// Cond 14 and 15 are therefore NOT inverses of each other.
bool conditionHolds(uint8_t cond, bool n, bool z, bool c, bool v);

// ARM "Logical (immediate)" bitmask decoding (DecodeBitMasks). Returns false
// for reserved/unallocated encodings (mandatory-zero violations and
// all-ones elements). The N:immr:imms triple is decoded into `mask`, which is
// then truncated to 32 bits when !is64. Exposed so tests can anchor it against
// independently produced encodings.
bool decodeLogicalImmediate(uint32_t immN, uint32_t immr, uint32_t imms,
                            bool is64, uint64_t& mask);

} // namespace aarch64
