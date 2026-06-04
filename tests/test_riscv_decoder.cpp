#include "riscv_decoder.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using riscv::DecodedInstruction;
using riscv::Decoder;
using riscv::InstructionFormat;
using riscv::Mnemonic;

namespace {

void Expect(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "RISC-V TEST FAILED: " << message << std::endl;
		std::exit(1);
	}
}

void ExpectField(const char* name, int64_t expected, int64_t actual) {
	if (expected != actual) {
		std::cerr << "RISC-V TEST FAILED: " << name << std::endl;
		std::cerr << "  expected: " << expected << std::endl;
		std::cerr << "  actual:   " << actual << std::endl;
		std::exit(1);
	}
}

void ExpectEnum(const char* name, int expected, int actual) {
	if (expected != actual) {
		std::cerr << "RISC-V TEST FAILED: " << name << std::endl;
		std::cerr << "  expected: " << expected << std::endl;
		std::cerr << "  actual:   " << actual << std::endl;
		exit(1);
	}
}

uint32_t EncodeU(uint8_t opcode, uint8_t rd, uint32_t imm20) {
	return (imm20 << 12) | (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm12) {
	return (static_cast<uint32_t>(imm12 & 0xFFF) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   (static_cast<uint32_t>(rd) << 7) |
		   opcode;
}

uint32_t EncodeS(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm12) {
	const uint32_t imm = static_cast<uint32_t>(imm12 & 0xFFF);
	return ((imm >> 5) << 25) |
		   (static_cast<uint32_t>(rs2) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   ((imm & 0x1F) << 7) |
		   opcode;
}

uint32_t EncodeB(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm13) {
	const uint32_t imm = static_cast<uint32_t>(imm13 & 0x1FFF);
	return (((imm >> 12) & 0x1) << 31) |
		   (((imm >> 5) & 0x3F) << 25) |
		   (static_cast<uint32_t>(rs2) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   (((imm >> 1) & 0xF) << 8) |
		   (((imm >> 11) & 0x1) << 7) |
		   opcode;
}

uint32_t EncodeJ(uint8_t opcode, uint8_t rd, int32_t imm21) {
	const uint32_t imm = static_cast<uint32_t>(imm21 & 0x1FFFFF);
	return (((imm >> 20) & 0x1) << 31) |
		   (((imm >> 1) & 0x3FF) << 21) |
		   (((imm >> 11) & 0x1) << 20) |
		   (((imm >> 12) & 0xFF) << 12) |
		   (static_cast<uint32_t>(rd) << 7) |
		   opcode;
}

void ExpectDecoded(const DecodedInstruction& insn,
					  const char* caseName,
				   Mnemonic mnemonic,
				   InstructionFormat format,
				   uint8_t rd,
				   uint8_t rs1,
				   uint8_t rs2,
				   uint8_t funct3,
				   uint8_t funct7,
				   int64_t imm,
				   const std::string& expectedExtension,
				   const std::string& disassembly) {
	ExpectEnum(caseName, static_cast<int>(mnemonic), static_cast<int>(insn.mnemonic));
	ExpectEnum("format", static_cast<int>(format), static_cast<int>(insn.format));
	if (format == InstructionFormat::R || format == InstructionFormat::I || format == InstructionFormat::U || format == InstructionFormat::J) {
		ExpectField("rd", rd, insn.rd);
	}
	if (format == InstructionFormat::R || format == InstructionFormat::S) {
		ExpectField("rs1", rs1, insn.rs1);
		ExpectField("rs2", rs2, insn.rs2);
		ExpectField("funct3", funct3, insn.funct3);
		ExpectField("funct7", funct7, insn.funct7);
	} else if (format == InstructionFormat::I) {
		ExpectField("rs1", rs1, insn.rs1);
		ExpectField("funct3", funct3, insn.funct3);
	} else if (format == InstructionFormat::B) {
		ExpectField("rs1", rs1, insn.rs1);
		ExpectField("rs2", rs2, insn.rs2);
		ExpectField("funct3", funct3, insn.funct3);
	} else if (format == InstructionFormat::U || format == InstructionFormat::J || format == InstructionFormat::System) {
		(void)rs1;
		(void)rs2;
		(void)funct3;
		(void)funct7;
	}
	ExpectField("imm", imm, insn.immediate);
	ExpectField("len", 4, insn.instructionLength);
	Expect(insn.extension == expectedExtension, "extension should match expected group");
	Expect(!insn.illegal, "instruction should be legal");
	Expect(insn.recognized, "instruction should be recognized");

	Decoder decoder;
	Expect(decoder.Disassemble(insn) == disassembly, "disassembly mismatch");
}

uint32_t EncodeR(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7) {
	return (static_cast<uint32_t>(funct7) << 25) |
		   (static_cast<uint32_t>(rs2) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   (static_cast<uint32_t>(rd) << 7) |
		   opcode;
}

uint32_t EncodeAtomic(uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct5, bool aq, bool rl) {
	return (static_cast<uint32_t>(funct5) << 27) |
		   (static_cast<uint32_t>(aq ? 1U : 0U) << 26) |
		   (static_cast<uint32_t>(rl ? 1U : 0U) << 25) |
		   (static_cast<uint32_t>(rs2) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   (static_cast<uint32_t>(rd) << 7) |
		   0x2FU;
}

} // namespace

void TestUFormat() {
	Decoder decoder;
	auto lui = decoder.Decode(EncodeU(0x37, 5, 0x12345));
	ExpectDecoded(lui, "lui", Mnemonic::LUI, InstructionFormat::U, 5, 0, 0, 0, 0, 0x12345000LL, "RV64I", "lui x5, 0x12345000");

	auto auipc = decoder.Decode(EncodeU(0x17, 1, 0x12345));
	ExpectDecoded(auipc, "auipc", Mnemonic::AUIPC, InstructionFormat::U, 1, 0, 0, 0, 0, 0x12345000LL, "RV64I", "auipc x1, 0x12345000");
}

void TestJFormat() {
	Decoder decoder;
	auto jal = decoder.Decode(EncodeJ(0x6F, 1, -4));
	ExpectDecoded(jal, "jal", Mnemonic::JAL, InstructionFormat::J, 1, 0, 0, 0, 0, -4, "RV64I", "jal x1, -4");
}

void TestBFormat() {
	Decoder decoder;
	auto beq = decoder.Decode(EncodeB(0x63, 0x0, 1, 2, -16));
	ExpectDecoded(beq, "beq", Mnemonic::BEQ, InstructionFormat::B, 0, 1, 2, 0, 0, -16, "RV64I", "beq x1, x2, -16");
}

void TestSFormat() {
	Decoder decoder;
	auto sd = decoder.Decode(EncodeS(0x23, 0x3, 9, 8, 24));
	ExpectDecoded(sd, "sd", Mnemonic::SD, InstructionFormat::S, 0, 9, 8, 3, 0, 24, "RV64I", "sd x8, 24(x9)");
}

void TestIFormat() {
	Decoder decoder;

	auto nop = decoder.Decode(0x00000013U);
	ExpectDecoded(nop, "nop/addi", Mnemonic::ADDI, InstructionFormat::I, 0, 0, 0, 0, 0, 0, "RV64I", "addi x0, x0, 0");

	auto addi = decoder.Decode(EncodeI(0x13, 10, 0x0, 11, -1));
	ExpectDecoded(addi, "addi", Mnemonic::ADDI, InstructionFormat::I, 10, 11, 0, 0, 0, -1, "RV64I", "addi x10, x11, -1");

	auto lb = decoder.Decode(EncodeI(0x03, 3, 0x0, 4, -32));
	ExpectDecoded(lb, "lb", Mnemonic::LB, InstructionFormat::I, 3, 4, 0, 0, 0, -32, "RV64I", "lb x3, -32(x4)");

	auto jalr = decoder.Decode(EncodeI(0x67, 1, 0x0, 2, 12));
	ExpectDecoded(jalr, "jalr", Mnemonic::JALR, InstructionFormat::I, 1, 2, 0, 0, 0, 12, "RV64I", "jalr x1, 12(x2)");

	auto slli = decoder.Decode((0x00U << 25) | (3U << 20) | (5U << 15) | (0x1U << 12) | (7U << 7) | 0x13U);
	ExpectDecoded(slli, "slli", Mnemonic::SLLI, InstructionFormat::I, 7, 5, 0, 1, 0x00, 3, "RV64I", "slli x7, x5, 3");

	auto srli = decoder.Decode((0x00U << 25) | (63U << 20) | (5U << 15) | (0x5U << 12) | (7U << 7) | 0x13U);
	ExpectDecoded(srli, "srli", Mnemonic::SRLI, InstructionFormat::I, 7, 5, 0, 5, 0x00, 63, "RV64I", "srli x7, x5, 63");

	auto srai = decoder.Decode((0x20U << 25) | (1U << 20) | (5U << 15) | (0x5U << 12) | (7U << 7) | 0x13U);
	ExpectDecoded(srai, "srai", Mnemonic::SRAI, InstructionFormat::I, 7, 5, 0, 5, 0x20, 1, "RV64I", "srai x7, x5, 1");

	auto addiw = decoder.Decode(EncodeI(0x1B, 4, 0x0, 5, -1));
	ExpectDecoded(addiw, "addiw", Mnemonic::ADDIW, InstructionFormat::I, 4, 5, 0, 0, 0, -1, "RV64I", "addiw x4, x5, -1");

	auto slliw = decoder.Decode((0x00U << 25) | (4U << 20) | (5U << 15) | (0x1U << 12) | (4U << 7) | 0x1BU);
	ExpectDecoded(slliw, "slliw", Mnemonic::SLLIW, InstructionFormat::I, 4, 5, 0, 1, 0x00, 4, "RV64I", "slliw x4, x5, 4");

	auto srliw = decoder.Decode((0x00U << 25) | (6U << 20) | (5U << 15) | (0x5U << 12) | (4U << 7) | 0x1BU);
	ExpectDecoded(srliw, "srliw", Mnemonic::SRLIW, InstructionFormat::I, 4, 5, 0, 5, 0x00, 6, "RV64I", "srliw x4, x5, 6");

	auto sraiw = decoder.Decode((0x20U << 25) | (2U << 20) | (5U << 15) | (0x5U << 12) | (4U << 7) | 0x1BU);
	ExpectDecoded(sraiw, "sraiw", Mnemonic::SRAIW, InstructionFormat::I, 4, 5, 0, 5, 0x20, 2, "RV64I", "sraiw x4, x5, 2");

	auto reservedSraiw = decoder.Decode((0x10U << 25) | (2U << 20) | (5U << 15) | (0x5U << 12) | (4U << 7) | 0x1BU);
	Expect(reservedSraiw.illegal, "reserved SRAIW imm[5] must decode as illegal");

	auto fence = decoder.Decode(0x0000000FU);
	ExpectDecoded(fence, "fence", Mnemonic::FENCE, InstructionFormat::System, 0, 0, 0, 0, 0, 0, "RV64I", "fence");

	auto fenceI = decoder.Decode(0x0000100FU);
	ExpectDecoded(fenceI, "fence.i", Mnemonic::FENCE_I, InstructionFormat::System, 0, 0, 0, 1, 0, 0, "RV64I", "fence.i");

	auto csrrw = decoder.Decode(EncodeI(0x73, 1, 0x1, 2, 0xC00));
	ExpectDecoded(csrrw, "csrrw", Mnemonic::CSRRW, InstructionFormat::I, 1, 2, 0, 1, 0, 0xC00, "Zicsr", "csrrw x1, x2, 3072");

	auto csrrs = decoder.Decode(EncodeI(0x73, 3, 0x2, 4, 0xC01));
	ExpectDecoded(csrrs, "csrrs", Mnemonic::CSRRS, InstructionFormat::I, 3, 4, 0, 2, 0, 0xC01, "Zicsr", "csrrs x3, x4, 3073");

	auto csrrc = decoder.Decode(EncodeI(0x73, 5, 0x3, 6, 0xC02));
	ExpectDecoded(csrrc, "csrrc", Mnemonic::CSRRC, InstructionFormat::I, 5, 6, 0, 3, 0, 0xC02, "Zicsr", "csrrc x5, x6, 3074");

	auto csrrwi = decoder.Decode(EncodeI(0x73, 7, 0x5, 0x8, 0xC03));
	ExpectDecoded(csrrwi, "csrrwi", Mnemonic::CSRRWI, InstructionFormat::I, 7, 8, 0, 5, 0, 3075, "Zicsr", "csrrwi x7, 8, 3075");

	auto csrrsi = decoder.Decode(EncodeI(0x73, 9, 0x6, 0xA, 0xC04));
	ExpectDecoded(csrrsi, "csrrsi", Mnemonic::CSRRSI, InstructionFormat::I, 9, 10, 0, 6, 0, 3076, "Zicsr", "csrrsi x9, 10, 3076");

	auto csrrci = decoder.Decode(EncodeI(0x73, 11, 0x7, 0xB, 0xC05));
	ExpectDecoded(csrrci, "csrrci", Mnemonic::CSRRCI, InstructionFormat::I, 11, 11, 0, 7, 0, 3077, "Zicsr", "csrrci x11, 11, 3077");

	auto ecall = decoder.Decode(0x00000073U);
	ExpectDecoded(ecall, "ecall", Mnemonic::ECALL, InstructionFormat::System, 0, 0, 0, 0, 0, 0, "SYSTEM", "ecall");

	auto ebreak = decoder.Decode(0x00100073U);
	ExpectDecoded(ebreak, "ebreak", Mnemonic::EBREAK, InstructionFormat::System, 0, 0, 0, 0, 0, 1, "SYSTEM", "ebreak");

	auto illegalSystem = decoder.Decode(0x00200073U);
	Expect(illegalSystem.illegal, "reserved system encoding should remain illegal");
}

void TestRFormat() {
	Decoder decoder;

	auto add = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x0U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(add, "add", Mnemonic::ADD, InstructionFormat::R, 3, 1, 2, 0, 0x00, 0, "RV64I", "add x3, x1, x2");

	auto sub = decoder.Decode((0x20U << 25) | (2U << 20) | (1U << 15) | (0x0U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(sub, "sub", Mnemonic::SUB, InstructionFormat::R, 3, 1, 2, 0, 0x20, 0, "RV64I", "sub x3, x1, x2");

	auto sll = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x1U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(sll, "sll", Mnemonic::SLL, InstructionFormat::R, 3, 1, 2, 1, 0x00, 0, "RV64I", "sll x3, x1, x2");

	auto slt = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x2U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(slt, "slt", Mnemonic::SLT, InstructionFormat::R, 3, 1, 2, 2, 0x00, 0, "RV64I", "slt x3, x1, x2");

	auto sltu = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x3U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(sltu, "sltu", Mnemonic::SLTU, InstructionFormat::R, 3, 1, 2, 3, 0x00, 0, "RV64I", "sltu x3, x1, x2");

	auto xorv = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x4U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(xorv, "xor", Mnemonic::XOR, InstructionFormat::R, 3, 1, 2, 4, 0x00, 0, "RV64I", "xor x3, x1, x2");

	auto srl = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x5U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(srl, "srl", Mnemonic::SRL, InstructionFormat::R, 3, 1, 2, 5, 0x00, 0, "RV64I", "srl x3, x1, x2");

	auto sra = decoder.Decode((0x20U << 25) | (2U << 20) | (1U << 15) | (0x5U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(sra, "sra", Mnemonic::SRA, InstructionFormat::R, 3, 1, 2, 5, 0x20, 0, "RV64I", "sra x3, x1, x2");

	auto orv = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x6U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(orv, "or", Mnemonic::OR, InstructionFormat::R, 3, 1, 2, 6, 0x00, 0, "RV64I", "or x3, x1, x2");

	auto andv = decoder.Decode((0x00U << 25) | (2U << 20) | (1U << 15) | (0x7U << 12) | (3U << 7) | 0x33U);
	ExpectDecoded(andv, "and", Mnemonic::AND, InstructionFormat::R, 3, 1, 2, 7, 0x00, 0, "RV64I", "and x3, x1, x2");

	auto addw = decoder.Decode((0x00U << 25) | (7U << 20) | (6U << 15) | (0x0U << 12) | (5U << 7) | 0x3BU);
	ExpectDecoded(addw, "addw", Mnemonic::ADDW, InstructionFormat::R, 5, 6, 7, 0, 0x00, 0, "RV64I", "addw x5, x6, x7");

	auto subw = decoder.Decode((0x20U << 25) | (7U << 20) | (6U << 15) | (0x0U << 12) | (5U << 7) | 0x3BU);
	ExpectDecoded(subw, "subw", Mnemonic::SUBW, InstructionFormat::R, 5, 6, 7, 0, 0x20, 0, "RV64I", "subw x5, x6, x7");

	auto sllw = decoder.Decode((0x00U << 25) | (7U << 20) | (6U << 15) | (0x1U << 12) | (5U << 7) | 0x3BU);
	ExpectDecoded(sllw, "sllw", Mnemonic::SLLW, InstructionFormat::R, 5, 6, 7, 1, 0x00, 0, "RV64I", "sllw x5, x6, x7");

	auto srlw = decoder.Decode((0x00U << 25) | (7U << 20) | (6U << 15) | (0x5U << 12) | (5U << 7) | 0x3BU);
	ExpectDecoded(srlw, "srlw", Mnemonic::SRLW, InstructionFormat::R, 5, 6, 7, 5, 0x00, 0, "RV64I", "srlw x5, x6, x7");

	auto sraw = decoder.Decode((0x20U << 25) | (7U << 20) | (6U << 15) | (0x5U << 12) | (5U << 7) | 0x3BU);
	ExpectDecoded(sraw, "sraw", Mnemonic::SRAW, InstructionFormat::R, 5, 6, 7, 5, 0x20, 0, "RV64I", "sraw x5, x6, x7");

	auto mul = decoder.Decode(EncodeR(0x33, 3, 0x0, 1, 2, 0x01));
	ExpectDecoded(mul, "mul", Mnemonic::MUL, InstructionFormat::R, 3, 1, 2, 0, 0x01, 0, "RV64M", "mul x3, x1, x2");

	auto mulh = decoder.Decode(EncodeR(0x33, 3, 0x1, 1, 2, 0x01));
	ExpectDecoded(mulh, "mulh", Mnemonic::MULH, InstructionFormat::R, 3, 1, 2, 1, 0x01, 0, "RV64M", "mulh x3, x1, x2");

	auto mulhsu = decoder.Decode(EncodeR(0x33, 3, 0x2, 1, 2, 0x01));
	ExpectDecoded(mulhsu, "mulhsu", Mnemonic::MULHSU, InstructionFormat::R, 3, 1, 2, 2, 0x01, 0, "RV64M", "mulhsu x3, x1, x2");

	auto mulhu = decoder.Decode(EncodeR(0x33, 3, 0x3, 1, 2, 0x01));
	ExpectDecoded(mulhu, "mulhu", Mnemonic::MULHU, InstructionFormat::R, 3, 1, 2, 3, 0x01, 0, "RV64M", "mulhu x3, x1, x2");

	auto div = decoder.Decode(EncodeR(0x33, 3, 0x4, 1, 2, 0x01));
	ExpectDecoded(div, "div", Mnemonic::DIV, InstructionFormat::R, 3, 1, 2, 4, 0x01, 0, "RV64M", "div x3, x1, x2");

	auto divu = decoder.Decode(EncodeR(0x33, 3, 0x5, 1, 2, 0x01));
	ExpectDecoded(divu, "divu", Mnemonic::DIVU, InstructionFormat::R, 3, 1, 2, 5, 0x01, 0, "RV64M", "divu x3, x1, x2");

	auto rem = decoder.Decode(EncodeR(0x33, 3, 0x6, 1, 2, 0x01));
	ExpectDecoded(rem, "rem", Mnemonic::REM, InstructionFormat::R, 3, 1, 2, 6, 0x01, 0, "RV64M", "rem x3, x1, x2");

	auto remu = decoder.Decode(EncodeR(0x33, 3, 0x7, 1, 2, 0x01));
	ExpectDecoded(remu, "remu", Mnemonic::REMU, InstructionFormat::R, 3, 1, 2, 7, 0x01, 0, "RV64M", "remu x3, x1, x2");

	auto mulw = decoder.Decode(EncodeR(0x3B, 5, 0x0, 6, 7, 0x01));
	ExpectDecoded(mulw, "mulw", Mnemonic::MULW, InstructionFormat::R, 5, 6, 7, 0, 0x01, 0, "RV64M", "mulw x5, x6, x7");

	auto divw = decoder.Decode(EncodeR(0x3B, 5, 0x4, 6, 7, 0x01));
	ExpectDecoded(divw, "divw", Mnemonic::DIVW, InstructionFormat::R, 5, 6, 7, 4, 0x01, 0, "RV64M", "divw x5, x6, x7");

	auto divuw = decoder.Decode(EncodeR(0x3B, 5, 0x5, 6, 7, 0x01));
	ExpectDecoded(divuw, "divuw", Mnemonic::DIVUW, InstructionFormat::R, 5, 6, 7, 5, 0x01, 0, "RV64M", "divuw x5, x6, x7");

	auto remw = decoder.Decode(EncodeR(0x3B, 5, 0x6, 6, 7, 0x01));
	ExpectDecoded(remw, "remw", Mnemonic::REMW, InstructionFormat::R, 5, 6, 7, 6, 0x01, 0, "RV64M", "remw x5, x6, x7");

	auto remuw = decoder.Decode(EncodeR(0x3B, 5, 0x7, 6, 7, 0x01));
	ExpectDecoded(remuw, "remuw", Mnemonic::REMUW, InstructionFormat::R, 5, 6, 7, 7, 0x01, 0, "RV64M", "remuw x5, x6, x7");
}

void TestAtomicFormat() {
	Decoder decoder;

	auto lrw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 0, 0x02, true, false));
	ExpectDecoded(lrw, "lr.w", Mnemonic::LR_W, InstructionFormat::R, 5, 6, 0, 2, 0x02, 0, "RV64A", "lr.w x5, x6, x0 [a-]");
	Expect(lrw.aq, "lr.w aq bit should be decoded");
	Expect(!lrw.rl, "lr.w rl bit should be decoded");

	auto scw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x03, false, true));
	ExpectDecoded(scw, "sc.w", Mnemonic::SC_W, InstructionFormat::R, 5, 6, 7, 2, 0x03, 0, "RV64A", "sc.w x5, x6, x7");
	Expect(!scw.aq, "sc.w aq bit should be decoded");
	Expect(scw.rl, "sc.w rl bit should be decoded");

	auto amoswapw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x01, false, false));
	ExpectDecoded(amoswapw, "amoswap.w", Mnemonic::AMOSWAP_W, InstructionFormat::R, 5, 6, 7, 2, 0x01, 0, "RV64A", "amoswap.w x5, x6, x7");

	auto amoaddw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x00, false, false));
	ExpectDecoded(amoaddw, "amoadd.w", Mnemonic::AMOADD_W, InstructionFormat::R, 5, 6, 7, 2, 0x00, 0, "RV64A", "amoadd.w x5, x6, x7");

	auto amoxorw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x04, false, false));
	ExpectDecoded(amoxorw, "amoxor.w", Mnemonic::AMOXOR_W, InstructionFormat::R, 5, 6, 7, 2, 0x04, 0, "RV64A", "amoxor.w x5, x6, x7");

	auto amoandw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x0C, false, false));
	ExpectDecoded(amoandw, "amoand.w", Mnemonic::AMOAND_W, InstructionFormat::R, 5, 6, 7, 2, 0x0C, 0, "RV64A", "amoand.w x5, x6, x7");

	auto amoorw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x08, false, false));
	ExpectDecoded(amoorw, "amoor.w", Mnemonic::AMOOR_W, InstructionFormat::R, 5, 6, 7, 2, 0x08, 0, "RV64A", "amoor.w x5, x6, x7");

	auto amominw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x10, false, false));
	ExpectDecoded(amominw, "amomin.w", Mnemonic::AMOMIN_W, InstructionFormat::R, 5, 6, 7, 2, 0x10, 0, "RV64A", "amomin.w x5, x6, x7");

	auto amomaxw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x14, false, false));
	ExpectDecoded(amomaxw, "amomax.w", Mnemonic::AMOMAX_W, InstructionFormat::R, 5, 6, 7, 2, 0x14, 0, "RV64A", "amomax.w x5, x6, x7");

	auto amominuw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x18, false, false));
	ExpectDecoded(amominuw, "amominu.w", Mnemonic::AMOMINU_W, InstructionFormat::R, 5, 6, 7, 2, 0x18, 0, "RV64A", "amominu.w x5, x6, x7");

	auto amomaxuw = decoder.Decode(EncodeAtomic(5, 0x2, 6, 7, 0x1C, false, false));
	ExpectDecoded(amomaxuw, "amomaxu.w", Mnemonic::AMOMAXU_W, InstructionFormat::R, 5, 6, 7, 2, 0x1C, 0, "RV64A", "amomaxu.w x5, x6, x7");

	auto lrd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 0, 0x02, false, false));
	ExpectDecoded(lrd, "lr.d", Mnemonic::LR_D, InstructionFormat::R, 5, 6, 0, 3, 0x02, 0, "RV64A", "lr.d x5, x6, x0 [--]");

	auto scd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x03, true, true));
	ExpectDecoded(scd, "sc.d", Mnemonic::SC_D, InstructionFormat::R, 5, 6, 7, 3, 0x03, 0, "RV64A", "sc.d x5, x6, x7");

	auto amoswapd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x01, false, false));
	ExpectDecoded(amoswapd, "amoswap.d", Mnemonic::AMOSWAP_D, InstructionFormat::R, 5, 6, 7, 3, 0x01, 0, "RV64A", "amoswap.d x5, x6, x7");

	auto amoaddd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x00, false, false));
	ExpectDecoded(amoaddd, "amoadd.d", Mnemonic::AMOADD_D, InstructionFormat::R, 5, 6, 7, 3, 0x00, 0, "RV64A", "amoadd.d x5, x6, x7");

	auto amoxord = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x04, false, false));
	ExpectDecoded(amoxord, "amoxor.d", Mnemonic::AMOXOR_D, InstructionFormat::R, 5, 6, 7, 3, 0x04, 0, "RV64A", "amoxor.d x5, x6, x7");

	auto amoandd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x0C, false, false));
	ExpectDecoded(amoandd, "amoand.d", Mnemonic::AMOAND_D, InstructionFormat::R, 5, 6, 7, 3, 0x0C, 0, "RV64A", "amoand.d x5, x6, x7");

	auto amoord = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x08, false, false));
	ExpectDecoded(amoord, "amoor.d", Mnemonic::AMOOR_D, InstructionFormat::R, 5, 6, 7, 3, 0x08, 0, "RV64A", "amoor.d x5, x6, x7");

	auto amomind = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x10, false, false));
	ExpectDecoded(amomind, "amomin.d", Mnemonic::AMOMIN_D, InstructionFormat::R, 5, 6, 7, 3, 0x10, 0, "RV64A", "amomin.d x5, x6, x7");

	auto amomaxd = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x14, false, false));
	ExpectDecoded(amomaxd, "amomax.d", Mnemonic::AMOMAX_D, InstructionFormat::R, 5, 6, 7, 3, 0x14, 0, "RV64A", "amomax.d x5, x6, x7");

	auto amominud = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x18, false, false));
	ExpectDecoded(amominud, "amominu.d", Mnemonic::AMOMINU_D, InstructionFormat::R, 5, 6, 7, 3, 0x18, 0, "RV64A", "amominu.d x5, x6, x7");

	auto amomaxud = decoder.Decode(EncodeAtomic(5, 0x3, 6, 7, 0x1C, false, false));
	ExpectDecoded(amomaxud, "amomaxu.d", Mnemonic::AMOMAXU_D, InstructionFormat::R, 5, 6, 7, 3, 0x1C, 0, "RV64A", "amomaxu.d x5, x6, x7");

	auto illegalWidth = decoder.Decode(EncodeAtomic(5, 0x1, 6, 7, 0x00, false, false));
	Expect(illegalWidth.illegal, "unsupported atomic funct3 should stay illegal");

	auto illegalReserved = decoder.Decode(EncodeAtomic(5, 0x2, 6, 1, 0x02, false, false));
	Expect(illegalReserved.illegal, "LR must keep rs2 zero");
}

void TestIllegalAndUnknown() {
	Decoder decoder;

	auto illegal = decoder.Decode(0xFFFFFFFFU);
	Expect(illegal.illegal, "unknown word should be illegal");
	Expect(illegal.mnemonic == Mnemonic::Illegal, "illegal mnemonic expected");
	Expect(decoder.Disassemble(illegal) == "illegal 0xffffffff", "illegal disassembly expected");
}

int main() {
	std::cout << "RISC-V RV64I/RV64M/Zicsr decoder smoke tests\n";
	TestUFormat();
	TestJFormat();
	TestBFormat();
	TestSFormat();
	TestIFormat();
	TestRFormat();
	TestAtomicFormat();
	TestIllegalAndUnknown();
	std::cout << "RISC-V RV64I/RV64M/Zicsr decoder smoke tests passed\n";
	return 0;
}
