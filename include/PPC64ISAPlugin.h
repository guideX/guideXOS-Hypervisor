#pragma once

#include "IISA.h"
#include "IMemory.h"
#include "MemoryReservation.h"
#include "PPC64PartitionMMU.h"
#include "PPC64RadixMMU.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ia64 {
namespace ppc64 {

class PPC64VirtualTimeBase;

// ============================================================================
// PPC64 Phase 1 + Phase 2 constants
// ============================================================================
//
// Target: PowerPC 64-bit, big-endian, interpreter, IBM pSeries / sPAPR.
// Fixed 32-bit instruction width. Single-CPU validation model.
//
// Phase 1 subset:
//   addi, addis, ori, oris, b (all AA/LK forms), nop (ori r0,r0,0).
// Phase 2 subset (OpenPOWER Power ISA v3.1C, Book I fixed-point + branches):
//   add[o][.], subf[o][.], addic, addic., addc[.], adde[.], subfc[.],
//   and[.], or[.], xor[.], andi., andis., xori, xoris,
//   cmp[l] (L=0/1, BF=0..7), cmpi, cmpli (L=0/1, BF=0..7),
//   bc[bca][bcl][bcla], bclr[bclrl], bcctr[bcctrl], mfspr/mtspr (XER/LR/CTR).
// Phase 3 subset (Power ISA v3.1C, Book I integer storage access + traps):
//   D-form: lbz[ux], lhz[ux], lha[ux], lwz[ux], stb[ux], sth[ux], stw[ux],
//   DS-form: ld[u], lwa, std[u] (XO=3 reserved),
//   X-form indexed: lbzx, lhzx, lhax, lwzx, lwax, ldx,
//                   stbx, sthx, stwx, stdx (Rc must be 0),
//   SC-form: sc (LEV=0 only; other opcode-17 patterns rejected).
// Phase 5 subset (Book II atomics and synchronization, Book I XL-form):
//   lwarx, ldarx (EH hint accepted, functionally ignored),
//   stwcx., stdcx. (Rc=1 mandatory; Rc=0 is an invalid form),
//   sync (L=0 heavyweight), lwsync (L=1), isync, eieio.
//   Only the exact documented encodings of sync/lwsync/isync/eieio are
//   accepted; other words in those XO slots (ptesync, persistent-storage
//   variants, reserved-field patterns) are deterministic emulator limitations.
// Everything else decodes as unsupported and executes as EXCEPTION.

constexpr size_t NUM_GPR = 32;
constexpr size_t INSN_WIDTH = 4;

// Serialization layout v1 (explicit little-endian field encoding, 304 bytes):
//   magic[4] = {'P','P','C','1'} | version u32 = 1
//   GPR[32] u64 | NIP u64 | LR u64 | CTR u64 | CR u32 | XER u32 | MSR u64
// Serialization layout v2 (Phase 4, 332 bytes): v1 plus privileged state:
//   SRR0 u64 | SRR1 u64 | DAR u64 | DSISR u32
// Legacy v1/v2 restores initialize PIDR to zero. serialize_state() preserves
// v1 when privileged state and PIDR are zero, emits v2 for nonzero privileged
// state with PIDR=0, and emits v3 when PIDR is nonzero. Explicit v2/v3 APIs
// select their respective layouts.
constexpr uint32_t STATE_MAGIC = 0x31435050;  // 'P','P','C','1' little-endian
constexpr uint32_t STATE_VERSION = 1;
constexpr size_t STATE_SIZE =
    4 + 4 + (NUM_GPR * 8) + 8 + 8 + 8 + 4 + 4 + 8;  // = 304 bytes
constexpr uint32_t STATE_VERSION_V2 = 2;
constexpr size_t STATE_SIZE_V2 =
    4 + 4 + (NUM_GPR * 8) + 8 + 8 + 8 + 4 + 4 + 8 + 8 + 8 + 8 + 4;  // = 332 bytes
constexpr uint32_t STATE_VERSION_V3 = 3;
constexpr size_t STATE_SIZE_V3 = STATE_SIZE_V2 + 8; // v2 plus guest-visible PIDR
constexpr uint32_t STATE_VERSION_V4 = 4;
constexpr size_t STATE_SIZE_V4 = STATE_SIZE_V3 + 8 + 8 + 8 + 4;

// ============================================================================
// PPC64 Phase 4 Book III-S profile (Power ISA v3.1C, server environment)
// ============================================================================
//
// Configuration: PowerPC 64-bit, big-endian, single processor, real-address
// guest exception vectors (base 0x0, MSR[IP]=0), 64-bit execution. POWER9
// process-scoped radix translation requires an explicit process table and
// partition configuration. Phase 8 permits HV only after host-configured
// storage-exception entry; guest instructions cannot change MSR[HV]. IP/LE
// remain unsupported, and no PAPR partition table is implied.
//
// MSR bit definitions below use Power ISA IBM bit numbering (0=MSB) with
// Linux-compatible masks (1ULL << LG where LG = 63 - IBMbit):
//   SF  IBM 0  mask 0x8000000000000000 (64-bit mode; stored, execution stays
//                                       64-bit even when 0 for Phase 1-3
//                                       reset compatibility)
//   HV  IBM 3  mask 0x1000000000000000 (hypervisor; entered only on a
//                                       supported partition-storage interrupt)
//   EE  IBM 48 mask 0x0000000000008000 (external interrupt enable)
//   PR  IBM 49 mask 0x0000000000004000 (0=privileged, 1=problem)
//   FP  IBM 50 mask 0x0000000000002000 (preserved, not interpreted)
//   ME  IBM 51 mask 0x0000000000001000 (preserved, not interpreted)
//   FE0 IBM 52 mask 0x0000000000000800 (preserved)
//   SE  IBM 53 mask 0x0000000000000400 (preserved)
//   BE  IBM 54 mask 0x0000000000000200 (preserved)
//   FE1 IBM 55 mask 0x0000000000000100 (preserved)
//   IP  IBM 57 mask 0x0000000000000040 (exception prefix; must be 0)
//   IR  IBM 58 mask 0x0000000000000020 (instruction relocate; radix profile)
//   DR  IBM 59 mask 0x0000000000000010 (data relocate; radix profile)
//   RI  IBM 62 mask 0x0000000000000002 (recoverable interrupt)
//   LE  IBM 63 mask 0x0000000000000001 (little-endian; must be 0, BE only)
// On guest exception entry the new MSR is oldMSR with EE/PR/IR/DR/RI
// cleared (enter privileged, non-recoverable, real-address, EE off);
// all other bits preserved.
constexpr uint64_t MSR_SF = 1ULL << 63;
constexpr uint64_t MSR_HV = 1ULL << 60;
constexpr uint64_t MSR_EE = 1ULL << 15;
constexpr uint64_t MSR_PR = 1ULL << 14;
constexpr uint64_t MSR_FP = 1ULL << 13;
constexpr uint64_t MSR_ME = 1ULL << 12;
constexpr uint64_t MSR_FE0 = 1ULL << 11;
constexpr uint64_t MSR_SE = 1ULL << 10;
constexpr uint64_t MSR_BE = 1ULL << 9;
constexpr uint64_t MSR_FE1 = 1ULL << 8;
constexpr uint64_t MSR_IP = 1ULL << 6;
constexpr uint64_t MSR_IR = 1ULL << 5;
constexpr uint64_t MSR_DR = 1ULL << 4;
constexpr uint64_t MSR_RI = 1ULL << 1;
constexpr uint64_t MSR_LE = 1ULL << 0;
// Bits unsupported by the bounded execution profile.
constexpr uint64_t MSR_MUST_BE_ZERO =
    MSR_IP | MSR_LE;
constexpr uint64_t MSR_RELOCATION = MSR_IR | MSR_DR;
// SRR1 mask: bits loaded from MSR upon interrupt (Linux SRR1_MSR_BITS).
// Remaining low bits (0x783F0000) carry interrupt-specific cause.
constexpr uint64_t SRR1_MSR_MASK = ~0x783F0000ULL;
// SRR1 cause bits (spec-backed, Linux arch/powerpc/include/asm/reg.h):
constexpr uint32_t SRR1_PROGILL = 0x00080000U;   // illegal instruction
constexpr uint32_t SRR1_PROGPRIV = 0x00040000U;  // privileged instruction
constexpr uint32_t SRR1_PROGTRAP = 0x00020000U;  // trap
constexpr uint32_t SRR1_PROGADDR = 0x00010000U;  // SRR0 is subsequent addr
constexpr uint32_t SRR1_ISI_NOPT = 0x40000000U;  // ISI: no translation found
constexpr uint32_t SRR1_ISI_PROT = 0x08000000U;  // ISI: protection fault
constexpr uint32_t SRR1_RADIX_BADCONFIG = 0x00080000U;
constexpr uint32_t SRR1_ATOMIC_RC = 0x00040000U;
// DSISR cause bits (spec-backed, Linux reg.h):
constexpr uint32_t DSISR_ISSTORE = 0x02000000U;   // access was a store
constexpr uint32_t DSISR_PROTFAULT = 0x08000000U;  // protection fault
constexpr uint32_t DSISR_NOHPTE = 0x40000000U;     // no translation found
constexpr uint32_t DSISR_RADIX_BADCONFIG = 0x00080000U;
constexpr uint32_t DSISR_ATOMIC_RC = 0x00040000U;
constexpr uint32_t DSISR_PRTABLE_FAULT = 0x00020000U;
constexpr uint32_t DSISR_SET_RC = 0x00040000U;
constexpr uint32_t SRR1_ISI_NOEXEC_OR_G = 0x10000000U;
constexpr uint32_t SRR1_NOEXEC_OR_G = 0x10000000U;

// Guest-visible privileged SPR numbers (architectural identities):
//   DSISR SPR 18, DAR SPR 19, SRR0 SPR 26, SRR1 SPR 27.
constexpr uint16_t SPR_DSISR = 18;
constexpr uint16_t SPR_DAR = 19;
constexpr uint16_t SPR_SRR0 = 26;
constexpr uint16_t SPR_SRR1 = 27;
constexpr uint16_t SPR_PIDR = 48;
constexpr uint16_t SPR_HDSISR = 306;
constexpr uint16_t SPR_HDAR = 307;
constexpr uint16_t SPR_HSRR0 = 314;
constexpr uint16_t SPR_HSRR1 = 315;

// Book III-S vector offsets for the supported initial profile (base 0x0).
constexpr uint64_t VEC_DSI = 0x300;
constexpr uint64_t VEC_ISI = 0x400;
constexpr uint64_t VEC_ALIGN = 0x600;
constexpr uint64_t VEC_PROG = 0x700;
constexpr uint64_t VEC_SC = 0xC00;
constexpr uint64_t VEC_HDSI = 0xE00;
constexpr uint64_t VEC_HISI = 0xE20;
// Real-address vector base for this profile (MSR[IP]=0).
constexpr uint64_t EXC_VECTOR_BASE = 0x0ULL;
// Bounded protection against non-progressing recursive faults.
constexpr int MAX_NESTED_EXCEPTIONS = 16;

// Primary opcodes (bits 26-31) covered by Phase 1 + Phase 2.
constexpr uint8_t OPCODE_ADDI = 14;
constexpr uint8_t OPCODE_ADDIS = 15;
constexpr uint8_t OPCODE_ORI = 24;
constexpr uint8_t OPCODE_ORIS = 25;
constexpr uint8_t OPCODE_B = 18;
constexpr uint8_t OPCODE_BC = 16;
constexpr uint8_t OPCODE_XL = 19;  // bclr / bcctr family
constexpr uint8_t OPCODE_XO = 31;  // X-form arithmetic/logical/compare + mfspr/mtspr
constexpr uint8_t OPCODE_CMPLI = 10;
constexpr uint8_t OPCODE_CMPI = 11;
constexpr uint8_t OPCODE_ADDIC = 12;
constexpr uint8_t OPCODE_ADDIC_DOT = 13;
constexpr uint8_t OPCODE_XORI = 26;
constexpr uint8_t OPCODE_XORIS = 27;
constexpr uint8_t OPCODE_ANDI_DOT = 28;
constexpr uint8_t OPCODE_ANDIS_DOT = 29;
// Phase 3 D-form integer load/store opcodes (Power ISA Book I §4.6).
constexpr uint8_t OPCODE_LWZ = 32;
constexpr uint8_t OPCODE_LWZU = 33;
constexpr uint8_t OPCODE_LBZ = 34;
constexpr uint8_t OPCODE_LBZU = 35;
constexpr uint8_t OPCODE_STW = 36;
constexpr uint8_t OPCODE_STWU = 37;
constexpr uint8_t OPCODE_STB = 38;
constexpr uint8_t OPCODE_STBU = 39;
constexpr uint8_t OPCODE_LHZ = 40;
constexpr uint8_t OPCODE_LHZU = 41;
constexpr uint8_t OPCODE_LHA = 42;
constexpr uint8_t OPCODE_LHAU = 43;
constexpr uint8_t OPCODE_STH = 44;
constexpr uint8_t OPCODE_STHU = 45;
// Phase 3 DS-form opcodes: 58 = ld/ldu/lwa, 62 = std/stdu.
constexpr uint8_t OPCODE_DS_LOAD = 58;
constexpr uint8_t OPCODE_DS_STORE = 62;
// Phase 3 SC-form opcode (sc trap).
constexpr uint8_t OPCODE_SC = 17;

// X-form 10-bit extended opcodes ((word >> 1) & 0x3FF), matching the
// OpenPOWER/IBM opcode tables. For the OE-capable arithmetic family the
// top bit of this field is the OE bit: base forms use the table value,
// OE=1 forms add 512 (addo=778, subfo=552; addco/addeo/subfco are valid
// per ISA but out of Phase 2 scope and rejected).
constexpr uint16_t XO_ADD = 266;
constexpr uint16_t XO_ADDC = 10;
constexpr uint16_t XO_ADDE = 138;
constexpr uint16_t XO_SUBF = 40;
constexpr uint16_t XO_SUBFC = 8;
constexpr uint16_t XO_AND = 28;    // no OE form
constexpr uint16_t XO_OR = 444;    // no OE form
constexpr uint16_t XO_XOR = 316;   // no OE form
constexpr uint16_t XO_CMP = 0;     // Rc must be 0
constexpr uint16_t XO_CMPL = 32;   // Rc must be 0

// X-form/XFX 10-bit combined extended opcode ((word >> 1) & 0x3FF).
constexpr uint16_t XO_MFSPR = 339;
constexpr uint16_t XO_MTSPR = 467;
constexpr uint16_t XO_BCLR = 16;
constexpr uint16_t XO_BCCTR = 528;
// Phase 4 X-form MSR access (opcode 31) and XL-form return (opcode 19).
// mfmsr RT: opcode 31, XO 83, RA=RB=0, Rc=0 (AIX Appendix F; Linux reg.h).
// mtmsr RS: opcode 31, XO 146, RA=RB=0, Rc=0. mtmsrd (XO 178) is valid per
// ISA but explicitly unsupported in this profile (rejected, not delivered).
// rfid: opcode 19, XO 18, exact encoding 0x4C000024 (all other XL fields 0;
// instructionsets.com/powerisa/rfid: XL-form, privileged, SRR0->PC/MSR<-SRR1).
constexpr uint16_t XO_MFMSR = 83;
constexpr uint16_t XO_MTMSR = 146;
constexpr uint16_t XO_MTMSRD = 178;
constexpr uint16_t XO_RFID = 18;
constexpr uint32_t ENCODING_RFID = 0x4C000024;
constexpr uint16_t XO_HRFID = 274;
constexpr uint32_t ENCODING_HRFID = 0x4C000224;

// Phase 5 atomic and synchronization opcodes (opcode 31 unless noted).
// Load/store-conditional XO values are the 10-bit field (word bits 21-30).
// EH (lwarx/ldarx) is bit 31 (LSB); stwcx./stdcx. require bit 31 = 1 (Rc).
constexpr uint16_t XO_LWARX = 20;
constexpr uint16_t XO_LDARX = 84;
constexpr uint16_t XO_STWCX = 150;
constexpr uint16_t XO_STDCX = 214;
// sync/lwsync (XO 598), eieio (XO 854); isync is XL-form opcode 19 XO 150.
constexpr uint16_t XO_SYNC = 598;
constexpr uint16_t XO_EIEIO = 854;
constexpr uint16_t XO_ISYNC = 150;
constexpr uint32_t ENCODING_SYNC = 0x7C0004AC;   // sync (L=0)
constexpr uint32_t ENCODING_LWSYNC = 0x7C2004AC; // lwsync (L=1)
constexpr uint32_t ENCODING_EIEIO = 0x7C0006AC;  // eieio
constexpr uint32_t ENCODING_ISYNC = 0x4C00012C;  // isync
// Valid-per-ISA atomic forms outside the Phase 5 subset (terminal limits).
constexpr uint16_t XO_LBARX = 52;
constexpr uint16_t XO_LHARX = 116;
constexpr uint16_t XO_STBCX = 694;
constexpr uint16_t XO_STHCX = 726;
constexpr uint16_t XO_LQARX = 276;
constexpr uint16_t XO_STQCX = 182;

// Phase 3 X-form indexed integer load/store extended opcodes (opcode 31).
// Only the non-update forms are in scope; update-indexed XO values
// (lbzux=119, lhzux=311, lhaux=375, lwzux=55, lwaux=373, ldux=53,
//  stbux=247, sthux=439, stwux=183, stdux=181) decode as unsupported.
constexpr uint16_t XO_LBZX = 87;
constexpr uint16_t XO_LHZX = 279;
constexpr uint16_t XO_LHAX = 343;
constexpr uint16_t XO_LWZX = 23;
constexpr uint16_t XO_LWAX = 341;
constexpr uint16_t XO_LDX = 21;
constexpr uint16_t XO_STBX = 215;
constexpr uint16_t XO_STHX = 407;
constexpr uint16_t XO_STWX = 151;
constexpr uint16_t XO_STDX = 149;

// Phase 3 DS-form 2-bit XO field (word bits 0-1) for opcodes 58/62.
constexpr uint8_t DS_XO_LOAD = 0;    // ld (opcode 58)
constexpr uint8_t DS_XO_LOAD_U = 1;  // ldu (opcode 58)
constexpr uint8_t DS_XO_LWA = 2;     // lwa (opcode 58)
constexpr uint8_t DS_XO_STORE = 0;   // std (opcode 62)
constexpr uint8_t DS_XO_STORE_U = 1;  // stdu (opcode 62)
// DS XO value 3 is reserved on both opcodes.

// Bounded SPR set for mfspr/mtspr (user-accessible fixed-point SPRs).
constexpr uint16_t SPR_XER = 1;
constexpr uint16_t SPR_LR = 8;
constexpr uint16_t SPR_CTR = 9;
constexpr uint16_t SPR_TBL = 268;
constexpr uint16_t SPR_TBU = 269;

// XER bit positions within the stored u32. Power ISA numbers the 64-bit XER
// with SO=XER[32], OV=XER[33], CA=XER[34]; the low 32 bits (XER[32:63]) are
// what this plugin stores, so SO is the MSB of the u32.
constexpr uint32_t XER_SO = 0x80000000U;
constexpr uint32_t XER_OV = 0x40000000U;
constexpr uint32_t XER_CA = 0x20000000U;

// CR field helpers use Power bit numbering: CR bit BI (0-31, MSB-first),
// CR field BF (0-7) holding {LT,GT,EQ,SO} from MSB to LSB.

// Conventional nop encoding: ori r0,r0,0
constexpr uint32_t ENCODING_NOP = 0x60000000;

// Supported system-call trap encoding: sc with LEV=0 and all reserved
// SC-form bits zero (Power ISA Book I §4.6.14 / Book III). Any other
// word with primary opcode 17 is rejected as an invalid encoding.
constexpr uint32_t ENCODING_SC = 0x44000002;
// SC-form LEV=1 is the PAPR hypercall instruction; it remains separate from sc 0.
constexpr uint32_t ENCODING_SC_HCALL = 0x44000022;

// Phase 3 per-CPU architectural fault/exception record.
//
// A host IMemory Read/Write failure is recorded as-is (with the host
// exception text preserved in `reason`); it is NOT automatically proof
// of a guest Data/Instruction Storage Interrupt. The `category` values
// name the emulator-observed condition and leave room for later Book III
// DSI/ISI/Alignment/Program-interrupt delivery, which is out of scope.
//
// Precision rules: the faulting NIP is preserved (never advanced past
// the faulting instruction), no partial destination/base register update
// is committed, and each CPU instance keeps its own record. reset()
// clears the record. The record is sticky: a later successful execution
// retains it for diagnostics until clearFault() or reset() is called.
// The record is host-side diagnostics only and is never serialized into
// the 304-byte Phase 1 checkpoint format.
enum class PPC64AccessType : uint8_t {
    NONE,   // No memory access attempted (trap / encoding error)
    FETCH,  // Instruction-fetch access at NIP
    READ,   // Data load access at the effective address
    WRITE,  // Data store access at the effective address
};

enum class PPC64FaultCategory : uint8_t {
    NONE,                // No fault recorded
    INVALID_INSTRUCTION,  // Unknown/unsupported opcode or XO form
    INVALID_ENCODING,    // Known form with reserved/invalid field combination
    FETCH_FAILURE,       // Instruction-fetch memory access failed
    DATA_LOAD_FAILURE,   // Data-load memory access failed
    DATA_STORE_FAILURE,  // Data-store memory access failed
    ALIGNMENT,           // Fetch or data alignment violation
    SYSCALL,             // System-call trap (sc)
    PRIVILEGE_VIOLATION,  // Privileged instruction in problem state (Phase 4)
    UNSUPPORTED_STATE,    // Valid encoding but unsupported MSR/mode/config
                          // (Phase 4: HV/IR/DR/LE/IP set, non-identity
                          // translation, unimplemented-valid ISA, hook-denied
                          // host errors, rfid/mtmsr illegal mode, nested
                          // overflow, vector-fetch terminal). Terminal:
                          // never delivered as a guest interrupt.
};

struct PPC64Fault {
    bool valid = false;  // True once any fault/trap has been recorded
    uint64_t cia = 0;    // Address of the faulting/trapping instruction
    uint32_t raw = 0;    // Faulting instruction encoding, if fetched
    bool hasRaw = false;
    uint64_t ea = 0;  // Effective address, if a data access was attempted
    bool hasEA = false;
    PPC64AccessType access = PPC64AccessType::NONE;
    uint32_t width = 0;  // Access width in bytes (0 when access == NONE)
    PPC64FaultCategory category = PPC64FaultCategory::NONE;
    std::string reason;   // Human-readable diagnostic (host-side only)
    // True when retrying the same instruction after fixing the environment
    // (e.g. mapping memory) could succeed. False for encodings that can
    // never execute (invalid opcode / reserved fields).
    bool resumable = false;
    PPC64TranslationFaultKind translationFault = PPC64TranslationFaultKind::NONE;
    PPC64TranslationStage translationStage = PPC64TranslationStage::NONE;
    uint64_t translationContextIdentifier = 0;
};

enum class DecodedKind {
    ADDI,
    ADDIS,
    ORI,
    ORIS,
    B,
    ADD,        // add[o][.]  (X-form, XO9 133; OE selects OV/SO update)
    SUBF,       // subf[o][.] (X-form, XO9 20; RT := RB - RA)
    ADDIC,      // addic      (D-form opcode 12; CA update, no CR0)
    ADDIC_DOT,  // addic.     (D-form opcode 13; CA + CR0 update)
    ADDC,       // addc[.]    (X-form, XO9 5; CA update; OE=1 rejected)
    ADDE,       // adde[.]    (X-form, XO9 69; CA in/out; OE=1 rejected)
    SUBFC,      // subfc[.]   (X-form, XO9 4; CA update; OE=1 rejected)
    AND,        // and[.]     (X-form, XO 28)
    OR,         // or[.]      (X-form, XO 444)
    XOR,        // xor[.]     (X-form, XO 316)
    ANDI_DOT,   // andi.      (D-form opcode 28; always records CR0)
    ANDIS_DOT,  // andis.     (D-form opcode 29; always records CR0)
    XORI,       // xori       (D-form opcode 26; never records)
    XORIS,      // xoris      (D-form opcode 27; never records)
    CMP,        // cmp        (X-form, XO 0; signed, L selects 32/64-bit)
    CMPL,       // cmpl       (X-form, XO 32; unsigned, L selects 32/64-bit)
    CMPI,       // cmpi       (D-form opcode 11; signed immediate)
    CMPLI,      // cmpli      (D-form opcode 10; unsigned immediate)
    BC,         // bc/bca/bcl/bcla (B-form, opcode 16)
    BCLR,       // bclr/bclrl (XL-form, opcode 19, XO 16; target LR & ~3)
    BCCTR,      // bcctr/bcctrl (XL-form, opcode 19, XO 528; target CTR & ~3)
    MFSPR,      // mfspr (XFX-form, XO 339; SPR in {1,8,9} + Phase 4 priv set)
    MTSPR,      // mtspr (XFX-form, XO 467; SPR in {1,8,9} + Phase 4 priv set)
    MFMSR,      // mfmsr (X-form, opcode 31 XO 83, privileged, Phase 4)
    MTMSR,      // mtmsr (X-form, opcode 31 XO 146, privileged, Phase 4)
    RFID,       // rfid (XL-form 0x4C000024, privileged, Phase 4)
    HRFID,      // hrfid (XL-form 0x4C000224, hypervisor privileged)
    LBZ,        // lbz   (D-form, opcode 34)
    LBZU,       // lbzu  (D-form, opcode 35; invalid if RA==0 or RA==RT)
    LHZ,        // lhz   (D-form, opcode 40)
    LHZU,       // lhzu  (D-form, opcode 41; invalid if RA==0 or RA==RT)
    LHA,        // lha   (D-form, opcode 42; sign-extending)
    LHAU,       // lhau  (D-form, opcode 43; invalid if RA==0 or RA==RT)
    LWZ,        // lwz   (D-form, opcode 32)
    LWZU,       // lwzu  (D-form, opcode 33; invalid if RA==0 or RA==RT)
    LWA,        // lwa   (DS-form, opcode 58 XO 2; word-algebraic, EA%4==0)
    LD,         // ld    (DS-form, opcode 58 XO 0)
    LDU,        // ldu   (DS-form, opcode 58 XO 1; invalid if RA==0 or RA==RT)
    STB,        // stb   (D-form, opcode 38)
    STBU,       // stbu  (D-form, opcode 39; invalid if RA==0)
    STH,        // sth   (D-form, opcode 44)
    STHU,       // sthu  (D-form, opcode 45; invalid if RA==0)
    STW,        // stw   (D-form, opcode 36)
    STWU,       // stwu  (D-form, opcode 37; invalid if RA==0)
    STD,        // std   (DS-form, opcode 62 XO 0)
    STDU,       // stdu  (DS-form, opcode 62 XO 1; invalid if RA==0)
    LBZX,       // lbzx  (X-form, opcode 31 XO 87)
    LHZX,       // lhzx  (X-form, opcode 31 XO 279)
    LHAX,       // lhax  (X-form, opcode 31 XO 343; sign-extending)
    LWZX,       // lwzx  (X-form, opcode 31 XO 23)
    LWAX,       // lwax  (X-form, opcode 31 XO 341; word-algebraic, EA%4==0)
    LDX,        // ldx   (X-form, opcode 31 XO 21)
    STBX,       // stbx  (X-form, opcode 31 XO 215)
    STHX,       // sthx  (X-form, opcode 31 XO 407)
    STWX,       // stwx  (X-form, opcode 31 XO 151)
    STDX,       // stdx  (X-form, opcode 31 XO 149)
    SC,         // sc 0 (system-call encoding 0x44000002)
    SC_HCALL,   // sc 1 (PAPR hypercall encoding 0x44000022)
    LWARX,      // lwarx (X-form, opcode 31 XO 20; establishes reservation)
    LDARX,      // ldarx (X-form, opcode 31 XO 84; establishes reservation)
    STWCX,      // stwcx. (X-form, opcode 31 XO 150, Rc=1 required)
    STDCX,      // stdcx. (X-form, opcode 31 XO 214, Rc=1 required)
    SYNC,       // sync / lwsync (opcode 31 XO 598; exact encodings only)
    ISYNC,      // isync (XL-form, opcode 19 XO 150; exact encoding only)
    EIEIO,      // eieio (X-form, opcode 31 XO 854; exact encoding only)
    UNSUPPORTED
};

struct DecodedInstruction {
    DecodedKind kind = DecodedKind::UNSUPPORTED;
    uint32_t raw = 0;
    uint8_t opcode = 0;
    uint8_t rt = 0;  // target / RS for OR-forms (bits 21-25)
    uint8_t ra = 0;  // source / target base (bits 16-20)
    uint8_t rb = 0;  // X-form second source (bits 11-15)
    int16_t simm = 0;   // D-form signed immediate
    uint16_t uimm = 0;  // D-form unsigned immediate
    uint32_t li = 0;    // I-form 24-bit LI field (bits 2-25 of offset)
    bool aa = false;    // I-form/B-form absolute-address bit
    bool lk = false;    // I-form/B-form/XL-form link bit
    bool supported = false;
    // Phase 2 fields:
    bool oe = false;     // X-form overflow-enable bit (arithmetic family)
    bool rc = false;     // X-form record bit (CR0 update)
    uint8_t bo = 0;      // B-form/XL-form branch options
    uint8_t bi = 0;      // B-form/XL-form CR bit selector
    uint16_t bd = 0;     // B-form 14-bit branch displacement field
    uint8_t bf = 0;      // Compare BF field (CR field selector 0-7)
    bool l = false;      // Compare L bit (false = 32-bit, true = 64-bit)
    uint16_t sprn = 0;   // XFX-form SPR number
    uint16_t ds = 0;     // DS-form 14-bit displacement field (bits 2-15)
    uint8_t dsXo = 0;    // DS-form 2-bit XO field (word bits 0-1)
};

/**
 * PPC64ISAState - PowerPC 64-bit architectural state (Phase 1 subset).
 *
 * 32x 64-bit GPRs (GPR0 is an ordinary writable register; the
 * "zero-base" behavior of addi/addis is an instruction encoding rule,
 * not a hardwired zero register), plus NIP, LR, CTR, CR, XER, MSR.
 *
 * MSR is stored with no Phase 1/2 side effects.
 *
 * Phase 2 architectural rules implemented here:
 * - CR holds 8 independent 4-bit fields {LT,GT,EQ,SO}, MSB-first.
 *   CR bit BI occupies u32 bit (31 - BI); field BF occupies u32 bits
 *   (28 - 4*BF + [0..3]). All access goes through crGetBit/crGetField/
 *   crSetField so bit numbering lives in exactly one place.
 * - XER stores SO=0x80000000, OV=0x40000000, CA=0x20000000 (the low
 *   32 bits of the architectural XER, i.e. XER[32:63]).
 * - Record-form fixed-point operations set CR0 from the wrapped result
 *   (LT=result<0, GT=result>0, EQ=result==0, SO=XER.SO after any OE
 *   update); non-record forms never touch CR.
 * - OE=1 arithmetic sets XER.OV from the signed-overflow calculation
 *   and accumulates XER.SO (sticky: SO := SO | OV).
 * - Carry producers (addc/adde/subfc/addic/addic.) set XER.CA from the
 *   unsigned carry out; plain add/subf never modify CA.
 */
class PPC64ISAState : public ISAState {
public:
    PPC64ISAState();
    ~PPC64ISAState() override = default;

