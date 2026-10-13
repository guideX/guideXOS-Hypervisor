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
// PPC64 Phase 1 constants
// ============================================================================
//
// Target: PowerPC 64-bit, big-endian, interpreter, IBM pSeries / sPAPR.
// Fixed 32-bit instruction width. Single-CPU validation model.
//
// Phase 1 executes a small authentic integer subset only:
//   addi, addis, ori, oris, b (all AA/LK forms), nop (ori r0,r0,0).
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

// Primary opcodes (bits 26-31) covered by Phase 1.
constexpr uint8_t OPCODE_ADDI = 14;
constexpr uint8_t OPCODE_ADDIS = 15;
constexpr uint8_t OPCODE_ORI = 24;
constexpr uint8_t OPCODE_ORIS = 25;
constexpr uint8_t OPCODE_B = 18;

// Conventional nop encoding: ori r0,r0,0
constexpr uint32_t ENCODING_NOP = 0x60000000;

enum class DecodedKind {
    ADDI,
    ADDIS,
    ORI,
    ORIS,
    B,
    UNSUPPORTED
};

struct DecodedInstruction {
    DecodedKind kind = DecodedKind::UNSUPPORTED;
    uint32_t raw = 0;
    uint8_t opcode = 0;
    uint8_t rt = 0;  // target / RS for OR-forms (bits 21-25)
    uint8_t ra = 0;  // source / target base (bits 16-20)
    int16_t simm = 0;   // D-form signed immediate
    uint16_t uimm = 0;  // D-form unsigned immediate
    uint32_t li = 0;    // I-form 24-bit LI field (bits 2-25 of offset)
    bool aa = false;    // I-form absolute-address bit
    bool lk = false;    // I-form link bit
    bool supported = false;
};

/**
 * PPC64ISAState - PowerPC 64-bit architectural state (Phase 1 subset).
 *
 * 32x 64-bit GPRs (GPR0 is an ordinary writable register; the
 * "zero-base" behavior of addi/addis is an instruction encoding rule,
 * not a hardwired zero register), plus NIP, LR, CTR, CR, XER, MSR.
 *
 * MSR/CR/XER fields exist so checkpoint payloads have a stable home;
 * Phase 1 defines no MSR side effects and no CR/XER-updating arithmetic.
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
    std::string getVersion() const override { return "1.0-phase1"; }
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

private:
    ISAExecutionResult executeDecoded(IMemory& memory, const DecodedInstruction& instr);

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
