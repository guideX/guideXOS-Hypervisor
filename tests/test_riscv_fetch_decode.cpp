#include "riscv_decoder.h"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using riscv::Decoder;
using riscv::DecodedInstruction;
using riscv::Mnemonic;

namespace {

void Expect(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "RISC-V FETCH/DECODE TEST FAILED: " << message << std::endl;
		std::exit(1);
	}
}

std::string Hex(uint64_t value, unsigned width) {
	std::ostringstream oss;
	oss << "0x" << std::hex << std::setfill('0') << std::setw(width) << value;
	return oss.str();
}

uint32_t EncodeI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm12) {
	return (static_cast<uint32_t>(imm12 & 0xFFF) << 20) |
		   (static_cast<uint32_t>(rs1) << 15) |
		   (static_cast<uint32_t>(funct3) << 12) |
		   (static_cast<uint32_t>(rd) << 7) |
		   opcode;
}

uint16_t EncodeCQuadrant1(uint8_t funct3, uint8_t rd, uint8_t imm6) {
	return static_cast<uint16_t>((static_cast<uint16_t>(funct3) << 13) |
								((static_cast<uint16_t>(imm6 & 0x1FU)) << 2) |
								((static_cast<uint16_t>((imm6 >> 5) & 0x1U)) << 12) |
								(static_cast<uint16_t>(rd) << 7) |
								0x1U);
}

void AppendHalfword(std::vector<uint8_t>& bytes, uint16_t value) {
	bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
	bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
}

