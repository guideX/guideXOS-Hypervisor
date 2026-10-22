#pragma once

// guideXOS Hypervisor — MIPS64 Release 2 CPU (Phases MIPS64-1 + MIPS64-2 + MIPS64-3).
//
// Isolated MIPS64r2 architectural state with fetch/decode/execute,
// Coprocessor 0 (CP0 management subset + TLB management subset), a real
// bounded TLB, genuine virtual-to-physical address translation, privilege
// enforcement and architecturally routed TLB/address-error exceptions with
// ERET return. Each Mips64Cpu instance owns its register file, PC, HI/LO,
// CP0, TLB, branch delay-slot bookkeeping, exception state and endianness
// configuration. There is no global mutable register state and no reuse of
// IA-64 register structures.
//
// Byte order policy:
// - The CPU is constructed with an explicit endianness (little-endian by
//   default, matching the MIPS64-1 initial execution mode).
// - Instruction words AND multi-byte data accesses are assembled from
//   raw bytes according to the configured endianness. Host-native typed
//   reads are never used for guest data.
//
// Execution / memory model (MIPS64-3 virtual memory):
// - Guest programs issue virtual addresses (VAs). The core translates every
//   instruction fetch, load and store to a physical address (PA) before
//   touching the repository IMemory abstraction, which is treated as
//   PHYSICAL memory.
// - Compatibility segments: kuseg (mapped, U/S/K), ckseg0/ckseg1
//   (direct-mapped kernel-only, PA = VA & 0x1FFFFFFF), cksseg (mapped,
//   S/K), ckseg3 (mapped, kernel-only). See the segment constants below.
// - 64-bit extended regions (XKSEG, XKPHYS, XSSEG) are NOT supported in
//   this phase: any VA outside the compatibility segments fails explicitly
//   with an Address Error (AdEL/AdES). A 64-bit VA is never silently
//   truncated to 32 bits.
// - UX/SX/KX are stored CP0 controls; they round-trip through Status but do
//   not enable additional regions in this phase (documented limitation).
// - Exception vectors are guest virtual addresses translated like any other
//   fetch target (boot vectors live in the direct-mapped ckseg1 region, so
//   they need no TLB entry).
//
// Exception vectors (MIPS64r2 PRA Rev 2.50; see the MIPS64-2 audit note in
// docs/MIPS64_PHASE3_TLB_MMU_PRIVILEGE.md — the MIPS64-2 BEV=1 general
// vector 0xFFFFFFFFBFC00180 was incorrect):
// - BEV=1 boot base: 0xFFFFFFFFBFC00200; offsets: refill 0x000, XTLB
//   refill 0x080, general 0x180  =>  general = 0xFFFFFFFFBFC00380.
// - BEV=0: EBase-derived base with the same offsets; reset EBase gives
//   refill 0xFFFFFFFF80000000 and general 0xFFFFFFFF80000180.
// - Only a TLB miss (refill) taken with Status.EXL == 0 uses a refill
//   vector (0x000 for the supported 32-bit compatibility regions, 0x080
//   for 64-bit regions once supported). TLB Invalid, TLB Modified, address
//   errors and every other exception use the general vector.

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
constexpr uint64_t kResetPc = 0;  // deterministic power-on PC (kuseg, needs a TLB entry)

enum class Mips64Endianness : uint8_t {
    Little = 0,
    Big = 1,
};

