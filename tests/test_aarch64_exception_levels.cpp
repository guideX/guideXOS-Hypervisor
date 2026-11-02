#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IMemory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aarch64;

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, unsigned line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "AARCH64-5 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression, unsigned line) {
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        std::cerr << "AARCH64-5 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x" << actual
                  << std::dec << '\n';
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(size_t size) : bytes_(size, 0) {}

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address)) {
            throw std::out_of_range("AARCH64-5 test memory read out of bounds");
        }
        std::memcpy(dest, bytes_.data() + static_cast<size_t>(address), size);
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address)) {
            throw std::out_of_range("AARCH64-5 test memory write out of bounds");
        }
        std::memcpy(bytes_.data() + static_cast<size_t>(address), src, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* src, size_t size) override {
        Write(address, src, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& src) override {
        Write(address, src.data(), src.size());
    }
    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

void putWord(FlatMemory& memory, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24),
    };
    memory.Write(address, bytes, sizeof(bytes));
}

void putWords(FlatMemory& memory, uint64_t address, const std::vector<uint32_t>& words) {
    for (uint32_t word : words) {
        putWord(memory, address, word);
        address += 4;
    }
}

uint32_t encodeMovz(uint8_t rd, uint16_t imm, uint8_t hw = 0) {
    return 0xD2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | rd;
}

uint32_t encodeAddImm(uint8_t rd, uint8_t rn, uint16_t imm) {
    return 0x91000000u | (static_cast<uint32_t>(imm) << 10) |
           (static_cast<uint32_t>(rn) << 5) | rd;
}

