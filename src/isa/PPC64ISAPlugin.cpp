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

// Sign-extend the 14-bit B-form displacement (BD || 0b00) to 64 bits.
int64_t signExtendBD(uint16_t bdField) {
    int32_t value = static_cast<int32_t>(bdField & 0x3FFFU);
    if ((value & 0x2000) != 0) {
        value |= ~0x3FFF;
    }
    return static_cast<int64_t>(value << 2);
}

// Signed overflow of a + b == result, computed on unsigned patterns so no
// host signed-overflow UB is possible.
bool addOverflows(uint64_t a, uint64_t b, uint64_t result) {
    return ((((a ^ result) & (b ^ result)) >> 63) & 1U) != 0;
}

// Signed overflow of b - a == result.
bool subOverflows(uint64_t a, uint64_t b, uint64_t result) {
    return ((((b ^ a) & (b ^ result)) >> 63) & 1U) != 0;
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
    out << "PPC64 State (BE, interpreter, phase 2):\n";
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

bool PPC64ISAPlugin::crGetBit(uint32_t cr, uint8_t bi) {
    if (bi > 31) {
        return false;
    }
    return ((cr >> (31 - bi)) & 1U) != 0;
}

uint8_t PPC64ISAPlugin::crGetField(uint32_t cr, uint8_t bf) {
    if (bf > 7) {
        return 0;
    }
    return static_cast<uint8_t>((cr >> (28 - 4 * bf)) & 0xFU);
}

uint32_t PPC64ISAPlugin::crSetField(uint32_t cr, uint8_t bf, uint8_t value) {
    if (bf > 7) {
        return cr;
    }
    const uint32_t shift = static_cast<uint32_t>(28 - 4 * bf);
    cr &= ~(0xFU << shift);
    cr |= (static_cast<uint32_t>(value & 0xFU) << shift);
    return cr;
}

void PPC64ISAPlugin::updateCR0(PPC64ISAState& state, uint64_t result) {
    // CR0 reflects the wrapped 64-bit result even when signed overflow
    // occurred; SO copies the (possibly just-updated) XER.SO.
    uint8_t field = 0;
    if (static_cast<int64_t>(result) < 0) {
        field |= 0x8U;  // LT
    } else if (static_cast<int64_t>(result) > 0) {
        field |= 0x4U;  // GT
    } else {
        field |= 0x2U;  // EQ
    }
    if ((state.xer & XER_SO) != 0) {
        field |= 0x1U;  // SO
    }
    state.cr = crSetField(state.cr, 0, field);
}

void PPC64ISAPlugin::setXERCA(PPC64ISAState& state, bool carry) {
    if (carry) {
        state.xer |= XER_CA;
    } else {
        state.xer &= ~XER_CA;
    }
}

void PPC64ISAPlugin::setXEROverflow(PPC64ISAState& state, bool overflow) {
    if (overflow) {
        state.xer |= XER_OV;
        state.xer |= XER_SO;  // SO is sticky: SO := SO | OV
    } else {
        state.xer &= ~XER_OV;
    }
}

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
        case OPCODE_BC:
            instr.kind = DecodedKind::BC;
            instr.bo = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.bi = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.bd = static_cast<uint16_t>((word >> 2) & 0x3FFFU);
            instr.aa = ((word >> 1) & 0x1U) != 0;
            instr.lk = (word & 0x1U) != 0;
            instr.supported = true;
            break;
        case OPCODE_XL: {
            const uint16_t xo = static_cast<uint16_t>((word >> 1) & 0x3FFU);
            instr.bo = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.bi = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.lk = (word & 0x1U) != 0;
            if (xo == XO_BCLR) {
                instr.kind = DecodedKind::BCLR;
                instr.supported = true;
            } else if (xo == XO_BCCTR) {
                // The architecturally invalid "decrement and test CTR"
                // form (BO[2]==0, i.e. middle BO bit clear) is rejected
                // here so execute() can report it deterministically.
                // BH hint bits (16-20) are ignored, as on hardware.
                instr.kind = DecodedKind::BCCTR;
                instr.supported = ((instr.bo & 0x04U) != 0);
            } else {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
        case OPCODE_CMPLI:
            // Reserved ISA bit 9 (word bit 22) must be zero; L is word bit 21.
            if (((word >> 22) & 0x1U) != 0) {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
                break;
            }
            instr.kind = DecodedKind::CMPLI;
            instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
            instr.l = ((word >> 21) & 0x1U) != 0;
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_CMPI:
            if (((word >> 22) & 0x1U) != 0) {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
                break;
            }
            instr.kind = DecodedKind::CMPI;
            instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
            instr.l = ((word >> 21) & 0x1U) != 0;
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIC:
            instr.kind = DecodedKind::ADDIC;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIC_DOT:
            instr.kind = DecodedKind::ADDIC_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_XORI:
            instr.kind = DecodedKind::XORI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_XORIS:
            instr.kind = DecodedKind::XORIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ANDI_DOT:
            instr.kind = DecodedKind::ANDI_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ANDIS_DOT:
            instr.kind = DecodedKind::ANDIS_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_XO: {
            const uint16_t combined = static_cast<uint16_t>((word >> 1) & 0x3FFU);
            const uint16_t base = static_cast<uint16_t>(combined & 0x1FFU);
            const bool oe = ((combined >> 9) & 0x1U) != 0;
            const bool rc = (word & 0x1U) != 0;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.rb = static_cast<uint8_t>((word >> 11) & 0x1F);
            instr.oe = oe;
            instr.rc = rc;
            // Arithmetic family (OE selects a valid overflow variant).
            if (base == XO_ADD) {
                instr.kind = DecodedKind::ADD;
                instr.supported = true;
            } else if (base == XO_SUBF) {
                instr.kind = DecodedKind::SUBF;
                instr.supported = true;
            } else if (base == XO_ADDC && !oe) {
                instr.kind = DecodedKind::ADDC;
                instr.supported = true;
            } else if (base == XO_ADDE && !oe) {
                instr.kind = DecodedKind::ADDE;
                instr.supported = true;
            } else if (base == XO_SUBFC && !oe) {
                instr.kind = DecodedKind::SUBFC;
                instr.supported = true;
            } else if (combined == XO_AND) {
                instr.kind = DecodedKind::AND;
                instr.supported = true;
            } else if (combined == XO_OR) {
                instr.kind = DecodedKind::OR;
                instr.supported = true;
            } else if (combined == XO_XOR) {
                instr.kind = DecodedKind::XOR;
                instr.supported = true;
            } else if (combined == XO_CMP && !rc) {
                // cmp: reserved ISA bit 9 (word bit 22) must be zero.
                if (((word >> 22) & 0x1U) != 0) {
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else {
                    instr.kind = DecodedKind::CMP;
                    instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
                    instr.l = ((word >> 21) & 0x1U) != 0;
                    instr.supported = true;
                }
            } else if (combined == XO_CMPL && !rc) {
                if (((word >> 22) & 0x1U) != 0) {
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else {
                    instr.kind = DecodedKind::CMPL;
                    instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
                    instr.l = ((word >> 21) & 0x1U) != 0;
                    instr.supported = true;
                }
            } else if (combined == XO_MFSPR || combined == XO_MTSPR) {
                // XFX-form: SPRN split across the RA/RB field positions.
                const uint16_t raPos = static_cast<uint16_t>((word >> 16) & 0x1FU);
                const uint16_t rbPos = static_cast<uint16_t>((word >> 11) & 0x1FU);
                instr.sprn = static_cast<uint16_t>((rbPos << 5) | raPos);
                if (rc) {
                    // No record bit exists on mfspr/mtspr; Rc=1 is invalid.
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else if (instr.sprn == SPR_XER || instr.sprn == SPR_LR ||
                           instr.sprn == SPR_CTR) {
                    instr.kind = (combined == XO_MFSPR) ? DecodedKind::MFSPR
                                                       : DecodedKind::MTSPR;
                    instr.supported = true;
                } else {
                    // Unsupported SPR numbers (including privileged ones)
                    // are rejected rather than manufacturing values.
                    instr.kind = (combined == XO_MFSPR) ? DecodedKind::MFSPR
                                                       : DecodedKind::MTSPR;
                    instr.supported = false;
                }
            } else {
                // Covers: OE=1 on addc/adde/subfc (valid-per-ISA but out of
                // Phase 2 scope), Rc=1 on cmp/cmpl, unknown XO values, and
                // OE-set non-arithmetic XO aliases. All report EXCEPTION.
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
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
        case DecodedKind::ADD:
            out << (instr.oe ? "addo" : "add");
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::SUBF:
            out << (instr.oe ? "subfo" : "subf");
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ADDIC:
            out << "addic r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDIC_DOT:
            out << "addic. r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDC:
            out << "addc";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ADDE:
            out << "adde";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::SUBFC:
            out << "subfc";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::AND:
            out << "and";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::OR:
            out << "or";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::XOR:
            out << "xor";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ANDI_DOT:
            out << "andi. r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::ANDIS_DOT:
            out << "andis. r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::XORI:
            out << "xori r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::XORIS:
            out << "xoris r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::CMP:
            out << (instr.l ? "cmpd " : "cmpw ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::CMPL:
            out << (instr.l ? "cmpld " : "cmplw ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::CMPI:
            out << (instr.l ? "cmpdi " : "cmpwi ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::CMPLI:
            out << (instr.l ? "cmpldi " : "cmplwi ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << "," << instr.uimm;
            break;
        case DecodedKind::BC: {
            int64_t offset = signExtendBD(instr.bd);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : address + static_cast<uint64_t>(offset);
            out << "bc " << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi)
                << ",0x" << std::hex << target;
            if (instr.aa) {
                out << " ; aa";
            }
            if (instr.lk) {
                out << " ; lk";
            }
            break;
        }
        case DecodedKind::BCLR:
            out << (instr.lk ? "bclrl " : "bclr ")
                << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi);
            break;
        case DecodedKind::BCCTR:
            out << (instr.lk ? "bcctrl " : "bcctr ")
                << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi);
            if (!instr.supported) {
                out << " ; invalid (decrement CTR)";
            }
            break;
        case DecodedKind::MFSPR:
            if (!instr.supported) {
                out << "mfspr r" << static_cast<int>(instr.rt)
                    << "," << instr.sprn << " ; unsupported SPR";
                break;
            }
            out << "mfspr r" << static_cast<int>(instr.rt)
                << "," << instr.sprn;
            break;
        case DecodedKind::MTSPR:
            if (!instr.supported) {
                out << "mtspr " << instr.sprn
                    << ",r" << static_cast<int>(instr.rt) << " ; unsupported SPR";
                break;
            }
            out << "mtspr " << instr.sprn
                << ",r" << static_cast<int>(instr.rt);
            break;
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

void PPC64ISAPlugin::evaluateBranch(const DecodedInstruction& instr, bool& tookBranch) {
    // Shared BO/BI evaluation (Power ISA Book I, MSB-first BO numbering):
    //   BO[0] (0x10): 1 => do not test CR.
    //   BO[1] (0x08): CR polarity (branch when CR[BI] == BO[1]).
    //   BO[2] (0x04): 1 => do not decrement/test CTR.
    //   BO[3] (0x02): CTR polarity (branch when (CTR==0) == BO[3]).
    // CTR is decremented first (wrapping), then the decremented value is
    // tested, so CTR updates happen even when the branch is not taken.
    const bool checkCR = ((instr.bo & 0x10U) == 0);
    const bool checkCTR = ((instr.bo & 0x04U) == 0);
    bool ctrOk = true;
    if (checkCTR) {
        state_.ctr -= 1;  // wraps mod 2^64, as on hardware
        const bool ctrIsZero = (state_.ctr == 0);
        const bool wantZero = ((instr.bo & 0x02U) != 0);
        ctrOk = (ctrIsZero == wantZero);
    }
    bool condOk = true;
    if (checkCR) {
        const bool crBit = crGetBit(state_.cr, instr.bi);
        const bool wantSet = ((instr.bo & 0x08U) != 0);
        condOk = (crBit == wantSet);
    }
    tookBranch = ctrOk && condOk;
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
        case DecodedKind::ADD: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = a + b;  // wraps mod 2^64
            state_.gpr[instr.rt] = result;
            // Plain add never touches XER.CA, even on unsigned carry.
            if (instr.oe) {
                setXEROverflow(state_, addOverflows(a, b, result));
            }
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::SUBF: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = b - a;  // RT := RB - RA, wraps mod 2^64
            state_.gpr[instr.rt] = result;
            if (instr.oe) {
                setXEROverflow(state_, subOverflows(a, b, result));
            }
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDIC:
        case DecodedKind::ADDIC_DOT: {
            // NOTE: addic uses plain (RA) — RA=0 reads GPR0, unlike
            // addi/addis where RA=0 selects the constant zero.
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = static_cast<uint64_t>(signExtend16(instr.simm));
            const uint64_t result = a + b;  // wraps mod 2^64
            state_.gpr[instr.rt] = result;
            setXERCA(state_, result < a);  // unsigned carry out
            if (instr.kind == DecodedKind::ADDIC_DOT) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDC: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = a + b;
            state_.gpr[instr.rt] = result;
            setXERCA(state_, result < a);
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDE: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t carryIn = ((state_.xer & XER_CA) != 0) ? 1U : 0U;
            const uint64_t partial = a + b;
            const uint64_t result = partial + carryIn;
            state_.gpr[instr.rt] = result;
            // Carry out of either half-addition propagates.
            setXERCA(state_, (partial < a) || (result < partial));
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::SUBFC: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = b - a;
            state_.gpr[instr.rt] = result;
            // CA is NOT borrow: CA=1 iff the subtraction needs no borrow.
            setXERCA(state_, b >= a);
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::AND:
        case DecodedKind::OR:
        case DecodedKind::XOR: {
            const uint64_t s = state_.gpr[instr.ra];
            const uint64_t t = state_.gpr[instr.rb];
            uint64_t result = 0;
            if (instr.kind == DecodedKind::AND) {
                result = s & t;
            } else if (instr.kind == DecodedKind::OR) {
                result = s | t;
            } else {
                result = s ^ t;
            }
            // X-form logicals: dest is the RT field, sources are RA/RB.
            state_.gpr[instr.rt] = result;
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ANDI_DOT:
        case DecodedKind::ANDIS_DOT: {
            // D-form logicals: RS in the RT field position, dest in RA.
            const uint64_t s = state_.gpr[instr.rt];
            uint64_t mask = static_cast<uint64_t>(instr.uimm);
            if (instr.kind == DecodedKind::ANDIS_DOT) {
                mask <<= 16;
            }
            const uint64_t result = s & mask;
            state_.gpr[instr.ra] = result;
            // andi./andis. always record into CR0.
            updateCR0(state_, result);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::XORI:
        case DecodedKind::XORIS: {
            const uint64_t s = state_.gpr[instr.rt];
            uint64_t mask = static_cast<uint64_t>(instr.uimm);
            if (instr.kind == DecodedKind::XORIS) {
                mask <<= 16;
            }
            state_.gpr[instr.ra] = s ^ mask;
            // xori/xoris never touch CR0.
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::CMP:
        case DecodedKind::CMPL: {
            const bool isSigned = (instr.kind == DecodedKind::CMP);
            bool lt = false, gt = false, eq = false;
            if (instr.l) {
                if (isSigned) {
                    const int64_t a = static_cast<int64_t>(state_.gpr[instr.ra]);
                    const int64_t bb = static_cast<int64_t>(state_.gpr[instr.rb]);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint64_t a = state_.gpr[instr.ra];
                    const uint64_t bb = state_.gpr[instr.rb];
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            } else {
                if (isSigned) {
                    const int32_t a = static_cast<int32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const int32_t bb = static_cast<int32_t>(state_.gpr[instr.rb] & 0xFFFFFFFFULL);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint32_t a = static_cast<uint32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const uint32_t bb = static_cast<uint32_t>(state_.gpr[instr.rb] & 0xFFFFFFFFULL);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            }
            uint8_t field = 0;
            if (lt) {
                field |= 0x8U;
            } else if (gt) {
                field |= 0x4U;
            } else if (eq) {
                field |= 0x2U;
            }
            if ((state_.xer & XER_SO) != 0) {
                field |= 0x1U;
            }
            state_.cr = crSetField(state_.cr, instr.bf, field);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::CMPI:
        case DecodedKind::CMPLI: {
            const bool isSigned = (instr.kind == DecodedKind::CMPI);
            bool lt = false, gt = false, eq = false;
            if (instr.l) {
                if (isSigned) {
                    const int64_t a = static_cast<int64_t>(state_.gpr[instr.ra]);
                    const int64_t bb = signExtend16(instr.simm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint64_t a = state_.gpr[instr.ra];
                    const uint64_t bb = static_cast<uint64_t>(instr.uimm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            } else {
                if (isSigned) {
                    const int32_t a = static_cast<int32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const int32_t bb = static_cast<int32_t>(signExtend16(instr.simm));
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint32_t a = static_cast<uint32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const uint32_t bb = static_cast<uint32_t>(instr.uimm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            }
            uint8_t field = 0;
            if (lt) {
                field |= 0x8U;
            } else if (gt) {
                field |= 0x4U;
            } else if (eq) {
                field |= 0x2U;
            }
            if ((state_.xer & XER_SO) != 0) {
                field |= 0x1U;
            }
            state_.cr = crSetField(state_.cr, instr.bf, field);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BC: {
            bool took = false;
            evaluateBranch(instr, took);
            const int64_t offset = signExtendBD(instr.bd);
            const uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                             : cia + static_cast<uint64_t>(offset);
            if (instr.lk) {
                // LK writes LR even when the branch falls through.
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BCLR: {
            // Capture the LR target before any LK update overwrites LR.
            const uint64_t target = state_.lr & ~0x3ULL;
            bool took = false;
            evaluateBranch(instr, took);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BCCTR: {
            // Decoder already rejected BO[2]==0 (decrement) forms.
            const uint64_t target = state_.ctr & ~0x3ULL;
            bool took = false;
            evaluateBranch(instr, took);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MFSPR: {
            uint64_t value = 0;
            if (instr.sprn == SPR_XER) {
                value = static_cast<uint64_t>(state_.xer);
            } else if (instr.sprn == SPR_LR) {
                value = state_.lr;
            } else if (instr.sprn == SPR_CTR) {
                value = state_.ctr;
            } else {
                return ISAExecutionResult::EXCEPTION;
            }
            state_.gpr[instr.rt] = value;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MTSPR: {
            // RT field position carries the source register for mtspr.
            const uint64_t value = state_.gpr[instr.rt];
            if (instr.sprn == SPR_XER) {
                state_.xer = static_cast<uint32_t>(value & 0xFFFFFFFFULL);
            } else if (instr.sprn == SPR_LR) {
                state_.lr = value;
            } else if (instr.sprn == SPR_CTR) {
                state_.ctr = value;
            } else {
                return ISAExecutionResult::EXCEPTION;
            }
            state_.nip = cia + INSN_WIDTH;
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
