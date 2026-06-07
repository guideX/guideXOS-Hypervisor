#include "decoder.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <iomanip>
#include <string>
#include <stdexcept>

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

void assert_string(const char* name, const std::string& expected, const std::string& actual) {
    if (expected != actual) {
        std::cerr << "TEST FAILED: " << name << std::endl;
        std::cerr << "  Expected: " << expected << std::endl;
        std::cerr << "  Actual:   " << actual << std::endl;
        exit(1);
    }
}

uint64_t build_tbit_z_slot(uint8_t qp, uint8_t p1, uint8_t p2, uint8_t r3, uint8_t pos) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(p1) & 0x3F) << 6) |
           ((static_cast<uint64_t>(pos) & 0x3F) << 14) |
           ((static_cast<uint64_t>(r3) & 0x7F) << 20) |
           ((static_cast<uint64_t>(p2) & 0x3F) << 27) |
           (5ULL << 37);
}

uint64_t build_tnat_z_slot(uint8_t qp, uint8_t p1, uint8_t p2, uint8_t r3) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(p1) & 0x3F) << 6) |
           (1ULL << 13) |
           ((static_cast<uint64_t>(r3) & 0x7F) << 20) |
           ((static_cast<uint64_t>(p2) & 0x3F) << 27) |
           (5ULL << 37);
}

uint64_t build_mov_from_ip_slot(uint8_t qp, uint8_t r1) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(r1) & 0x7F) << 6) |
           (0x30ULL << 27);
}