void AppendWord(std::vector<uint8_t>& bytes, uint32_t value) {
	bytes.push_back(static_cast<uint8_t>(value & 0xFFU));
	bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
	bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
	bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

class GuestMemory {
public:
	GuestMemory(uint64_t baseAddress, std::vector<uint8_t> bytes)
		: baseAddress_(baseAddress)
		, bytes_(std::move(bytes)) {
	}

	uint64_t BaseAddress() const { return baseAddress_; }
	uint64_t EndAddress() const { return baseAddress_ + static_cast<uint64_t>(bytes_.size()); }

	size_t Remaining(uint64_t pc) const {
		if (pc < baseAddress_ || pc >= EndAddress()) {
			return 0;
		}
		return static_cast<size_t>(EndAddress() - pc);
	}

	bool ReadHalfword(uint64_t pc, uint16_t& value) const {
		if (Remaining(pc) < 2U) {
			return false;
		}
		const size_t index = static_cast<size_t>(pc - baseAddress_);
		value = static_cast<uint16_t>(bytes_[index]) |
				(static_cast<uint16_t>(bytes_[index + 1]) << 8);
		return true;
	}

	bool ReadWord(uint64_t pc, uint32_t& value) const {
		if (Remaining(pc) < 4U) {
			return false;
		}
		const size_t index = static_cast<size_t>(pc - baseAddress_);
		value = static_cast<uint32_t>(bytes_[index]) |
				(static_cast<uint32_t>(bytes_[index + 1]) << 8) |
				(static_cast<uint32_t>(bytes_[index + 2]) << 16) |
				(static_cast<uint32_t>(bytes_[index + 3]) << 24);
		return true;
	}

	uint32_t ReadPartialWord(uint64_t pc) const {
		const size_t remaining = Remaining(pc);
		const size_t index = static_cast<size_t>(pc - baseAddress_);
		uint32_t value = 0;
		for (size_t i = 0; i < remaining && i < 4U; ++i) {
			value |= static_cast<uint32_t>(bytes_[index + i]) << (static_cast<unsigned>(i) * 8U);
		}
		return value;
	}

private:
	uint64_t baseAddress_;
	std::vector<uint8_t> bytes_;
};

struct TraceEntry {
	size_t step;
	uint64_t pc;
	uint32_t raw;
	uint8_t length;
	std::string disassembly;
	std::string status;
	uint64_t nextPc;
};

struct HarnessResult {
	std::vector<TraceEntry> trace;
	std::string stopReason;
};

std::string MnemonicStatus(const DecodedInstruction& insn) {
	if (insn.illegal) {
		return "illegal";
	}
	return "ok";
}

HarnessResult RunFetchDecodeHarness(const GuestMemory& memory, uint64_t startPc, size_t maxInstructions) {
	HarnessResult result;
	Decoder decoder;
	uint64_t pc = startPc;

	for (size_t step = 0; step < maxInstructions; ++step) {
		if (pc < memory.BaseAddress() || pc >= memory.EndAddress()) {
			result.stopReason = "out_of_range";
			break;
		}

		const size_t remaining = memory.Remaining(pc);
		if (remaining == 0U) {
			result.stopReason = "out_of_range";
			break;
		}
		if (remaining < 2U) {
			const uint32_t raw = memory.ReadPartialWord(pc);
			TraceEntry entry{step, pc, raw, 2U, std::string("illegal ") + Hex(raw, 2U), "truncated", pc + 2ULL};
			result.trace.push_back(entry);
			result.stopReason = "truncated";
			break;
		}

		uint16_t halfword = 0;
		Expect(memory.ReadHalfword(pc, halfword), "halfword fetch should succeed inside range");

		if ((halfword & 0x3U) != 0x3U) {
			const DecodedInstruction insn = decoder.DecodeCompressed(halfword);
			TraceEntry entry{step, pc, halfword, 2U, decoder.Disassemble(insn), MnemonicStatus(insn), pc + 2ULL};
			result.trace.push_back(entry);

			if (insn.mnemonic == Mnemonic::C_EBREAK) {
				result.stopReason = "halt_marker";
				break;
			}
			if (insn.illegal) {
				result.stopReason = "illegal";
				break;
			}

			pc += 2ULL;
			continue;
		}

		if (remaining < 4U) {
			const uint32_t raw = memory.ReadPartialWord(pc);
			TraceEntry entry{step, pc, raw, 4U, std::string("illegal ") + Hex(raw, 2U + static_cast<unsigned>(remaining) * 2U), "truncated", pc + 4ULL};
			result.trace.push_back(entry);
			result.stopReason = "truncated";
			break;
		}

		uint32_t word = 0;
		Expect(memory.ReadWord(pc, word), "word fetch should succeed when 4 bytes remain");
		const DecodedInstruction insn = decoder.Decode(word);
		TraceEntry entry{step, pc, word, 4U, decoder.Disassemble(insn), MnemonicStatus(insn), pc + 4ULL};
		result.trace.push_back(entry);

		if (insn.illegal) {
			result.stopReason = "illegal";
			break;
		}

		pc += 4ULL;
	}

	if (result.stopReason.empty()) {
		result.stopReason = "max_steps";
	}

	return result;
}

std::string TraceLine(const TraceEntry& entry) {
	std::ostringstream oss;
	oss << "step=" << entry.step
		<< " pc=" << Hex(entry.pc, 8U)
		<< " len=" << static_cast<unsigned>(entry.length)
		<< " raw=" << Hex(entry.raw, entry.length == 2U ? 4U : 8U)
		<< " disasm=\"" << entry.disassembly << "\""
		<< " status=" << entry.status
		<< " next_pc=" << Hex(entry.nextPc, 8U);
	return oss.str();
}

void PrintTrace(const HarnessResult& result, const char* title) {
	std::cout << title << '\n';
	for (const auto& entry : result.trace) {
		std::cout << TraceLine(entry) << '\n';
	}
	std::cout << "stop_reason=" << result.stopReason << '\n';
}

void ExpectTraceLine(const TraceEntry& entry, size_t step, uint64_t pc, uint8_t len, uint32_t raw, const std::string& disasm, const std::string& status, uint64_t nextPc) {
	Expect(entry.step == step, "step should match");
	Expect(entry.pc == pc, "pc should match");
	Expect(entry.length == len, "length should match");
	Expect(entry.raw == raw, "raw should match");
	Expect(entry.disassembly == disasm, "disassembly should match");
	Expect(entry.status == status, "status should match");
	Expect(entry.nextPc == nextPc, "next pc should match");
}

void TestMixedStream() {
	std::vector<uint8_t> bytes;
	AppendHalfword(bytes, EncodeCQuadrant1(0x2, 10, 0x00));
	AppendWord(bytes, EncodeI(0x13, 10, 0x0, 0, 1));
	AppendHalfword(bytes, 0x9002U);

	const GuestMemory memory(0x80000000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80000000ULL, 8U);

	Expect(result.stopReason == "halt_marker", "mixed stream should stop on c.ebreak halt marker");
	Expect(result.trace.size() == 3U, "mixed stream should emit three trace entries");
	ExpectTraceLine(result.trace[0], 0U, 0x80000000ULL, 2U, EncodeCQuadrant1(0x2, 10, 0x00), "c.li x10, 0", "ok", 0x80000002ULL);
	ExpectTraceLine(result.trace[1], 1U, 0x80000002ULL, 4U, EncodeI(0x13, 10, 0x0, 0, 1), "addi x10, x0, 1", "ok", 0x80000006ULL);
	ExpectTraceLine(result.trace[2], 2U, 0x80000006ULL, 2U, 0x9002U, "c.ebreak", "ok", 0x80000008ULL);

	PrintTrace(result, "MIXED STREAM TRACE");
}

void TestThirtyTwoBitOnlyStream() {
	std::vector<uint8_t> bytes;
	AppendWord(bytes, EncodeI(0x13, 5, 0x0, 0, 1));
	AppendWord(bytes, EncodeI(0x13, 6, 0x0, 5, -1));

	const GuestMemory memory(0x80001000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80001000ULL, 2U);

	Expect(result.stopReason == "max_steps", "32-bit stream should stop at max steps");
	Expect(result.trace.size() == 2U, "32-bit stream should emit two entries");
	ExpectTraceLine(result.trace[0], 0U, 0x80001000ULL, 4U, EncodeI(0x13, 5, 0x0, 0, 1), "addi x5, x0, 1", "ok", 0x80001004ULL);
	ExpectTraceLine(result.trace[1], 1U, 0x80001004ULL, 4U, EncodeI(0x13, 6, 0x0, 5, -1), "addi x6, x5, -1", "ok", 0x80001008ULL);

	PrintTrace(result, "32-BIT ONLY TRACE");
}

void TestCompressedOnlyStream() {
	std::vector<uint8_t> bytes;
	AppendHalfword(bytes, EncodeCQuadrant1(0x0, 0, 0x00));
	AppendHalfword(bytes, EncodeCQuadrant1(0x2, 6, 0x21));

	const GuestMemory memory(0x80002000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80002000ULL, 4U);

	Expect(result.stopReason == "out_of_range", "compressed stream should stop at memory end");
	Expect(result.trace.size() == 2U, "compressed stream should emit two entries");
	ExpectTraceLine(result.trace[0], 0U, 0x80002000ULL, 2U, EncodeCQuadrant1(0x0, 0, 0x00), "c.nop x0, 0", "ok", 0x80002002ULL);
	ExpectTraceLine(result.trace[1], 1U, 0x80002002ULL, 2U, EncodeCQuadrant1(0x2, 6, 0x21), "c.li x6, -31", "ok", 0x80002004ULL);

	PrintTrace(result, "COMPRESSED ONLY TRACE");
}

void TestTruncatedSingleByte() {
	const std::vector<uint8_t> bytes = {0x13};
	const GuestMemory memory(0x80003000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80003000ULL, 4U);

	Expect(result.stopReason == "truncated", "single trailing byte should stop as truncated");
	Expect(result.trace.size() == 1U, "single trailing byte should emit one entry");
	ExpectTraceLine(result.trace[0], 0U, 0x80003000ULL, 2U, 0x13U, "illegal 0x13", "truncated", 0x80003002ULL);

	PrintTrace(result, "TRUNCATED SINGLE BYTE TRACE");
}

void TestTruncatedThirtyTwoBitFetch() {
	std::vector<uint8_t> bytes;
	AppendHalfword(bytes, EncodeCQuadrant1(0x0, 0, 0x00));
	bytes.push_back(0x13);
	bytes.push_back(0x00);
	bytes.push_back(0x00);

	const GuestMemory memory(0x80004000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80004002ULL, 4U);

	Expect(result.stopReason == "truncated", "truncated 32-bit fetch should stop as truncated");
	Expect(result.trace.size() == 1U, "truncated 32-bit fetch should emit one entry");
	ExpectTraceLine(result.trace[0], 0U, 0x80004002ULL, 4U, 0x13U, "illegal 0x00000013", "truncated", 0x80004006ULL);

	PrintTrace(result, "TRUNCATED 32-BIT TRACE");
}

void TestOutOfRangePc() {
	const std::vector<uint8_t> bytes = {0x01, 0x00, 0x13, 0x00};
	const GuestMemory memory(0x80005000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80005010ULL, 4U);

	Expect(result.stopReason == "out_of_range", "out-of-range pc should stop immediately");
	Expect(result.trace.empty(), "out-of-range pc should not emit trace entries");
}

void TestExplicitMaxStepsStop() {
	std::vector<uint8_t> bytes;
	AppendHalfword(bytes, EncodeCQuadrant1(0x0, 0, 0x00));
	AppendHalfword(bytes, EncodeCQuadrant1(0x0, 0, 0x00));
	AppendHalfword(bytes, EncodeCQuadrant1(0x0, 0, 0x00));

	const GuestMemory memory(0x80006000ULL, bytes);
	const auto result = RunFetchDecodeHarness(memory, 0x80006000ULL, 2U);

	Expect(result.stopReason == "max_steps", "max steps should stop the harness");
	Expect(result.trace.size() == 2U, "max steps should cap the trace length");
	Expect(result.trace[0].status == "ok", "first capped trace entry should be ok");
	Expect(result.trace[1].status == "ok", "second capped trace entry should be ok");
}

} // namespace

int main() {
	std::cout << "RISC-V guest-memory fetch/decode smoke harness\n";
	TestMixedStream();
	TestThirtyTwoBitOnlyStream();
	TestCompressedOnlyStream();
	TestTruncatedSingleByte();
	TestTruncatedThirtyTwoBitFetch();
	TestOutOfRangePc();
	TestExplicitMaxStepsStop();
	std::cout << "RISC-V guest-memory fetch/decode smoke harness passed\n";
	return 0;
}