// Diagnostic fault codes. These describe why the most recent step did not
// retire normally; synchronous MIPS exceptions additionally update CP0
// (Cause.ExcCode/BD, EPC, BadVAddr, EntryHi, Context/XContext where
// applicable, Status.EXL) and redirect PC to the selected exception vector.
// See raiseException / raiseTlbException / docs for the mapping.
//
// NOTE: numeric values 0..14 are frozen since MIPS64-2 (snapshot compat);
// MIPS64-3 codes are appended (15..25).
enum class Mips64Fault : uint32_t {
    None = 0,
    MisalignedFetch = 1,        // PC not word-aligned on fetch (-> AdEL entry)
    MemoryFaultFetch = 2,       // physical memory rejected the fetch (-> IBE entry)
    ReservedInstruction = 3,    // reserved encoding (-> RI entry)
    UnsupportedInstruction = 4,  // valid MIPS64 encoding outside the subset (no entry)
    MisalignedLoad = 5,         // load address violates alignment (-> AdEL entry)
    MisalignedStore = 6,        // store address violates alignment (-> AdES entry)
    MemoryFaultLoad = 7,        // physical memory rejected a load (-> DBE entry)
    MemoryFaultStore = 8,       // physical memory rejected a store (-> DBE entry)
    BranchInDelaySlot = 9,      // control-flow insn inside a delay slot (explicitly rejected)
    MisalignedBranchTarget = 10,  // JR/JALR target not word-aligned (no entry)
    Cp0Violation = 11,          // unsupported/invalid CP0 access (no entry)
    SyscallTrap = 12,           // SYSCALL retired into an exception (-> Sys entry)
    BreakTrap = 13,             // BREAK retired into an exception (-> Bp entry)
    OverflowTrap = 14,          // trapping arithmetic overflow (-> Ov entry)
    // -- MIPS64-3 additions --------------------------------------------------
    PrivilegeViolation = 15,    // privileged COP0/TLB/ERET op in S/U mode (-> RI entry)
    AddressErrorFetch = 16,     // bad segment/privilege/size on fetch (-> AdEL entry)
    AddressErrorLoad = 17,      // bad segment/privilege/size on load (-> AdEL entry)
    AddressErrorStore = 18,     // bad segment/privilege/size on store (-> AdES entry)
    TlbRefillFetch = 19,        // no TLB match on fetch (-> TLBL, refill vector if EXL=0)
    TlbRefillLoad = 20,         // no TLB match on load (-> TLBL, refill vector if EXL=0)
    TlbRefillStore = 21,        // no TLB match on store (-> TLBS, refill vector if EXL=0)
    TlbInvalidFetch = 22,       // matched TLB entry invalid on fetch (-> TLBL, general vector)
    TlbInvalidLoad = 23,        // matched TLB entry invalid on load (-> TLBL, general vector)
    TlbInvalidStore = 24,       // matched TLB entry invalid on store (-> TLBS, general vector)
    TlbModified = 25,           // store to valid non-dirty mapping (-> Mod, general vector)
};

// Step outcome. Returning Fault always coincides with fault() !=
// Mips64Fault::None and lastError() set. Faults that raise synchronous
// MIPS exceptions additionally perform full exception entry (CP0 update +
// PC redirected to the selected exception vector).
enum class Mips64StepResult : uint8_t {
    Continue = 0,  // instruction retired; PC advanced per delay-slot rules
    Fault = 1,     // instruction did not retire; see fault()/lastError()
    Interrupt = 2, // asynchronous interrupt delivered; PC redirected
};

// MIPS64r2 architectural exception codes (Cause.ExcCode) produced by this
// core, including the MIPS64-3 TLB codes.
enum class Mips64ExcCode : uint32_t {
    Int = 0,    // interrupt (Count/Compare or software IP)
    Mod = 1,    // TLB modified (store to valid but non-dirty mapping)
    TLBL = 2,   // TLB refill/invalid on load or instruction fetch
    TLBS = 3,   // TLB refill/invalid on store
    AdEL = 4,   // address error on load or instruction fetch
    AdES = 5,   // address error on store
    IBE = 6,    // bus error on instruction fetch (physical memory rejection)
    DBE = 7,    // bus error on data access (physical memory rejection)
    Sys = 8,    // SYSCALL
    Bp = 9,     // BREAK
    RI = 10,    // reserved instruction (incl. privileged ops in S/U mode)
    Ov = 12,    // arithmetic overflow (trapping ADD/ADDI/SUB/DADD/DADDI/DSUB)
};

// ============================================================================
// CP0 architectural state (MIPS64r2 subset incl. TLB management)
// ============================================================================

