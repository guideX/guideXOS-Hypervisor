#pragma once

// AArch64 (ARMv8-A, A64 instruction set) CPU architectural state,
// instruction fetch/decode and execution for guideXOS Hypervisor Phase AARCH64-1.
//
// SCOPE (Phase AARCH64-1):
//   - General-purpose registers X0-X30 plus a DISTINCT stack pointer (SP),
//     program counter (PC) and PSTATE with NZCV in bits [31:28].
//   - Register 31 is context-sensitive (SP vs XZR/WZR) and is NEVER modelled
//     as one ordinary register.
//   - A64 fixed 32-bit little-endian fetch, deterministic reset/entry profile,
//     bounded stepping, and explicit faults for everything unsupported.
// OUT OF SCOPE: MMU, GIC, EL1 exception model, timers, PL011, virtio, SMP,
//   JIT, SIMD/FP, and the rest of the A64 ISA (see docs/aarch64/).
//
// Encoding tables used: ARM Architecture Reference Manual ARMv8-A (ARM DDI 0487),
//   sections C6.6 (branches), C6.2 (add/sub immediate), C6.2 (move wide
//   immediate) and C6.2 (HINT/NOP = 0xD503201F).
// This header is intentionally decoupled from IA-64 state (cpu_state.h),
// the IA-64 decoder and IA-64 firmware. It only depends on the generic
// IMemory contract (forward declared) for instruction fetch.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ia64 {
class IMemory; // generic memory contract; no IA-64 specifics required
}

namespace aarch64 {

// Fixed A64 instruction size in bytes.
inline constexpr std::size_t kInstructionSize = 4;

// PSTATE condition-flag bit positions (NZCV lives in PSTATE[31:28]).
inline constexpr uint32_t kPstateN = 1u << 31;
inline constexpr uint32_t kPstateZ = 1u << 30;
inline constexpr uint32_t kPstateC = 1u << 29;
inline constexpr uint32_t kPstateV = 1u << 28;

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
    UnsupportedFeature,  // allocated A64 encoding outside the AARCH64-1 subset
};

// Phase AARCH64-1 operation set.
enum class Op {
    Invalid = 0,
    Nop,
    Movz,
    Movk,
    AddImm,
    SubImm,
    B,
    Bl,
    Ret,
};

// Result of decoding one 32-bit A64 instruction word.
struct Decoded {
    Op op = Op::Invalid;
    bool valid = false;
    bool sf64 = true;          // true = 64-bit (X) form, false = 32-bit (W) form
    uint8_t rd = 0;            // destination register number 0..31
    uint8_t rn = 0;            // source register number 0..31
    uint8_t hw = 0;            // MOVZ/MOVK halfword selector 0..3
    uint16_t imm16 = 0;        // MOVZ/MOVK 16-bit immediate
    uint16_t imm12 = 0;        // ADD/SUB 12-bit immediate
    uint8_t shift = 0;         // ADD/SUB shift amount (0 or 12); MOVZ/MOVK pos = hw*16
    int64_t branchOffset = 0;  // B/BL signed byte offset (already scaled by 4)
    uint64_t address = 0;      // address the word was fetched from
    uint32_t word = 0;         // raw instruction word
    Fault fault = Fault::None;
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
// Reset/entry convention (initial emulator profile, NOT a claim about all
// physical ARM reset configurations): reset(entry) zeroes X0-X30, SP, NZCV
// and the rest of PSTATE, and sets PC = entry (default 0).
class Cpu {
public:
    explicit Cpu(uint64_t entryPoint = 0) { reset(entryPoint); }

    // -- reset ------------------------------------------------------------
    void reset(uint64_t entryPoint = 0);

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
    uint64_t sp() const { return sp_; }
    void setSp(uint64_t value) { sp_ = value; }
    uint64_t pc() const { return pc_; }
    void setPc(uint64_t value) { pc_ = value; }
    uint32_t pstate() const { return pstate_; }
    void setPstate(uint32_t value) { pstate_ = value; }
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
    // Execute a previously decoded instruction. Executing an invalid
    // Decoded returns its fault and changes no architectural state.
    // None of the AARCH64-1 instructions access guest memory, so the
    // memory parameter is currently unused (kept for IISA symmetry).
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
    uint64_t x_[31] = {}; // X0..X30. There is deliberately no x_[31].
    uint64_t sp_ = 0;     // Stack pointer (register 31 in SP contexts).
    uint64_t pc_ = 0;     // Program counter (byte address, 4-aligned).
    uint32_t pstate_ = 0; // NZCV in bits [31:28]; rest reserved-zero in phase 1.
    Fault lastFault_ = Fault::None;
    std::string lastError_;
};

// Canonical 64-bit sign extension of an N-bit field value.
int64_t signExtend(uint32_t value, unsigned bits);

} // namespace aarch64