    std::unique_ptr<ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override { return STATE_SIZE; }
    std::string toString() const override;
    void reset() override;

    // Phase 4 explicit v2 helpers (332-byte layout, version 2). serialize()
    // above remains v1 (304 bytes) for Phase 1-3 compatibility; these carry
    // the privileged SRR0/SRR1/DAR/DSISR extension.
    void serializeV2(uint8_t* buffer) const;
    void deserializeV2(const uint8_t* buffer);
    void serializeV3(uint8_t* buffer) const;
    void deserializeV3(const uint8_t* buffer);
    void serializeV4(uint8_t* buffer) const;
    void deserializeV4(const uint8_t* buffer);
    static constexpr size_t v2Size() { return STATE_SIZE_V2; }
    static constexpr size_t v3Size() { return STATE_SIZE_V3; }
    static constexpr size_t v4Size() { return STATE_SIZE_V4; }

    std::array<uint64_t, NUM_GPR> gpr{};
    uint64_t nip = 0;  // Next-instruction pointer (program counter)
    uint64_t lr = 0;   // Link Register
    uint64_t ctr = 0;  // Count Register
    uint32_t cr = 0;   // Condition Register
    uint32_t xer = 0;  // Fixed-Point Exception Register
    uint64_t msr = 0;  // Machine State Register (Phase 4 subset semantics)
    // Phase 4 guest-visible privileged CPU state (Book III-S):
    uint64_t srr0 = 0;   // Save/Restore Register 0 (SPR 26)
    uint64_t srr1 = 0;   // Save/Restore Register 1 (SPR 27)
    uint64_t dar = 0;    // Data Address Register (SPR 19)
    uint32_t dsisr = 0;  // Data Storage Interrupt Status Register (SPR 18)
    uint32_t pidr = 0;   // Process Identifier Register (SPR 48)
    uint64_t hsrr0 = 0;  // Hypervisor Save/Restore Register 0 (SPR 314)
    uint64_t hsrr1 = 0;  // Hypervisor Save/Restore Register 1 (SPR 315)
    uint64_t hdar = 0;   // Hypervisor Data Address Register (SPR 307)
    uint32_t hdsisr = 0; // Hypervisor DSI Status Register (SPR 306)
};

/**
 * PPC64ISAPlugin - big-endian PPC64 interpreter behind the IISA contract.
 *
 * Construction requires no factory data: the Phase 1 factory ignores its
 * void* argument (unlike the IA-64 factory, which requires
 * IA64FactoryData). Fetch is aligned 32-bit big-endian through IMemory.
 */
class PPC64ISAPlugin : public IISA {
public:
    PPC64ISAPlugin();
    ~PPC64ISAPlugin() override = default;