uint64_t build_mov_from_pr_slot(uint8_t qp, uint8_t r1) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(r1) & 0x7F) << 6) |
           (0x33ULL << 27);
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

    InstructionEx cmp_and(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_and.SetOperands4(20, 1, 3, 21);
    cmp_and.SetCompareCompleter(CompareCompleter::AND);
    cpu.SetPR(20, true);
    cpu.SetPR(21, true);
    cmp_and.Execute(cpu, memory);
    assert_true("CMP.EQ.AND should clear p20 when result is false", !cpu.GetPR(20));
    assert_true("CMP.EQ.AND should clear p21 when result is false", !cpu.GetPR(21));

    InstructionEx cmp_or(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_or.SetOperands4(22, 1, 2, 23);
    cmp_or.SetCompareCompleter(CompareCompleter::OR);
    cmp_or.Execute(cpu, memory);
    assert_true("CMP.EQ.OR should set p22 when result is true", cpu.GetPR(22));
    assert_true("CMP.EQ.OR should set p23 when result is true", cpu.GetPR(23));

    InstructionEx cmp_unc(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_unc.SetPredicate(31);
    cmp_unc.SetOperands4(24, 1, 2, 25);
    cmp_unc.SetCompareCompleter(CompareCompleter::UNC);
    cpu.SetPR(24, true);
    cpu.SetPR(25, true);
    cmp_unc.Execute(cpu, memory);
    assert_true("CMP.EQ.UNC should clear p24 when qp is false", !cpu.GetPR(24));
    assert_true("CMP.EQ.UNC should clear p25 when qp is false", !cpu.GetPR(25));
    
    std::cout << "  ? Compare instructions passed" << std::endl;
}

void test_compare_ne_decoder() {
    std::cout << "Testing compare-ne decoder mapping..." << std::endl;

    InstructionDecoder decoder;
    InstructionEx cmp_ne = decoder.DecodeSlot(0x1a801300180ULL, UnitType::I_UNIT, 0x36e70);

    assert_true("CMP.NE raw slot should decode as CMP_NE",
                cmp_ne.GetType() == InstructionType::CMP_NE);
    assert_equal("CMP.NE destination predicate", 6, cmp_ne.GetDst());
    assert_equal("CMP.NE lhs register", 0, cmp_ne.GetSrc1());
    assert_equal("CMP.NE rhs register", 19, cmp_ne.GetSrc2());
    assert_equal("CMP.NE complement predicate", 0, cmp_ne.GetSrc3());

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(19, 3);
    cmp_ne.Execute(cpu, memory);
    assert_true("CMP.NE should set p6 while r19 is non-zero", cpu.GetPR(6));

    cpu.SetGR(19, 0);
    cmp_ne.Execute(cpu, memory);
    assert_true("CMP.NE should clear p6 when r19 reaches zero", !cpu.GetPR(6));

    std::cout << "  ? Compare-ne decoder mapping passed" << std::endl;
}

void test_latest_boot_log_blockers() {
    std::cout << "Testing latest boot-log raw instructions..." << std::endl;

    InstructionDecoder decoder;

    InstructionEx cmp_ltu = decoder.DecodeSlot(0x1a031b34000ULL, UnitType::I_UNIT, 0x36ec0);
    assert_true("Boot raw cmp.ltu should decode", cmp_ltu.GetType() == InstructionType::CMP_LTU);
    assert_equal("Boot cmp.ltu p1 decode", 0, cmp_ltu.GetDst());
    assert_equal("Boot cmp.ltu lhs register", 26, cmp_ltu.GetSrc1());
    assert_equal("Boot cmp.ltu rhs register", 27, cmp_ltu.GetSrc2());
    assert_equal("Boot cmp.ltu p2 decode", 6, cmp_ltu.GetSrc3());
    assert_string("Boot cmp.ltu disassembly",
                  "cmp.ltu p0, p6 = r26, r27",
                  cmp_ltu.GetDisassembly());

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(26, 1);
    cpu.SetGR(27, 2);
    cmp_ltu.Execute(cpu, memory);
    assert_true("Boot cmp.ltu should clear complement when true", !cpu.GetPR(6));

    cpu.SetGR(26, 3);
    cpu.SetGR(27, 2);
    cmp_ltu.Execute(cpu, memory);
    assert_true("Boot cmp.ltu should set complement when false", cpu.GetPR(6));

    InstructionEx cmp_eq = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::I_UNIT, 0x36ed0);
    assert_true("Boot raw cmp.eq should decode", cmp_eq.GetType() == InstructionType::CMP_EQ);
    assert_equal("Boot cmp.eq p1 decode", 10, cmp_eq.GetDst());
    assert_equal("Boot cmp.eq lhs register", 8, cmp_eq.GetSrc1());
    assert_equal("Boot cmp.eq rhs register", 10, cmp_eq.GetSrc2());
    assert_equal("Boot cmp.eq p2 decode", 9, cmp_eq.GetSrc3());
    assert_string("Boot cmp.eq disassembly",
                  "cmp.eq p10, p9 = r8, r10",
                  cmp_eq.GetDisassembly());

    InstructionEx cmp_eq_m_unit = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::M_UNIT, 0x36ed0);
    assert_true("Boot raw cmp.eq should decode in M-unit slot",
                cmp_eq_m_unit.GetType() == InstructionType::CMP_EQ);
    assert_string("Boot M-unit cmp.eq disassembly",
                  "cmp.eq p10, p9 = r8, r10",
                  cmp_eq_m_unit.GetDisassembly());

    cpu.SetGR(8, 0x1234);
    cpu.SetGR(10, 0x1234);
    cmp_eq.Execute(cpu, memory);
    assert_true("Boot cmp.eq should set p10 when true", cpu.GetPR(10));
    assert_true("Boot cmp.eq should clear p9 when true", !cpu.GetPR(9));

    cpu.SetGR(10, 0x5678);
    cmp_eq.Execute(cpu, memory);
    assert_true("Boot cmp.eq should clear p10 when false", !cpu.GetPR(10));
    assert_true("Boot cmp.eq should set p9 when false", cpu.GetPR(9));

    InstructionEx getf_sig = decoder.DecodeSlot(0x8708014540ULL, UnitType::M_UNIT, 0x36ee0);
    assert_true("Boot raw getf.sig should decode", getf_sig.GetType() == InstructionType::GETF_SIG);
    assert_equal("Boot getf.sig destination register", 21, getf_sig.GetDst());
    assert_equal("Boot getf.sig source FP register", 10, getf_sig.GetSrc1());
    assert_string("Boot getf.sig disassembly",
                  "getf.sig r21 = f10",
                  getf_sig.GetDisassembly());

    uint8_t fr10[16] = {};
    const uint64_t significand = 0x0123456789abcdefULL;
    for (int i = 0; i < 8; ++i) {
        fr10[i] = static_cast<uint8_t>((significand >> (i * 8)) & 0xff);
    }
    cpu.SetFR(10, fr10);
    getf_sig.Execute(cpu, memory);
    assert_equal("Boot getf.sig should copy significand bytes", significand, cpu.GetGR(21));

    InstructionEx fcvt_fx = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::F_UNIT, 0x36ed0);
    assert_true("Boot raw F-unit fcvt.fx should decode", fcvt_fx.GetType() == InstructionType::FCVT_FX);
    assert_equal("Boot fcvt.fx destination FP register", 10, fcvt_fx.GetDst());
    assert_equal("Boot fcvt.fx source FP register", 8, fcvt_fx.GetSrc1());
    assert_string("Boot fcvt.fx disassembly",
                  "fcvt.fx f10 = f8",
                  fcvt_fx.GetDisassembly());

    uint8_t fr8[16] = {};
    const uint64_t convertedSignificand = 0x0fedcba987654321ULL;
    for (int i = 0; i < 8; ++i) {
        fr8[i] = static_cast<uint8_t>((convertedSignificand >> (i * 8)) & 0xff);
    }
    cpu.SetFR(8, fr8);
    fcvt_fx.Execute(cpu, memory);
    getf_sig.Execute(cpu, memory);
    assert_equal("Boot fcvt.fx should preserve significand for getf.sig", convertedSignificand, cpu.GetGR(21));

    InstructionEx mov_to_br = decoder.DecodeSlot(0xe0014a000ULL, UnitType::I_UNIT, 0x30700);
    assert_true("Boot raw mov-to-branch should decode", mov_to_br.GetType() == InstructionType::MOV_TO_BR);
    assert_equal("Boot mov-to-branch destination branch register", 0, mov_to_br.GetDst());
    assert_equal("Boot mov-to-branch source general register", 37, mov_to_br.GetSrc1());
    assert_string("Boot mov-to-branch disassembly",
                  "mov b0 = r37",
                  mov_to_br.GetDisassembly());

    cpu.SetGR(37, 0x123456789abcdef0ULL);
    mov_to_br.Execute(cpu, memory);
    assert_equal("Boot mov-to-branch should restore b0", 0x123456789abcdef0ULL, cpu.GetBR(0));

    InstructionEx shladd_scale5 = decoder.DecodeSlot(0x10088e1c200ULL, UnitType::I_UNIT, 0xa100);
    assert_true("Boot raw shladd scale-5 should decode",
                shladd_scale5.GetType() == InstructionType::SHLADD);
    assert_equal("Boot shladd scale-5 destination", 8, shladd_scale5.GetDst());
    assert_equal("Boot shladd scale-5 source", 14, shladd_scale5.GetSrc1());
    assert_equal("Boot shladd scale-5 addend", 14, shladd_scale5.GetSrc2());
    assert_equal("Boot shladd scale-5 count", 2, shladd_scale5.GetImmediate());
    assert_string("Boot shladd scale-5 disassembly",
                  "shladd r8 = r14, 2, r14",
                  shladd_scale5.GetDisassembly());

    cpu.SetGR(14, 7);
    shladd_scale5.Execute(cpu, memory);
    assert_equal("Boot shladd scale-5 should compute index * 5", 35, cpu.GetGR(8));

    InstructionEx shladd_scale40 = decoder.DecodeSlot(0x10091110400ULL, UnitType::I_UNIT, 0xa110);
    assert_true("Boot raw shladd scale-40 should decode",
                shladd_scale40.GetType() == InstructionType::SHLADD);
    assert_equal("Boot shladd scale-40 destination", 16, shladd_scale40.GetDst());
    assert_equal("Boot shladd scale-40 source", 8, shladd_scale40.GetSrc1());
    assert_equal("Boot shladd scale-40 addend", 17, shladd_scale40.GetSrc2());
    assert_equal("Boot shladd scale-40 count", 3, shladd_scale40.GetImmediate());
    assert_string("Boot shladd scale-40 disassembly",
                  "shladd r16 = r8, 3, r17",
                  shladd_scale40.GetDisassembly());

    cpu.SetGR(8, 35);
    cpu.SetGR(17, 0x1000);
    shladd_scale40.Execute(cpu, memory);
    assert_equal("Boot shladd scale-40 should compute base + index * 40", 0x1118, cpu.GetGR(16));

    InstructionEx zxt4_return = decoder.DecodeSlot(0xb0800200ULL, UnitType::I_UNIT, 0x34b10);
    assert_true("Boot raw zxt4 should decode", zxt4_return.GetType() == InstructionType::ZXT4);
    assert_equal("Boot zxt4 destination", 8, zxt4_return.GetDst());
    assert_equal("Boot zxt4 source", 8, zxt4_return.GetSrc1());
    assert_string("Boot zxt4 disassembly",
                  "zxt4 r8 = r8",
                  zxt4_return.GetDisassembly());

    cpu.SetGR(8, 0xffffffff80000001ULL);
    zxt4_return.Execute(cpu, memory);
    assert_equal("Boot zxt4 should clear high 32 bits", 0x80000001ULL, cpu.GetGR(8));

    InstructionEx mov_m_from_ar = decoder.DecodeSlot(0x2112400ac0ULL, UnitType::M_UNIT, 0x32a60);
    assert_true("Boot raw mov.m from AR should decode",
                mov_m_from_ar.GetType() == InstructionType::MOV_FROM_AR);
    assert_equal("Boot mov.m from AR destination", 43, mov_m_from_ar.GetDst());
    assert_equal("Boot mov.m from AR source application register", 36, mov_m_from_ar.GetSrc1());
    assert_string("Boot mov.m from AR disassembly",
                  "mov r43 = ar.36",
                  mov_m_from_ar.GetDisassembly());

    cpu.SetAR(36, 0x0123456789abcdefULL);
    cpu.SetGRNaT(43, true);
    mov_m_from_ar.Execute(cpu, memory);
    assert_equal("Boot mov.m from AR should copy application register", 0x0123456789abcdefULL, cpu.GetGR(43));
    assert_true("Boot mov.m from AR should clear destination NaT", !cpu.GetGRNaT(43));

    InstructionEx or_imm = decoder.DecodeSlot(0x10170e0e440ULL, UnitType::M_UNIT, 0x32590);
    assert_true("Boot raw OR immediate should decode", or_imm.GetType() == InstructionType::OR_IMM);
    assert_equal("Boot OR immediate destination", 17, or_imm.GetDst());
    assert_equal("Boot OR immediate source", 14, or_imm.GetSrc1());
    assert_equal("Boot OR immediate value", 7, or_imm.GetImmediate());
    assert_string("Boot OR immediate disassembly",
                  "or r17 = r14, 7",
                  or_imm.GetDisassembly());

    cpu.SetGR(14, 0x12340);
    or_imm.Execute(cpu, memory);
    assert_equal("Boot OR immediate should set low immediate bits", 0x12347, cpu.GetGR(17));

    InstructionEx zxt2_value = decoder.DecodeSlot(0x88800fc0ULL, UnitType::I_UNIT, 0x31c50);
    assert_true("Boot raw zxt2 should decode", zxt2_value.GetType() == InstructionType::ZXT2);
    assert_equal("Boot zxt2 destination", 63, zxt2_value.GetDst());
    assert_equal("Boot zxt2 source", 8, zxt2_value.GetSrc1());
    assert_string("Boot zxt2 disassembly",
                  "zxt2 r63 = r8",
                  zxt2_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffff807fULL);
    zxt2_value.Execute(cpu, memory);
    assert_equal("Boot zxt2 should keep low 16 bits", 0x807fULL, cpu.GetGR(63));

    InstructionEx sxt1_value = decoder.DecodeSlot(0xa0800200ULL, UnitType::I_UNIT, 0x31c60);
    assert_true("Boot raw sxt1 should decode", sxt1_value.GetType() == InstructionType::SXT1);
    assert_equal("Boot sxt1 destination", 8, sxt1_value.GetDst());
    assert_equal("Boot sxt1 source", 8, sxt1_value.GetSrc1());
    assert_string("Boot sxt1 disassembly",
                  "sxt1 r8 = r8",
                  sxt1_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffffff80ULL);
    sxt1_value.Execute(cpu, memory);
    assert_equal("Boot sxt1 should sign-extend byte", 0xffffffffffffff80ULL, cpu.GetGR(8));

    InstructionEx sxt2_value = decoder.DecodeSlot(0xa8800200ULL, UnitType::I_UNIT, 0x31c70);
    assert_true("Boot raw sxt2 should decode", sxt2_value.GetType() == InstructionType::SXT2);
    assert_equal("Boot sxt2 destination", 8, sxt2_value.GetDst());
    assert_equal("Boot sxt2 source", 8, sxt2_value.GetSrc1());
    assert_string("Boot sxt2 disassembly",
                  "sxt2 r8 = r8",
                  sxt2_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffff8001ULL);
    sxt2_value.Execute(cpu, memory);
    assert_equal("Boot sxt2 should sign-extend halfword", 0xffffffffffff8001ULL, cpu.GetGR(8));

    InstructionEx sxt4_value = decoder.DecodeSlot(0xb0800200ULL, UnitType::I_UNIT, 0x31c80);
    assert_true("Boot raw sxt4 should decode", sxt4_value.GetType() == InstructionType::SXT4);
    assert_equal("Boot sxt4 destination", 8, sxt4_value.GetDst());
    assert_equal("Boot sxt4 source", 8, sxt4_value.GetSrc1());
    assert_string("Boot sxt4 disassembly",
                  "sxt4 r8 = r8",
                  sxt4_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffff80000001ULL);
    sxt4_value.Execute(cpu, memory);
    assert_equal("Boot sxt4 should sign-extend word", 0xffffffff80000001ULL, cpu.GetGR(8));

    InstructionEx cloop = decoder.DecodeSlot(0xb1ffffc140ULL, UnitType::B_UNIT, 0xa120);
    assert_true("Boot raw counted-loop branch should decode",
                cloop.GetType() == InstructionType::BR_CLOOP);
    assert_equal("Boot counted-loop target", 0xa100, cloop.GetBranchTarget());
    assert_string("Boot counted-loop disassembly",
                  "br.cloop 0xa100",
                  cloop.GetDisassembly());

    cpu.SetAR(65, 2);
    cloop.Execute(cpu, memory);
    assert_equal("br.cloop should decrement ar.lc when nonzero", 1, cpu.GetAR(65));

    InstructionEx chk_a_clr = decoder.DecodeSlot(0xa00018280ULL, UnitType::M_UNIT, 0xeb30);
    assert_true("Boot raw chk.a.clr should decode",
                chk_a_clr.GetType() == InstructionType::CHK_A_CLR);
    assert_equal("Boot chk.a.clr checked register", 10, chk_a_clr.GetDst());
    assert_equal("Boot chk.a.clr recovery target", 0xebf0, chk_a_clr.GetBranchTarget());
    assert_string("Boot chk.a.clr disassembly",
                  "chk.a.clr r10, 0xebf0",
                  chk_a_clr.GetDisassembly());

    cpu.SetGR(10, 0x1122334455667788ULL);
    chk_a_clr.Execute(cpu, memory);
    assert_equal("chk.a.clr stub should leave checked register unchanged",
                 0x1122334455667788ULL, cpu.GetGR(10));

    InstructionEx load_options_chars = decoder.DecodeSlot(0xa5f2104846ULL, UnitType::I_UNIT, 0x86b0);
    assert_true("Boot raw load-options byte-to-char extract should decode",
                load_options_chars.GetType() == InstructionType::EXTR);
    assert_equal("Boot load-options extract destination", 33, load_options_chars.GetDst());
    assert_equal("Boot load-options extract source", 33, load_options_chars.GetSrc1());
    assert_equal("Boot load-options extract position", 1, load_options_chars.GetImmediate() & 0x3f);
    assert_equal("Boot load-options extract encoded length", 62, load_options_chars.GetImmediate() >> 6);
    assert_string("Boot load-options extract disassembly",
                  "extr r33 = r33, 1, 63",
                  load_options_chars.GetDisassembly());

    cpu.SetGR(2, 1);
    cpu.SetGR(33, 2);
    load_options_chars.Execute(cpu, memory, true);
    assert_equal("Boot load-options extract should ignore stale r2 and halve byte count",
                 1, cpu.GetGR(33));

    InstructionEx shrp = decoder.DecodeSlot(0xadf2104846ULL, UnitType::I_UNIT, 0x86b0);
    assert_true("Boot raw shrp should decode",
                shrp.GetType() == InstructionType::SHRP);
    assert_equal("Boot shrp destination", 33, shrp.GetDst());
    assert_equal("Boot shrp high source", 2, shrp.GetSrc1());
    assert_equal("Boot shrp low source", 33, shrp.GetSrc2());
    assert_equal("Boot shrp count", 62, shrp.GetImmediate());
    assert_string("Boot shrp disassembly",
                  "shrp r33 = r2, r33, 62",
                  shrp.GetDisassembly());

    cpu.SetGR(2, 0x0123456789abcdefULL);
    cpu.SetGR(33, 0xf000000000000000ULL);
    shrp.Execute(cpu, memory, true);
    assert_equal("shrp should concatenate high:low and keep shifted low half",
                 0x048d159e26af37bfULL, cpu.GetGR(33));

    InstructionEx loop_cmp = decoder.DecodeSlot(0x1a03a11e180ULL, UnitType::I_UNIT, 0x86c0);
    assert_true("Loop cmp.ltu should decode as register compare",
                loop_cmp.GetType() == InstructionType::CMP_LTU);
    assert_equal("Loop cmp.ltu p1 decode", 6, loop_cmp.GetDst());
    assert_equal("Loop cmp.ltu lhs register", 15, loop_cmp.GetSrc1());
    assert_equal("Loop cmp.ltu rhs register", 33, loop_cmp.GetSrc2());
    assert_equal("Loop cmp.ltu p2 decode", 7, loop_cmp.GetSrc3());
    assert_true("Loop cmp.ltu should not be immediate", !loop_cmp.HasImmediate());
    assert_string("Loop cmp.ltu disassembly",
                  "cmp.ltu p6, p7 = r15, r33",
                  loop_cmp.GetDisassembly());

    cpu.SetGR(15, 3);
    cpu.SetGR(33, 4);
    loop_cmp.Execute(cpu, memory);
    assert_true("Loop cmp.ltu should keep p6 true while index is below bound", cpu.GetPR(6));
    assert_true("Loop cmp.ltu should clear p7 while index is below bound", !cpu.GetPR(7));

    cpu.SetGR(15, 4);
    cpu.SetGR(33, 4);
    loop_cmp.Execute(cpu, memory);
    assert_true("Loop cmp.ltu should clear p6 at loop bound", !cpu.GetPR(6));
    assert_true("Loop cmp.ltu should set p7 at loop bound", cpu.GetPR(7));

    InstructionEx next_cmp = decoder.DecodeSlot(0x1a0521202c0ULL, UnitType::I_UNIT, 0x86d0);
    assert_true("Loop next cmp.ltu should decode",
                next_cmp.GetType() == InstructionType::CMP_LTU);
    assert_equal("Loop next cmp.ltu p1 decode", 11, next_cmp.GetDst());
    assert_equal("Loop next cmp.ltu lhs register", 16, next_cmp.GetSrc1());
    assert_equal("Loop next cmp.ltu rhs register", 33, next_cmp.GetSrc2());
    assert_equal("Loop next cmp.ltu p2 decode", 10, next_cmp.GetSrc3());
    assert_string("Loop next cmp.ltu disassembly",
                  "cmp.ltu p11, p10 = r16, r33",
                  next_cmp.GetDisassembly());

    InstructionEx space_cmp = decoder.DecodeSlot(0x1ce30e411c0ULL, UnitType::I_UNIT, 0x86e0);
    assert_true("Loop space compare should decode as cmp4.ne",
                space_cmp.GetType() == InstructionType::CMP4_NE);
    assert_true("Loop space compare should use or.andcm completer",
                space_cmp.GetCompareCompleter() == CompareCompleter::OR_ANDCM);
    assert_equal("Loop space compare p1 decode", 7, space_cmp.GetDst());
    assert_equal("Loop space compare source register", 14, space_cmp.GetSrc2());
    assert_equal("Loop space compare immediate", 32, space_cmp.GetImmediate());
    assert_string("Loop space compare disassembly",
                  "cmp4.ne.or.andcm p7, p6 = 32, r14",
                  space_cmp.GetDisassembly());

    cpu.SetPR(6, true);
    cpu.SetPR(7, false);
    cpu.SetGR(14, 32);
    space_cmp.Execute(cpu, memory);
    assert_true("cmp4.ne.or.andcm should leave p6 true for a space", cpu.GetPR(6));
    assert_true("cmp4.ne.or.andcm should leave p7 false for a space", !cpu.GetPR(7));

    cpu.SetPR(6, true);
    cpu.SetPR(7, false);
    cpu.SetGR(14, 'A');
    space_cmp.Execute(cpu, memory);
    assert_true("cmp4.ne.or.andcm should clear p6 for non-space", !cpu.GetPR(6));
    assert_true("cmp4.ne.or.andcm should set p7 for non-space", cpu.GetPR(7));

    InstructionEx nul_cmp = decoder.DecodeSlot(0x1cc40e00240ULL, UnitType::I_UNIT, 0x86e0);
    assert_true("Loop null compare should decode as cmp4.eq",
                nul_cmp.GetType() == InstructionType::CMP4_EQ);
    assert_equal("Loop null compare p1 decode", 9, nul_cmp.GetDst());
    assert_equal("Loop null compare source register", 14, nul_cmp.GetSrc2());
    assert_equal("Loop null compare immediate", 0, nul_cmp.GetImmediate());
    assert_string("Loop null compare disassembly",
                  "cmp4.eq p9, p8 = 0, r14",
                  nul_cmp.GetDisassembly());

    std::cout << "  ? Latest boot-log raw instructions passed" << std::endl;
}

