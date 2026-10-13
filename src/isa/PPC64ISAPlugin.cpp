#include "PPC64ISAPlugin.h"
#include "ISAPluginRegistry.h"

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace ia64 {
namespace ppc64 {

namespace {

// Explicit little-endian codec for the checkpoint payload so snapshots
// are byte-stable regardless of host endianness.
void putU32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void putU64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

uint32_t getU32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t getU64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

int64_t signExtend16(int16_t value) {
    return static_cast<int64_t>(value);
}

// Sign-extend the (SIMM || 0x0000) 32-bit pattern used by addis.
int64_t signExtendShifted16(int16_t simm) {
    int32_t shifted = static_cast<int32_t>(simm) << 16;
    return static_cast<int64_t>(shifted);
}

// Sign-extend the 26-bit branch offset (LI || 0b00) to 64 bits.
int64_t signExtend26(uint32_t liField) {
    uint32_t offset26 = (liField << 2) & 0x03FFFFFFU;
    if ((offset26 & 0x02000000U) != 0) {
        return static_cast<int64_t>(static_cast<uint64_t>(offset26) | 0xFFFFFFFFFC000000ULL);
    }
    return static_cast<int64_t>(offset26);
}

}  // namespace

// ============================================================================
// PPC64ISAState
// ============================================================================

PPC64ISAState::PPC64ISAState() {
    reset();
}

void PPC64ISAState::reset() {
    gpr.fill(0);
    nip = 0;
    lr = 0;
    ctr = 0;
    cr = 0;
    xer = 0;
    msr = 0;
}

std::unique_ptr<ISAState> PPC64ISAState::clone() const {
    auto copy = std::make_unique<PPC64ISAState>();
    copy->gpr = gpr;
    copy->nip = nip;
    copy->lr = lr;
    copy->ctr = ctr;
    copy->cr = cr;
    copy->xer = xer;
    copy->msr = msr;
    return copy;
}

void PPC64ISAState::serialize(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serialize: null buffer");
    }
    size_t offset = 0;
    putU32(buffer + offset, STATE_MAGIC);
    offset += 4;
    putU32(buffer + offset, STATE_VERSION);
    offset += 4;
    for (size_t i = 0; i < NUM_GPR; ++i) {
        putU64(buffer + offset, gpr[i]);
        offset += 8;
    }
    putU64(buffer + offset, nip);
    offset += 8;
    putU64(buffer + offset, lr);
    offset += 8;
    putU64(buffer + offset, ctr);
    offset += 8;
    putU32(buffer + offset, cr);
    offset += 4;
    putU32(buffer + offset, xer);
    offset += 4;
    putU64(buffer + offset, msr);
    offset += 8;
}

void PPC64ISAState::deserialize(const uint8_t* buffer) {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 deserialize: null buffer");
    }
    size_t offset = 0;
    uint32_t magic = getU32(buffer + offset);
    offset += 4;
    uint32_t version = getU32(buffer + offset);
    offset += 4;
    if (magic != STATE_MAGIC) {
        throw std::invalid_argument("PPC64 deserialize: bad magic");
    }
    if (version != STATE_VERSION) {
        throw std::invalid_argument("PPC64 deserialize: unsupported version");
    }
    for (size_t i = 0; i < NUM_GPR; ++i) {
        gpr[i] = getU64(buffer + offset);
        offset += 8;
    }
    nip = getU64(buffer + offset);
    offset += 8;
    lr = getU64(buffer + offset);
    offset += 8;
    ctr = getU64(buffer + offset);
    offset += 8;
    cr = getU32(buffer + offset);
    offset += 4;
    xer = getU32(buffer + offset);
    offset += 4;
    msr = getU64(buffer + offset);
    offset += 8;
}

std::string PPC64ISAState::toString() const {
    std::ostringstream out;
    out << "PPC64 State (BE, interpreter, phase 1):\n";
    out << "  NIP: 0x" << std::hex << nip << std::dec << "\n";
    out << "  LR:  0x" << std::hex << lr << std::dec << "\n";
    out << "  CTR: 0x" << std::hex << ctr << std::dec << "\n";
    out << "  CR:  0x" << std::hex << cr << std::dec << "\n";
    out << "  XER: 0x" << std::hex << xer << std::dec << "\n";
    out << "  MSR: 0x" << std::hex << msr << std::dec << "\n";
    for (size_t i = 0; i < NUM_GPR; ++i) {
        out << "  r" << i << ": 0x" << std::hex << gpr[i] << std::dec << "\n";
    }
    return out.str();
}

// ============================================================================
// PPC64ISAPlugin
// ============================================================================

PPC64ISAPlugin::PPC64ISAPlugin() = default;