    // A plugin owns its reservation record. Copying would alias one reservation
    // between two CPUs, which the reservation model forbids.
    PPC64ISAPlugin(const PPC64ISAPlugin&) = delete;
    PPC64ISAPlugin& operator=(const PPC64ISAPlugin&) = delete;

    // IISA identity
    std::string getName() const override { return "PPC64"; }
    std::string getVersion() const override { return "8.0-phase8-partial"; }
    ISAArchitecture getArchitecture() const override { return ISAArchitecture::PPC64; }
    ISAExecutionUnit getExecutionUnit() const override { return ISAExecutionUnit::INSTRUCTION; }
    bool readGeneralRegister(size_t index, uint64_t& value) const override {
        value = readGPR(index);
        return true;
    }
    bool writeGeneralRegister(size_t index, uint64_t value) override {
        writeGPR(index, value);
        return true;
    }
    std::string getLastExecutionDiagnostic() const override {
        return lastFault_.valid ? lastFault_.reason : std::string();
    }
    void reset() override;

    // Decode / execute
    ISADecodeResult decode(IMemory& memory) override;
    size_t getInstructionLength(IMemory& memory, uint64_t address) override;
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& decodeResult) override;
    // Phase 4: step must invoke execute() even when decode() reports invalid
    // so that guest-delivery mode can enter vectors for fetch/decode faults.
    // Legacy mode still returns EXCEPTION (execute preserves the decode
    // fault), so Phase 1-3 behavior is unchanged.
    ISAExecutionResult step(IMemory& memory) override;