// CP0 register numbers (rd) and selectors (sel) implemented here.
constexpr unsigned kCp0RegIndex = 0;
constexpr unsigned kCp0SelIndex = 0;
constexpr unsigned kCp0RegRandom = 1;
constexpr unsigned kCp0SelRandom = 0;
constexpr unsigned kCp0RegEntryLo0 = 2;
constexpr unsigned kCp0SelEntryLo0 = 0;
constexpr unsigned kCp0RegEntryLo1 = 3;
constexpr unsigned kCp0SelEntryLo1 = 0;
constexpr unsigned kCp0RegContext = 4;
constexpr unsigned kCp0SelContext = 0;
constexpr unsigned kCp0RegPageMask = 5;
constexpr unsigned kCp0SelPageMask = 0;
constexpr unsigned kCp0RegWired = 6;
constexpr unsigned kCp0SelWired = 0;
constexpr unsigned kCp0RegBadVAddr = 8;
constexpr unsigned kCp0SelBadVAddr = 0;
constexpr unsigned kCp0RegCount = 9;
constexpr unsigned kCp0SelCount = 0;
constexpr unsigned kCp0RegEntryHi = 10;
constexpr unsigned kCp0SelEntryHi = 0;
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
constexpr unsigned kCp0RegXContext = 20;
constexpr unsigned kCp0SelXContext = 0;
constexpr unsigned kCp0RegErrorEpc = 30;
constexpr unsigned kCp0SelErrorEpc = 0;

// Status bit positions and the software-writable mask for this subset.
// Writable: IE, EXL, ERL, KSU, UX, SX, KX, IM[7:0], BEV. Everything else
// (CU, RP, FR, RE, MX, PX, TS, SR, NMI, Impl, ...) reads 0 in this phase.
// KSU == 0b11 is reserved; this core treats it as kernel-equivalent
// (implementation-defined, documented; supervisor/user 01/10 enforce).
constexpr uint64_t kStatusBitIe = 1ULL << 0;
constexpr uint64_t kStatusBitExl = 1ULL << 1;
constexpr uint64_t kStatusBitErl = 1ULL << 2;
constexpr unsigned kStatusKsuShift = 3;
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
constexpr uint64_t kCauseBitDc = 1ULL << 23;  // Disable Count (Release 2)
constexpr uint64_t kCauseBitIv = 1ULL << 24;  // Interrupt Vector select
constexpr uint64_t kCauseBitTi = 1ULL << 30;  // Timer Interrupt pending
constexpr uint64_t kCauseIp7Bit = 1ULL << 15; // IP7 hardware-pending (timer)
constexpr unsigned kCauseExcCodeShift = 2;
constexpr uint64_t kCauseExcCodeMask = 0x7CULL;  // bits 6:2

// EBase: exception-base bits 31:12 writable, low 12 bits (incl. CPUNum)
// read-only zero; upper 32 bits read zero in this phase.
constexpr uint64_t kEBaseWritableMask = 0xFFFFF000ULL;
constexpr uint64_t kEBaseReset = 0x80000000ULL;

// Exception vectors (MIPS64r2 PRA Rev 2.50). BEV=1 uses the boot base plus
// the refill/XTLB/general offsets; BEV=0 uses the EBase-derived base plus
// the same offsets.
constexpr uint64_t kVectorBootBase = 0xFFFFFFFFBFC00200ULL;    // BEV=1 base
constexpr uint64_t kVectorOffsetRefill = 0x000ULL;            // TLB refill (EXL=0)
constexpr uint64_t kVectorOffsetXtlb = 0x080ULL;              // XTLB refill (EXL=0)
constexpr uint64_t kVectorOffsetGeneral = 0x180ULL;            // everything else
constexpr uint64_t kVectorBev1General = 0xFFFFFFFFBFC00380ULL;  // BEV=1 general
constexpr uint64_t kVectorBev1Refill = 0xFFFFFFFFBFC00200ULL;   // BEV=1 refill
constexpr uint64_t kVectorBev1Xtlb = 0xFFFFFFFFBFC00280ULL;     // BEV=1 XTLB refill