void PPC64ISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
    cachedAddress_ = 0;
}

DecodedInstruction PPC64ISAPlugin::decodeWord(uint32_t word) {
    DecodedInstruction instr;
    instr.raw = word;
    instr.opcode = static_cast<uint8_t>((word >> 26) & 0x3F);

    switch (instr.opcode) {
        case OPCODE_ADDI:
            instr.kind = DecodedKind::ADDI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIS:
            instr.kind = DecodedKind::ADDIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ORI:
            instr.kind = DecodedKind::ORI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ORIS:
            instr.kind = DecodedKind::ORIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_B:
            instr.kind = DecodedKind::B;
            instr.li = (word >> 2) & 0x00FFFFFFU;
            instr.aa = ((word >> 1) & 0x1U) != 0;
            instr.lk = (word & 0x1U) != 0;
            instr.supported = true;
            break;
        default:
            instr.kind = DecodedKind::UNSUPPORTED;
            instr.supported = false;
            break;
    }
    return instr;
}

std::string PPC64ISAPlugin::disassembleDecoded(const DecodedInstruction& instr, uint64_t address) {
    std::ostringstream out;
    switch (instr.kind) {
        case DecodedKind::ADDI:
            out << "addi r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDIS:
            out << "addis r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ORI:
            if (instr.raw == ENCODING_NOP) {
                out << "nop";
            } else {
                out << "ori r" << static_cast<int>(instr.ra)
                    << ",r" << static_cast<int>(instr.rt)
                    << "," << instr.uimm;
            }
            break;
        case DecodedKind::ORIS:
            out << "oris r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::B: {
            int64_t offset = signExtend26(instr.li);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : address + static_cast<uint64_t>(offset);
            if (instr.aa && instr.lk) {
                out << "bla 0x";
            } else if (instr.aa) {
                out << "ba 0x";
            } else if (instr.lk) {
                out << "bl 0x";
            } else {
                out << "b 0x";
            }
            out << std::hex << target;
            break;
        }
        case DecodedKind::UNSUPPORTED:
            out << "<unsupported ppc64 opcode " << static_cast<int>(instr.opcode) << ">";
            break;
    }
    return out.str();
}

ISADecodeResult PPC64ISAPlugin::decode(IMemory& memory) {
    ISADecodeResult result;
    result.instructionAddress = state_.nip;
    result.instructionLength = 0;
    result.internalData = nullptr;
    result.valid = false;

    // Fixed 32-bit width: misaligned NIP is a deterministic decode failure.
    if ((state_.nip % INSN_WIDTH) != 0) {
        result.disassembly = "<misaligned fetch>";
        hasCachedInstruction_ = false;
        return result;
    }

    uint8_t bytes[INSN_WIDTH] = {0, 0, 0, 0};
    try {
        memory.Read(state_.nip, bytes, INSN_WIDTH);
    } catch (const std::exception& e) {
        result.disassembly = std::string("<fetch fault: ") + e.what() + ">";
        hasCachedInstruction_ = false;
        return result;
    }

    // Big-endian assembly: first byte in memory is the most significant.
    uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
                    (static_cast<uint32_t>(bytes[1]) << 16) |
                    (static_cast<uint32_t>(bytes[2]) << 8) |
                    static_cast<uint32_t>(bytes[3]);

    cachedInstruction_ = decodeWord(word);
    cachedAddress_ = state_.nip;
    hasCachedInstruction_ = true;

    result.valid = cachedInstruction_.supported;
    result.instructionAddress = state_.nip;
    result.instructionLength = INSN_WIDTH;
    result.disassembly = disassembleDecoded(cachedInstruction_, state_.nip);
    result.internalData = hasCachedInstruction_ ? &cachedInstruction_ : nullptr;
    if (!cachedInstruction_.supported) {
        result.internalData = nullptr;
    }
    return result;
}

size_t PPC64ISAPlugin::getInstructionLength(IMemory&, uint64_t) {
    // Fixed-width ISA: every instruction is 4 bytes.
    return INSN_WIDTH;
}

