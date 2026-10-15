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
 * The CPU distinguishes successful execution from every failure mode:
 * - Ok: instruction fetched, decoded with the production decoder, executed
 *   with correct semantics; registers, guest memory and PC updated.
 * - Halt: EBREAK (32-bit) or C.EBREAK breakpoint/stop marker. PC points at
 *   the breakpoint instruction (not advanced).
 * - Unsupported: valid decoded instruction with no execution support in this
 *   phase (includes all compressed instructions except C.EBREAK). PC points
 *   at the unexecuted instruction (not advanced).
 * - Illegal: production decoder rejected the encoding. PC not advanced.
 * - FetchFault: PC out of bounds or truncated instruction bytes. PC not
 *   advanced and no architectural state changed.
 * - MemoryFault: LD/SD effective address out of guest-memory bounds. PC not
 *   advanced and destination register not written.
 */
enum class StepOutcome {
    Ok,
    Halt,
    Unsupported,
    Illegal,
    FetchFault,
    MemoryFault,
};

inline const char* StepOutcomeToString(StepOutcome outcome) {
    switch (outcome) {
        case StepOutcome::Ok: return "ok";
        case StepOutcome::Halt: return "halt";
        case StepOutcome::Unsupported: return "unsupported";
        case StepOutcome::Illegal: return "illegal";
        case StepOutcome::FetchFault: return "fetch_fault";
        case StepOutcome::MemoryFault: return "memory_fault";
    }
    return "unknown";
}

/**
 * StepResult - details of one executed step.
 * pc is the entry PC; nextPc is the architectural PC after the step.
 */
struct StepResult {
    StepOutcome outcome = StepOutcome::FetchFault;
    uint64_t pc = 0;
    uint64_t nextPc = 0;
    uint8_t length = 0;
    DecodedInstruction insn;
};

/**
 * RV64CPU - standalone RISC-V RV64 CPU core (Phase RISCV-1).
 *
 * Compatibility decision (deliberate, documented):
 * - RV64CPU is standalone and does NOT inherit ia64::ICPU. The IA-64
 *   interface carries predicate registers, branch registers, frame markers
 *   (CFM), rotation bases and ia64::CPUState, none of which exist in RV64.
 *   No in-tree consumer uses RV64CPU through ia64::ICPU (verified by repo
 *   search: only riscv_cpu.* and the RISC-V test reference RV64CPU), so no
 *   adapter is retained. The IA-64 ICPU interface is intentionally untouched.
 * - Guest memory is a single bounded byte region [base, base+size) covering
 *   both instruction fetch and LD/SD data access, little-endian.
 * - Each step() fetches at the guest PC, determines length from the low two
 *   bits, decodes with the production riscv::Decoder, executes one of the
 *   Phase RISCV-1 subset (LUI, AUIPC, ADDI, ADD, SUB, LD, SD, BEQ, BNE, JAL,
 *   JALR, EBREAK-as-halt) and returns an explicit StepResult. Anything else
 *   valid-but-unimplemented yields Unsupported; rejected encodings yield
 *   Illegal. Compressed execution stays deferred (Unsupported, never Ok),
 *   except C.EBREAK which yields Halt.
 */
class RV64CPU {
public:
    RV64CPU();

    // Deterministic reset: registers/PC zeroed, counters cleared.
    // Loaded guest memory is preserved across reset.
    void reset();

    // Load a bounded guest-memory image at [base, base+bytes.size()).
    // Replaces any previous image. Sets PC to base.
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

    // Deterministic-harness counters
    uint64_t stepsTaken() const { return stepsTaken_; }
    uint64_t instructionsRetired() const { return instructionsRetired_; }

    void dump() const;

private:
    bool fetchBytes(uint64_t addr, uint8_t* dst, size_t size) const;
    bool storeU64(uint64_t addr, uint64_t value);
    bool loadU64Internal(uint64_t addr, uint64_t& value) const;

    RV64CPUState state_;
    Decoder decoder_;
    std::vector<uint8_t> mem_;
    uint64_t memBase_ = 0;
    bool memLoaded_ = false;
    uint64_t stepsTaken_ = 0;
    uint64_t instructionsRetired_ = 0;
};

}  // namespace riscv