// ============================================================================
// Virtual address map (32-bit compatibility segments + explicit exclusions)
// ============================================================================
//
// Only properly sign-extended 32-bit compatibility addresses are supported:
//   kuseg  0x0000000000000000 - 0x000000007FFFFFFF  mapped, U/S/K via TLB
//   ckseg0 0xFFFFFFFF80000000 - 0xFFFFFFFF9FFFFFFF  direct, kernel-only
//   ckseg1 0xFFFFFFFFA0000000 - 0xFFFFFFFFBFFFFFFF  direct, kernel-only
//   cksseg 0xFFFFFFFFC0000000 - 0xFFFFFFFFDFFFFFFF  mapped, S/K via TLB
//   ckseg3 0xFFFFFFFFE0000000 - 0xFFFFFFFFFFFFFFFF  mapped, kernel-only via TLB
// Direct segments translate as PA = VA & kDirectMapMask (29-bit physical).
// Anything else (zero-extended 0x80000000-0xFFFFFFFF, XKSEG, XKPHYS, XSSEG
// and the rest of the 64-bit space) raises AdEL/AdES explicitly and is
// never silently truncated. 64-bit physical addresses are therefore capped
// at 36 bits by the EntryLo PFN format (24-bit PFN + 12-bit offset).
constexpr uint64_t kKusegEnd = 0x000000007FFFFFFFULL;
constexpr uint64_t kCKSeg0Base = 0xFFFFFFFF80000000ULL;
constexpr uint64_t kCKSeg0End = 0xFFFFFFFF9FFFFFFFULL;
constexpr uint64_t kCKSeg1Base = 0xFFFFFFFFA0000000ULL;
constexpr uint64_t kCKSeg1End = 0xFFFFFFFFBFFFFFFFULL;
constexpr uint64_t kCKSsegBase = 0xFFFFFFFFC0000000ULL;
constexpr uint64_t kCKSsegEnd = 0xFFFFFFFFDFFFFFFFULL;
constexpr uint64_t kCKSeg3Base = 0xFFFFFFFFE0000000ULL;
constexpr uint64_t kCKSeg3End = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint64_t kDirectMapMask = 0x1FFFFFFFULL;  // PA = VA & mask (512 MiB)

// ============================================================================
// TLB architecture (MIPS64-3, bounded, per-CPU)
// ============================================================================

constexpr unsigned kTlbEntries = 16;  // fixed JTLB size for this phase

// TLBWI/TLBWR are constrained by Wired: Random cycles deterministically in
// [Wired, kTlbEntries). Wired is clamped to [0, kTlbEntries] on write.
constexpr unsigned kTlbMaxWired = kTlbEntries;

// EntryHi layout: VPN2 = VA[63:13], bits 12:8 read-only zero, ASID = bits 7:0.
constexpr uint64_t kEntryHiVpn2Mask = 0xFFFFFFFFFFFFE000ULL;
constexpr uint64_t kEntryHiAsidMask = 0xFFULL;
constexpr uint64_t kEntryHiWritableMask = kEntryHiVpn2Mask | kEntryHiAsidMask;

// PageMask register: only bits 24:13 (the 12-bit Mask field) are writable.
// Supported Mask values (ones-patterns) and their page sizes:
//   0x000 -> 4 KiB, 0x003 -> 16 KiB, 0x00F -> 64 KiB, 0x03F -> 256 KiB,
//   0x0FF -> 1 MiB, 0x3FF -> 4 MiB, 0xFFF -> 16 MiB (per paired entry both
//   pages share the size; a pair spans twice the page size).
// Any other Mask pattern is UNSUPPORTED: TLBWI/TLBWR store the entry but
// flag it so it never matches (deterministic, never silent 4 KiB alias).
constexpr uint64_t kPageMaskWritableMask = 0x01FFE000ULL;
constexpr unsigned kPageMaskShift = 13;

// EntryLo layout (32-bit): G=bit0, V=bit1, D=bit2, C=bits5:3, PFN=bits29:6.
// Bits 31:30 are reserved zero in this phase (36-bit physical cap).
constexpr uint64_t kEntryLoWritableMask = 0x3FFFFFFFULL;
constexpr unsigned kEntryLoPfnShift = 6;
constexpr uint32_t kEntryLoPfnMask = 0xFFFFFFU;
constexpr unsigned kEntryLoCShift = 3;
constexpr uint32_t kEntryLoDBit = 1U << 2;
constexpr uint32_t kEntryLoVBit = 1U << 1;
constexpr uint32_t kEntryLoGBit = 1U << 0;