void test_memory_bounds_throw() {
    std::cout << "Testing memory bounds diagnostics..." << std::endl;

    Memory memory(0x1000);
    uint64_t value = 0;
    bool threw = false;

    try {
        memory.Read(0x1000, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    } catch (const std::out_of_range& ex) {
        threw = std::string(ex.what()).find("out of bounds") != std::string::npos;
    }
    assert_true("Out-of-range read should throw instead of asserting", threw);

    threw = false;
    try {
        memory.Read(0xffc, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    } catch (const std::out_of_range& ex) {
        threw = std::string(ex.what()).find("exceeds bounds") != std::string::npos;
    }
    assert_true("Overlapping read should throw instead of asserting", threw);

    std::cout << "  ? Memory bounds diagnostics passed" << std::endl;
}

void test_application_register_moves() {
    std::cout << "Testing application register moves..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    InstructionEx mov_to_pfs = decoder.DecodeSlot(0x15404c000ULL, UnitType::M_UNIT, 0x306e0);
    assert_true("Boot raw mov-to-ar.pfs should decode in M-unit",
                mov_to_pfs.GetType() == InstructionType::MOV_TO_AR);
    assert_equal("mov-to-ar.pfs application register", 64, mov_to_pfs.GetDst());
    assert_equal("mov-to-ar.pfs source register", 38, mov_to_pfs.GetSrc1());
    assert_string("mov-to-ar.pfs disassembly",
                  "mov ar.pfs = r38",
                  mov_to_pfs.GetDisassembly());

    cpu.SetGR(38, 0x12345);
    mov_to_pfs.Execute(cpu, memory);
    assert_equal("mov-to-ar.pfs should update CFM", 0x12345, cpu.GetCFM());
    assert_equal("mov-to-ar.pfs should update AR storage", 0x12345, cpu.GetAR(64));

    InstructionEx mov_to_pfs_i = decoder.DecodeSlot(0x15404a000ULL, UnitType::I_UNIT, 0x35400);
    assert_true("Boot raw mov-to-ar.pfs should decode in I-unit",
                mov_to_pfs_i.GetType() == InstructionType::MOV_TO_AR);
    assert_equal("mov-to-ar.pfs I-unit source register", 37, mov_to_pfs_i.GetSrc1());

    InstructionEx mov_to_pr = decoder.DecodeSlot(0x16ff04bfc0ULL, UnitType::I_UNIT, 0x2f0c0);
    assert_true("Boot raw mov-to-predicate should decode",
                mov_to_pr.GetType() == InstructionType::MOV_TO_PR);
    assert_equal("mov-to-predicate source register", 37, mov_to_pr.GetSrc1());
    assert_equal("mov-to-predicate mask", 0xfffffffffffffffeULL, mov_to_pr.GetImmediate());
    assert_string("mov-to-predicate disassembly",
                  "mov pr = r37, 0xfffffffffffffffe",
                  mov_to_pr.GetDisassembly());

    cpu.SetGR(37, (1ULL << 1) | (1ULL << 16) | (1ULL << 63));
    cpu.SetPR(2, true);
    cpu.SetPR(10, true);
    mov_to_pr.Execute(cpu, memory);
    assert_true("mov-to-predicate should keep PR0 true", cpu.GetPR(0));
    assert_true("mov-to-predicate should set PR1 from source bit", cpu.GetPR(1));
    assert_true("mov-to-predicate should clear PR2 from source bit", !cpu.GetPR(2));
    assert_true("mov-to-predicate should clear PR10 from source bit", !cpu.GetPR(10));
    assert_true("mov-to-predicate should set rotating PR16", cpu.GetPR(16));
    assert_true("mov-to-predicate should set high rotating predicate", cpu.GetPR(63));

    InstructionEx mov_from_ip = decoder.DecodeSlot(build_mov_from_ip_slot(0, 11), UnitType::I_UNIT, 0x2f000);
    assert_true("mov from ip should decode", mov_from_ip.GetType() == InstructionType::MOV_FROM_IP);
    assert_equal("mov from ip destination register", 11, mov_from_ip.GetDst());
    assert_string("mov from ip disassembly",
                  "mov r11 = ip",
                  mov_from_ip.GetDisassembly());

    cpu.SetIP(0x123456789abcdef0ULL);
    mov_from_ip.Execute(cpu, memory);
    assert_equal("mov from ip should copy the current IP", 0x123456789abcdef0ULL, cpu.GetGR(11));

    InstructionEx mov_from_pr = decoder.DecodeSlot(build_mov_from_pr_slot(0, 12), UnitType::I_UNIT, 0x2f010);
    assert_true("mov from pr should decode", mov_from_pr.GetType() == InstructionType::MOV_FROM_PR);
    assert_equal("mov from pr destination register", 12, mov_from_pr.GetDst());
    assert_string("mov from pr disassembly",
                  "mov r12 = pr",
                  mov_from_pr.GetDisassembly());

    cpu.SetPR(1, true);
    cpu.SetPR(16, true);
    cpu.SetPR(63, true);
    mov_from_pr.Execute(cpu, memory);
    assert_equal("mov from pr should pack predicate bits into a GR",
                 0x8000000000010002ULL, cpu.GetGR(12));

    InstructionEx filler_m_nop = decoder.DecodeSlot(0x2b86ULL, UnitType::M_UNIT, 0x42008);
    assert_true("Final-loop predicated M nop should decode",
                filler_m_nop.GetType() == InstructionType::NOP);
    assert_equal("Final-loop M nop predicate", 6, filler_m_nop.GetPredicate());

    InstructionEx filler_i_nop = decoder.DecodeSlot(0x0ULL, UnitType::I_UNIT, 0x42008);
    assert_true("Final-loop zero I nop should decode",
                filler_i_nop.GetType() == InstructionType::NOP);

    std::cout << "  ? Application register moves passed" << std::endl;
}

void test_test_instructions() {
    std::cout << "Testing test-bit/test-NaT instructions..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(10, 0x20);

    InstructionEx tbit_z(InstructionType::TBIT_Z, UnitType::I_UNIT);
    tbit_z.SetOperands4(1, 10, 0, 2);
    tbit_z.SetImmediate(5);
    tbit_z.Execute(cpu, memory);
    assert_true("TBIT.Z: p1 should be false when selected bit is one", !cpu.GetPR(1));
    assert_true("TBIT.Z: p2 should be true when selected bit is one", cpu.GetPR(2));

    InstructionEx tbit_nz(InstructionType::TBIT_NZ, UnitType::I_UNIT);
    tbit_nz.SetOperands4(3, 10, 0, 4);
    tbit_nz.SetImmediate(5);
    tbit_nz.Execute(cpu, memory);
    assert_true("TBIT.NZ: p3 should be true when selected bit is one", cpu.GetPR(3));
    assert_true("TBIT.NZ: p4 should be false when selected bit is one", !cpu.GetPR(4));

    cpu.SetGR(11, 0x1234);
    cpu.SetGRNaT(11, false);

    InstructionEx tnat_z(InstructionType::TNAT_Z, UnitType::I_UNIT);
    tnat_z.SetOperands4(5, 11, 0, 6);
    tnat_z.Execute(cpu, memory);
    assert_true("TNAT.Z: p5 should be true for non-NaT register", cpu.GetPR(5));
    assert_true("TNAT.Z: p6 should be false for non-NaT register", !cpu.GetPR(6));

    cpu.SetGRNaT(11, true);
    InstructionEx tnat_nz(InstructionType::TNAT_NZ, UnitType::I_UNIT);
    tnat_nz.SetOperands4(7, 11, 0, 8);
    tnat_nz.Execute(cpu, memory);
    assert_true("TNAT.NZ: p7 should be true for NaT register", cpu.GetPR(7));
    assert_true("TNAT.NZ: p8 should be false for NaT register", !cpu.GetPR(8));

    InstructionDecoder decoder;
    InstructionEx decoded_tbit = decoder.DecodeSlot(build_tbit_z_slot(0, 9, 10, 10, 5),
                                                    UnitType::I_UNIT, 0);
    assert_true("TBIT.Z slot should decode", decoded_tbit.GetType() == InstructionType::TBIT_Z);
    assert_equal("TBIT.Z p1 decode", 9, decoded_tbit.GetDst());
    assert_equal("TBIT.Z source decode", 10, decoded_tbit.GetSrc1());
    assert_equal("TBIT.Z p2 decode", 10, decoded_tbit.GetSrc3());
    assert_equal("TBIT.Z position decode", 5, decoded_tbit.GetImmediate());

    InstructionEx decoded_tnat = decoder.DecodeSlot(build_tnat_z_slot(0, 11, 12, 11),
                                                    UnitType::I_UNIT, 0);
    assert_true("TNAT.Z slot should decode", decoded_tnat.GetType() == InstructionType::TNAT_Z);
    assert_equal("TNAT.Z p1 decode", 11, decoded_tnat.GetDst());
    assert_equal("TNAT.Z source decode", 11, decoded_tnat.GetSrc1());
    assert_equal("TNAT.Z p2 decode", 12, decoded_tnat.GetSrc3());

    std::cout << "  ? Test instructions passed" << std::endl;
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
    assert_equal("ANDCM", 0xF000, cpu.GetGR(6));
    
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
        test_compare_ne_decoder();
        test_latest_boot_log_blockers();
        test_memory_bounds_throw();
        test_application_register_moves();
        test_test_instructions();
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
