#pragma once

#include <cstdint>
#include <string>

namespace riscv {

enum class InstructionFormat {
	Unknown,
	R,
	I,
	S,
	B,
	U,
	J,
	System
};

enum class Mnemonic {
	Illegal,
	LUI,
	AUIPC,
	JAL,
	JALR,
	BEQ,
	BNE,
	BLT,
	BGE,
	BLTU,
	BGEU,
	LB,
	LH,
	LW,
	LD,
	LBU,
	LHU,
	LWU,
	SB,
	SH,
	SW,
	SD,
	ADDI,
	SLTI,
	SLTIU,
	XORI,
	ORI,
	ANDI,
	SLLI,
	SRLI,
	SRAI,
	ADD,
	SUB,
	SLL,
	SLT,
	SLTU,
	XOR,
	SRL,
	SRA,
	OR,
	AND,
	ADDIW,
	SLLIW,
	SRLIW,
	SRAIW,
	ADDW,
	SUBW,
	SLLW,
	SRLW,
	SRAW,
	MUL,
	MULH,
	MULHSU,
	MULHU,
	DIV,
	DIVU,
	REM,
	REMU,
	MULW,
	DIVW,
	DIVUW,
	REMW,
	REMUW,
		LR_W,
		SC_W,
		AMOSWAP_W,
		AMOADD_W,
		AMOXOR_W,
		AMOAND_W,
		AMOOR_W,
		AMOMIN_W,
		AMOMAX_W,
		AMOMINU_W,
		AMOMAXU_W,
		LR_D,
		SC_D,
		AMOSWAP_D,
		AMOADD_D,
		AMOXOR_D,
		AMOAND_D,
		AMOOR_D,
		AMOMIN_D,
		AMOMAX_D,
		AMOMINU_D,
		AMOMAXU_D,
	FENCE,
	FENCE_I
};

struct DecodedInstruction {
	uint32_t rawWord;
	Mnemonic mnemonic;
	InstructionFormat format;
	uint8_t opcode;
	uint8_t rd;
	uint8_t rs1;
	uint8_t rs2;
	uint8_t funct3;
	uint8_t funct7;
	int64_t immediate;
	bool aq;
	bool rl;
	uint8_t instructionLength;
	std::string extension;
	bool recognized;
	bool illegal;

	DecodedInstruction();
};

class Decoder {
public:
	Decoder() = default;

	DecodedInstruction Decode(uint32_t rawWord) const;
	std::string Disassemble(const DecodedInstruction& instruction) const;

private:
	static int64_t SignExtend(uint64_t value, unsigned bits);
	static int64_t DecodeUImmediate(uint32_t rawWord);
	static int64_t DecodeJImmediate(uint32_t rawWord);
	static int64_t DecodeBImmediate(uint32_t rawWord);
	static int64_t DecodeSImmediate(uint32_t rawWord);
	static std::string MnemonicToString(Mnemonic mnemonic);
	static std::string RegisterName(uint8_t reg);
};

} // namespace riscv