    // State management
    ISAState& getState() override { return state_; }
    const ISAState& getState() const override { return state_; }
    void setState(const ISAState& state) override;
    uint64_t getPC() const override { return state_.nip; }
    void setPC(uint64_t pc) override;

    // Checkpointing
    std::vector<uint8_t> serialize_state() const override;
    bool deserialize_state(const std::vector<uint8_t>& data) override;

    // Debugging
    std::string dumpState() const override;
    std::string disassemble(IMemory& memory, uint64_t address) override;

    // Capabilities: 64-bit, big-endian interpreter.
    size_t getWordSize() const override { return 64; }
    bool isLittleEndian() const override { return false; }
    bool hasFeature(const std::string& feature) const override;

    // PPC64-specific accessors (Phase 1 architectural state).
    uint64_t readGPR(size_t index) const;
    void writeGPR(size_t index, uint64_t value);
    uint64_t getNIP() const { return state_.nip; }
    uint64_t getLR() const { return state_.lr; }
    uint64_t getCTR() const { return state_.ctr; }
    uint32_t getCR() const { return state_.cr; }
    uint32_t getXER() const { return state_.xer; }
    uint64_t getMSR() const { return state_.msr; }
    uint32_t getPIDR() const { return state_.pidr; }
    void setPIDR(uint32_t value) {
        if (state_.pidr != value) {
            state_.pidr = value;
            clearReservation();
            hasCachedInstruction_ = false;
        }
    }
    bool configureRadixTranslation(const PPC64RadixConfiguration& config,
                                   IMemory& memory, std::string& reason);
    // Host-owned physical RAM contract for POWER9 HDSI/HISI vectors and code.
    // Guest accesses to the backing range are denied even when a guest page
    // table aliases it. HV fetch/data accesses use this host-real mapping.
    bool configureHypervisorMemory(IMemory& memory, uint64_t physicalBase,
                                   uint64_t size, std::string& reason);
    void clearHypervisorMemoryConfiguration();
    bool hasHypervisorMemoryConfiguration() const {
        return hypervisorMemoryConfigured_;
    }
    uint64_t getHypervisorMemoryBase() const { return hypervisorMemoryBase_; }
    uint64_t getHypervisorMemorySize() const { return hypervisorMemorySize_; }
    void clearRadixTranslationConfiguration();
    bool hasRadixTranslationConfiguration() const { return radixConfigured_; }
    const PPC64RadixConfiguration& getRadixTranslationConfiguration() const {
        return radixConfiguration_;
    }
    PPC64TranslationResult translateEffectiveAddress(
        IMemory& memory, uint64_t effectiveAddress,
        PPC64TranslationAccess access) const;
    // Hypervisor-owned activation API; guest instructions cannot select LPIDs.
    bool activatePartitionContext(
        const std::shared_ptr<PPC64PartitionContext>& context,
        std::string& reason);
    void deactivatePartitionContext();
    bool hasActivePartitionContext() const {
        return partitionContext_ != nullptr && partitionContext_->isActive();
    }
    uint16_t getActiveLPID() const {
        return hasActivePartitionContext() ? partitionContext_->lpid() : 0;
    }
    // Phase 4 guest-visible privileged state accessors (direct state view;
    // production code paths use mfspr/mtspr; these exist for tests/setup).
    uint64_t getSRR0() const { return state_.srr0; }
    uint64_t getSRR1() const { return state_.srr1; }
    uint64_t getDAR() const { return state_.dar; }
    uint32_t getDSISR() const { return state_.dsisr; }
    uint64_t getHSRR0() const { return state_.hsrr0; }
    uint64_t getHSRR1() const { return state_.hsrr1; }
    uint64_t getHDAR() const { return state_.hdar; }
    uint32_t getHDSISR() const { return state_.hdsisr; }
    void attachVirtualTimeBase(PPC64VirtualTimeBase* timeBase) {
        virtualTimeBase_ = timeBase;
    }
    void setSRR0(uint64_t v) { state_.srr0 = v; }
    void setSRR1(uint64_t v) { state_.srr1 = v; }
    void setDAR(uint64_t v) { state_.dar = v; }
    void setDSISR(uint32_t v) { state_.dsisr = v; }
    void setHSRR0(uint64_t v) { state_.hsrr0 = v; }
    void setHSRR1(uint64_t v) { state_.hsrr1 = v; }
    void setHDAR(uint64_t v) { state_.hdar = v; }
    void setHDSISR(uint32_t v) { state_.hdsisr = v; }
    void setMSR(uint64_t v) {
        if ((state_.msr & MSR_RELOCATION) != (v & MSR_RELOCATION)) {
            clearReservation();
            hasCachedInstruction_ = false;
        }
        state_.msr = v;
    }

