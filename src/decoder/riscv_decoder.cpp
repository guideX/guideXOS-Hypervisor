#include "riscv_decoder.h"

#include <sstream>

namespace riscv {

namespace {

constexpr uint8_t kOpcodeLui = 0x37;
constexpr uint8_t kOpcodeAuipc = 0x17;
constexpr uint8_t kOpcodeJal = 0x6F;
constexpr uint8_t kOpcodeJalr = 0x67;
constexpr uint8_t kOpcodeBranch = 0x63;
constexpr uint8_t kOpcodeLoad = 0x03;
constexpr uint8_t kOpcodeStore = 0x23;
constexpr uint8_t kOpcodeOpImm = 0x13;
constexpr uint8_t kOpcodeOp = 0x33;
constexpr uint8_t kOpcodeOpImm32 = 0x1B;
constexpr uint8_t kOpcodeOp32 = 0x3B;
constexpr uint8_t kOpcodeAtomic = 0x2F;
constexpr uint8_t kOpcodeFence = 0x0F;
constexpr uint8_t kOpcodeSystem = 0x73;
constexpr uint8_t kFunct7M = 0x01;

bool IsShiftImmLegal(uint8_t funct7, uint8_t funct3) {
	if (funct3 == 0x1) {
		return funct7 == 0x00;
	}
	if (funct3 == 0x5) {
		return funct7 == 0x00 || funct7 == 0x20;
	}
	return false;
}

bool IsShiftImmWLegal(uint8_t funct7, uint8_t funct3) {
	if (funct3 == 0x1) {
		return funct7 == 0x00;
	}
	if (funct3 == 0x5) {
		return funct7 == 0x00 || funct7 == 0x20;
	}
	return false;
}

} // namespace

DecodedInstruction::DecodedInstruction()
	: rawWord(0)
	, mnemonic(Mnemonic::Illegal)
	, format(InstructionFormat::Unknown)
	, opcode(0)
	, rd(0)
	, rs1(0)
	, rs2(0)
	, funct3(0)
	, funct7(0)
	, immediate(0)
	, aq(false)
	, rl(false)
	, instructionLength(4)
	, extension("RV64I")
	, recognized(false)
	, illegal(true) {
}

int64_t Decoder::SignExtend(uint64_t value, unsigned bits) {
	const uint64_t signBit = 1ULL << (bits - 1);
	const uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
	if ((value & signBit) != 0) {
		return static_cast<int64_t>(value | ~mask);
	}
	return static_cast<int64_t>(value & mask);
}

int64_t Decoder::DecodeUImmediate(uint32_t rawWord) {
	return SignExtend(static_cast<uint64_t>(rawWord & 0xFFFFF000U), 32);
}

int64_t Decoder::DecodeJImmediate(uint32_t rawWord) {
	uint32_t imm = 0;
	imm |= ((rawWord >> 31) & 0x1U) << 20;
	imm |= ((rawWord >> 21) & 0x3FFU) << 1;
	imm |= ((rawWord >> 20) & 0x1U) << 11;
	imm |= ((rawWord >> 12) & 0xFFU) << 12;
	return SignExtend(imm, 21);
}

int64_t Decoder::DecodeBImmediate(uint32_t rawWord) {
	uint32_t imm = 0;
	imm |= ((rawWord >> 31) & 0x1U) << 12;
	imm |= ((rawWord >> 25) & 0x3FU) << 5;
	imm |= ((rawWord >> 8) & 0xFU) << 1;
	imm |= ((rawWord >> 7) & 0x1U) << 11;
	return SignExtend(imm, 13);
}

int64_t Decoder::DecodeSImmediate(uint32_t rawWord) {
	uint32_t imm = ((rawWord >> 25) & 0x7FU) << 5;
	imm |= ((rawWord >> 7) & 0x1FU);
	return SignExtend(imm, 12);
}

std::string Decoder::MnemonicToString(Mnemonic mnemonic) {
	switch (mnemonic) {
	case Mnemonic::LUI: return "lui";
	case Mnemonic::AUIPC: return "auipc";
	case Mnemonic::JAL: return "jal";
	case Mnemonic::JALR: return "jalr";
	case Mnemonic::BEQ: return "beq";
	case Mnemonic::BNE: return "bne";
	case Mnemonic::BLT: return "blt";
	case Mnemonic::BGE: return "bge";
	case Mnemonic::BLTU: return "bltu";
	case Mnemonic::BGEU: return "bgeu";
	case Mnemonic::LB: return "lb";
	case Mnemonic::LH: return "lh";
	case Mnemonic::LW: return "lw";
	case Mnemonic::LD: return "ld";
	case Mnemonic::LBU: return "lbu";
	case Mnemonic::LHU: return "lhu";
	case Mnemonic::LWU: return "lwu";
	case Mnemonic::SB: return "sb";
	case Mnemonic::SH: return "sh";
	case Mnemonic::SW: return "sw";
	case Mnemonic::SD: return "sd";
	case Mnemonic::ADDI: return "addi";
	case Mnemonic::SLTI: return "slti";
	case Mnemonic::SLTIU: return "sltiu";
	case Mnemonic::XORI: return "xori";
	case Mnemonic::ORI: return "ori";
	case Mnemonic::ANDI: return "andi";
	case Mnemonic::SLLI: return "slli";
	case Mnemonic::SRLI: return "srli";
	case Mnemonic::SRAI: return "srai";
	case Mnemonic::ADD: return "add";
	case Mnemonic::SUB: return "sub";
	case Mnemonic::SLL: return "sll";
	case Mnemonic::SLT: return "slt";
	case Mnemonic::SLTU: return "sltu";
	case Mnemonic::XOR: return "xor";
	case Mnemonic::SRL: return "srl";
	case Mnemonic::SRA: return "sra";
	case Mnemonic::OR: return "or";
	case Mnemonic::AND: return "and";
	case Mnemonic::ADDIW: return "addiw";
	case Mnemonic::SLLIW: return "slliw";
	case Mnemonic::SRLIW: return "srliw";
	case Mnemonic::SRAIW: return "sraiw";
	case Mnemonic::ADDW: return "addw";
	case Mnemonic::SUBW: return "subw";
	case Mnemonic::SLLW: return "sllw";
	case Mnemonic::SRLW: return "srlw";
	case Mnemonic::SRAW: return "sraw";
	case Mnemonic::MUL: return "mul";
	case Mnemonic::MULH: return "mulh";
	case Mnemonic::MULHSU: return "mulhsu";
	case Mnemonic::MULHU: return "mulhu";
	case Mnemonic::DIV: return "div";
	case Mnemonic::DIVU: return "divu";
	case Mnemonic::REM: return "rem";
	case Mnemonic::REMU: return "remu";
	case Mnemonic::MULW: return "mulw";
	case Mnemonic::DIVW: return "divw";
	case Mnemonic::DIVUW: return "divuw";
	case Mnemonic::REMW: return "remw";
	case Mnemonic::REMUW: return "remuw";
	case Mnemonic::LR_W: return "lr.w";
	case Mnemonic::SC_W: return "sc.w";
	case Mnemonic::AMOSWAP_W: return "amoswap.w";
	case Mnemonic::AMOADD_W: return "amoadd.w";
	case Mnemonic::AMOXOR_W: return "amoxor.w";
	case Mnemonic::AMOAND_W: return "amoand.w";
	case Mnemonic::AMOOR_W: return "amoor.w";
	case Mnemonic::AMOMIN_W: return "amomin.w";
	case Mnemonic::AMOMAX_W: return "amomax.w";
	case Mnemonic::AMOMINU_W: return "amominu.w";
	case Mnemonic::AMOMAXU_W: return "amomaxu.w";
	case Mnemonic::LR_D: return "lr.d";
	case Mnemonic::SC_D: return "sc.d";
	case Mnemonic::AMOSWAP_D: return "amoswap.d";
	case Mnemonic::AMOADD_D: return "amoadd.d";
	case Mnemonic::AMOXOR_D: return "amoxor.d";
	case Mnemonic::AMOAND_D: return "amoand.d";
	case Mnemonic::AMOOR_D: return "amoor.d";
	case Mnemonic::AMOMIN_D: return "amomin.d";
	case Mnemonic::AMOMAX_D: return "amomax.d";
	case Mnemonic::AMOMINU_D: return "amominu.d";
	case Mnemonic::AMOMAXU_D: return "amomaxu.d";
	case Mnemonic::CSRRW: return "csrrw";
	case Mnemonic::CSRRS: return "csrrs";
	case Mnemonic::CSRRC: return "csrrc";
	case Mnemonic::CSRRWI: return "csrrwi";
	case Mnemonic::CSRRSI: return "csrrsi";
	case Mnemonic::CSRRCI: return "csrrci";
	case Mnemonic::ECALL: return "ecall";
	case Mnemonic::EBREAK: return "ebreak";
	case Mnemonic::MRET: return "mret";
	case Mnemonic::SRET: return "sret";
	case Mnemonic::WFI: return "wfi";
	case Mnemonic::SFENCE_VMA: return "sfence.vma";
	case Mnemonic::FENCE: return "fence";
	case Mnemonic::FENCE_I: return "fence.i";
	case Mnemonic::Illegal:
	default:
		return "illegal";
	}
}

std::string Decoder::RegisterName(uint8_t reg) {
	std::ostringstream oss;
	oss << 'x' << static_cast<unsigned>(reg);
	return oss.str();
}

DecodedInstruction Decoder::Decode(uint32_t rawWord) const {
	DecodedInstruction insn;
	insn.rawWord = rawWord;
	insn.opcode = static_cast<uint8_t>(rawWord & 0x7FU);
	insn.rd = static_cast<uint8_t>((rawWord >> 7) & 0x1FU);
	insn.funct3 = static_cast<uint8_t>((rawWord >> 12) & 0x7U);
	insn.rs1 = static_cast<uint8_t>((rawWord >> 15) & 0x1FU);
	insn.rs2 = static_cast<uint8_t>((rawWord >> 20) & 0x1FU);
	insn.funct7 = static_cast<uint8_t>((rawWord >> 25) & 0x7FU);

	auto markLegal = [&insn]() {
		insn.recognized = true;
		insn.illegal = false;
	};

	auto markMExtension = [&insn]() {
		insn.extension = "RV64M";
	};

	auto markSystemExtension = [&insn]() {
		insn.extension = "SYSTEM";
	};

	auto markZicsrExtension = [&insn]() {
		insn.extension = "Zicsr";
	};

	auto markAExtension = [&insn]() {
		insn.extension = "RV64A";
	};

	auto decodeAtomic = [&](bool isDoubleword) {
		const uint8_t funct5 = static_cast<uint8_t>((rawWord >> 27) & 0x1FU);
		const uint8_t aq = static_cast<uint8_t>((rawWord >> 26) & 0x1U);
		const uint8_t rl = static_cast<uint8_t>((rawWord >> 25) & 0x1U);
		const bool widthOk = isDoubleword ? (insn.funct3 == 0x3U) : (insn.funct3 == 0x2U);
		const bool rs2Ok = (funct5 == 0x02U) ? (insn.rs2 == 0U) : true;

		insn.format = InstructionFormat::R;
		insn.aq = aq != 0U;
		insn.rl = rl != 0U;

		if (!widthOk || !rs2Ok) {
			return;
		}

		markAExtension();
		switch (funct5) {
		case 0x02U:
			insn.mnemonic = isDoubleword ? Mnemonic::LR_D : Mnemonic::LR_W;
			markLegal();
			break;
		case 0x03U:
			insn.mnemonic = isDoubleword ? Mnemonic::SC_D : Mnemonic::SC_W;
			markLegal();
			break;
		case 0x01U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOSWAP_D : Mnemonic::AMOSWAP_W;
			markLegal();
			break;
		case 0x00U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOADD_D : Mnemonic::AMOADD_W;
			markLegal();
			break;
		case 0x04U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOXOR_D : Mnemonic::AMOXOR_W;
			markLegal();
			break;
		case 0x0CU:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOAND_D : Mnemonic::AMOAND_W;
			markLegal();
			break;
		case 0x08U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOOR_D : Mnemonic::AMOOR_W;
			markLegal();
			break;
		case 0x10U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOMIN_D : Mnemonic::AMOMIN_W;
			markLegal();
			break;
		case 0x14U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOMAX_D : Mnemonic::AMOMAX_W;
			markLegal();
			break;
		case 0x18U:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOMINU_D : Mnemonic::AMOMINU_W;
			markLegal();
			break;
		case 0x1CU:
			insn.mnemonic = isDoubleword ? Mnemonic::AMOMAXU_D : Mnemonic::AMOMAXU_W;
			markLegal();
			break;
		default:
			break;
		}
	};

	switch (insn.opcode) {
	case kOpcodeLui:
		insn.mnemonic = Mnemonic::LUI;
		insn.format = InstructionFormat::U;
		insn.immediate = DecodeUImmediate(rawWord);
		markLegal();
		break;

	case kOpcodeAuipc:
		insn.mnemonic = Mnemonic::AUIPC;
		insn.format = InstructionFormat::U;
		insn.immediate = DecodeUImmediate(rawWord);
		markLegal();
		break;

	case kOpcodeJal:
		insn.mnemonic = Mnemonic::JAL;
		insn.format = InstructionFormat::J;
		insn.immediate = DecodeJImmediate(rawWord);
		markLegal();
		break;

	case kOpcodeJalr:
		insn.format = InstructionFormat::I;
		insn.immediate = SignExtend((rawWord >> 20) & 0xFFFU, 12);
		if (insn.funct3 == 0x0U) {
			insn.mnemonic = Mnemonic::JALR;
			markLegal();
		}
		break;

	case kOpcodeSystem: {
		insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0xFFFU);
		const uint32_t csr = static_cast<uint32_t>((rawWord >> 20) & 0xFFFU);
		const uint32_t zimm = static_cast<uint32_t>((rawWord >> 15) & 0x1FU);

		switch (insn.funct3) {
		case 0x0:
			insn.format = InstructionFormat::System;
			if (csr == 0x000U) {
				insn.mnemonic = Mnemonic::ECALL;
				markSystemExtension();
				markLegal();
			} else if (csr == 0x001U) {
				insn.mnemonic = Mnemonic::EBREAK;
				markSystemExtension();
				markLegal();
			} else if (csr == 0x302U) {
				insn.mnemonic = Mnemonic::MRET;
				markSystemExtension();
				markLegal();
			} else if (csr == 0x102U) {
				insn.mnemonic = Mnemonic::SRET;
				markSystemExtension();
				markLegal();
			} else if (csr == 0x105U) {
				insn.mnemonic = Mnemonic::WFI;
				markSystemExtension();
				markLegal();
			}
			break;
		case 0x1:
		case 0x2:
		case 0x3:
		case 0x5:
		case 0x6:
		case 0x7:
			insn.format = InstructionFormat::I;
			insn.immediate = static_cast<int64_t>(csr);
			markZicsrExtension();
			switch (insn.funct3) {
			case 0x1: insn.mnemonic = Mnemonic::CSRRW; markLegal(); break;
			case 0x2:
				if (zimm != 0U) {
					insn.mnemonic = Mnemonic::CSRRS;
					markLegal();
				}
				break;
			case 0x3:
				if (zimm != 0U) {
					insn.mnemonic = Mnemonic::CSRRC;
					markLegal();
				}
				break;
			case 0x5:
				insn.mnemonic = Mnemonic::CSRRWI;
				insn.rs1 = static_cast<uint8_t>(zimm);
				insn.immediate = static_cast<int64_t>(csr);
				markLegal();
				break;
			case 0x6:
				insn.mnemonic = Mnemonic::CSRRSI;
				insn.rs1 = static_cast<uint8_t>(zimm);
				insn.immediate = static_cast<int64_t>(csr);
				markLegal();
				break;
			case 0x7:
				insn.mnemonic = Mnemonic::CSRRCI;
				insn.rs1 = static_cast<uint8_t>(zimm);
				insn.immediate = static_cast<int64_t>(csr);
				markLegal();
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
		break;
	}

	case kOpcodeBranch:
		insn.format = InstructionFormat::B;
		insn.immediate = DecodeBImmediate(rawWord);
		switch (insn.funct3) {
		case 0x0: insn.mnemonic = Mnemonic::BEQ; markLegal(); break;
		case 0x1: insn.mnemonic = Mnemonic::BNE; markLegal(); break;
		case 0x4: insn.mnemonic = Mnemonic::BLT; markLegal(); break;
		case 0x5: insn.mnemonic = Mnemonic::BGE; markLegal(); break;
		case 0x6: insn.mnemonic = Mnemonic::BLTU; markLegal(); break;
		case 0x7: insn.mnemonic = Mnemonic::BGEU; markLegal(); break;
		default: break;
		}
		break;

	case kOpcodeLoad:
		insn.format = InstructionFormat::I;
		insn.immediate = SignExtend((rawWord >> 20) & 0xFFFU, 12);
		switch (insn.funct3) {
		case 0x0: insn.mnemonic = Mnemonic::LB; markLegal(); break;
		case 0x1: insn.mnemonic = Mnemonic::LH; markLegal(); break;
		case 0x2: insn.mnemonic = Mnemonic::LW; markLegal(); break;
		case 0x3: insn.mnemonic = Mnemonic::LD; markLegal(); break;
		case 0x4: insn.mnemonic = Mnemonic::LBU; markLegal(); break;
		case 0x5: insn.mnemonic = Mnemonic::LHU; markLegal(); break;
		case 0x6: insn.mnemonic = Mnemonic::LWU; markLegal(); break;
		default: break;
		}
		break;

	case kOpcodeStore:
		insn.format = InstructionFormat::S;
		insn.immediate = DecodeSImmediate(rawWord);
		switch (insn.funct3) {
		case 0x0: insn.mnemonic = Mnemonic::SB; markLegal(); break;
		case 0x1: insn.mnemonic = Mnemonic::SH; markLegal(); break;
		case 0x2: insn.mnemonic = Mnemonic::SW; markLegal(); break;
		case 0x3: insn.mnemonic = Mnemonic::SD; markLegal(); break;
		default: break;
		}
		break;

	case kOpcodeOpImm:
		insn.format = InstructionFormat::I;
		insn.immediate = SignExtend((rawWord >> 20) & 0xFFFU, 12);
		switch (insn.funct3) {
		case 0x0: insn.mnemonic = Mnemonic::ADDI; markLegal(); break;
		case 0x2: insn.mnemonic = Mnemonic::SLTI; markLegal(); break;
		case 0x3: insn.mnemonic = Mnemonic::SLTIU; markLegal(); break;
		case 0x4: insn.mnemonic = Mnemonic::XORI; markLegal(); break;
		case 0x6: insn.mnemonic = Mnemonic::ORI; markLegal(); break;
		case 0x7: insn.mnemonic = Mnemonic::ANDI; markLegal(); break;
		case 0x1:
			if (((rawWord >> 26) & 0x3FU) == 0x00U) {
				insn.mnemonic = Mnemonic::SLLI;
				insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0x3FU);
				markLegal();
			}
			break;
		case 0x5:
			if (((rawWord >> 26) & 0x3FU) == 0x00U || ((rawWord >> 26) & 0x3FU) == 0x10U) {
				insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0x3FU);
				insn.mnemonic = (((rawWord >> 26) & 0x3FU) == 0x10U) ? Mnemonic::SRAI : Mnemonic::SRLI;
				markLegal();
			}
			break;
		default:
			break;
		}
		break;

	case kOpcodeOp:
		insn.format = InstructionFormat::R;
		switch (insn.funct3) {
		case 0x0:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::ADD; markLegal(); }
			else if (insn.funct7 == 0x20U) { insn.mnemonic = Mnemonic::SUB; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::MUL; markMExtension(); markLegal(); }
			break;
		case 0x1:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SLL; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::MULH; markMExtension(); markLegal(); }
			break;
		case 0x2:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SLT; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::MULHSU; markMExtension(); markLegal(); }
			break;
		case 0x3:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SLTU; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::MULHU; markMExtension(); markLegal(); }
			break;
		case 0x4:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::XOR; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::DIV; markMExtension(); markLegal(); }
			break;
		case 0x5:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SRL; markLegal(); }
			else if (insn.funct7 == 0x20U) { insn.mnemonic = Mnemonic::SRA; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::DIVU; markMExtension(); markLegal(); }
			break;
		case 0x6:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::OR; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::REM; markMExtension(); markLegal(); }
			break;
		case 0x7:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::AND; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::REMU; markMExtension(); markLegal(); }
			break;
		default:
			break;
		}
		break;

	case kOpcodeOpImm32:
		insn.format = InstructionFormat::I;
		switch (insn.funct3) {
		case 0x0:
			insn.immediate = SignExtend((rawWord >> 20) & 0xFFFU, 12);
			insn.mnemonic = Mnemonic::ADDIW;
			markLegal();
			break;
		case 0x1:
			if (IsShiftImmWLegal(insn.funct7, insn.funct3)) {
				insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0x1FU);
				insn.mnemonic = Mnemonic::SLLIW;
				markLegal();
			}
			break;
		case 0x5:
			if (IsShiftImmWLegal(insn.funct7, insn.funct3)) {
				insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0x1FU);
				insn.mnemonic = (insn.funct7 == 0x20U) ? Mnemonic::SRAIW : Mnemonic::SRLIW;
				markLegal();
			}
			break;
		default:
			break;
		}
		break;

	case kOpcodeOp32:
		insn.format = InstructionFormat::R;
		switch (insn.funct3) {
		case 0x0:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::ADDW; markLegal(); }
			else if (insn.funct7 == 0x20U) { insn.mnemonic = Mnemonic::SUBW; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::MULW; markMExtension(); markLegal(); }
			break;
		case 0x1:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SLLW; markLegal(); }
			break;
		case 0x5:
			if (insn.funct7 == 0x00U) { insn.mnemonic = Mnemonic::SRLW; markLegal(); }
			else if (insn.funct7 == 0x20U) { insn.mnemonic = Mnemonic::SRAW; markLegal(); }
			else if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::DIVUW; markMExtension(); markLegal(); }
			break;
		case 0x4:
			if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::DIVW; markMExtension(); markLegal(); }
			break;
		case 0x6:
			if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::REMW; markMExtension(); markLegal(); }
			break;
		case 0x7:
			if (insn.funct7 == kFunct7M) { insn.mnemonic = Mnemonic::REMUW; markMExtension(); markLegal(); }
			break;
		default:
			break;
		}
		break;

	case kOpcodeAtomic:
		if (insn.funct3 == 0x2U) {
			decodeAtomic(false);
		} else if (insn.funct3 == 0x3U) {
			decodeAtomic(true);
		}
		break;

	case kOpcodeFence:
		insn.format = InstructionFormat::System;
		if (insn.funct3 == 0x0U) {
			insn.mnemonic = Mnemonic::FENCE;
			insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0xFFFU);
			markLegal();
		} else if (insn.funct3 == 0x1U) {
			insn.mnemonic = Mnemonic::FENCE_I;
			insn.immediate = static_cast<int64_t>((rawWord >> 20) & 0xFFFU);
			markLegal();
		}
		break;

	default:
		break;
	}

	if (!insn.recognized) {
		insn.mnemonic = Mnemonic::Illegal;
		insn.format = InstructionFormat::Unknown;
		insn.illegal = true;
	}

	return insn;
}

