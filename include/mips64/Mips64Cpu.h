#pragma once

// guideXOS Hypervisor — MIPS64 Release 2 CPU foundation (Phase MIPS64-1).
//
// This header defines the isolated MIPS64r2 architectural state and the
// fetch/decode/execute core. The core is intentionally self-contained:
// each Mips64Cpu instance owns its register file, PC, HI/LO, branch
// delay-slot bookkeeping, minimal exception state and endianness
// configuration. There is no global mutable register state and no reuse
// of IA-64 register structures.
//
// Byte order policy:
// - The CPU is constructed with an explicit endianness (little-endian by
//   default, matching the MIPS64-1 initial execution mode).
// - Instruction words AND multi-byte data accesses are assembled from
//   raw bytes according to the configured endianness. Host-native typed
//   reads are never used for guest data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Forward declaration of the repository memory abstraction so the core
// header does not pull in the full memory implementation.
namespace ia64 {
class IMemory;
}

namespace mips64 {

// ============================================================================
// Architecture constants
// ============================================================================

constexpr unsigned kNumGpr = 32;
constexpr unsigned kZeroReg = 0;
constexpr unsigned kReturnAddrReg = 31;  // default $ra
constexpr std::size_t kInstructionSize = 4;
constexpr uint64_t kResetPc = 0;  // deterministic power-on PC

enum class Mips64Endianness : uint8_t {
    Little = 0,
    Big = 1,
};

// Minimal, explicitly documented exception state for MIPS64-1.
// Full CP0/TLB/privileged exception handling is a future milestone;
// this records only what the phase-1 core can actually produce.
enum class Mips64Fault : uint32_t {
    None = 0,
    MisalignedFetch = 1,       // PC not word-aligned on fetch
    MemoryFaultFetch = 2,      // IMemory rejected the instruction fetch
    ReservedInstruction = 3,   // decoded a reserved/unallocated encoding
    UnsupportedInstruction = 4,  // valid MIPS64 encoding outside the phase-1 subset
    MisalignedLoad = 5,        // load address violates access alignment
    MisalignedStore = 6,       // store address violates access alignment
    MemoryFaultLoad = 7,       // IMemory rejected a load
    MemoryFaultStore = 8,      // IMemory rejected a store
    BranchInDelaySlot = 9,     // control-flow insn inside a delay slot (explicitly rejected)
    MisalignedBranchTarget = 10,  // JR/JALR target not word-aligned
};

// Step outcome. Returning false from step()/executeDecoded() always
// coincides with fault() != Mips64Fault::None and lastError() set.
enum class Mips64StepResult : uint8_t {
    Continue = 0,  // instruction retired; PC advanced per delay-slot rules
    Fault = 1,     // instruction did not retire; see fault()/lastError()
};

// ============================================================================
// Decoded instruction
// ============================================================================

enum class Mips64Op : uint8_t {
    Invalid = 0,
    // Immediate and arithmetic
    Lui, Ori, Andi, Xori, Addiu, Daddiu,
    Addu, DaddU, Subu, Dsubu,
    // Register operations
    And, Or, Xor, Nor, Slt, Sltu,
    // Shifts
    Sll, Srl, Sra, Dsll, Dsrl, Dsra,
    // Memory
    Lb, Lbu, Lh, Lhu, Lw, Lwu, Ld, Sb, Sh, Sw, Sd,
    // Control flow (all have a branch delay slot)
    Beq, Bne, J, Jal, Jr, Jalr,
};

struct Mips64Decoded {
    Mips64Op op = Mips64Op::Invalid;
    uint32_t raw = 0;
    uint8_t rs = 0;
    uint8_t rt = 0;
    uint8_t rd = 0;
    uint8_t sa = 0;
    uint16_t imm = 0;
    uint32_t index = 0;  // J/JAL 26-bit target index
    bool valid = false;
    Mips64Fault fault = Mips64Fault::None;
    std::string error;  // human-readable reason when !valid
};

// Pure decode of a 32-bit instruction word. No memory access.
Mips64Decoded decodeInstruction(uint32_t word);

// ============================================================================
// CPU core
// ============================================================================

class Mips64Cpu {
public:
    explicit Mips64Cpu(Mips64Endianness endianness = Mips64Endianness::Little);
    Mips64Cpu(const Mips64Cpu&) = default;
    Mips64Cpu& operator=(const Mips64Cpu&) = default;