    // Phase 4 exception-delivery compatibility mode. Default false (legacy):
    // Phase 3 EXCEPTION/SYSCALL results with NIP held at the fault. When
    // true, supported guest faults enter Book III-S vectors and return
    // CONTINUE (see enterGuestInterrupt); host diagnostics stay in
    // getLastFault(). Unsupported emulator operations never masquerade as
    // guest interrupts in either mode.
    void setGuestDeliveryEnabled(bool enabled) {
        guestDeliveryEnabled_ = enabled;
        consecutiveGuestExceptions_ = 0;
    }
    bool isGuestDeliveryEnabled() const { return guestDeliveryEnabled_; }
    int getConsecutiveGuestExceptions() const { return consecutiveGuestExceptions_; }

    // Phase 4 explicit v2 checkpoint helpers (332 bytes, version 2).
    std::vector<uint8_t> serialize_state_v2() const;
    bool deserialize_state_v2(const std::vector<uint8_t>& data);
    std::vector<uint8_t> serialize_state_v3() const;
    bool deserialize_state_v3(const std::vector<uint8_t>& data);
    std::vector<uint8_t> serialize_state_v4() const;
    bool deserialize_state_v4(const std::vector<uint8_t>& data);

    // Phase 3 per-CPU fault/exception record. Independent per instance,
    // cleared by reset(), sticky across successful execution (see the
    // PPC64Fault contract above), never part of checkpoint serialization.
    const PPC64Fault& getLastFault() const { return lastFault_; }
    bool hasFault() const { return lastFault_.valid; }
    void clearFault() { lastFault_ = PPC64Fault(); }