// Index layout: P (probe failure) = bit 31, index = low 4 bits.
constexpr uint64_t kIndexPBit = 1ULL << 31;
constexpr uint64_t kIndexFieldMask = 0xFULL;
constexpr uint64_t kIndexWritableMask = kIndexPBit | kIndexFieldMask;

// Context (32-bit): BadVPN2 = VA[31:23] in bits 31:23 (HW-written),
// PTEBase = bits 22:4 (software-writable).
constexpr uint64_t kContextWritableMask = 0x007FFFF0ULL;  // bits 22:4

// XContext (64-bit, implemented layout): PTEBase = bits 51:34 (writable),
// R = VA[63:61] in bits 33:31 (HW-written),
// BadVPN2 = VA[30:13] in bits 30:13 (HW-written, 4 KiB view), zeros below.
constexpr uint64_t kXContextPteBaseMask = 0x000FFFFC00000000ULL;  // bits 51:34

// One paired-page TLB entry (architectural shape, per-CPU bounded array).
struct Mips64TlbEntry {
    uint64_t vpn2 = 0;   // VPN2 = VA[63:13] (51 bits)
    uint32_t pfn0 = 0;   // even-page PFN (24 bits used)
    uint32_t pfn1 = 0;   // odd-page PFN (24 bits used)
    uint16_t mask = 0;   // 12-bit PageMask value (only low bits used per size)
    uint8_t asid = 0;    // address space identifier
    uint8_t c0 = 0;      // even-page cache attribute (stored, not emulated)
    uint8_t c1 = 0;      // odd-page cache attribute (stored, not emulated)
    bool v0 = false;     // even valid
    bool d0 = false;     // even dirty
    bool v1 = false;     // odd valid
    bool d1 = false;     // odd dirty
    bool g = false;      // global (matches any ASID)
    bool badMask = false;  // unsupported PageMask pattern: never matches
};

// Supported PageMask values (12-bit Mask field, ones-patterns only).
inline bool isSupportedPageMask(uint16_t mask) {
    switch (mask) {
        case 0x000:
        case 0x003:
        case 0x00F:
        case 0x03F:
        case 0x0FF:
        case 0x3FF:
        case 0xFFF:
            return true;
        default:
            return false;
    }
}

// Memory access kinds for translation (selects TLBL vs TLBS vs Mod/AdEL/AdES).
enum class Mips64Access : uint8_t {
    Fetch = 0,
    Load = 1,
    Store = 2,
};

