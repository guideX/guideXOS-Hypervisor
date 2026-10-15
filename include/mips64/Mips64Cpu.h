#pragma once

// guideXOS Hypervisor — MIPS64 Release 2 CPU (Phases MIPS64-1 + MIPS64-2).
//
// Isolated MIPS64r2 architectural state with fetch/decode/execute,
// Coprocessor 0 (bounded subset), real exception entry/return (ERET),
// trapping integer arithmetic, integer HI/LO multiply/divide and the
// extended branch family. Each Mips64Cpu instance owns its register file,
// PC, HI/LO, CP0, branch delay-slot bookkeeping, exception state and
// endianness configuration. There is no global mutable register state and
// no reuse of IA-64 register structures.
//
// Byte order policy:
// - The CPU is constructed with an explicit endianness (little-endian by
//   default, matching the MIPS64-1 initial execution mode).
// - Instruction words AND multi-byte data accesses are assembled from
//   raw bytes according to the configured endianness. Host-native typed
//   reads are never used for guest data.
//
// Execution model (pre-MMU, explicitly documented):
// - The core issues guest virtual addresses directly to the repository
//   IMemory abstraction. No TLB/MMU translation is performed and no
//   identity mapping for arbitrary MIPS virtual addresses is invented.
// - Exception vectors are ordinary addresses served by whatever IMemory
//   implementation the harness provides (tests back the vector addresses
//   with a sparse test memory). See docs/MIPS64_PHASE2_CP0_EXCEPTIONS.md.

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
constexpr uint64_t kResetPc = 0;  // deterministic power-on PC (pre-MMU model)

enum class Mips64Endianness : uint8_t {
    Little = 0,
    Big = 1,
};

// Diagnostic fault codes. These describe why the most recent step did not
// retire normally; synchronous MIPS exceptions additionally update CP0
// (Cause.ExcCode/BD, EPC, BadVAddr, Status.EXL) and redirect PC to the
// exception vector. See raiseException / docs for the mapping.
enum class Mips64Fault : uint32_t {
    None = 0,
    MisalignedFetch = 1,        // PC not word-aligned on fetch (-> AdEL entry)
    MemoryFaultFetch = 2,       // IMemory rejected the fetch (-> IBE entry)
    ReservedInstruction = 3,    // reserved encoding (-> RI entry)
    UnsupportedInstruction = 4,  // valid MIPS64 encoding outside the subset (no entry)
    MisalignedLoad = 5,         // load address violates alignment (-> AdEL entry)
    MisalignedStore = 6,        // store address violates alignment (-> AdES entry)
    MemoryFaultLoad = 7,        // IMemory rejected a load (-> DBE entry)
    MemoryFaultStore = 8,       // IMemory rejected a store (-> DBE entry)
    BranchInDelaySlot = 9,      // control-flow insn inside a delay slot (explicitly rejected)
    MisalignedBranchTarget = 10,  // JR/JALR target not word-aligned (no entry)
    Cp0Violation = 11,          // unsupported/invalid CP0 access (no entry)
    SyscallTrap = 12,           // SYSCALL retired into an exception (-> Sys entry)
    BreakTrap = 13,             // BREAK retired into an exception (-> Bp entry)
    OverflowTrap = 14,          // trapping arithmetic overflow (-> Ov entry)
};

// Step outcome. Returning Fault always coincides with fault() !=
// Mips64Fault::None and lastError() set. Faults that raise synchronous
// MIPS exceptions additionally perform full exception entry (CP0 update +
// PC redirected to the general exception vector).
enum class Mips64StepResult : uint8_t {
    Continue = 0,  // instruction retired; PC advanced per delay-slot rules
    Fault = 1,     // instruction did not retire; see fault()/lastError()
};

// MIPS64r2 architectural exception codes (Cause.ExcCode) produced by this
// core. TLB codes are intentionally absent: no TLB is modeled in this phase.
enum class Mips64ExcCode : uint32_t {
    AdEL = 4,   // address error on load or instruction fetch
    AdES = 5,   // address error on store
    IBE = 6,    // bus error on instruction fetch (IMemory rejection)
    DBE = 7,    // bus error on data access (IMemory rejection)
    Sys = 8,    // SYSCALL
    Bp = 9,     // BREAK
    RI = 10,    // reserved instruction
    Ov = 12,    // arithmetic overflow (trapping ADD/ADDI/SUB/DADD/DADDI/DSUB)
};