    // Phase 5 reservation diagnostics. The reservation is per-CPU execution
    // state: it is never serialized and never a guest-visible SPR.
    // hasReservation() is true only while the record is valid, its backend is
    // alive, and no invalidating event has occurred since establishment.
    bool hasReservation() const;
    uint64_t getReservationAddress() const;
    uint32_t getReservationWidth() const;

    // Pure decoder helpers (no state); also used by disassemble().
    static DecodedInstruction decodeWord(uint32_t word);
    static std::string disassembleDecoded(const DecodedInstruction& instr, uint64_t address);

    // Central CR/XER helpers (single definition of CR bit numbering).
    static bool crGetBit(uint32_t cr, uint8_t bi);
    static uint8_t crGetField(uint32_t cr, uint8_t bf);
    static uint32_t crSetField(uint32_t cr, uint8_t bf, uint8_t value);
    static void updateCR0(PPC64ISAState& state, uint64_t result);
    static void setXERCA(PPC64ISAState& state, bool carry);
    static void setXEROverflow(PPC64ISAState& state, bool overflow);

private:
    // Single-instruction execution. execute() wraps it and drops the
    // reservation on every non-CONTINUE outcome (exception/interrupt entry).
    ISAExecutionResult executeInstruction(IMemory& memory, const ISADecodeResult& decodeResult);
    ISAExecutionResult executeDecoded(IMemory& memory, const DecodedInstruction& instr);
    void evaluateBranch(const DecodedInstruction& instr, bool& tookBranch);
    // Phase 5 atomic reservation helpers (defined in PPC64ISAPlugin.cpp).
    ISAExecutionResult executeReservation(IMemory& memory, const DecodedInstruction& instr,
                                          uint64_t cia);
    // Guest data-storage fault for reservation accesses: DSI for protection or
    // unmapped storage (DAR=EA, DSISR per Phase 4 rules); terminal otherwise.
    ISAExecutionResult reservationStorageFault(IMemory& memory, const DecodedInstruction& instr,
                                               uint64_t cia, uint64_t ea, uint32_t width,
                                               bool isStore, const std::string& reason,
                                               const PPC64TranslationResult* translation = nullptr);
    // Replace this CPU's reservation with one established on `hub`.
    void establishReservation(const std::shared_ptr<MemoryReservationHub>& hub, uint64_t ea,
                              uint32_t width);
    // True if this CPU's reservation is still held on `hub` for exactly
    // [ea, ea+width).
    bool reservationHeld(const std::shared_ptr<MemoryReservationHub>& hub, uint64_t ea,
                         uint32_t width) const;
    // Drop this CPU's reservation (record stays allocated for reuse).
    void clearReservation();
    // CR0 for stwcx./stdcx.: LT=GT=0, EQ=success, SO=XER.SO; other fields kept.
    void setCR0Conditional(bool success);
    // Phase 3 helpers (defined in PPC64ISAPlugin.cpp).
    void recordFault(PPC64FaultCategory category, uint64_t cia, uint32_t raw, bool hasRaw,
                     uint64_t ea, bool hasEA, PPC64AccessType access, uint32_t width,
                     const std::string& reason, bool resumable,
                     PPC64TranslationFaultKind translationFault = PPC64TranslationFaultKind::NONE,
                     uint64_t translationContextIdentifier = 0,
                     PPC64TranslationStage translationStage = PPC64TranslationStage::NONE);
    // Sign-extend the DS-form 14-bit field (ds || 0b00) to 64 bits.
    static int64_t signExtendDS(uint16_t dsField);
    // Wrapping effective-address generation (mod 2^64, no host UB).
    uint64_t eaDForm(uint8_t ra, int16_t d) const;
    uint64_t eaDSForm(uint8_t ra, uint16_t dsField) const;
    uint64_t eaXForm(uint8_t ra, uint8_t rb) const;
    // Bounded big-endian memory transactions. Both validate the full
    // [ea, ea+width) range for wrap-around up front, then perform a single
    // IMemory call so a failure throws before any architectural register is
    // touched. Returns false with a diagnostic in `reason` on any host
    // exception (bounds, permission/PageFault, hook denial, MMIO failure).
    static bool memLoadBE(IMemory& memory, uint64_t ea, uint32_t width, uint64_t& out,
                          std::string& reason);
    static bool memStoreBE(IMemory& memory, uint64_t ea, uint32_t width, uint64_t value,
                           std::string& reason);
    // Execute one decoded integer load/store with precise fault semantics.
    ISAExecutionResult executeLoadStore(IMemory& memory, const DecodedInstruction& instr,
                                       uint64_t cia);

