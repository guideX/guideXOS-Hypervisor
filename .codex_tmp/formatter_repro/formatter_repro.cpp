#include "cpu_state.h"
#include "decoder.h"
#include "memory.h"

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Step {
    uint64_t ip;
    uint64_t raw;
    ia64::UnitType unit;
};

uint64_t read64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

void printFR(const ia64::CPUState& cpu, unsigned reg) {
    uint8_t bytes[16] = {};
    cpu.GetFR(reg, bytes);
    const uint64_t significand = read64(bytes);
    const uint64_t signExponent = read64(bytes + 8);
    const uint64_t exponent = signExponent & 0x1FFFFULL;
    const bool negative = (signExponent & (1ULL << 17)) != 0;
    const bool natVal = (signExponent & 0x3FFFFULL) == 0x1FFFEULL;
    std::cout << " f" << reg << "={sig=0x" << std::hex << significand
              << ",se=0x" << signExponent << ",exp=0x" << exponent
              << ",sign=" << negative << ",nat=" << natVal << ",bytes=";
    for (unsigned i = 0; i < 16; ++i) {
        if (i != 0) std::cout << ' ';
        std::cout << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(bytes[i]);
    }
    std::cout << std::setfill(' ') << "}" << std::dec;
}

void printState(const ia64::CPUState& cpu) {
    std::cout << " r8=0x" << std::hex << cpu.GetGR(8)
              << " r32=0x" << cpu.GetGR(32)
              << " r33=0x" << cpu.GetGR(33)
              << " p6=" << cpu.GetPR(6)
              << " p7=" << cpu.GetPR(7)
              << " fpsr=0x" << cpu.GetAR(40)
              << " cfm=0x" << cpu.GetCFM()
              << " pfs=0x" << cpu.GetPFS() << std::dec;
}

void run(const std::string& label,
         ia64::CPUState& cpu,
         ia64::Memory& memory,
         const std::vector<Step>& steps) {
    ia64::InstructionDecoder decoder;
    std::cout << "=== " << label << " ===\n";
    for (const Step& step : steps) {
        const ia64::InstructionEx instruction =
            decoder.DecodeSlot(step.raw, step.unit, step.ip);
        std::cout << "pre ip=0x" << std::hex << step.ip
                  << " raw=0x" << step.raw << std::dec
                  << " \"" << instruction.GetDisassembly() << "\"";
        printState(cpu);
        std::cout << "\n";
        instruction.Execute(cpu, memory);
        std::cout << "post ip=0x" << std::hex << step.ip
                  << " \"" << instruction.GetDisassembly() << "\"";
        printState(cpu);
        printFR(cpu, 8);
        printFR(cpu, 9);
        printFR(cpu, 10);
        printFR(cpu, 11);
        printFR(cpu, 12);
        printFR(cpu, 13);
        printFR(cpu, 14);
        std::cout << "\n";
    }
}

} // namespace

int main() {
    ia64::CPUState cpu;
    ia64::Memory memory(64 * 1024);
    cpu.SetGR(32, 0x20d);
    cpu.SetGR(33, 0xa);
    cpu.SetAR(40, 1ULL << 9);

    const std::vector<Step> firstHelper = {
        {0x37010, 0xc708040380ULL, ia64::UnitType::M_UNIT},
        {0x37010, 0xc708042240ULL, ia64::UnitType::M_UNIT},
        {0x37010, 0x1c8021011c0ULL, ia64::UnitType::I_UNIT},
        {0x37020, 0x10408e00200ULL, ia64::UnitType::F_UNIT},
        {0x37030, 0x10408900240ULL, ia64::UnitType::F_UNIT},
        {0x37040, 0x630910280ULL, ia64::UnitType::F_UNIT},
        {0x37050, 0x10450800306ULL, ia64::UnitType::F_UNIT},
        {0x37060, 0x184509022c6ULL, ia64::UnitType::F_UNIT},
        {0x37070, 0x10460b18306ULL, ia64::UnitType::F_UNIT},
        {0x37080, 0x10458b00346ULL, ia64::UnitType::F_UNIT},
        {0x37090, 0x10450b14286ULL, ia64::UnitType::F_UNIT},
        {0x370a0, 0x10460d182c6ULL, ia64::UnitType::F_UNIT},
        {0x370b0, 0x1002a100840ULL, ia64::UnitType::I_UNIT},
        {0x370b0, 0x10450d14286ULL, ia64::UnitType::F_UNIT},
        {0x370c0, 0x18458910306ULL, ia64::UnitType::F_UNIT},
        {0x370d0, 0xc708042240ULL, ia64::UnitType::M_UNIT},
        {0x370d0, 0x10450c16286ULL, ia64::UnitType::F_UNIT},
        {0x370e0, 0x4d8014280ULL, ia64::UnitType::F_UNIT},
        {0x370f0, 0x1d048a1c280ULL, ia64::UnitType::F_UNIT},
        {0x37100, 0x8708014200ULL, ia64::UnitType::M_UNIT},
    };

    const std::vector<Step> correctionHelper = {
        {0x36f20, 0xc708040200ULL, ia64::UnitType::M_UNIT},
        {0x36f20, 0xc708042240ULL, ia64::UnitType::M_UNIT},
        {0x36f20, 0x1c8021011c0ULL, ia64::UnitType::I_UNIT},
        {0x36f30, 0x10408800200ULL, ia64::UnitType::F_UNIT},
        {0x36f40, 0x10408900240ULL, ia64::UnitType::F_UNIT},
        {0x36f50, 0x630910280ULL, ia64::UnitType::F_UNIT},
        {0x36f60, 0x184509022c6ULL, ia64::UnitType::F_UNIT},
        {0x36f70, 0x10450800306ULL, ia64::UnitType::F_UNIT},
        {0x36f80, 0x10458b00346ULL, ia64::UnitType::F_UNIT},
        {0x36f90, 0x10460b18306ULL, ia64::UnitType::F_UNIT},
        {0x36fa0, 0x10450b14286ULL, ia64::UnitType::F_UNIT},
        {0x36fb0, 0x10460d182c6ULL, ia64::UnitType::F_UNIT},
        {0x36fc0, 0x10450d14286ULL, ia64::UnitType::F_UNIT},
        {0x36fd0, 0x18458910306ULL, ia64::UnitType::F_UNIT},
        {0x36fe0, 0x10450c16286ULL, ia64::UnitType::F_UNIT},
        {0x36ff0, 0x4d8014280ULL, ia64::UnitType::F_UNIT},
        {0x37000, 0x8708014200ULL, ia64::UnitType::M_UNIT},
    };

    run("ELILO 0x37010 helper, first live inputs", cpu, memory, firstHelper);
    run("ELILO 0x36f20 correction helper, first live inputs", cpu, memory, correctionHelper);

    return 0;
}