// ============================================================================
// CP0 architectural state (bounded MIPS64r2 subset)
// ============================================================================

// CP0 register numbers (rd) and selectors (sel) implemented here.
constexpr unsigned kCp0RegBadVAddr = 8;
constexpr unsigned kCp0SelBadVAddr = 0;
constexpr unsigned kCp0RegCount = 9;
constexpr unsigned kCp0SelCount = 0;
constexpr unsigned kCp0RegCompare = 11;
constexpr unsigned kCp0SelCompare = 0;
constexpr unsigned kCp0RegStatus = 12;
constexpr unsigned kCp0SelStatus = 0;
constexpr unsigned kCp0RegCause = 13;
constexpr unsigned kCp0SelCause = 0;
constexpr unsigned kCp0RegEpc = 14;
constexpr unsigned kCp0SelEpc = 0;
constexpr unsigned kCp0RegEBase = 15;
constexpr unsigned kCp0SelEBase = 1;
constexpr unsigned kCp0RegErrorEpc = 30;
constexpr unsigned kCp0SelErrorEpc = 0;

// Status bit positions and the software-writable mask for this subset.
// Writable: IE, EXL, ERL, KSU, UX, SX, KX, IM[7:0], BEV. Everything else
// (CU, RP, FR, RE, MX, PX, TS, SR, NMI, Impl, ...) reads 0 in this phase.
constexpr uint64_t kStatusBitIe = 1ULL << 0;
constexpr uint64_t kStatusBitExl = 1ULL << 1;
constexpr uint64_t kStatusBitErl = 1ULL << 2;
constexpr uint64_t kStatusKsuMask = 0x18ULL;     // bits 4:3
constexpr uint64_t kStatusBitUx = 1ULL << 5;
constexpr uint64_t kStatusBitSx = 1ULL << 6;
constexpr uint64_t kStatusBitKx = 1ULL << 7;
constexpr uint64_t kStatusImMask = 0xFF00ULL;    // bits 15:8
constexpr uint64_t kStatusBitBev = 1ULL << 22;
constexpr uint64_t kStatusWritableMask = 0x0040FFFFULL;
constexpr uint64_t kStatusReset = kStatusBitBev | kStatusBitErl;  // 0x00400004

// Cause bit positions. Only the SW interrupt bits IP[1:0] (bits 9:8) are
// software-writable; ExcCode/BD/TI/CE/IP[7:2] are managed by the core.
constexpr uint64_t kCauseBitBd = 1ULL << 31;
constexpr uint64_t kCauseIpSwMask = 0x00000300ULL;  // IP[1:0], bits 9:8
constexpr unsigned kCauseExcCodeShift = 2;
constexpr uint64_t kCauseExcCodeMask = 0x7CULL;  // bits 6:2

// EBase: exception-base bits 31:12 writable, low 12 bits (incl. CPUNum)
// read-only zero; upper 32 bits read zero in this phase.
constexpr uint64_t kEBaseWritableMask = 0xFFFFF000ULL;
constexpr uint64_t kEBaseReset = 0x80000000ULL;

// General exception vector (offset 0x180; TLB-refill vectors are out of
// scope for this phase and are not modeled).
constexpr uint64_t kVectorOffsetGeneral = 0x180ULL;
constexpr uint64_t kVectorBev1 = 0xFFFFFFFFBFC00180ULL;  // BEV=1 boot vectors

// ============================================================================
// Decoded instruction
// ============================================================================