    // Phase 4 helpers.
    // Privilege: true when running in problem state (MSR[PR]=1).
    bool inProblemState() const { return (state_.msr & MSR_PR) != 0; }
    // True when the SPR number names a privileged register (SRR0/SRR1/DAR/
    // DSISR and any other SPR outside {XER,LR,CTR}).
    static bool isPrivilegedSPR(uint16_t sprn);
    static bool isHypervisorOnlySPR(uint16_t sprn);
    // HV execution is limited to the explicitly configured host-real handler
    // region and does not use guest relocation. IP and LE remain unsupported.
    bool msrHasUnsupportedMode() const {
        return (state_.msr & (MSR_IP | MSR_LE)) != 0 ||
               ((state_.msr & MSR_HV) != 0 &&
                (!hypervisorMemoryConfigured_ ||
                 (state_.msr & (MSR_PR | MSR_RELOCATION)) != 0));
    }
    // Validate an mtmsr/rfid MSR value without allowing the instruction to
    // change the current HV authority. IR/DR require a valid guest radix context.
    bool isProposedMSRValid(uint64_t proposed, IMemory& memory,
                            std::string& reason) const;
    bool isHypervisorReturnMSRValid(uint64_t proposed, IMemory& memory,
                                    std::string& reason) const;
    bool inHypervisorState() const { return (state_.msr & MSR_HV) != 0; }
    bool hypervisorMemoryContains(uint64_t address, uint64_t size) const;
    bool guestBackingRangeProtected(uint64_t address, uint64_t size) const;
    bool guestAccessToProtectedMemory(IMemory& memory, uint64_t address,
                                      uint32_t size) const;
    bool hypervisorRealRead(IMemory& memory, uint64_t address, uint32_t size,
                            uint8_t* out, std::string& reason) const;
    bool hypervisorRealWrite(IMemory& memory, uint64_t address, uint32_t size,
                             const uint8_t* bytes, std::string& reason) const;
    bool restoreArchitecturalState(const PPC64ISAState& restored);
    ISAExecutionResult deliverHypervisorStorageInterrupt(
        uint64_t vector, uint64_t savedNIP, uint64_t effectiveAddress,
        uint32_t cause,
        bool updateHdarHdsisr);
    bool translationModeReady(IMemory& memory, uint64_t proposedMSR,
                              std::string& reason) const;
    // True for valid-per-ISA but explicitly unimplemented encodings that must
    // stay terminal emulator EXCEPTIONs (never guest Program interrupts):
    // update-indexed load/stores, OE=1 on addc/adde/subfc (addco/addeo/
    // subfco), mtmsrd (XO 178). Determined from the decoded word/kind.
    static bool isUnimplementedValid(const DecodedInstruction& instr);
    // Per-page MMU permission validation for the full [ea,ea+width) range.
    // Returns true when every touched page passes. On false, `reason`
    // carries a host-style diagnostic naming the failing page and
    // `failIsUnmapped`/`failIsPermission` classify it for DSI/ISI cause
    // bits (bounds/wrap count as unmapped-like; hook/host errors count as
    // neither, i.e. terminal, not guest-deliverable).
    static bool validateDataRange(IMemory& memory, uint64_t ea, uint32_t width,
                                  bool isStore, std::string& reason,
                                  bool& failIsUnmapped, bool& failIsPermission);
    static bool validateFetchRange(IMemory& memory, uint64_t addr,
                                   std::string& reason, bool& failIsUnmapped,
                                   bool& failIsPermission);
    struct TranslatedMemorySegment {
        uint64_t effectiveAddress = 0;
        uint64_t guestRealAddress = 0;
        uint64_t hostBackingAddress = 0;
        uint32_t size = 0;
    };
    bool translateAccessRange(IMemory& memory, uint64_t effectiveAddress,
                              uint32_t width, PPC64TranslationAccess access,
                              std::vector<TranslatedMemorySegment>& segments,
                              PPC64TranslationResult& failure,
                              std::string& reason) const;
    bool translatedMemLoadBE(IMemory& memory, uint64_t effectiveAddress,
                             uint32_t width, uint64_t& value,
                             PPC64TranslationResult& failure,
                             std::string& reason) const;
    bool translatedMemStoreBE(IMemory& memory, uint64_t effectiveAddress,
                              uint32_t width, uint64_t value,
                              PPC64TranslationResult& failure,
                              std::string& reason) const;
    bool flatGuestRealAccessRange(
        IMemory& memory, uint64_t guestRealAddress, uint32_t width,
        PPC64TranslationAccess access,
        std::vector<TranslatedMemorySegment>& segments,
        PPC64TranslationResult& failure, std::string& reason) const;
    bool flatGuestRealMemLoadBE(IMemory& memory, uint64_t guestRealAddress,
                                uint32_t width, uint64_t& value,
                                PPC64TranslationResult& failure,
                                std::string& reason) const;
    bool flatGuestRealMemStoreBE(IMemory& memory, uint64_t guestRealAddress,
                                 uint32_t width, uint64_t value,
                                 PPC64TranslationResult& failure,
                                 std::string& reason) const;
    bool partitionGuestRealAccessRange(
        IMemory& memory, uint64_t guestRealAddress, uint32_t width,
        PPC64TranslationAccess access,
        std::vector<TranslatedMemorySegment>& segments,
        PPC64TranslationResult& failure, std::string& reason) const;
    bool partitionGuestRealMemLoadBE(IMemory& memory, uint64_t guestRealAddress,
                                     uint32_t width, uint64_t& value,
                                     PPC64TranslationResult& failure,
                                     std::string& reason) const;
    bool partitionGuestRealMemStoreBE(IMemory& memory, uint64_t guestRealAddress,
                                      uint32_t width, uint64_t value,
                                      PPC64TranslationResult& failure,
                                      std::string& reason) const;
    // Central guest-delivery dispatcher. Assumes the host fault was already
    // recorded via recordFault (for getLastFault diagnostics). When guest
    // delivery is disabled or the fault is terminal (unsupported-state,
    // unimplemented-valid, host-internal), returns the legacy result
    // (EXCEPTION, or SYSCALL for sc legacy). Otherwise updates SRR0/SRR1/
    // DAR/DSISR/MSR/NIP per the vector rules, bumps the nesting counter,
    // enforces MAX_NESTED_EXCEPTIONS, and returns CONTINUE. Vector-fetch
    // failures are detected on the next decode; unbounded recursion ends in
    // a terminal UNSUPPORTED_STATE EXCEPTION, never a silent loop.
    ISAExecutionResult maybeDeliver(IMemory& memory, ISAExecutionResult legacy,
                                    uint64_t vecOffset, uint64_t srr0,
                                    uint32_t srr1cause, bool updateDAR,
                                    uint64_t dar, uint32_t dsisr);
    void markGuestProgress() { consecutiveGuestExceptions_ = 0; }
    void countGuestDelivery() { ++consecutiveGuestExceptions_; }