// Side-effect-free translation probe result (no CP0 update, no entry).
struct Mips64Probe {
    bool ok = false;                       // true: phys is the translated PA
    uint64_t phys = 0;                      // translated physical address
    Mips64ExcCode code = Mips64ExcCode::AdEL;  // exception code if !ok
    Mips64Fault fault = Mips64Fault::None;     // diagnostic fault if !ok
    bool isRefill = false;                 // true: miss eligible for refill vector
};

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
    // TLB management (MIPS64-3; privileged, kernel-only)
    Tlbp, Tlbr, Tlbwi, Tlbwr,
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
    // EBase = 0x80000000, Count = Compare = 0, TLB management registers
    // zeroed, Wired = 0, Random = kTlbEntries - 1, Index = 0, all TLB
    // entries invalid), fault cleared.
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
    // MIPS64-3 TLB management registers.
    uint64_t cp0Index() const { return index_; }
    uint64_t cp0Random() const { return random_; }
    uint64_t cp0EntryHi() const { return entryHi_; }
    uint64_t cp0EntryLo0() const { return entryLo0_; }
    uint64_t cp0EntryLo1() const { return entryLo1_; }
    uint64_t cp0Context() const { return context_; }
    uint64_t cp0PageMask() const { return pageMask_; }
    uint64_t cp0Wired() const { return wired_; }
    uint64_t cp0XContext() const { return xcontext_; }

    bool statusExl() const { return (status_ & kStatusBitExl) != 0; }
    bool statusErl() const { return (status_ & kStatusBitErl) != 0; }
    bool statusBev() const { return (status_ & kStatusBitBev) != 0; }
    bool causeBd() const { return (cause_ & kCauseBitBd) != 0; }
    uint32_t causeExcCode() const {
        return static_cast<uint32_t>((cause_ & kCauseExcCodeMask) >> kCauseExcCodeShift);
    }

    // Effective privilege (EXL/ERL force kernel; KSU==0b11 reserved is
    // treated as kernel-equivalent, documented implementation choice).
    unsigned statusKsu() const {
        return static_cast<unsigned>((status_ & kStatusKsuMask) >> kStatusKsuShift);
    }
    bool isKernelMode() const {
        return statusExl() || statusErl() || statusKsu() == 0U || statusKsu() == 3U;
    }
    bool isSupervisorMode() const {
        return !statusExl() && !statusErl() && statusKsu() == 1U;
    }
    bool isUserMode() const {
        return !statusExl() && !statusErl() && statusKsu() == 2U;
    }

    // Current general exception vector for the configured BEV/EBase state.
    uint64_t exceptionVector() const;
    // Pure vector computation for any BEV/EBase/offset combination.
    static uint64_t exceptionVectorFor(bool bev, uint64_t ebase, uint64_t offset);
    // Refill vector for a faulting VA (offset 0x000 for the supported
    // 32-bit compatibility regions, 0x080 for 64-bit regions).
    uint64_t refillVector(uint64_t faultVa) const;

    // Side-effect-free virtual-to-physical translation probe.
    Mips64Probe probeAddress(uint64_t va, Mips64Access access) const;

    // TLB inspection (bounded, per-CPU).
    static constexpr unsigned tlbSize() { return kTlbEntries; }
    bool tlbEntry(unsigned index, Mips64TlbEntry& out) const;

    // -- exception state ------------------------------------------------------
    Mips64Fault fault() const { return fault_; }
    const std::string& lastError() const { return lastError_; }

    // -- endianness -----------------------------------------------------------
    Mips64Endianness endianness() const { return endianness_; }
    bool isLittleEndian() const { return endianness_ == Mips64Endianness::Little; }

    // -- fetch/decode/execute -------------------------------------------------
    // Fetch one instruction word at virtual `address` per configured
    // endianness (translates the VA; records but never enters exceptions).
    // Returns false (with fault set) on misalignment, translation failure
    // or physical memory rejection.
    bool fetchWord(ia64::IMemory& memory, uint64_t address, uint32_t& out);

    // Decode the instruction at virtual `address` without executing it.
    // Decode has no architectural side effects (no CP0/PC update).
    Mips64Decoded decodeAt(ia64::IMemory& memory, uint64_t address);

    // Execute a previously decoded instruction whose fetch address was
    // `address`. Implements delay-slot scheduling, link-register writes
    // and PC advancement. Returns Continue on retire, Fault otherwise.
    // Reserved encodings, address errors, TLB exceptions, SYSCALL/BREAK,
    // trapping overflows and user/supervisor privileged-operation traps
    // perform genuine exception entry here.
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
    //   offset 4:  format version u8 (=3), endianness u8, reserved u16
    //   offset 8:  GPR[32] x u64 LE (256 bytes)
    //   offset 264: PC u64, HI u64, LO u64 (24 bytes)
    //   offset 288: delayArmed u8, delayTarget u64 LE, delayBranch u64 LE
    //   offset 305: fault u32 LE (4 bytes)
    //   offset 309: CP0 Status, Cause, EPC, ErrorEPC, BadVAddr, EBase,
    //               Count, Compare (8 x u64 LE, 64 bytes)
    //   offset 373: TLB management: Index, Random, EntryHi, EntryLo0,
    //               EntryLo1, PageMask, Wired, Context, XContext (9 x u64 LE)
    //   offset 445: TLB entries: 16 x 32 bytes
    //               (vpn2 u64, pfn0 u32, pfn1 u32, mask u16, asid u8,
    //                c0 u8, c1 u8, flags u8 [v0,d0,v1,d1,g,badMask],
    //                reserved 10 bytes zero)
    // Total: 957 bytes. Format v1 (magic 'MP61', 301 bytes) and v2 (version
    // 2, 373 bytes) are rejected; truncation, bad magic/version, null data
    // and out-of-range field values fail safely with live state untouched.
    static constexpr std::size_t kSerializedSize = 957;
    std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, std::size_t size);

    // -- debugging ---------------------------------------------------------------
    std::string toString() const;
    static const char* faultName(Mips64Fault fault);
    static const char* opName(Mips64Op op);