enum class Mips64Op : uint8_t {
    Invalid = 0,
    // Immediate and arithmetic (non-trapping)
    Lui, Ori, Andi, Xori, Addiu, Daddiu,
    Addu, DaddU, Subu, Dsubu,
    // Trapping arithmetic (overflow raises Ov)
    Add, Addi, Sub, Dadd, Daddi, Dsub,
    // Register operations
    And, Or, Xor, Nor, Slt, Sltu,
    // Shifts
    Sll, Srl, Sra, Dsll, Dsrl, Dsra,
    // Integer multiply/divide (HI/LO)
    Mult, Multu, Div, Divu, Dmult, Dmultu, Ddiv, Ddivu,
    Mfhi, Mflo, Mthi, Mtlo,
    // Memory
    Lb, Lbu, Lh, Lhu, Lw, Lwu, Ld, Sb, Sh, Sw, Sd,
    // Control flow (all have a branch delay slot unless annulled)
    Beq, Bne, J, Jal, Jr, Jalr,
    Bltz, Bgez, Bltzal, Bgezal, Bltzl, Bgezl, Bltzall, Bgezall,
    Blez, Bgtz, Blezl, Bgtzl,
    // CP0 / exceptions
    Mfc0, Mtc0, Dmfc0, Dmtc0, Eret, Syscall, Break,
};

struct Mips64Decoded {
    Mips64Op op = Mips64Op::Invalid;
    uint32_t raw = 0;
    uint8_t rs = 0;
    uint8_t rt = 0;
    uint8_t rd = 0;
    uint8_t sa = 0;
    uint8_t sel = 0;     // COP0 selector (bits 2:0) for MFC0/MTC0 family
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
    // GPR/PC/HI/LO/delay state zeroed, CP0 set to reset values
    // (Status = BEV|ERL, Cause = 0, EPC/ErrorEPC/BadVAddr = 0,
    // EBase = 0x80000000, Count = Compare = 0), fault cleared.
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
    // where execution resumes after the delay slot retires; delayBranch()
    // is the address of the branch that armed the slot (needed for EPC).
    bool delayArmed() const { return delayArmed_; }
    uint64_t delayTarget() const { return delayTarget_; }
    uint64_t delayBranch() const { return delayBranch_; }

    // -- CP0 access -----------------------------------------------------------
    uint64_t cp0Status() const { return status_; }
    uint64_t cp0Cause() const { return cause_; }
    uint64_t cp0Epc() const { return epc_; }
    uint64_t cp0ErrorEpc() const { return errorEpc_; }
    uint64_t cp0BadVAddr() const { return badVAddr_; }
    uint64_t cp0EBase() const { return ebase_; }
    uint64_t cp0Count() const { return count_; }
    uint64_t cp0Compare() const { return compare_; }

    bool statusExl() const { return (status_ & kStatusBitExl) != 0; }
    bool statusErl() const { return (status_ & kStatusBitErl) != 0; }
    bool statusBev() const { return (status_ & kStatusBitBev) != 0; }
    bool causeBd() const { return (cause_ & kCauseBitBd) != 0; }
    uint32_t causeExcCode() const {
        return static_cast<uint32_t>((cause_ & kCauseExcCodeMask) >> kCauseExcCodeShift);
    }

    // Current general exception vector for the configured BEV/EBase state.
    uint64_t exceptionVector() const;

    // -- exception state ------------------------------------------------------
    Mips64Fault fault() const { return fault_; }
    const std::string& lastError() const { return lastError_; }

    // -- endianness -----------------------------------------------------------
    Mips64Endianness endianness() const { return endianness_; }
    bool isLittleEndian() const { return endianness_ == Mips64Endianness::Little; }

    // -- fetch/decode/execute -------------------------------------------------
    // Fetch one instruction word at `address` per configured endianness.
    // Returns false (with fault set and synchronous exception entry
    // performed by step()) on misalignment or memory rejection.
    bool fetchWord(ia64::IMemory& memory, uint64_t address, uint32_t& out);

    // Decode the instruction at `address` without executing it. Decode
    // has no architectural side effects (no CP0/PC update).
    Mips64Decoded decodeAt(ia64::IMemory& memory, uint64_t address);

    // Execute a previously decoded instruction whose fetch address was
    // `address`. Implements delay-slot scheduling, link-register writes
    // and PC advancement. Returns Continue on retire, Fault otherwise.
    // Reserved encodings, address errors, SYSCALL/BREAK and trapping
    // overflows perform genuine exception entry here.
    Mips64StepResult executeDecoded(ia64::IMemory& memory,
                                    const Mips64Decoded& decoded,
                                    uint64_t address);

