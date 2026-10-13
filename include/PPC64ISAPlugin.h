#pragma once

#include "IISA.h"
#include "IMemory.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ia64 {
namespace ppc64 {

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
// Everything else decodes as unsupported and executes as EXCEPTION.

constexpr size_t NUM_GPR = 32;
constexpr size_t INSN_WIDTH = 4;

// Serialization layout (explicit little-endian field encoding):
//   magic[4] = {'P','P','C','1'} | version u32 = 1
//   GPR[32] u64 | NIP u64 | LR u64 | CTR u64 | CR u32 | XER u32 | MSR u64
constexpr uint32_t STATE_MAGIC = 0x31435050;  // 'P','P','C','1' little-endian
constexpr uint32_t STATE_VERSION = 1;
constexpr size_t STATE_SIZE =
    4 + 4 + (NUM_GPR * 8) + 8 + 8 + 8 + 4 + 4 + 8;  // = 304 bytes

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

// Bounded SPR set for mfspr/mtspr (user-accessible fixed-point SPRs).
constexpr uint16_t SPR_XER = 1;
constexpr uint16_t SPR_LR = 8;
constexpr uint16_t SPR_CTR = 9;

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
    MFSPR,      // mfspr (XFX-form, XO 339; SPR in {1,8,9})
    MTSPR,      // mtspr (XFX-form, XO 467; SPR in {1,8,9})
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

    std::array<uint64_t, NUM_GPR> gpr{};
    uint64_t nip = 0;  // Next-instruction pointer (program counter)
    uint64_t lr = 0;   // Link Register
    uint64_t ctr = 0;  // Count Register
    uint32_t cr = 0;   // Condition Register
    uint32_t xer = 0;  // Fixed-Point Exception Register
    uint64_t msr = 0;  // Machine State Register (stored, no Phase 1 semantics)
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

    // IISA identity
    std::string getName() const override { return "PPC64"; }
    std::string getVersion() const override { return "2.0-phase2"; }
    void reset() override;

    // Decode / execute
    ISADecodeResult decode(IMemory& memory) override;
    size_t getInstructionLength(IMemory& memory, uint64_t address) override;
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& decodeResult) override;

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
    ISAExecutionResult executeDecoded(IMemory& memory, const DecodedInstruction& instr);
    void evaluateBranch(const DecodedInstruction& instr, bool& tookBranch);

    PPC64ISAState state_;
    DecodedInstruction cachedInstruction_;
    bool hasCachedInstruction_ = false;
    uint64_t cachedAddress_ = 0;
};

/**
 * Factory function for creating PPC64 ISA instances.
 * The registry factory wrapper ignores its void* argument so no
 * IA-64 factory data is ever required on this path.
 */
std::unique_ptr<IISA> createPPC64ISA();

}  // namespace ppc64
}  // namespace ia64