private:
    void setFault(Mips64Fault fault, const std::string& message);
    void clearFault();

    // Shared synchronous exception-entry core. Updates Cause.ExcCode/BD
    // (always), EPC (only when Status.EXL was clear), sets Status.EXL,
    // cancels any pending delay slot and redirects PC to `vector`.
    // Records fault_/lastError_ for diagnostics. `faultPc` is the faulting
    // instruction address and `branchPc` the originating branch when
    // inDelaySlot is true.
    void enterException(Mips64ExcCode code, Mips64Fault fault,
                        const std::string& message, uint64_t faultPc,
                        bool inDelaySlot, uint64_t branchPc, uint64_t vector);

    // General exception entry (general vector; BadVAddr updated only when
    // hasBadVAddr, for AdEL/AdES).
    void raiseException(Mips64ExcCode code, Mips64Fault fault,
                        const std::string& message, uint64_t faultPc,
                        bool inDelaySlot, uint64_t branchPc,
                        bool hasBadVAddr, uint64_t badVAddrValue);

    // TLB exception entry. Always updates BadVAddr, Context.BadVPN2,
    // XContext.R/BadVPN2 and EntryHi.VPN2 (ASID preserved), then enters
    // via the refill vector (miss with EXL clear) or the general vector.
    void raiseTlbException(Mips64ExcCode code, Mips64Fault fault,
                           const std::string& message, uint64_t faultPc,
                           bool inDelaySlot, uint64_t branchPc,
                           uint64_t faultVa, bool isRefill);

    // Enter the exception described by a failed probe (fetch or data path).
    void raiseProbe(const Mips64Probe& probe, uint64_t faultPc,
                    bool inDelaySlot, uint64_t branchPc, uint64_t faultVa);

    // Privileged-operation gate for COP0/TLB/ERET: kernel effective mode
    // required, else RI entry with PrivilegeViolation diagnostics.
    bool requireKernel(uint64_t address, bool inDelaySlot, uint64_t branchPc,
                       const char* what);

    // TLB lookup: lowest matching entry index, or -1. Matches EntryHi VPN2
    // under each entry's own PageMask plus ASID/global semantics.
    int tlbProbe(uint64_t vpn2, uint8_t asid) const;
    bool tlbMatch(const Mips64TlbEntry& entry, uint64_t vpn2, uint8_t asid) const;

    // CP0 move helpers. readCp0 returns false for unsupported rd/sel.
    // writeCp0 applies per-register writable masks and returns false for
    // unsupported rd/sel (no state change). Random writes are accepted and
    // ignored (read-only register).
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
    Mips64StepResult executeTlb(const Mips64Decoded& decoded,
                                uint64_t address,
                                bool inDelaySlot,
                                uint64_t branchPc);
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
    // CP0 TLB management (MIPS64-3).
    uint64_t index_ = 0;      // P in bit 31, entry index in low bits
    uint64_t random_ = kTlbEntries - 1;
    uint64_t entryHi_ = 0;
    uint32_t entryLo0_ = 0;
    uint32_t entryLo1_ = 0;
    uint32_t pageMask_ = 0;   // Mask field in bits 24:13
    uint64_t wired_ = 0;
    uint32_t context_ = 0;    // BadVPN2 in bits 31:23, PTEBase in bits 22:4
    uint64_t xcontext_ = 0;   // PTEBase bits 51:34, R bits 33:31, BadVPN2 bits 30:13
    std::array<Mips64TlbEntry, kTlbEntries> tlb_{};
    Mips64Fault fault_ = Mips64Fault::None;
    std::string lastError_;
    Mips64Endianness endianness_ = Mips64Endianness::Little;
};

}  // namespace mips64
