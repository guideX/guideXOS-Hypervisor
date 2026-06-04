#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace riscv {

enum class InstructionFormat {
	Unknown,
	R,
	I,
	S,
	B,
	U,
	J,
	System,
	Compressed
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
	CSRRW,
	CSRRS,
	CSRRC,
	CSRRWI,
	CSRRSI,
	CSRRCI,
	ECALL,
	EBREAK,
	MRET,
	SRET,
	WFI,
	SFENCE_VMA,
	FENCE,
	FENCE_I,
	C_NOP,
	C_ADDI,
	C_LI,
	C_LUI,
	C_ADDI16SP,
	C_ADDI4SPN,
	C_LW,
	C_LD,
	C_SW,
	C_SD,
	C_J,
	C_JR,
	C_JALR,
	C_BEQZ,
	C_BNEZ,
	C_SLLI,
	C_SRLI,
	C_SRAI,
	C_ANDI,
	C_MV,
	C_ADD,
	C_EBREAK
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

struct DecodedStreamEntry {
	uint64_t pc;
	DecodedInstruction instruction;
	std::string disassembly;
	bool truncated;
};

class Decoder {
public:
	Decoder() = default;

	DecodedInstruction Decode(uint32_t rawWord) const;
	std::string Disassemble(const DecodedInstruction& instruction) const;
	std::vector<DecodedStreamEntry> DecodeStream(const std::vector<uint32_t>& words, uint64_t startingPc) const;
	std::vector<DecodedStreamEntry> DecodeByteStream(const std::vector<uint8_t>& bytes, uint64_t startingPc) const;

private:
	static int64_t SignExtend(uint64_t value, unsigned bits);
	static int64_t DecodeUImmediate(uint32_t rawWord);
	static int64_t DecodeJImmediate(uint32_t rawWord);
	static int64_t DecodeBImmediate(uint32_t rawWord);
	static int64_t DecodeSImmediate(uint32_t rawWord);
	DecodedInstruction DecodeCompressed(uint16_t rawHalfword) const;
	static std::string MnemonicToString(Mnemonic mnemonic);
	static std::string RegisterName(uint8_t reg);
};

} // namespace riscv