std::string Decoder::Disassemble(const DecodedInstruction& instruction) const {
	std::ostringstream oss;
	const std::string mnemonic = MnemonicToString(instruction.mnemonic);

	if (instruction.illegal) {
		oss << "illegal 0x" << std::hex << instruction.rawWord;
		return oss.str();
	}

	switch (instruction.format) {
	case InstructionFormat::R:
		oss << mnemonic << ' ' << RegisterName(instruction.rd)
			<< ", " << RegisterName(instruction.rs1)
			<< ", " << RegisterName(instruction.rs2);
		if (instruction.mnemonic == Mnemonic::LR_W || instruction.mnemonic == Mnemonic::LR_D) {
			oss << " [" << (instruction.aq ? 'a' : '-') << (instruction.rl ? 'r' : '-') << "]";
		}
		break;

	case InstructionFormat::I:
		if (instruction.mnemonic == Mnemonic::JALR) {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << instruction.immediate
				<< "(" << RegisterName(instruction.rs1) << ")";
		} else if (instruction.mnemonic == Mnemonic::CSRRW || instruction.mnemonic == Mnemonic::CSRRS ||
				   instruction.mnemonic == Mnemonic::CSRRC) {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << RegisterName(instruction.rs1)
				<< ", " << instruction.immediate;
		} else if (instruction.mnemonic == Mnemonic::CSRRWI || instruction.mnemonic == Mnemonic::CSRRSI ||
				   instruction.mnemonic == Mnemonic::CSRRCI) {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << static_cast<unsigned>(instruction.rs1)
				<< ", " << instruction.immediate;
		} else if (instruction.mnemonic == Mnemonic::LB || instruction.mnemonic == Mnemonic::LH ||
				   instruction.mnemonic == Mnemonic::LW || instruction.mnemonic == Mnemonic::LD ||
				   instruction.mnemonic == Mnemonic::LBU || instruction.mnemonic == Mnemonic::LHU ||
				   instruction.mnemonic == Mnemonic::LWU) {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << instruction.immediate
				<< "(" << RegisterName(instruction.rs1) << ")";
		} else if (instruction.mnemonic == Mnemonic::SLLI || instruction.mnemonic == Mnemonic::SRLI ||
				   instruction.mnemonic == Mnemonic::SRAI || instruction.mnemonic == Mnemonic::SLLIW ||
				   instruction.mnemonic == Mnemonic::SRLIW || instruction.mnemonic == Mnemonic::SRAIW) {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << RegisterName(instruction.rs1)
				<< ", " << instruction.immediate;
		} else if (instruction.mnemonic == Mnemonic::FENCE || instruction.mnemonic == Mnemonic::FENCE_I) {
			oss << mnemonic;
		} else {
			oss << mnemonic << ' ' << RegisterName(instruction.rd)
				<< ", " << RegisterName(instruction.rs1)
				<< ", " << instruction.immediate;
		}
		break;

	case InstructionFormat::S:
		oss << mnemonic << ' ' << RegisterName(instruction.rs2)
			<< ", " << instruction.immediate
			<< "(" << RegisterName(instruction.rs1) << ")";
		break;

	case InstructionFormat::B:
		oss << mnemonic << ' ' << RegisterName(instruction.rs1)
			<< ", " << RegisterName(instruction.rs2)
			<< ", " << instruction.immediate;
		break;

	case InstructionFormat::U:
		oss << mnemonic << ' ' << RegisterName(instruction.rd)
			<< ", 0x" << std::hex << static_cast<uint64_t>(instruction.immediate);
		break;

	case InstructionFormat::J:
		oss << mnemonic << ' ' << RegisterName(instruction.rd)
			<< ", " << instruction.immediate;
		break;

	case InstructionFormat::System:
		oss << mnemonic;
		break;

	case InstructionFormat::Unknown:
	default:
		oss << "illegal 0x" << std::hex << instruction.rawWord;
		break;
	}

	return oss.str();
}

} // namespace riscv