ISAExecutionResult PPC64ISAPlugin::execute(IMemory& memory, const ISADecodeResult& decodeResult) {
    (void)memory;
    if (!decodeResult.valid || !hasCachedInstruction_) {
        hasCachedInstruction_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    if (decodeResult.instructionAddress != cachedAddress_ ||
        decodeResult.instructionAddress != state_.nip) {
        hasCachedInstruction_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    DecodedInstruction instr = cachedInstruction_;
    hasCachedInstruction_ = false;
    if (!instr.supported) {
        return ISAExecutionResult::EXCEPTION;
    }
    return executeDecoded(memory, instr);
}

ISAExecutionResult PPC64ISAPlugin::executeDecoded(IMemory&, const DecodedInstruction& instr) {
    const uint64_t cia = state_.nip;
    switch (instr.kind) {
        case DecodedKind::ADDI: {
            // RA=0 selects the constant zero, not the contents of GPR0.
            uint64_t base = (instr.ra == 0) ? 0 : state_.gpr[instr.ra];
            int64_t exts = signExtend16(instr.simm);
            state_.gpr[instr.rt] = base + static_cast<uint64_t>(exts);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDIS: {
            uint64_t base = (instr.ra == 0) ? 0 : state_.gpr[instr.ra];
            int64_t exts = signExtendShifted16(instr.simm);
            state_.gpr[instr.rt] = base + static_cast<uint64_t>(exts);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ORI: {
            // S-field (instr.rt) is the source, A-field (instr.ra) the target.
            state_.gpr[instr.ra] = state_.gpr[instr.rt] | static_cast<uint64_t>(instr.uimm);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ORIS: {
            state_.gpr[instr.ra] =
                state_.gpr[instr.rt] | (static_cast<uint64_t>(instr.uimm) << 16);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::B: {
            int64_t offset = signExtend26(instr.li);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : cia + static_cast<uint64_t>(offset);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = target;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::UNSUPPORTED:
            break;
    }
    // Unsupported opcodes are never silent NOPs.
    return ISAExecutionResult::EXCEPTION;
}

void PPC64ISAPlugin::setState(const ISAState& state) {
    const PPC64ISAState* ppcState = dynamic_cast<const PPC64ISAState*>(&state);
    if (ppcState == nullptr) {
        throw std::invalid_argument("PPC64 setState: incompatible ISAState");
    }
    state_ = *ppcState;
    hasCachedInstruction_ = false;
}

void PPC64ISAPlugin::setPC(uint64_t pc) {
    state_.nip = pc;
    hasCachedInstruction_ = false;
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state() const {
    std::vector<uint8_t> data(STATE_SIZE);
    state_.serialize(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE) {
        return false;
    }
    try {
        state_.deserialize(data.data());
    } catch (...) {
        return false;
    }
    hasCachedInstruction_ = false;
    return true;
}

std::string PPC64ISAPlugin::dumpState() const {
    return state_.toString();
}

std::string PPC64ISAPlugin::disassemble(IMemory& memory, uint64_t address) {
    if ((address % INSN_WIDTH) != 0) {
        return "<misaligned fetch>";
    }
    uint8_t bytes[INSN_WIDTH] = {0, 0, 0, 0};
    try {
        memory.Read(address, bytes, INSN_WIDTH);
    } catch (const std::exception& e) {
        return std::string("<fetch fault: ") + e.what() + ">";
    }
    uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
                    (static_cast<uint32_t>(bytes[1]) << 16) |
                    (static_cast<uint32_t>(bytes[2]) << 8) |
                    static_cast<uint32_t>(bytes[3]);
    return disassembleDecoded(decodeWord(word), address);
}

bool PPC64ISAPlugin::hasFeature(const std::string& feature) const {
    if (feature == "interpreter") {
        return true;
    }
    if (feature == "big_endian") {
        return true;
    }
    if (feature == "fixed_width_32") {
        return true;
    }
    if (feature == "pseries") {
        return true;
    }
    if (feature == "spapr") {
        return true;
    }
    return false;
}

uint64_t PPC64ISAPlugin::readGPR(size_t index) const {
    if (index >= NUM_GPR) {
        throw std::out_of_range("PPC64 readGPR: register index out of range");
    }
    return state_.gpr[index];
}

void PPC64ISAPlugin::writeGPR(size_t index, uint64_t value) {
    if (index >= NUM_GPR) {
        throw std::out_of_range("PPC64 writeGPR: register index out of range");
    }
    state_.gpr[index] = value;
}

std::unique_ptr<IISA> createPPC64ISA() {
    return std::make_unique<PPC64ISAPlugin>();
}

namespace {

std::unique_ptr<IISA> ppc64Factory(void* data) {
    // Phase 1 needs no construction data; any pointer (including nullptr
    // and IA-64 factory blocks) is accepted and ignored so this path can
    // never demand IA-64 factory data.
    (void)data;
    return createPPC64ISA();
}

struct PPC64AutoRegistrar {
    PPC64AutoRegistrar() {
        ISAPluginRegistry::instance().registerISA("PPC64", ppc64Factory);
    }
};

PPC64AutoRegistrar ppc64AutoRegistrar;

}  // namespace

}  // namespace ppc64
}  // namespace ia64