    // -- power-on reset (deterministic) ------------------------------------
    void reset();

    // -- register access ----------------------------------------------------
    // $zero (index 0) always reads 0; writes are ignored (reported as
    // success). Out-of-range indices fail safely: reads return 0 and
    // writes return false with no state change.
    uint64_t readGpr(unsigned index) const;
    bool writeGpr(unsigned index, uint64_t value);

    uint64_t pc() const { return pc_; }
    void setPc(uint64_t value) { pc_ = value; }

    uint64_t hi() const { return hi_; }
    uint64_t lo() const { return lo_; }
    void setHi(uint64_t value) { hi_ = value; }
    void setLo(uint64_t value) { lo_ = value; }

    // -- delay-slot bookkeeping ---------------------------------------------
    // delayArmed() is true while the next instruction to execute is the
    // delay slot of a previously retired branch/jump; delayTarget() is
    // where execution resumes after the delay slot retires.
    bool delayArmed() const { return delayArmed_; }
    uint64_t delayTarget() const { return delayTarget_; }

    // -- exception state ------------------------------------------------------
    Mips64Fault fault() const { return fault_; }
    const std::string& lastError() const { return lastError_; }

    // -- endianness -----------------------------------------------------------
    Mips64Endianness endianness() const { return endianness_; }
    bool isLittleEndian() const { return endianness_ == Mips64Endianness::Little; }

    // -- fetch/decode/execute -------------------------------------------------
    // Fetch one instruction word at `address` per configured endianness.
    // Returns false (with fault set) on misalignment or memory rejection.
    bool fetchWord(ia64::IMemory& memory, uint64_t address, uint32_t& out);

    // Decode the instruction at `address` without executing it.
    Mips64Decoded decodeAt(ia64::IMemory& memory, uint64_t address);

    // Execute a previously decoded instruction whose fetch address was
    // `address`. Implements delay-slot scheduling, link-register writes
    // and PC advancement. Returns Continue on retire, Fault otherwise.
    Mips64StepResult executeDecoded(ia64::IMemory& memory,
                                    const Mips64Decoded& decoded,
                                    uint64_t address);

    // Combined fetch + decode + execute at the current PC.
    Mips64StepResult step(ia64::IMemory& memory);

    // Disassembly of a decoded instruction (no memory access).
    std::string disassemble(const Mips64Decoded& decoded) const;

    // -- snapshot / serialization ----------------------------------------------
    // Serialization blob layout (blob itself is always little-endian and
    // versioned; the configured CPU endianness is stored and restored):
    //   offset 0:  magic 'M','P','6','1' (4 bytes)
    //   offset 4:  format version u8 (=1), endianness u8, reserved u16
    //   offset 8:  GPR[32] x u64 LE (256 bytes)
    //   offset 264: PC u64, HI u64, LO u64 (24 bytes)
    //   offset 288: delayArmed u8, delayTarget u64 LE (9 bytes)
    //   offset 297: fault u32 LE (4 bytes)
    // Total: 301 bytes.
    static constexpr std::size_t kSerializedSize = 301;
    std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, std::size_t size);

    // -- debugging ---------------------------------------------------------------
    std::string toString() const;
    static const char* faultName(Mips64Fault fault);
    static const char* opName(Mips64Op op);

private:
    void setFault(Mips64Fault fault, const std::string& message);
    void clearFault();

    // True 64-bit helpers used by the interpreter core.
    Mips64StepResult executeMemory(ia64::IMemory& memory,
                                   const Mips64Decoded& decoded,
                                   uint64_t address);
    Mips64StepResult executeBranch(ia64::IMemory& memory,
                                   const Mips64Decoded& decoded,
                                   uint64_t address);

    std::array<uint64_t, kNumGpr> gpr_{};
    uint64_t pc_ = kResetPc;
    uint64_t hi_ = 0;
    uint64_t lo_ = 0;
    bool delayArmed_ = false;
    uint64_t delayTarget_ = 0;
    Mips64Fault fault_ = Mips64Fault::None;
    std::string lastError_;
    Mips64Endianness endianness_ = Mips64Endianness::Little;
};

}  // namespace mips64