    // Combined fetch + decode + execute at the current PC. Ticks Count by
    // one per call (documented simple tick; Compare delivers no interrupt).
    Mips64StepResult step(ia64::IMemory& memory);

    // Disassembly of a decoded instruction (no memory access).
    std::string disassemble(const Mips64Decoded& decoded) const;

    // -- snapshot / serialization ----------------------------------------------
    // Serialization blob layout (blob itself is always little-endian and
    // versioned; the configured CPU endianness is stored and restored):
    //   offset 0:  magic 'M','P','6','2' (4 bytes)
    //   offset 4:  format version u8 (=2), endianness u8, reserved u16
    //   offset 8:  GPR[32] x u64 LE (256 bytes)
    //   offset 264: PC u64, HI u64, LO u64 (24 bytes)
    //   offset 288: delayArmed u8, delayTarget u64 LE, delayBranch u64 LE
    //   offset 305: fault u32 LE (4 bytes)
    //   offset 309: CP0 Status, Cause, EPC, ErrorEPC, BadVAddr, EBase,
    //               Count, Compare (8 x u64 LE, 64 bytes)
    // Total: 373 bytes. Format v1 (magic 'MP61', 301 bytes) is rejected.
    static constexpr std::size_t kSerializedSize = 373;
    std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, std::size_t size);

    // -- debugging ---------------------------------------------------------------
    std::string toString() const;
    static const char* faultName(Mips64Fault fault);
    static const char* opName(Mips64Op op);

private:
    void setFault(Mips64Fault fault, const std::string& message);
    void clearFault();

    // Synchronous exception entry. Updates Cause.ExcCode/BD (always),
    // BadVAddr (when hasBadVAddr), EPC (only when Status.EXL was clear),
    // sets Status.EXL, cancels any pending delay slot and redirects PC to
    // the general exception vector. Records fault_/lastError_ for
    // diagnostics. `faultPc` is the faulting instruction address and
    // `branchPc` the originating branch when inDelaySlot is true.
    void raiseException(Mips64ExcCode code, Mips64Fault fault,
                        const std::string& message, uint64_t faultPc,
                        bool inDelaySlot, uint64_t branchPc,
                        bool hasBadVAddr, uint64_t badVAddrValue);

    // CP0 move helpers. readCp0 returns false for unsupported rd/sel.
    // writeCp0 applies per-register writable masks and returns false for
    // unsupported rd/sel (no state change).
    bool readCp0(unsigned rd, unsigned sel, uint64_t& out) const;
    bool writeCp0(unsigned rd, unsigned sel, uint64_t value);

    Mips64StepResult executeMemory(ia64::IMemory& memory,
                                   const Mips64Decoded& decoded,
                                   uint64_t address,
                                   bool inDelaySlot,
                                   uint64_t branchPc);
    Mips64StepResult executeBranch(ia64::IMemory& memory,
                                   const Mips64Decoded& decoded,
                                   uint64_t address);
    // Portable 128-bit product of two 64-bit values (no __int128).
    static void multiplyU64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo);

    std::array<uint64_t, kNumGpr> gpr_{};
    uint64_t pc_ = kResetPc;
    uint64_t hi_ = 0;
    uint64_t lo_ = 0;
    bool delayArmed_ = false;
    uint64_t delayTarget_ = 0;
    uint64_t delayBranch_ = 0;
    // CP0 subset.
    uint64_t status_ = kStatusReset;
    uint64_t cause_ = 0;
    uint64_t epc_ = 0;
    uint64_t errorEpc_ = 0;
    uint64_t badVAddr_ = 0;
    uint64_t ebase_ = kEBaseReset;
    uint64_t count_ = 0;
    uint64_t compare_ = 0;
    Mips64Fault fault_ = Mips64Fault::None;
    std::string lastError_;
    Mips64Endianness endianness_ = Mips64Endianness::Little;
};

}  // namespace mips64