    PPC64ISAState state_;
    DecodedInstruction cachedInstruction_;
    bool hasCachedInstruction_ = false;
    uint64_t cachedAddress_ = 0;
    PPC64Fault lastFault_;
    // Phase 5 per-CPU reservation record (execution state, not serialized).
    std::shared_ptr<ReservationRecord> reservation_;
    // Phase 4 delivery mode + bounded nesting guard (per-CPU).
    bool guestDeliveryEnabled_ = false;
    int consecutiveGuestExceptions_ = 0;
    bool justDelivered_ = false;
    bool hypervisorMemoryConfigured_ = false;
    uint64_t hypervisorMemoryBase_ = 0;
    uint64_t hypervisorMemorySize_ = 0;
    bool hypervisorReturnValid_ = false;
    bool hypervisorReturnHV_ = false;
    int repeatedHypervisorStorageFaults_ = 0;
    uint64_t lastHypervisorFaultNIP_ = 0;
    uint64_t lastHypervisorFaultEA_ = 0;
    uint32_t lastHypervisorFaultCause_ = 0;
    PPC64RadixConfiguration radixConfiguration_;
    bool radixConfigured_ = false;
    uint32_t radixConfigurationGeneration_ = 0;
    std::shared_ptr<PPC64PartitionContext> partitionContext_;
    PPC64VirtualTimeBase* virtualTimeBase_ = nullptr; // owned by the containing VM
};

/**
 * Factory function for creating PPC64 ISA instances.
 * The registry factory wrapper ignores its void* argument so no
 * IA-64 factory data is ever required on this path.
 */
std::unique_ptr<IISA> createPPC64ISA();

}  // namespace ppc64
}  // namespace ia64