uint32_t encodeSys(bool isRead, uint8_t op0, uint8_t op1, uint8_t crn,
                   uint8_t crm, uint8_t op2, uint8_t rt) {
    return (isRead ? 0xD5200000u : 0xD5000000u) |
           (static_cast<uint32_t>(op0) << 19) |
           (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | rt;
}

uint32_t encodePstateImm(uint8_t op1, uint8_t imm, uint8_t op2) {
    return 0xD500401Fu | (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(imm) << 8) | (static_cast<uint32_t>(op2) << 5);
}

uint32_t encodeSvc(uint16_t imm) { return 0xD4000001u | (static_cast<uint32_t>(imm) << 5); }
uint32_t encodeBrk(uint16_t imm) { return 0xD4200000u | (static_cast<uint32_t>(imm) << 5); }

constexpr uint32_t kMrsCurrentElX0 = 0xD5384240u;
constexpr uint32_t kMrsNzcvX1 = 0xD53B4201u;
constexpr uint32_t kMsrNzcvX2 = 0xD51B4202u;
constexpr uint32_t kMrsEsrX3 = 0xD5385203u;
constexpr uint32_t kMrsSpEl0X4 = 0xD5384104u;
constexpr uint32_t kMrsSpselX5 = 0xD5384205u;
constexpr uint32_t kMrsSpsrX6 = 0xD5384006u;
constexpr uint32_t kMrsElrX7 = 0xD5384027u;
constexpr uint32_t kMsrVbarX0 = 0xD518C000u;
constexpr uint32_t kMsrElrX1 = 0xD5184021u;
constexpr uint32_t kMsrSpsrX1 = 0xD5184001u;
constexpr uint32_t kEret = 0xD69F03E0u;
constexpr uint32_t kNop = 0xD503201Fu;

struct ProgramResult {
    std::vector<uint8_t> state;
    uint64_t steps = 0;
    uint64_t finalPc = 0;
    uint32_t finalPstate = 0;
    std::vector<uint8_t> memory;
};

ProgramResult runLowerElProgramA() {
    constexpr uint64_t kInit = 0x1000;
    constexpr uint64_t kApp = 0x2000;
    constexpr uint64_t kVbar = 0x2000;
    constexpr uint64_t kVector = kVbar + 0x400;
    FlatMemory memory(0x10000);
    AArch64ISAPlugin plugin(kInit, ExceptionLevel::EL1);
    Cpu& cpu = plugin.cpu();
    cpu.setSpEl0(0xD000);
    cpu.setSpEl1(0xE000);

    // EL1 configures VBAR and the ERET frame entirely with guest instructions,
    // then enters the EL0 program. This is the explicit emulator test-entry path.
    putWords(memory, kInit, {
        encodeMovz(0, 0x2000), kMsrVbarX0,
        encodeMovz(1, static_cast<uint16_t>(kApp)), kMsrElrX1,
        encodeMovz(1, 0), kMsrSpsrX1, kEret,
    });
    putWords(memory, kApp, {
        encodeMovz(10, 2),
        encodeAddImm(10, 10, 3),
        encodeAddImm(31, 31, 4), // EL0 SP operand selects SP_EL0
        encodeSvc(0x1234),
        encodeAddImm(11, 10, 1),
    });
    putWords(memory, kVector, {
        encodeAddImm(31, 31, 16), // exception entry must select SP_EL1
        encodeSys(true, 3, 0, 5, 2, 0, 4), // MRS X4, ESR_EL1
        encodeSys(true, 3, 0, 4, 0, 1, 5), // MRS X5, ELR_EL1
        encodeSys(true, 3, 0, 4, 0, 0, 6), // MRS X6, SPSR_EL1
        kMrsCurrentElX0 | 7u,             // MRS X7, CurrentEL
        encodeSys(true, 3, 0, 4, 1, 0, 8), // MRS X8, SP_EL0
        encodeSys(true, 3, 0, 4, 2, 0, 9), // MRS X9, SPSel
        encodePstateImm(3, 0xF, 6),        // DAIFSet #0xF
        encodePstateImm(3, 0xF, 7),        // DAIFClr #0xF
        kEret,
    });

    uint64_t steps = 0;
    bool sawSvcEntry = false;
    while (cpu.pc() != 0x2014 && steps < 64) {
        const uint64_t beforePc = cpu.pc();
        const auto result = plugin.step(memory);
        CHECK(result == ia64::ISAExecutionResult::CONTINUE);
        ++steps;
        if (beforePc == 0x200C) {
            sawSvcEntry = true;
            CHECK_EQ(kVector, cpu.pc());
            CHECK_EQ(0x2010u, cpu.elrEl1());
            CHECK_EQ(0u, cpu.spsrEl1());
            CHECK_EQ(0x56001234u, cpu.esrEl1());
            CHECK_EQ(0x3C5u, cpu.pstate());
            CHECK_EQ(0xE000u, cpu.sp());
        }
    }
    CHECK(sawSvcEntry);
    CHECK_EQ(22u, steps);
    CHECK_EQ(0x2014u, cpu.pc());
    CHECK_EQ(0u, static_cast<uint32_t>(cpu.exceptionLevel()));
    CHECK_EQ(0x2010u, cpu.elrEl1());
    CHECK_EQ(0x56001234u, cpu.readX(4));
    CHECK_EQ(0x2010u, cpu.readX(5));
    CHECK_EQ(0u, cpu.readX(6));
    CHECK_EQ(4u, cpu.readX(7));
    CHECK_EQ(0xD004u, cpu.readX(8));
    CHECK_EQ(1u, cpu.readX(9));
    CHECK_EQ(5u, cpu.readX(10));
    CHECK_EQ(6u, cpu.readX(11));
    CHECK_EQ(0xD004u, cpu.spEl0());
    CHECK_EQ(0xE010u, cpu.spEl1());
    CHECK_EQ(0u, cpu.pstate());

    return {plugin.serialize_state(), steps, cpu.pc(), cpu.pstate(), memory.bytes()};
}

void testProgramAEl0SvcEl1ReturnAndDeterminism() {
    const ProgramResult first = runLowerElProgramA();
    const ProgramResult second = runLowerElProgramA();
    CHECK_EQ(first.steps, second.steps);
    CHECK_EQ(first.finalPc, second.finalPc);
    CHECK_EQ(first.finalPstate, second.finalPstate);
    CHECK(first.state == second.state);
    CHECK(first.memory == second.memory);
}

void testDeterministicEntryProfiles() {
    Cpu el0(0x800, ExceptionLevel::EL0);
    CHECK_EQ(0x800u, el0.pc());
    CHECK_EQ(0u, static_cast<uint32_t>(el0.exceptionLevel()));
    CHECK_EQ(0u, el0.pstate());
    el0.setSpEl0(0x1110);
    el0.setSpEl1(0x2220);
    CHECK_EQ(0x1110u, el0.sp());

    Cpu el1(0x1000, ExceptionLevel::EL1);
    CHECK_EQ(0x1000u, el1.pc());
    CHECK_EQ(1u, static_cast<uint32_t>(el1.exceptionLevel()));
    CHECK_EQ(5u, el1.pstate()); // deterministic EL1h/SP_EL1 emulator entry
    el1.setSpEl0(0x3330);
    el1.setSpEl1(0x4440);
    CHECK_EQ(0x4440u, el1.sp());
    el1.reset(0x2000, ExceptionLevel::EL0);
    CHECK_EQ(0x2000u, el1.pc());
    CHECK_EQ(0u, static_cast<uint32_t>(el1.exceptionLevel()));
    CHECK_EQ(0u, el1.spEl0());
    CHECK_EQ(0u, el1.spEl1());

    AArch64ISAPlugin plugin(0x3000, ExceptionLevel::EL1);
    plugin.reset();
    CHECK_EQ(0x3000u, plugin.getPC());
    CHECK_EQ(1u, static_cast<uint32_t>(plugin.cpu().exceptionLevel()));
}

void runSameLevelProgramB(bool useSpEl1) {
    constexpr uint64_t kInit = 0x1000;
    constexpr uint64_t kVbar = 0x3000;
    const uint64_t vector = kVbar + (useSpEl1 ? 0x200u : 0x000u);
    FlatMemory memory(0x10000);
    AArch64ISAPlugin plugin(kInit, ExceptionLevel::EL1);
    Cpu& cpu = plugin.cpu();
    cpu.setSpEl0(0xA000);
    cpu.setSpEl1(0xB000);
    cpu.setNzcv(true, false, true, true);
    putWords(memory, kInit, {
        encodeMovz(0, static_cast<uint16_t>(kVbar)),
        kMsrVbarX0,
        encodePstateImm(0, useSpEl1 ? 1 : 0, 5), // SPSel
        encodeAddImm(31, 31, 4),                // active SP bank follows SPSel
        encodePstateImm(3, 0xF, 6),             // mask all async interrupts
        encodeSvc(0x55),
        kNop,
    });
    putWords(memory, vector, {
        encodeAddImm(31, 31, 8),
        encodeSys(true, 3, 0, 4, 0, 0, 2), // saved PSTATE
        encodeSys(true, 3, 0, 4, 0, 1, 3), // return PC
        encodeSys(true, 3, 0, 4, 2, 0, 4),
        kEret,
    });

    uint64_t steps = 0;
    bool sawSvc = false;
    while (cpu.pc() != 0x101C && steps < 32) {
        const uint64_t beforePc = cpu.pc();
        CHECK(plugin.step(memory) == ia64::ISAExecutionResult::CONTINUE);
        ++steps;
        if (beforePc == 0x1014) {
            sawSvc = true;
            CHECK_EQ(vector, cpu.pc());
            CHECK_EQ(0x1018u, cpu.elrEl1());
            const uint32_t savedMode = useSpEl1 ? 5u : 4u;
            const uint32_t savedPstate = 0xB0000000u | savedMode | kPstateDAIF;
            CHECK_EQ(savedMode, cpu.spsrEl1() & 0x1Fu);
            CHECK_EQ(savedPstate, cpu.spsrEl1());
            CHECK_EQ(0x56000055u, cpu.esrEl1());
            CHECK_EQ(0xB00003C5u, cpu.pstate()); // entry preserves NZCV and masks DAIF
            CHECK_EQ(0xB000u + (useSpEl1 ? 4u : 0u), cpu.sp());
        }
    }
    CHECK(sawSvc);
    CHECK_EQ(12u, steps);
    CHECK_EQ(0x101Cu, cpu.pc());
    CHECK_EQ(1u, static_cast<uint32_t>(cpu.exceptionLevel()));
    CHECK_EQ(useSpEl1 ? 5u : 4u, cpu.pstate() & kPstateModeMask);
    CHECK_EQ(0x1018u, cpu.readX(3));
    const uint32_t savedPstate = 0xB0000000u | (useSpEl1 ? 5u : 4u) | kPstateDAIF;
    CHECK_EQ(savedPstate, cpu.readX(2));
    CHECK_EQ(1u, cpu.readX(4)); // exception handler always uses SP_EL1
    CHECK_EQ(0xA000u + (useSpEl1 ? 0u : 4u), cpu.spEl0());
    CHECK_EQ(0xB008u + (useSpEl1 ? 4u : 0u), cpu.spEl1());
    CHECK_EQ(savedPstate, cpu.pstate()); // NZCV, DAIF and SPSel are restored
}

void testProgramBEl1SameLevelVectorsAndStackBanks() {
    runSameLevelProgramB(true);  // EL1h -> VBAR+0x200
    runSameLevelProgramB(false); // EL1t -> VBAR+0x000
}

void testBrkReturnAddressAndVectorClassification() {
    FlatMemory memory(0x8000);
    constexpr uint64_t vbar = 0x2000;

    Cpu fromEl0(0x1000, ExceptionLevel::EL0);
    fromEl0.setVbarEl1(vbar);
    putWord(memory, 0x1000, encodeBrk(0x4567));
    CHECK(fromEl0.step(memory) == Fault::None);
    CHECK_EQ(vbar + 0x400, fromEl0.pc());
    CHECK_EQ(0x1000u, fromEl0.elrEl1()); // BRK retries at its own PC
    CHECK_EQ(0xF2004567u, fromEl0.esrEl1()); // EC=0x3C, IL=1, imm16

    Cpu fromEl1(0x1100, ExceptionLevel::EL1);
    fromEl1.setVbarEl1(vbar);
    putWord(memory, 0x1100, encodeBrk(0x1234));
    CHECK(fromEl1.step(memory) == Fault::None);
    CHECK_EQ(vbar + 0x200, fromEl1.pc());
    CHECK_EQ(0x1100u, fromEl1.elrEl1());
    CHECK_EQ(0xF6001234u, fromEl1.esrEl1()); // EC=0x3D for same EL
}

void testSystemRegistersAndPstateControls() {
    FlatMemory memory(0x4000);
    Cpu cpu(0x100, ExceptionLevel::EL1);
    cpu.setSpEl0(0x11110000);
    cpu.setSpEl1(0x22220000);
    cpu.setNzcv(true, false, true, true);
    cpu.writeX(2, 0xF0000000u);
    cpu.writeX(4, 0x50000000u);
    cpu.writeX(7, 0);
    putWords(memory, 0x100, {
        kMrsCurrentElX0,
        kMrsNzcvX1,
        kMsrNzcvX2,
        encodeSys(true, 3, 0, 4, 1, 0, 3), // SP_EL0
        encodeSys(false, 3, 0, 4, 1, 0, 4),
        encodeSys(true, 3, 0, 4, 2, 0, 5), // SPSel
        encodeSys(false, 3, 0, 4, 2, 0, 7), // register-form MSR SPSel,x7
        encodePstateImm(0, 0, 5),
        encodePstateImm(0, 1, 5),
        encodePstateImm(3, 0xF, 6),
        encodeSys(true, 3, 3, 4, 2, 1, 6), // DAIF
        encodePstateImm(3, 0xF, 7),
    });
    for (int i = 0; i < 12; ++i) CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(4u, cpu.readX(0));
    CHECK_EQ(0xB0000000u, cpu.readX(1));
    CHECK_EQ(0xF0000000u, cpu.pstate() & 0xF0000000u); // NZCV only write mask
    CHECK_EQ(0x11110000u, cpu.readX(3));
    CHECK_EQ(0x50000000u, cpu.spEl0());
    CHECK_EQ(1u, cpu.readX(5));
    CHECK_EQ(0x3C0u, cpu.readX(6));
    CHECK_EQ(0u, cpu.pstate() & kPstateDAIF);
    CHECK_EQ(0x22220000u, cpu.spEl1());
    CHECK_EQ(0x22220000u, cpu.sp());

    const Decoded known = Cpu::decodeWord(kMrsCurrentElX0, 0x100);
    CHECK(known.valid && known.op == Op::Mrs);
    CHECK(known.disassembly.find("currentel") != std::string::npos);
    const Decoded immediate = Cpu::decodeWord(encodePstateImm(3, 0xF, 6), 0);
    CHECK(immediate.valid && immediate.op == Op::MsrPstateImm);
    CHECK(immediate.disassembly.find("daifset") != std::string::npos);
}

void testAccessFaultAndBoundaryControls() {
    FlatMemory memory(0x5000);
    Cpu writableNzcv(0x1E0, ExceptionLevel::EL0);
    writableNzcv.setPstate(kPstateDAIF);
    writableNzcv.writeX(2, 0xA0000000u);
    putWords(memory, 0x1E0, {kMsrNzcvX2, kMrsNzcvX1});
    CHECK(writableNzcv.step(memory) == Fault::None);
    CHECK(writableNzcv.step(memory) == Fault::None);
    CHECK_EQ(0xA0000000u, writableNzcv.readX(1));
    CHECK_EQ(kPstateDAIF | 0xA0000000u, writableNzcv.pstate());

    Cpu el0(0x100, ExceptionLevel::EL0);
    el0.setVbarEl1(0x1000);
    el0.setNzcv(true, false, true, false);
    putWords(memory, 0x100, {
        kMrsNzcvX1,
        kMrsCurrentElX0,
        encodePstateImm(3, 0x2, 6), // EL0 DAIFSet is trapped with UMA=0
        encodeSys(false, 3, 0, 12, 0, 0, 0), // MSR VBAR_EL1,x0
    });
    CHECK(el0.step(memory) == Fault::None);
    CHECK_EQ(0xA0000000u, el0.readX(1)); // NZCV allowed at EL0
    CHECK(el0.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, el0.pc());
    CHECK_EQ(0x104u, el0.elrEl1());
    CHECK_EQ(0x02000000u, el0.esrEl1()); // privileged CurrentEL access is UNDEFINED
    CHECK_EQ(0x1000u, el0.vbarEl1()); // trapped write did not occur
    CHECK(el0.step(memory) == Fault::UnallocatedEncoding); // backed vector contains zero

    Cpu denied(0x200, ExceptionLevel::EL0);
    denied.setVbarEl1(0x1000);
    putWord(memory, 0x200, encodePstateImm(3, 2, 6));
    CHECK(denied.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, denied.pc());
    CHECK_EQ(0x62000000u, denied.esrEl1() & 0xFE000000u); // DAIF trap with UMA=0

    Cpu el0Write(0x220, ExceptionLevel::EL0);
    el0Write.setVbarEl1(0x1000);
    el0Write.writeX(0, 0x2FFF);
    putWord(memory, 0x220, encodeSys(false, 3, 0, 12, 0, 0, 0));
    CHECK(el0Write.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, el0Write.pc());
    CHECK_EQ(0x1000u, el0Write.vbarEl1());
    CHECK_EQ(0x02000000u, el0Write.esrEl1()); // EL0 MSR VBAR is UNDEFINED

    Cpu el0SpReg(0x260, ExceptionLevel::EL0);
    el0SpReg.setVbarEl1(0x1000);
    el0SpReg.setSpEl0(0xA000);
    putWord(memory, 0x260, kMrsSpEl0X4);
    CHECK(el0SpReg.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, el0SpReg.pc());
    CHECK_EQ(0x02000000u, el0SpReg.esrEl1()); // MRS SP_EL0 is UNDEFINED at EL0

    Cpu el1tSpReg(0x280, ExceptionLevel::EL1);
    el1tSpReg.setVbarEl1(0x1000);
    el1tSpReg.setSpEl0(0xA000);
    el1tSpReg.setPstate(0x4u); // EL1t, current SP is SP_EL0
    putWord(memory, 0x280, kMrsSpEl0X4);
    CHECK(el1tSpReg.step(memory) == Fault::None);
    CHECK_EQ(0x1000u, el1tSpReg.pc()); // current EL using SP_EL0 vector
    CHECK_EQ(0x280u, el1tSpReg.elrEl1());
    CHECK_EQ(0x02000000u, el1tSpReg.esrEl1()); // direct MRS is UNDEFINED when SPSel=0
    CHECK_EQ(0xA000u, el1tSpReg.spEl0());

    Cpu el0Spsel(0x2A0, ExceptionLevel::EL0);
    el0Spsel.setVbarEl1(0x1000);
    putWord(memory, 0x2A0, encodePstateImm(0, 1, 5)); // MSR SPSel,#1 is UNDEFINED at EL0
    CHECK(el0Spsel.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, el0Spsel.pc());
    CHECK_EQ(0x02000000u, el0Spsel.esrEl1());

    Cpu readOnly(0x240, ExceptionLevel::EL1);
    readOnly.setVbarEl1(0x1000);
    putWord(memory, 0x240, encodeSys(false, 3, 0, 4, 2, 2, 0)); // MSR CurrentEL,X0
    CHECK(readOnly.step(memory) == Fault::None);
    CHECK_EQ(0x1200u, readOnly.pc()); // EL1h synchronous vector
    CHECK_EQ(0x240u, readOnly.elrEl1());
    CHECK_EQ(0x02000000u, readOnly.esrEl1()); // UNDEFINED instruction, IL=1

    Cpu unsupported(0x300, ExceptionLevel::EL1);
    const uint32_t unsupportedReg = encodeSys(true, 3, 0, 15, 15, 7, 0);
    const Decoded unsupportedDecoded = Cpu::decodeWord(unsupportedReg, 0x300);
    CHECK(unsupportedDecoded.valid && unsupportedDecoded.op == Op::Mrs);
    CHECK(unsupported.executeDecoded(unsupportedDecoded, memory) == Fault::UnsupportedFeature);
    CHECK_EQ(0x300u, unsupported.pc());
    CHECK(unsupported.lastError().find("outside the implemented subset") != std::string::npos);

    Cpu badVbar(0x400, ExceptionLevel::EL1);
    badVbar.writeX(0, 0x2FFF);
    putWord(memory, 0x400, encodeSys(false, 3, 0, 12, 0, 0, 0));
    CHECK(badVbar.step(memory) == Fault::None);
    CHECK_EQ(0x2800u, badVbar.vbarEl1()); // architectural RES0 low bits

    Cpu misaligned(0x503, ExceptionLevel::EL0);
    CHECK(misaligned.step(memory) == Fault::UnalignedPC);

    FlatMemory shortMemory(0x1000);
    putWord(shortMemory, 0, encodeSvc(1));
    Cpu noVector(0, ExceptionLevel::EL0);
    noVector.setVbarEl1(0x2000);
    CHECK(noVector.step(shortMemory) == Fault::None);
    CHECK_EQ(0x2400u, noVector.pc());
    CHECK(noVector.step(shortMemory) == Fault::InvalidFetch);

    Cpu badHandler(0, ExceptionLevel::EL0);
    badHandler.setVbarEl1(0x1000);
    putWord(memory, 0, encodeSvc(2));
    putWord(memory, 0x1400, 0xFFFFFFFFu);
    CHECK(badHandler.step(memory) == Fault::None);
    CHECK(badHandler.step(memory) == Fault::UnallocatedEncoding);
    CHECK_EQ(0x1400u, badHandler.pc());

    Cpu level2(0x500, ExceptionLevel::EL0);
    level2.setPstate(0x9u); // explicit invalid-for-profile EL2h state
    putWord(memory, 0x500, encodeSvc(3));
    CHECK(level2.step(memory) == Fault::UnsupportedFeature);
    CHECK_EQ(0x500u, level2.pc());
}

void testIllegalAndUnsupportedEretStates() {
    FlatMemory memory(0x4000);
    Cpu illegal(0x100, ExceptionLevel::EL1);
    illegal.setVbarEl1(0x1000);
    illegal.setSpsrEl1(0x2u); // reserved A64 mode
    illegal.setElrEl1(0x800);
    putWord(memory, 0x100, kEret);
    CHECK(illegal.step(memory) == Fault::None);
    CHECK_EQ(0x1200u, illegal.pc());
    CHECK_EQ(0x100u, illegal.elrEl1());
    CHECK_EQ(0x3A000000u, illegal.esrEl1()); // Illegal Execution State EC=0x0E, IL=1
    CHECK_EQ(0x5u, illegal.spsrEl1()); // exception entry saves prior EL1h PSTATE

    Cpu unsupported(0x200, ExceptionLevel::EL1);
    unsupported.setSpsrEl1(0x9u); // valid EL2h mode, outside profile
    unsupported.setElrEl1(0x900);
    putWord(memory, 0x200, kEret);
    CHECK(unsupported.step(memory) == Fault::UnsupportedFeature);
    CHECK_EQ(0x200u, unsupported.pc());
    CHECK_EQ(0x9u, unsupported.spsrEl1());

    Cpu unprivileged(0x300, ExceptionLevel::EL0);
    unprivileged.setVbarEl1(0x1000);
    putWord(memory, 0x300, kEret);
    CHECK(unprivileged.step(memory) == Fault::None);
    CHECK_EQ(0x1400u, unprivileged.pc());
    CHECK_EQ(0x02000000u, unprivileged.esrEl1()); // ERET is UNDEFINED at EL0, EC=0
}

void testRepeatedExceptionsAndPluginState() {
    FlatMemory memory(0x5000);
    AArch64ISAPlugin plugin(0x1000, ExceptionLevel::EL1);
    Cpu& cpu = plugin.cpu();
    cpu.setVbarEl1(0x2000);
    cpu.setSpEl0(0xA000);
    cpu.setSpEl1(0xB000);
    putWords(memory, 0x1000, {encodeSvc(0x11), encodeSvc(0x22), kNop});
    putWord(memory, 0x2200, kEret);
    // Same-level EL1h vector is VBAR+0x200.
    uint64_t steps = 0;
    for (int i = 0; i < 5; ++i) {
        CHECK(plugin.step(memory) == ia64::ISAExecutionResult::CONTINUE);
        ++steps;
    }
    CHECK_EQ(5u, steps);
    CHECK_EQ(0x100Cu, cpu.pc());
    CHECK_EQ(0x1008u, cpu.elrEl1());
    CHECK_EQ(0x56000022u, cpu.esrEl1());

    cpu.setElrEl1(0x7777);
    cpu.setSpsrEl1(0x15u | kPstateDAIF);
    cpu.setFarEl1(0xABCDEF0012345678ull);
    const std::vector<uint8_t> snapshot = plugin.serialize_state();
    CHECK_EQ(320u, snapshot.size());
    CHECK_EQ(2u, static_cast<uint32_t>(snapshot[4]));
    AArch64ISAPlugin restored(0, ExceptionLevel::EL0);
    CHECK(restored.deserialize_state(snapshot));
    CHECK_EQ(cpu.spEl0(), restored.cpu().spEl0());
    CHECK_EQ(cpu.spEl1(), restored.cpu().spEl1());
    CHECK_EQ(cpu.elrEl1(), restored.cpu().elrEl1());
    CHECK_EQ(cpu.spsrEl1(), restored.cpu().spsrEl1());
    CHECK_EQ(cpu.vbarEl1(), restored.cpu().vbarEl1());
    CHECK_EQ(cpu.esrEl1(), restored.cpu().esrEl1());
    CHECK_EQ(cpu.farEl1(), restored.cpu().farEl1());
    CHECK_EQ(cpu.pstate(), restored.cpu().pstate());

    // Version-1 records restore to the compatible EL0 profile, with the old
    // single SP mapped to SP_EL0 and no unrepresented exception state.
    std::vector<uint8_t> v1(kSerializedStateV1Size, 0);
    v1[0] = 0x31; v1[1] = 0x34; v1[2] = 0x36; v1[3] = 0x41;
    v1[4] = 1;
    const auto write64 = [&](size_t at, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) v1[at + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    write64(256, 0x12345000);
    write64(264, 0x400);
    v1[272] = 0x90;
    v1[275] = 0xA0;
    CHECK(restored.deserialize_state(v1));
    CHECK_EQ(0x12345000u, restored.cpu().spEl0());
    CHECK_EQ(0u, restored.cpu().spEl1());
    CHECK_EQ(0x400u, restored.cpu().pc());
    CHECK_EQ(0xA0000000u, restored.cpu().pstate());
    CHECK_EQ(0u, static_cast<uint32_t>(restored.cpu().exceptionLevel()));

    const std::string before = restored.dumpState();
    const std::vector<uint8_t> beforeState = restored.serialize_state();
    auto bad = snapshot;
    bad[4] = 9;
    CHECK(!restored.deserialize_state(bad));
    CHECK(restored.dumpState() == before);
    CHECK(restored.serialize_state() == beforeState);
    CHECK(!restored.deserialize_state(std::vector<uint8_t>(319, 0)));
    bad = snapshot;
    bad[316] = 1; // reserved word in v2
    CHECK(!restored.deserialize_state(bad));
    CHECK(restored.dumpState() == before);
    CHECK(restored.serialize_state() == beforeState);
    bad = snapshot;
    // SPSR_EL1 invalid reserved bits: v2 deserialize validates saved state.
    bad[294] = 0x01;
    CHECK(!restored.deserialize_state(bad));
    CHECK(restored.dumpState() == before);
    CHECK(restored.serialize_state() == beforeState);
    bad = snapshot;
    bad[282] = 1; // reserved PSTATE bit
    CHECK(!restored.deserialize_state(bad));
    CHECK(restored.serialize_state() == beforeState);
    bad = snapshot;
    bad[296] = 1; // unaligned VBAR_EL1 record
    CHECK(!restored.deserialize_state(bad));
    CHECK(restored.serialize_state() == beforeState);

    // Two plugin instances serialize independent exception banks.
    AArch64ISAPlugin other(0x3000, ExceptionLevel::EL0);
    other.cpu().setSpEl0(0x1111);
    other.cpu().setSpEl1(0x2222);
    CHECK(other.serialize_state() != snapshot);
    CHECK(plugin.hasFeature("MRS") && plugin.hasFeature("MSR"));
    CHECK(plugin.hasFeature("SVC") && plugin.hasFeature("BRK") && plugin.hasFeature("ERET"));
    CHECK(plugin.disassemble(memory, 0x1000).find("svc #0x11") == 0);
}

void testArchitecturalEncodingAnchors() {
    // These canonical words were independently produced by clang's AArch64
    // integrated assembler and llvm-objdump, then frozen here as test anchors.
    const std::pair<uint32_t, const char*> anchors[] = {
        {0xD5384240u, "mrs x0, currentel"},
        {0xD53B4201u, "mrs x1, nzcv"},
        {0xD51B4202u, "msr nzcv, x2"},
        {0xD5184208u, "msr spsel, x8"},
        {0xD5034FDFu, "msr daifset, #15"},
        {0xD50041BFu, "msr spsel, #1"},
        {0xD4024681u, "svc #0x1234"},
        {0xD42ACF00u, "brk #0x5678"},
        {0xD69F03E0u, "eret"},
    };
    for (const auto& anchor : anchors) {
        const Decoded decoded = Cpu::decodeWord(anchor.first, 0x1000);
        CHECK(decoded.valid);
        CHECK(decoded.disassembly == anchor.second);
    }
}

} // namespace

int main() {
    testArchitecturalEncodingAnchors();
    testDeterministicEntryProfiles();
    testProgramAEl0SvcEl1ReturnAndDeterminism();
    testProgramBEl1SameLevelVectorsAndStackBanks();
    testBrkReturnAddressAndVectorClassification();
    testSystemRegistersAndPstateControls();
    testAccessFaultAndBoundaryControls();
    testIllegalAndUnsupportedEretStates();
    testRepeatedExceptionsAndPluginState();
    std::cout << "AArch64ExceptionLevelTests: " << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
