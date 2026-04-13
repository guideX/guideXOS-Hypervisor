#include "decoder.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <iomanip>

using namespace ia64;

// Test helper
void assert_equal(const char* name, uint64_t expected, uint64_t actual) {
    if (expected != actual) {
        std::cerr << "TEST FAILED: " << name << std::endl;
        std::cerr << "  Expected: 0x" << std::hex << expected << std::dec << std::endl;
        std::cerr << "  Actual:   0x" << std::hex << actual << std::dec << std::endl;
        exit(1);
    }
}

void assert_true(const char* name, bool condition) {
    if (!condition) {
        std::cerr << "TEST FAILED: " << name << " - condition is false" << std::endl;
        exit(1);
    }
}

// Test CMP instructions
void test_compare_instructions() {
    std::cout << "Testing compare instructions..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Test CMP.EQ
    cpu.SetGR(1, 100);
    cpu.SetGR(2, 100);
    cpu.SetGR(3, 50);
    
    InstructionEx cmp_eq(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_eq.SetOperands4(1, 1, 2, 2);  // p1, p2 = r1, r2 (100 == 100)
    cmp_eq.Execute(cpu, memory);
    
    assert_true("CMP.EQ: p1 should be true", cpu.GetPR(1));
    assert_true("CMP.EQ: p2 should be false", !cpu.GetPR(2));
    
    // Test CMP.LT signed
    cpu.SetGR(4, static_cast<uint64_t>(-10));  // Negative number
    cpu.SetGR(5, 5);
    
    InstructionEx cmp_lt(InstructionType::CMP_LT, UnitType::I_UNIT);
    cmp_lt.SetOperands4(3, 4, 5, 4);  // p3, p4 = r4, r5 (-10 < 5)
    cmp_lt.Execute(cpu, memory);
    
    assert_true("CMP.LT: p3 should be true (signed)", cpu.GetPR(3));
    assert_true("CMP.LT: p4 should be false", !cpu.GetPR(4));
    
    // Test CMP.LTU unsigned
    InstructionEx cmp_ltu(InstructionType::CMP_LTU, UnitType::I_UNIT);
    cmp_ltu.SetOperands4(5, 4, 5, 6);  // p5, p6 = r4, r5 (unsigned)
    cmp_ltu.Execute(cpu, memory);
    
    assert_true("CMP.LTU: p5 should be false (unsigned)", !cpu.GetPR(5));
    assert_true("CMP.LTU: p6 should be true", cpu.GetPR(6));
    
    std::cout << "  ? Compare instructions passed" << std::endl;
}

// Test bitwise operations
void test_bitwise_operations() {
    std::cout << "Testing bitwise operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 0xFF00);
    cpu.SetGR(2, 0x0F0F);
    
    // Test AND
    InstructionEx and_insn(InstructionType::AND, UnitType::I_UNIT);
    and_insn.SetOperands(3, 1, 2);
    and_insn.Execute(cpu, memory);
    assert_equal("AND", 0x0F00, cpu.GetGR(3));
    
    // Test OR
    InstructionEx or_insn(InstructionType::OR, UnitType::I_UNIT);
    or_insn.SetOperands(4, 1, 2);
    or_insn.Execute(cpu, memory);
    assert_equal("OR", 0xFF0F, cpu.GetGR(4));
    
    // Test XOR
    InstructionEx xor_insn(InstructionType::XOR, UnitType::I_UNIT);
    xor_insn.SetOperands(5, 1, 2);
    xor_insn.Execute(cpu, memory);
    assert_equal("XOR", 0xF00F, cpu.GetGR(5));
    
    // Test ANDCM (AND complement)
    InstructionEx andcm_insn(InstructionType::ANDCM, UnitType::I_UNIT);
    andcm_insn.SetOperands(6, 1, 2);
    andcm_insn.Execute(cpu, memory);
    assert_equal("ANDCM", 0xF0F0, cpu.GetGR(6));
    
    std::cout << "  ? Bitwise operations passed" << std::endl;
}

// Test shift operations
void test_shift_operations() {
    std::cout << "Testing shift operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 0x12345678);
    cpu.SetGR(2, 4);
    
    // Test SHL
    InstructionEx shl(InstructionType::SHL, UnitType::I_UNIT);
    shl.SetOperands(3, 1, 2);
    shl.Execute(cpu, memory);
    assert_equal("SHL", 0x123456780ULL, cpu.GetGR(3));
    
    // Test SHR (logical)
    InstructionEx shr(InstructionType::SHR, UnitType::I_UNIT);
    shr.SetOperands(4, 1, 2);
    shr.Execute(cpu, memory);
    assert_equal("SHR", 0x01234567ULL, cpu.GetGR(4));
    
    // Test SHRA (arithmetic)
    cpu.SetGR(5, 0x8000000000000000ULL);  // Negative number
    cpu.SetGR(6, 4);
    InstructionEx shra(InstructionType::SHRA, UnitType::I_UNIT);
    shra.SetOperands(7, 5, 6);
    shra.Execute(cpu, memory);
    assert_equal("SHRA", 0xF800000000000000ULL, cpu.GetGR(7));
    
    // Test SHLADD
    cpu.SetGR(8, 10);
    cpu.SetGR(9, 100);
    InstructionEx shladd(InstructionType::SHLADD, UnitType::I_UNIT);
    shladd.SetOperands(10, 8, 9);
    shladd.SetImmediate(2);  // Shift by 2
    shladd.Execute(cpu, memory);
    assert_equal("SHLADD", 140, cpu.GetGR(10));  // (10 << 2) + 100 = 40 + 100
    
    std::cout << "  ? Shift operations passed" << std::endl;
}

// Test extract/deposit operations
void test_extract_deposit() {
    std::cout << "Testing extract/deposit operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Test ZXT (zero extend)
    cpu.SetGR(1, 0xFFFFFFFFFFFFFF80ULL);
    
    InstructionEx zxt1(InstructionType::ZXT1, UnitType::I_UNIT);
    zxt1.SetOperands(2, 1, 0);
    zxt1.Execute(cpu, memory);
    assert_equal("ZXT1", 0x80ULL, cpu.GetGR(2));
    
    InstructionEx zxt2(InstructionType::ZXT2, UnitType::I_UNIT);
    zxt2.SetOperands(3, 1, 0);
    zxt2.Execute(cpu, memory);
    assert_equal("ZXT2", 0xFF80ULL, cpu.GetGR(3));
    
    InstructionEx zxt4(InstructionType::ZXT4, UnitType::I_UNIT);
    zxt4.SetOperands(4, 1, 0);
    zxt4.Execute(cpu, memory);
    assert_equal("ZXT4", 0xFFFFFF80ULL, cpu.GetGR(4));
    
    // Test SXT (sign extend)
    cpu.SetGR(5, 0x80);  // Negative byte
    
    InstructionEx sxt1(InstructionType::SXT1, UnitType::I_UNIT);
    sxt1.SetOperands(6, 5, 0);
    sxt1.Execute(cpu, memory);
    assert_equal("SXT1", 0xFFFFFFFFFFFFFF80ULL, cpu.GetGR(6));
    
    cpu.SetGR(7, 0x8000);  // Negative word
    InstructionEx sxt2(InstructionType::SXT2, UnitType::I_UNIT);
    sxt2.SetOperands(8, 7, 0);
    sxt2.Execute(cpu, memory);
    assert_equal("SXT2", 0xFFFFFFFFFFFF8000ULL, cpu.GetGR(8));
    
    std::cout << "  ? Extract/deposit operations passed" << std::endl;
}

// Test memory operations
void test_memory_operations() {
    std::cout << "Testing memory operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    uint64_t base_addr = 0x1000;
    cpu.SetGR(1, base_addr);
    
    // Test ST1/LD1
    cpu.SetGR(2, 0x42);
    InstructionEx st1(InstructionType::ST1, UnitType::M_UNIT);
    st1.SetOperands(1, 2, 0);
    st1.Execute(cpu, memory);
    
    InstructionEx ld1(InstructionType::LD1, UnitType::M_UNIT);
    ld1.SetOperands(3, 1, 0);
    ld1.Execute(cpu, memory);
    assert_equal("LD1/ST1", 0x42, cpu.GetGR(3));
    
    // Test ST2/LD2
    cpu.SetGR(1, base_addr + 0x10);
    cpu.SetGR(4, 0x1234);
    InstructionEx st2(InstructionType::ST2, UnitType::M_UNIT);
    st2.SetOperands(1, 4, 0);
    st2.Execute(cpu, memory);
    
    InstructionEx ld2(InstructionType::LD2, UnitType::M_UNIT);
    ld2.SetOperands(5, 1, 0);
    ld2.Execute(cpu, memory);
    assert_equal("LD2/ST2", 0x1234, cpu.GetGR(5));
    
    // Test ST4/LD4
    cpu.SetGR(1, base_addr + 0x20);
    cpu.SetGR(6, 0x12345678);
    InstructionEx st4(InstructionType::ST4, UnitType::M_UNIT);
    st4.SetOperands(1, 6, 0);
    st4.Execute(cpu, memory);
    
    InstructionEx ld4(InstructionType::LD4, UnitType::M_UNIT);
    ld4.SetOperands(7, 1, 0);
    ld4.Execute(cpu, memory);
    assert_equal("LD4/ST4", 0x12345678, cpu.GetGR(7));
    
    // Test ST8/LD8
    cpu.SetGR(1, base_addr + 0x30);
    cpu.SetGR(8, 0x123456789ABCDEF0ULL);
    InstructionEx st8(InstructionType::ST8, UnitType::M_UNIT);
    st8.SetOperands(1, 8, 0);
    st8.Execute(cpu, memory);
    
    InstructionEx ld8(InstructionType::LD8, UnitType::M_UNIT);
    ld8.SetOperands(9, 1, 0);
    ld8.Execute(cpu, memory);
    assert_equal("LD8/ST8", 0x123456789ABCDEF0ULL, cpu.GetGR(9));
    
    std::cout << "  ? Memory operations passed" << std::endl;
}

// Test predicated execution
void test_predicated_execution() {
    std::cout << "Testing predicated execution..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 100);
    cpu.SetGR(2, 200);
    
    // Set predicate registers
    cpu.SetPR(1, true);
    cpu.SetPR(2, false);
    
    // Test with true predicate
    InstructionEx add1(InstructionType::ADD, UnitType::I_UNIT);
    add1.SetPredicate(1);
    add1.SetOperands(3, 1, 2);
    add1.Execute(cpu, memory);
    assert_equal("Predicated ADD (true)", 300, cpu.GetGR(3));
    
    // Test with false predicate
    InstructionEx add2(InstructionType::ADD, UnitType::I_UNIT);
    add2.SetPredicate(2);
    add2.SetOperands(4, 1, 2);
    add2.Execute(cpu, memory);
    assert_equal("Predicated ADD (false)", 0, cpu.GetGR(4));  // Should not execute
    
    std::cout << "  ? Predicated execution passed" << std::endl;
}

// Test ALLOC instruction
void test_alloc_instruction() {
    std::cout << "Testing ALLOC instruction..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Set initial CFM
    cpu.SetCFM(0x12345678);
    
    // ALLOC: sof=10, sol=5, sor=2
    // immediate = (sor << 14) | (sol << 7) | sof
    uint64_t imm = (2ULL << 14) | (5ULL << 7) | 10ULL;
    
    InstructionEx alloc(InstructionType::ALLOC, UnitType::I_UNIT);
    alloc.SetOperands(10, 0, 0);  // r10 = ar.pfs
    alloc.SetImmediate(imm);
    alloc.Execute(cpu, memory);
    
    // Check saved CFM
    assert_equal("ALLOC: saved CFM", 0x12345678, cpu.GetGR(10));
    
    // Check new CFM fields
    uint64_t new_cfm = cpu.GetCFM();
    assert_equal("ALLOC: new SOF", 10, new_cfm & 0x7F);
    assert_equal("ALLOC: new SOL", 5, (new_cfm >> 7) & 0x7F);
    assert_equal("ALLOC: new SOR", 2, (new_cfm >> 14) & 0xF);
    
    std::cout << "  ? ALLOC instruction passed" << std::endl;
}

// Test 32-bit compare instructions
void test_cmp4_instructions() {
    std::cout << "Testing CMP4 (32-bit compare) instructions..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Use values that differ in upper 32 bits
    cpu.SetGR(1, 0x1000000000000064ULL);  // Upper bits differ
    cpu.SetGR(2, 0x2000000000000064ULL);  // Upper bits differ
    
    // CMP4 should only compare lower 32 bits
    InstructionEx cmp4_eq(InstructionType::CMP4_EQ, UnitType::I_UNIT);
    cmp4_eq.SetOperands4(1, 1, 2, 2);
    cmp4_eq.Execute(cpu, memory);
    
    assert_true("CMP4.EQ: p1 should be true (lower 32 bits equal)", cpu.GetPR(1));
    assert_true("CMP4.EQ: p2 should be false", !cpu.GetPR(2));
    
    std::cout << "  ? CMP4 instructions passed" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "IA-64 Instruction Set Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    try {
        test_compare_instructions();
        test_bitwise_operations();
        test_shift_operations();
        test_extract_deposit();
        test_memory_operations();
        test_predicated_execution();
        test_alloc_instruction();
        test_cmp4_instructions();
        
        std::cout << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "? ALL TESTS PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST SUITE FAILED: " << e.what() << std::endl;
        return 1;
    }
}
