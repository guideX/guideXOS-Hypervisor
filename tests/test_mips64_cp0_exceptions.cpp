// MIPS64-2 focused test: CP0 state, exception entry/return, trapping
// arithmetic, HI/LO multiply/divide, extended branches (both byte orders),
// serialization and a real encoded exception-handler demo program.
//
// Uses genuine MIPS64r2 instruction words executed against a sparse test
// memory that backs arbitrary 64-bit addresses (code, vectors, data).
// Deterministic and bounded.

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "IMemory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_mmu_setup.h"

using namespace mips64;
using namespace mips64_test;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            ++g_pass;                                                                \
        } else {                                                                     \
            ++g_fail;                                                                \
            std::cout << "FAIL " << __func__ << " line " << __LINE__ << ": " #cond    \
                      << "\n";                                                       \
        }                                                                            \
    } while (0)

// -- sparse test memory: backs any 64-bit address, throws when unmapped --------
class SparseMemory : public ia64::IMemory {
public:
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        for (size_t i = 0; i < size; ++i) {
            auto it = storage_.find(address + i);
            if (it == storage_.end()) {
                throw std::out_of_range("sparse memory: unmapped read");
            }
            dest[i] = it->second;
        }
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            storage_[address + i] = src[i];
        }
    }
    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override {
        Write(address, buffer, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override {
        if (!buffer.empty()) {
            Write(address, buffer.data(), buffer.size());
        }
    }
    size_t GetTotalSize() const override { return storage_.size(); }
    void Clear() override { storage_.clear(); }
    const uint8_t* GetRawData() const override { return nullptr; }

private:
    std::unordered_map<uint64_t, uint8_t> storage_;
};

// -- tiny MIPS64r2 encoders ------------------------------------------------------
static uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | funct;
}

static uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | imm;
}

static uint32_t encodeCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel) {
    return (static_cast<uint32_t>(0x10) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | (static_cast<uint32_t>(rd) << 11) | sel;
}

static void storeWord(ia64::IMemory& mem, uint64_t addr, uint32_t word, bool littleEndian) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[littleEndian ? i : 3 - i] = static_cast<uint8_t>((word >> (8 * i)) & 0xFFU);
    }
    mem.Write(addr, bytes, 4);
}

static void fillZeros(ia64::IMemory& mem, uint64_t addr, size_t size) {
    std::vector<uint8_t> zeros(size, 0);
    mem.Write(addr, zeros.data(), zeros.size());
}

static bool stepOk(Mips64Cpu& cpu, ia64::IMemory& mem) {
    return cpu.step(mem) == Mips64StepResult::Continue;
}

static bool stepFault(Mips64Cpu& cpu, ia64::IMemory& mem, Mips64Fault expected) {
    return cpu.step(mem) == Mips64StepResult::Fault && cpu.fault() == expected;
}

static const uint64_t kBev1Vector = 0xFFFFFFFFBFC00380ULL;  // BEV=1 general exception vector (base 0xBFC00200 + 0x180)
static const uint64_t kBev0Vector = 0xFFFFFFFF80000180ULL;

// -- 1. CP0 reset values ----------------------------------------------------------
static void testCp0ResetValues() {
    Mips64Cpu cpu;
    CHECK(cpu.cp0Status() == 0x00400004ULL);  // BEV | ERL
    CHECK(cpu.cp0Cause() == 0);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(cpu.cp0ErrorEpc() == 0);
    CHECK(cpu.cp0BadVAddr() == 0);
    CHECK(cpu.cp0EBase() == 0x80000000ULL);
    CHECK(cpu.cp0Count() == 0);
    CHECK(cpu.cp0Compare() == 0);
    CHECK(!cpu.statusExl());
    CHECK(cpu.statusErl());
    CHECK(cpu.statusBev());
    CHECK(!cpu.causeBd());
    CHECK(cpu.causeExcCode() == 0);
    CHECK(cpu.exceptionVector() == kBev1Vector);
    // Reset is deterministic even after heavy state mutation.
    cpu.setPc(0x1234);
    cpu.setHi(1);
    cpu.setLo(2);
    cpu.reset();
    CHECK(cpu.cp0Status() == 0x00400004ULL);
    CHECK(cpu.cp0EBase() == 0x80000000ULL);
    CHECK(cpu.pc() == 0 && cpu.hi() == 0 && cpu.lo() == 0);
    CHECK(cpu.fault() == Mips64Fault::None);
}

// -- 2. CP0 supported writable fields ---------------------------------------------
static void testCp0WritableFields() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // MTC0 Status with all ones: only the writable mask sticks.
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(mem, 0, encodeCp0(4, 8, 12, 0), true);  // mtc0 $8, $12, 0
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Status() == 0x0040FFFFULL);
    // DMTC0 Status sets the same mask from a full 64-bit value.
    cpu.writeGpr(8, 0);
    storeWord(mem, 4, encodeCp0(5, 8, 12, 0), true);  // dmtc0 $8, $12, 0
    cpu.setPc(4);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Status() == 0);
    CHECK(!cpu.statusBev() && !cpu.statusExl() && !cpu.statusErl());
    CHECK(cpu.exceptionVector() == kBev0Vector);  // BEV cleared -> EBase vector

    // Cause: only SW interrupt bits IP[1:0] are software-writable.
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(mem, 8, encodeCp0(4, 8, 13, 0), true);  // mtc0 $8, $13, 0
    cpu.setPc(8);
    CHECK(stepOk(cpu, mem));
    CHECK((cpu.cp0Cause() & 0xFFFFFFFFFFFFFC00ULL) == 0);  // only IP1:0/BD/ExcCode readable
    CHECK((cpu.cp0Cause() & 0x300ULL) == 0x300ULL);  // IP1:0 set
    // Clearing them again keeps ExcCode/BD management intact.
    cpu.writeGpr(8, 0);
    cpu.setPc(8);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Cause() == 0);

    // EBase: only bits 31:12 stick; low 12 (incl. CPUNum) read zero.
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(mem, 12, encodeCp0(5, 8, 15, 1), true);  // dmtc0 $8, $15, 1
    cpu.setPc(12);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0EBase() == 0xFFFFF000ULL);
    CHECK(cpu.exceptionVector() == 0xFFFFFFFFFFFFF180ULL);

    // EPC / ErrorEPC / BadVAddr hold full 64-bit values via DMTC0.
    cpu.writeGpr(8, 0x123456789ABCDEF0ULL);
    storeWord(mem, 16, encodeCp0(5, 8, 14, 0), true);  // dmtc0 $8, $14 (EPC)
    storeWord(mem, 20, encodeCp0(5, 8, 30, 0), true);  // dmtc0 $8, $30 (ErrorEPC)
    storeWord(mem, 24, encodeCp0(5, 8, 8, 0), true);   // dmtc0 $8, $8 (BadVAddr)
    cpu.setPc(16);
    CHECK(stepOk(cpu, mem));
    cpu.setPc(20);
    CHECK(stepOk(cpu, mem));
    cpu.setPc(24);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Epc() == 0x123456789ABCDEF0ULL);
    CHECK(cpu.cp0ErrorEpc() == 0x123456789ABCDEF0ULL);
    CHECK(cpu.cp0BadVAddr() == 0x123456789ABCDEF0ULL);

    // Count/Compare keep the low 32 bits (upper bits dropped).
    cpu.writeGpr(8, 0x1FFFFFFFFULL);
    storeWord(mem, 28, encodeCp0(5, 8, 9, 0), true);   // dmtc0 Count
    storeWord(mem, 32, encodeCp0(5, 8, 11, 0), true);  // dmtc0 Compare
    cpu.setPc(28);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Count() == 0xFFFFFFFFULL);  // read before the next step ticks it
    cpu.setPc(32);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Compare() == 0xFFFFFFFFULL);
}

// -- 3. MFC0/DMFC0 32-bit vs 64-bit transfer semantics ------------------------------
static void testCp0TransferWidths() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // Plant a 64-bit EPC with nonzero upper half, then read both widths.
    cpu.writeGpr(8, 0xFFFFFFFF80001234ULL);
    storeWord(mem, 0, encodeCp0(5, 8, 14, 0), true);  // dmtc0 -> EPC
    storeWord(mem, 4, encodeCp0(0, 9, 14, 0), true);   // mfc0 $9, $14
    storeWord(mem, 8, encodeCp0(1, 10, 14, 0), true);  // dmfc0 $10, $14
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(9) == 0xFFFFFFFF80001234ULL);  // low word sign-extended
    CHECK(cpu.readGpr(10) == 0xFFFFFFFF80001234ULL);

    cpu.writeGpr(8, 0x12348000ULL);  // positive low word
    cpu.setPc(0);
    CHECK(stepOk(cpu, mem));  // dmtc0 EPC = 0x12348000
    cpu.setPc(4);
    CHECK(stepOk(cpu, mem));  // mfc0
    CHECK(cpu.readGpr(9) == 0x12348000ULL);  // zero upper after sign extension

    // MTC0 sign-extends the low word into the register.
    cpu.writeGpr(8, 0xDEAD80001234ULL);  // low32 = 0x80001234 (negative)
    storeWord(mem, 12, encodeCp0(4, 8, 14, 0), true);  // mtc0 -> EPC
    cpu.setPc(12);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Epc() == 0xFFFFFFFF80001234ULL);
}

// -- 4. unsupported CP0 register/selector handling -----------------------------------
static void testUnsupportedCp0() {
    SparseMemory mem;
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xABC);
    cpu.writeGpr(9, 0x1234);  // sentinel: must not be overwritten
    CHECK(setupKusegIdentityMap(cpu, mem));
    // Genuinely unsupported CP0 register/selector pairs: PRId, Status sel 1,
    // Config, ErrorEPC sel 1, Cause sel 1, BadVAddr sel 1. Index (0,0) is
    // supported in MIPS64-3 and is exercised by testCp0TlbRegisters.
    const uint32_t words[6] = {
        encodeCp0(0, 9, 15, 0), encodeCp0(0, 9, 12, 1), encodeCp0(1, 9, 16, 0),
        encodeCp0(4, 8, 30, 1), encodeCp0(0, 9, 13, 1), encodeCp0(0, 9, 8, 1),
    };
    for (int i = 0; i < 6; ++i) {
        storeWord(mem, static_cast<uint64_t>(i) * 4, words[i], true);
        cpu.setPc(static_cast<uint64_t>(i) * 4);
        CHECK(stepFault(cpu, mem, Mips64Fault::Cp0Violation));
        CHECK(cpu.readGpr(9) == 0x1234);  // no destination write
        CHECK(cpu.pc() == static_cast<uint64_t>(i) * 4);
        CHECK(!cpu.statusExl());  // no exception entry
        CHECK(!cpu.lastError().empty());
    }
    // COP0 with a reserved rs value is a reserved encoding (-> RI entry).
    storeWord(mem, 64, encodeCp0(2, 0, 0, 0), true);
    cpu.setPc(64);
    CHECK(stepFault(cpu, mem, Mips64Fault::ReservedInstruction));
    CHECK(cpu.causeExcCode() == 10);
    // Malformed ERET (rt != 0) is likewise reserved.
    storeWord(mem, 68, 0x42010018U, true);
    cpu.setPc(68);
    CHECK(stepFault(cpu, mem, Mips64Fault::ReservedInstruction));
    CHECK(cpu.causeExcCode() == 10);
}

// -- 5. trapping arithmetic: overflow entries + exact boundary behavior -----------------
static void testTrappingOverflow() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // ADD 0x7FFFFFFF + 1 overflows; ADDU-style wrap must NOT trap.
    cpu.writeGpr(8, 0x7FFFFFFFULL);
    cpu.writeGpr(9, 1);
    cpu.writeGpr(10, 0xDEADULL);  // sentinel
    storeWord(mem, 0, encodeR(8, 9, 10, 0, 0x20), true);  // add $10,$8,$9
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(stepFault(cpu, mem, Mips64Fault::OverflowTrap));
    CHECK(cpu.causeExcCode() == 12);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(!cpu.causeBd());
    CHECK(cpu.statusExl());
    CHECK(cpu.readGpr(10) == 0xDEADULL);  // no write on overflow
    CHECK(cpu.pc() == kBev1Vector);

    // Non-overflowing ADD retires with a sign-extended word result:
    // 0x40000000 + 0x3FFFFFFF = 0x7FFFFFFF (INT32_MAX, in range).
    // (0x40000000 + 0x40000000 would genuinely overflow signed 32-bit.)
    Mips64Cpu cpu2;
    CHECK(setupKusegIdentityMap(cpu2, mem));
    cpu2.writeGpr(8, 0x40000000ULL);
    cpu2.writeGpr(9, 0x3FFFFFFFULL);
    storeWord(mem, 64, encodeR(8, 9, 10, 0, 0x20), true);
    cpu2.setPc(64);
    CHECK(stepOk(cpu2, mem));
    CHECK(cpu2.readGpr(10) == 0x7FFFFFFFULL);
}

// -- (overflow continued: corrected expectations) --------------------------------------
static void testTrappingOverflow2() {
    SparseMemory mem;
    // ADD INT32_MIN + (-1) overflows as well.
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0xFFFFFFFF80000000ULL);
    cpu.writeGpr(9, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(mem, 0, encodeR(8, 9, 10, 0, 0x20), true);
    CHECK(stepFault(cpu, mem, Mips64Fault::OverflowTrap));
    CHECK(cpu.causeExcCode() == 12);

    // ADDI boundary: 0x7FFF + 1 (imm) overflows; 0x7FFE + 1 is fine.
    Mips64Cpu c2;
    c2.writeGpr(8, 0x7FFF);
    storeWord(mem, 0, encodeI(0x08, 8, 10, 1), true);  // addi $10,$8,1
    CHECK(setupKusegIdentityMap(c2, mem));
    CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(10) == 0x8000);
    Mips64Cpu c3;
    c3.writeGpr(8, 0x7FFFFFFF);
    storeWord(mem, 4, encodeI(0x08, 8, 10, 1), true);
    c3.setPc(4);
    CHECK(setupKusegIdentityMap(c3, mem));
    CHECK(stepFault(c3, mem, Mips64Fault::OverflowTrap));
    CHECK(c3.cp0Epc() == 4);

    // SUB: INT32_MIN - 1 overflows; 5 - 10 is fine (-5).
    Mips64Cpu c4;
    CHECK(setupKusegIdentityMap(c4, mem));
    c4.writeGpr(8, 0xFFFFFFFF80000000ULL);
    c4.writeGpr(9, 1);
    storeWord(mem, 8, encodeR(8, 9, 10, 0, 0x22), true);  // sub
    c4.setPc(8);
    CHECK(stepFault(c4, mem, Mips64Fault::OverflowTrap));
    Mips64Cpu c5;
    CHECK(setupKusegIdentityMap(c5, mem));
    c5.writeGpr(8, 5);
    c5.writeGpr(9, 10);
    c5.setPc(8);
    CHECK(stepOk(c5, mem));
    CHECK(c5.readGpr(10) == 0xFFFFFFFFFFFFFFFBULL);

    // DADD: INT64_MAX + 1 overflows; DADDU-style wrap is separate.
    Mips64Cpu c6;
    c6.writeGpr(8, 0x7FFFFFFFFFFFFFFFULL);
    c6.writeGpr(9, 1);
    storeWord(mem, 12, encodeR(8, 9, 10, 0, 0x2C), true);  // dadd
    c6.setPc(12);
    CHECK(setupKusegIdentityMap(c6, mem));
    CHECK(stepFault(c6, mem, Mips64Fault::OverflowTrap));
    CHECK(c6.causeExcCode() == 12);

    // DADDI: INT64_MIN + (-1) overflows; small adds retire.
    Mips64Cpu c7;
    CHECK(setupKusegIdentityMap(c7, mem));
    c7.writeGpr(8, 0x8000000000000000ULL);
    storeWord(mem, 16, encodeI(0x18, 8, 10, 0xFFFF), true);  // daddi $10,$8,-1
    c7.setPc(16);
    CHECK(stepFault(c7, mem, Mips64Fault::OverflowTrap));
    Mips64Cpu c8;
    CHECK(setupKusegIdentityMap(c8, mem));
    c8.writeGpr(8, 100);
    c8.setPc(16);
    CHECK(stepOk(c8, mem));
    CHECK(c8.readGpr(10) == 99);

    // DSUB: INT64_MIN - 1 overflows.
    Mips64Cpu c9;
    CHECK(setupKusegIdentityMap(c9, mem));
    c9.writeGpr(8, 0x8000000000000000ULL);
    c9.writeGpr(9, 1);
    storeWord(mem, 20, encodeR(8, 9, 10, 0, 0x2E), true);  // dsub
    c9.setPc(20);
    CHECK(stepFault(c9, mem, Mips64Fault::OverflowTrap));
    Mips64Cpu c10;
    CHECK(setupKusegIdentityMap(c10, mem));
    c10.writeGpr(8, 0);
    c10.writeGpr(9, 1);
    c10.setPc(20);
    CHECK(stepOk(c10, mem));
    CHECK(c10.readGpr(10) == 0xFFFFFFFFFFFFFFFFULL);
}

// -- 6. SYSCALL and BREAK --------------------------------------------------------------
static void testSyscallBreak() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    storeWord(mem, 0, 0x0000000CU, true);  // syscall
    storeWord(mem, 4, 0x0000000DU, true);  // break
    CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu.causeExcCode() == 8);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(!cpu.causeBd());
    CHECK(cpu.statusExl());
    CHECK(cpu.pc() == kBev1Vector);

    // BREAK with a code field still traps as Bp (code ignored).
    Mips64Cpu cpu2;
    CHECK(setupKusegIdentityMap(cpu2, mem));
    cpu2.setPc(4);
    CHECK(stepFault(cpu2, mem, Mips64Fault::BreakTrap));
    CHECK(cpu2.causeExcCode() == 9);
    CHECK(cpu2.cp0Epc() == 4);
    CHECK(cpu2.pc() == kBev1Vector);
}

// -- 7. reserved instructions raise RI ----------------------------------------------------
static void testReservedInstructionEntry() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    storeWord(mem, 0, encodeR(0, 0, 0, 0, 0x05), true);  // reserved SPECIAL
    storeWord(mem, 4, encodeI(0x1D, 0, 0, 0), true);     // reserved primary
    CHECK(stepFault(cpu, mem, Mips64Fault::ReservedInstruction));
    CHECK(cpu.causeExcCode() == 10);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(!cpu.causeBd());
    CHECK(cpu.statusExl());
    CHECK(cpu.pc() == kBev1Vector);
    // Second entry while EXL is set: nested, EPC preserved.
    Mips64Cpu cpu2;
    CHECK(setupKusegIdentityMap(cpu2, mem));
    cpu2.setPc(4);
    CHECK(stepFault(cpu2, mem, Mips64Fault::ReservedInstruction));
    CHECK(cpu2.causeExcCode() == 10);
    CHECK(cpu2.cp0Epc() == 4);
}

// -- 8/9. misaligned fetch/load/store entries ----------------------------------------------
static void testMisalignedEntries() {
    SparseMemory mem;
    fillZeros(mem, 0, 64);
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.setPc(2);  // misaligned fetch
    CHECK(stepFault(cpu, mem, Mips64Fault::MisalignedFetch));
    CHECK(cpu.causeExcCode() == 4);
    CHECK(cpu.cp0BadVAddr() == 2);
    CHECK(cpu.cp0Epc() == 2);

    // Misaligned LW: base 0x1000 + 1.
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0x1000);
    storeWord(mem, 0, encodeI(0x23, 8, 9, 1), true);  // lw $9, 1($8)
    CHECK(stepFault(c2, mem, Mips64Fault::MisalignedLoad));
    CHECK(c2.causeExcCode() == 4);
    CHECK(c2.cp0BadVAddr() == 0x1001);
    CHECK(c2.cp0Epc() == 0);
    CHECK(c2.readGpr(9) == 0);  // no partial write

    // Misaligned SW -> AdES.
    Mips64Cpu c3;
    CHECK(setupKusegIdentityMap(c3, mem));
    c3.writeGpr(8, 0x1000);
    storeWord(mem, 4, encodeI(0x2B, 8, 9, 3), true);  // sw $9, 3($8)
    c3.setPc(4);
    CHECK(stepFault(c3, mem, Mips64Fault::MisalignedStore));
    CHECK(c3.causeExcCode() == 5);
    CHECK(c3.cp0BadVAddr() == 0x1003);
}

// -- 10/11. EPC + Cause.BD incl. delay-slot exceptions ---------------------------------------
static void testEpcAndBd() {
    SparseMemory mem;
    // beq taken at 0x100; delay slot at 0x104 is a misaligned LH.
    storeWord(mem, 0x100, encodeI(0x04, 0, 0, 1), true);  // beq $0,$0,1 -> 0x108
    storeWord(mem, 0x104, encodeI(0x21, 8, 9, 1), true);  // lh odd -> AdEL in slot
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0x1000);
    cpu.setPc(0x100);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayArmed());
    CHECK(stepFault(cpu, mem, Mips64Fault::MisalignedLoad));
    CHECK(cpu.causeExcCode() == 4);
    CHECK(cpu.causeBd());  // faulted inside the delay slot
    CHECK(cpu.cp0Epc() == 0x100);  // originating branch, not the slot
    CHECK(cpu.cp0BadVAddr() == 0x1001);
    CHECK(!cpu.delayArmed());  // pending transfer cancelled
    CHECK(cpu.pc() == kBev1Vector);

    // SYSCALL in a delay slot: BD set, EPC = branch.
    storeWord(mem, 0x200, encodeI(0x05, 8, 9, 1), true);  // bne (taken: 1 != 2)
    storeWord(mem, 0x204, 0x0000000CU, true);             // syscall in slot
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 1);
    c2.writeGpr(9, 2);
    c2.setPc(0x200);
    CHECK(stepOk(c2, mem));
    CHECK(stepFault(c2, mem, Mips64Fault::SyscallTrap));
    CHECK(c2.causeExcCode() == 8);
    CHECK(c2.causeBd());
    CHECK(c2.cp0Epc() == 0x200);

    // BREAK in a delay slot of a not-taken branch: still BD.
    storeWord(mem, 0x300, encodeI(0x04, 8, 9, 1), true);  // beq not taken
    storeWord(mem, 0x304, 0x0000000DU, true);             // break in slot
    Mips64Cpu c3;
    CHECK(setupKusegIdentityMap(c3, mem));
    c3.writeGpr(8, 1);
    c3.writeGpr(9, 2);
    c3.setPc(0x300);
    CHECK(stepOk(c3, mem));
    CHECK(stepFault(c3, mem, Mips64Fault::BreakTrap));
    CHECK(c3.causeExcCode() == 9);
    CHECK(c3.causeBd());
    CHECK(c3.cp0Epc() == 0x300);

    // Overflow in a delay slot.
    storeWord(mem, 0x400, encodeI(0x05, 8, 9, 1), true);  // bne taken
    storeWord(mem, 0x404, encodeR(10, 11, 12, 0, 0x20), true);  // add (overflows)
    Mips64Cpu c4;
    CHECK(setupKusegIdentityMap(c4, mem));
    c4.writeGpr(8, 1);
    c4.writeGpr(9, 2);
    c4.writeGpr(10, 0x7FFFFFFF);
    c4.writeGpr(11, 1);
    c4.setPc(0x400);
    CHECK(stepOk(c4, mem));
    CHECK(stepFault(c4, mem, Mips64Fault::OverflowTrap));
    CHECK(c4.causeExcCode() == 12);
    CHECK(c4.causeBd());
    CHECK(c4.cp0Epc() == 0x400);
}

// -- 12. BEV/EBase vector selection ----------------------------------------------------------
static void testVectorSelection() {
    SparseMemory mem;
    storeWord(mem, 0, 0x0000000CU, true);  // syscall
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(cpu.exceptionVector() == kBev1Vector);
    CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu.pc() == kBev1Vector);

    // Clear BEV via MTC0 Status -> EBase vector (reset EBase 0x80000000).
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0);
    storeWord(mem, 16, encodeCp0(4, 8, 12, 0), true);  // mtc0 Status = 0
    c2.setPc(16);
    CHECK(stepOk(c2, mem));
    CHECK(c2.exceptionVector() == kBev0Vector);
    c2.setPc(0);
    CHECK(stepFault(c2, mem, Mips64Fault::SyscallTrap));
    CHECK(c2.pc() == kBev0Vector);

    // Reprogram EBase via DMTC0 -> vector follows.
    Mips64Cpu c3;
    CHECK(setupKusegIdentityMap(c3, mem));
    c3.writeGpr(8, 0);
    c3.writeGpr(9, 0xA0000000ULL);
    storeWord(mem, 16, encodeCp0(4, 8, 12, 0), true);  // mtc0 Status = 0 (BEV=0)
    storeWord(mem, 20, encodeCp0(5, 9, 15, 1), true);  // dmtc0 EBase
    c3.setPc(16);
    CHECK(stepOk(c3, mem));
    c3.setPc(20);
    CHECK(stepOk(c3, mem));
    CHECK(c3.exceptionVector() == 0xFFFFFFFFA0000180ULL);
    c3.setPc(0);
    CHECK(stepFault(c3, mem, Mips64Fault::SyscallTrap));
    CHECK(c3.pc() == 0xFFFFFFFFA0000180ULL);
}

// -- 13. ERET behavior ----------------------------------------------------------------------------
static void testEret() {
    SparseMemory mem;
    // Reset leaves Status.ERL set (as on hardware); boot software clears it
    // via MTC0 before relying on the EPC/EXL return path, which is what the
    // prologue below does. Then take a SYSCALL entry and ERET: EXL path.
    storeWord(mem, 0, encodeCp0(4, 8, 12, 0), true);  // mtc0 $8, Status
    storeWord(mem, 4, 0x0000000CU, true);             // syscall
    // The ERET at the BEV=1 general vector lives at CKSEG1, whose physical
    // address is VA & 0x1FFFFFFF.
    fillZeros(mem, kBev1Vector & kDirectMapMask, 16);
    storeWordPhys(mem, kBev1Vector & kDirectMapMask, 0x42000018U, true);  // eret
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0x400000ULL);  // BEV only: clears ERL/EXL, keeps boot vectors
    CHECK(stepOk(cpu, mem));
    CHECK(!cpu.statusErl());
    CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu.statusExl());
    CHECK(cpu.pc() == kBev1Vector);
    CHECK(stepOk(cpu, mem));  // eret -> EPC
    CHECK(cpu.pc() == 4);
    CHECK(!cpu.statusExl());
    CHECK(!cpu.delayArmed());  // eret has no delay slot

    // ERL path: set ERL + ErrorEPC, then ERET uses ErrorEPC and clears ERL.
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0x5000);
    c2.writeGpr(9, static_cast<uint32_t>(kStatusBitBev | kStatusBitErl | kStatusBitExl));
    storeWord(mem, 32, encodeCp0(5, 8, 30, 0), true);  // dmtc0 ErrorEPC
    storeWord(mem, 36, encodeCp0(4, 9, 12, 0), true);  // mtc0 Status (ERL|EXL|BEV)
    storeWord(mem, 40, 0x42000018U, true);             // eret
    c2.setPc(32);
    CHECK(stepOk(c2, mem));
    c2.setPc(36);
    CHECK(stepOk(c2, mem));
    CHECK(c2.statusErl() && c2.statusExl());
    c2.setPc(40);
    CHECK(stepOk(c2, mem));
    CHECK(c2.pc() == 0x5000);
    CHECK(!c2.statusErl());
    CHECK(c2.statusExl());  // EXL path untouched when ERL was set

    // ERET inside a delay slot is explicitly rejected.
    Mips64Cpu c3;
    CHECK(setupKusegIdentityMap(c3, mem));
    storeWord(mem, 64, encodeI(0x04, 0, 0, 1), true);  // beq taken
    storeWord(mem, 68, 0x42000018U, true);             // eret in slot
    c3.setPc(64);
    CHECK(stepOk(c3, mem));
    CHECK(stepFault(c3, mem, Mips64Fault::BranchInDelaySlot));
}

// -- 14. nested exception under EXL -------------------------------------------------------------------
static void testNestedException() {
    SparseMemory mem;
    storeWord(mem, 0, 0x0000000CU, true);  // syscall
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
    const uint64_t outerEpc = cpu.cp0Epc();
    CHECK(cpu.statusExl());
    // A second exception while EXL is set: EPC preserved, Cause/vector updated.
    cpu.setPc(0x9000);
    fillZeros(mem, 0x9000, 4);
    storeWord(mem, 0x9000, 0x0000000DU, true);  // break
    CHECK(stepFault(cpu, mem, Mips64Fault::BreakTrap));
    CHECK(cpu.causeExcCode() == 9);
    CHECK(cpu.cp0Epc() == outerEpc);  // outer restart address intact
    CHECK(cpu.statusExl());
    CHECK(cpu.pc() == kBev1Vector);

    // Equivalent via MTC0-set EXL (simulating handler context).
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0xABCDULL);
    c2.writeGpr(9, static_cast<uint32_t>(kStatusBitBev | kStatusBitExl));
    storeWord(mem, 32, encodeCp0(5, 8, 14, 0), true);  // dmtc0 EPC = 0xABCD
    storeWord(mem, 36, encodeCp0(4, 9, 12, 0), true);  // mtc0 Status (EXL)
    storeWord(mem, 40, 0x0000000CU, true);             // syscall
    c2.setPc(32);
    CHECK(stepOk(c2, mem));
    c2.setPc(36);
    CHECK(stepOk(c2, mem));
    c2.setPc(40);
    CHECK(stepFault(c2, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu.causeExcCode() == 9);  // first cpu still reports its own BREAK entry
    CHECK(c2.causeExcCode() == 8);
    CHECK(c2.cp0Epc() == 0xABCDULL);  // not clobbered
    CHECK(!c2.causeBd());
}

// -- 15. signed/unsigned MULT/DIV --------------------------------------------------------------------------
static void testMultDiv32() {
    SparseMemory mem;
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFEULL);  // -2 (signed word)
    cpu.writeGpr(9, 3);
    // mult $-2,3 ; mflo ; mfhi ; multu ; mflo ; mfhi
    storeWord(mem, 0, encodeR(8, 9, 0, 0, 0x18), true);
    storeWord(mem, 4, encodeR(0, 0, 10, 0, 0x12), true);  // mflo $10
    storeWord(mem, 8, encodeR(0, 0, 11, 0, 0x10), true);  // mfhi $11
    storeWord(mem, 12, encodeR(8, 9, 0, 0, 0x19), true);  // multu
    storeWord(mem, 16, encodeR(0, 0, 12, 0, 0x12), true);
    storeWord(mem, 20, encodeR(0, 0, 13, 0, 0x10), true);
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 6; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFFFFFFFFAULL);  // -6 sign-extended
    CHECK(cpu.readGpr(11) == 0xFFFFFFFFFFFFFFFFULL);  // high word sign-extended
    // multu treats low words unsigned: 0xFFFFFFFE * 3 = 0x2FFFFFFFA.
    CHECK(cpu.readGpr(12) == 0xFFFFFFFAULL);
    CHECK(cpu.readGpr(13) == 2);

    // div -7,3 -> q -2 r -1 ; divu 7,3 -> q 2 r 1.
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0xFFFFFFFFFFFFFFF9ULL);  // -7
    c2.writeGpr(9, 3);
    storeWord(mem, 64, encodeR(8, 9, 0, 0, 0x1A), true);   // div
    storeWord(mem, 68, encodeR(0, 0, 10, 0, 0x12), true);  // mflo
    storeWord(mem, 72, encodeR(0, 0, 11, 0, 0x10), true);  // mfhi
    storeWord(mem, 76, encodeR(8, 9, 0, 0, 0x1B), true);   // divu (huge/3)
    storeWord(mem, 80, encodeR(0, 0, 12, 0, 0x12), true);
    storeWord(mem, 84, encodeR(0, 0, 13, 0, 0x10), true);
    c2.setPc(64);
    for (int i = 0; i < 6; ++i) CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(10) == 0xFFFFFFFFFFFFFFFEULL);  // -2
    CHECK(c2.readGpr(11) == 0xFFFFFFFFFFFFFFFFULL);  // -1
    CHECK(c2.readGpr(12) == 0x55555553ULL);  // 0xFFFFFFF9 / 3 = 1431655763
    CHECK(c2.readGpr(13) == 0);
}

// -- 16. 64-bit multiplication HI/LO ------------------------------------------------------------------------------
static void testDmult64() {
    SparseMemory mem;
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);  // -1
    cpu.writeGpr(9, 2);
    storeWord(mem, 0, encodeR(8, 9, 0, 0, 0x1C), true);   // dmult
    storeWord(mem, 4, encodeR(0, 0, 10, 0, 0x12), true);  // mflo
    storeWord(mem, 8, encodeR(0, 0, 11, 0, 0x10), true);  // mfhi
    storeWord(mem, 12, encodeR(8, 9, 0, 0, 0x1D), true);  // dmultu
    storeWord(mem, 16, encodeR(0, 0, 12, 0, 0x12), true);
    storeWord(mem, 20, encodeR(0, 0, 13, 0, 0x10), true);
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 6; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFFFFFFFFEULL);  // -2
    CHECK(cpu.readGpr(11) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.readGpr(12) == 0xFFFFFFFFFFFFFFFEULL);
    CHECK(cpu.readGpr(13) == 1ULL);

    // Hand-computed 64x64 cases (portable partial-product implementation).
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0x100000000ULL);
    c2.writeGpr(9, 0x100000000ULL);
    storeWord(mem, 64, encodeR(8, 9, 0, 0, 0x1D), true);  // dmultu 2^32 * 2^32
    storeWord(mem, 68, encodeR(0, 0, 10, 0, 0x12), true);
    storeWord(mem, 72, encodeR(0, 0, 11, 0, 0x10), true);
    c2.setPc(64);
    for (int i = 0; i < 3; ++i) CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(10) == 0);
    CHECK(c2.readGpr(11) == 1);

    Mips64Cpu c3;
    c3.writeGpr(8, 0x8000000000000000ULL);  // INT64_MIN
    c3.writeGpr(9, 0x8000000000000000ULL);
    storeWord(mem, 96, encodeR(8, 9, 0, 0, 0x1C), true);  // dmult MIN*MIN = 2^126
    storeWord(mem, 100, encodeR(0, 0, 10, 0, 0x12), true);
    storeWord(mem, 104, encodeR(0, 0, 11, 0, 0x10), true);
    c3.setPc(96);
    CHECK(setupKusegIdentityMap(c3, mem));
    for (int i = 0; i < 3; ++i) CHECK(stepOk(c3, mem));
    CHECK(c3.readGpr(10) == 0);
    CHECK(c3.readGpr(11) == 0x4000000000000000ULL);
}

// -- 17. division edge cases (deterministic UNPREDICTABLE handling) ------------------------------------------------------
static void testDivEdges() {
    SparseMemory mem;
    auto runDiv = [&](uint64_t lhs, uint64_t rhs, uint8_t funct) {
        Mips64Cpu cpu;
        cpu.writeGpr(8, lhs);
        cpu.writeGpr(9, rhs);
        storeWord(mem, 0, encodeR(8, 9, 0, 0, funct), true);
        storeWord(mem, 4, encodeR(0, 0, 10, 0, 0x12), true);
        storeWord(mem, 8, encodeR(0, 0, 11, 0, 0x10), true);
        CHECK(setupKusegIdentityMap(cpu, mem));
        for (int i = 0; i < 3; ++i) {
            if (!stepOk(cpu, mem)) return std::make_pair(uint64_t(0), uint64_t(0));
        }
        return std::make_pair(cpu.readGpr(10), cpu.readGpr(11));
    };
    // DIV by zero: quotient -1 (non-negative dividend) / +1 (negative), rem = dividend.
    CHECK(runDiv(7, 0, 0x1A) == std::make_pair(0xFFFFFFFFFFFFFFFFULL, 7ULL));
    CHECK(runDiv(0xFFFFFFFFFFFFFFF9ULL, 0, 0x1A) == std::make_pair(1ULL, 0xFFFFFFFFFFFFFFF9ULL));
    // DIVU by zero: quotient all-ones, rem = dividend.
    CHECK(runDiv(7, 0, 0x1B) == std::make_pair(0xFFFFFFFFULL, 7ULL));
    // DIV INT32_MIN / -1: quotient MIN, remainder 0.
    CHECK(runDiv(0xFFFFFFFF80000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0x1A) ==
          std::make_pair(0xFFFFFFFF80000000ULL, 0ULL));
    // DDIV by zero (64-bit).
    CHECK(runDiv(7, 0, 0x1E) == std::make_pair(0xFFFFFFFFFFFFFFFFULL, 7ULL));
    CHECK(runDiv(0x8000000000000000ULL, 0, 0x1E) ==
          std::make_pair(1ULL, 0x8000000000000000ULL));
    // DDIVU by zero.
    CHECK(runDiv(7, 0, 0x1F) == std::make_pair(0xFFFFFFFFFFFFFFFFULL, 7ULL));
    // DDIV INT64_MIN / -1.
    CHECK(runDiv(0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0x1E) ==
          std::make_pair(0x8000000000000000ULL, 0ULL));
    // Normal 64-bit cases: DDIV -7/3, DDIVU max/3.
    CHECK(runDiv(0xFFFFFFFFFFFFFFFFULL - 6, 3, 0x1E) ==
          std::make_pair(0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL));
    CHECK(runDiv(0xFFFFFFFFFFFFFFFFULL, 3, 0x1F) ==
          std::make_pair(0x5555555555555555ULL, 0ULL));
}

// -- 18. MFHI/MFLO/MTHI/MTLO --------------------------------------------------------------------------------------
static void testHiLoMoves() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0x1111111111111111ULL);
    cpu.writeGpr(9, 0x2222222222222222ULL);
    storeWord(mem, 0, encodeR(8, 0, 0, 0, 0x11), true);  // mthi $8
    storeWord(mem, 4, encodeR(9, 0, 0, 0, 0x13), true);  // mtlo $9
    storeWord(mem, 8, encodeR(0, 0, 10, 0, 0x10), true);  // mfhi $10
    storeWord(mem, 12, encodeR(0, 0, 11, 0, 0x12), true);  // mflo $11
    for (int i = 0; i < 4; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.hi() == 0x1111111111111111ULL);
    CHECK(cpu.lo() == 0x2222222222222222ULL);
    CHECK(cpu.readGpr(10) == 0x1111111111111111ULL);
    CHECK(cpu.readGpr(11) == 0x2222222222222222ULL);
    // HI/LO survive reset clearing and snapshot (checked elsewhere); moves to $zero drop.
    storeWord(mem, 16, encodeR(0, 0, 0, 0, 0x10), true);  // mfhi $0
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(0) == 0);
}

// -- 19. REGIMM taken/not-taken -----------------------------------------------------------------------------------------
static void testRegimmBranches() {
    SparseMemory mem;
    // BLTZ taken (-1), BGEZ not taken (-1), BLTZ not taken (0), BGEZ taken (0).
    storeWord(mem, 0, encodeI(0x01, 8, 0x00, 2), true);   // bltz $8, +2
    storeWord(mem, 8, encodeI(0x01, 8, 0x01, 2), true);   // bgez $8, +2
    storeWord(mem, 16, encodeI(0x01, 9, 0x00, 2), true);  // bltz $9(0), +2 (not taken)
    storeWord(mem, 24, encodeI(0x01, 9, 0x01, 2), true);  // bgez $9(0), +2 (taken)
    // Full 64-bit condition: high bit set but low word zero is still negative.
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGpr(9, 0);
    cpu.setPc(0);
    CHECK(stepOk(cpu, mem));  // bltz taken -> delay at 4, target 12
    CHECK(cpu.delayArmed() && cpu.pc() == 4);
    CHECK(cpu.delayTarget() == 12 && cpu.delayBranch() == 0);

    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0x8000000000000000ULL);  // negative via high bit only
    c2.setPc(8);
    CHECK(stepOk(c2, mem));  // bgez not taken -> delay 12, fallthrough 16
    CHECK(c2.delayArmed() && c2.delayTarget() == 16);

    Mips64Cpu c3;
    CHECK(setupKusegIdentityMap(c3, mem));
    c3.writeGpr(9, 5);  // positive
    c3.setPc(16);
    CHECK(stepOk(c3, mem));  // bltz not taken
    CHECK(c3.delayArmed() && c3.delayTarget() == 24);
    Mips64Cpu c4;
    CHECK(setupKusegIdentityMap(c4, mem));
    c4.writeGpr(9, 5);
    c4.setPc(24);
    CHECK(stepOk(c4, mem));  // bgez taken -> target 24+4+8 = 36
    CHECK(c4.delayArmed() && c4.delayTarget() == 36);
}

// -- 20. link + delay-slot behavior for BLTZAL/BGEZAL ---------------------------------------------------------------------------
static void testRegimmLink() {
    SparseMemory mem;
    // bltzal $8(-1), +2 (taken): link written, delay executes, target 12.
    // bgezal $9(0), +2 (taken): link overwritten to 32.
    storeWord(mem, 0, encodeI(0x01, 8, 0x10, 2), true);   // bltzal
    storeWord(mem, 4, encodeI(0x09, 0, 10, 7), true);     // delay: addiu $10, 7
    storeWord(mem, 12, encodeI(0x01, 9, 0x11, 2), true);  // bgezal
    storeWord(mem, 16, encodeI(0x09, 0, 11, 9), true);    // delay: addiu $11, 9
    storeWord(mem, 20, encodeI(0x09, 0, 12, 1), true);    // skipped
    storeWord(mem, 24, encodeI(0x09, 0, 13, 2), true);   // target
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGpr(9, 0);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(31) == 8);  // link = PC+8 even though taken
    CHECK(stepOk(cpu, mem));      // delay slot
    CHECK(cpu.pc() == 12 && cpu.readGpr(10) == 7);
    CHECK(stepOk(cpu, mem));      // bgezal
    CHECK(cpu.readGpr(31) == 20);
    CHECK(stepOk(cpu, mem));      // delay slot
    CHECK(cpu.pc() == 24 && cpu.readGpr(11) == 9);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(13) == 2);

    // Link is written even when the condition is false.
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 5);  // positive -> bltzal not taken
    storeWord(mem, 64, encodeI(0x01, 8, 0x10, 2), true);
    storeWord(mem, 68, encodeI(0x09, 0, 10, 3), true);
    c2.setPc(64);
    CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(31) == 72);
    CHECK(stepOk(c2, mem));
    CHECK(c2.pc() == 72 && c2.readGpr(10) == 3);
}

// -- 21a. branch-likely annul semantics ------------------------------------------------------------------------------
static void testBranchLikely() {
    SparseMemory mem;
    // bltzl not taken -> delay slot annulled (skipped), PC = branch+8.
    storeWord(mem, 0, encodeI(0x01, 8, 0x02, 2), true);  // bltzl $8(5), +2
    storeWord(mem, 4, encodeI(0x09, 0, 10, 7), true);    // delay (must NOT run)
    storeWord(mem, 8, encodeI(0x09, 0, 11, 9), true);    // fallthrough
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 5);
    CHECK(stepOk(cpu, mem));
    CHECK(!cpu.delayArmed());
    CHECK(cpu.pc() == 8);
    CHECK(cpu.readGpr(10) == 0);  // annulled
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(11) == 9);

    // bgezl taken -> delay slot executes normally.
    storeWord(mem, 32, encodeI(0x01, 8, 0x03, 2), true);  // bgezl $8(5), +2
    storeWord(mem, 36, encodeI(0x09, 0, 10, 7), true);
    storeWord(mem, 44, encodeI(0x09, 0, 12, 1), true);
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 5);
    c2.setPc(32);
    CHECK(stepOk(c2, mem));
    CHECK(c2.delayArmed() && c2.pc() == 36);
    CHECK(stepOk(c2, mem));
    CHECK(c2.pc() == 44 && c2.readGpr(10) == 7);

}

// -- (branch-likely continued: blez/bgtz + blezl/gtzl annul) ----------------------------------------------------------
static void testBranchLikely2() {
    SparseMemory mem;
    // blez needs a mapped delay slot: lay out contiguously.
    // 64: blez $8, +2 -> target 76 ; 68: delay addiu $10,4 ; 72: skipped ; 76: target
    storeWord(mem, 64, encodeI(0x06, 8, 0, 2), true);
    storeWord(mem, 68, encodeI(0x09, 0, 10, 4), true);
    storeWord(mem, 72, encodeI(0x09, 0, 11, 5), true);
    storeWord(mem, 76, encodeI(0x09, 0, 12, 6), true);
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0);
    cpu.setPc(64);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayTarget() == 76);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.pc() == 76 && cpu.readGpr(10) == 4 && cpu.readGpr(11) == 0);

    // bgtz not taken (0) -> fallthrough.
    storeWord(mem, 96, encodeI(0x07, 8, 0, 2), true);  // bgtz $8(0) not taken
    storeWord(mem, 100, encodeI(0x09, 0, 10, 1), true);
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0);
    c2.setPc(96);
    CHECK(stepOk(c2, mem));
    CHECK(c2.delayTarget() == 104);
    CHECK(stepOk(c2, mem));
    CHECK(c2.pc() == 104 && c2.readGpr(10) == 1);
}

// -- (branch-likely continued, fixed layout) ------------------------------------------------------------------------------
static void testBranchLikely3() {
    SparseMemory mem;
    // Contiguous: 0: blezl taken -> 12 ; 4: delay ; 8: skipped-insn; 12: bgtzl not taken
    // 16: (annulled delay) ; 20: fallthrough land ; 24: next
    storeWord(mem, 0, encodeI(0x16, 8, 0, 2), true);   // blezl $8, +2
    storeWord(mem, 4, encodeI(0x09, 0, 10, 8), true);  // delay runs
    storeWord(mem, 8, encodeI(0x09, 0, 11, 1), true);  // skipped
    storeWord(mem, 12, encodeI(0x17, 9, 0, 2), true);  // bgtzl $9(0) not taken
    storeWord(mem, 16, encodeI(0x09, 0, 12, 8), true);  // annulled
    storeWord(mem, 20, encodeI(0x09, 0, 13, 1), true);  // fallthrough target (skipped)
    storeWord(mem, 24, encodeI(0x09, 0, 14, 2), true);  // land
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFDULL);
    cpu.writeGpr(9, 0);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayArmed() && cpu.delayTarget() == 12);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.pc() == 12 && cpu.readGpr(10) == 8 && cpu.readGpr(11) == 0);
    CHECK(stepOk(cpu, mem));  // bgtzl not taken -> annul
    CHECK(!cpu.delayArmed() && cpu.pc() == 20);
    CHECK(cpu.readGpr(12) == 0);  // delay annulled
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.pc() == 24);

    // bltzall/bgezall link + likely combined.
    storeWord(mem, 64, encodeI(0x01, 8, 0x12, 1), true);  // bltzall $8(-1) taken
    storeWord(mem, 68, encodeI(0x09, 0, 10, 3), true);
    storeWord(mem, 72, encodeI(0x09, 0, 11, 4), true);
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    c2.setPc(64);
    CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(31) == 72);
    CHECK(stepOk(c2, mem));
    CHECK(c2.pc() == 72 && c2.readGpr(10) == 3);
}

// -- 22. big-endian cross-check: CP0, exception, MULT, branch ------------------------------------------
static void testBigEndianCoverage() {
    SparseMemory mem;
    Mips64Cpu cpu(Mips64Endianness::Big);
    CHECK(!cpu.isLittleEndian());
    CHECK(setupKusegIdentityMap(cpu, mem));
    // dmtc0/mfc0 Status round trip in BE fetch (BEV cleared -> EBase vector).
    cpu.writeGpr(8, 0xFFFFULL);
    storeWord(mem, 0, encodeCp0(5, 8, 12, 0), false);  // dmtc0 Status
    storeWord(mem, 4, encodeCp0(0, 9, 12, 0), false);  // mfc0 $9, Status
    storeWord(mem, 8, 0x0000000CU, false);             // syscall
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(9) == 0xFFFFULL);
    CHECK(!cpu.statusBev());
    CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu.causeExcCode() == 8);
    CHECK(cpu.pc() == kBev0Vector);

    // BE MULT + BLTZ.
    Mips64Cpu c2(Mips64Endianness::Big);
    CHECK(setupKusegIdentityMap(c2, mem));
    c2.writeGpr(8, 0xFFFFFFFEULL);  // low32 = -2
    c2.writeGpr(9, 0xFFFFFFFBULL);  // low32 = -5
    storeWord(mem, 64, encodeR(8, 9, 0, 0, 0x18), false);  // mult -> 10
    storeWord(mem, 68, encodeR(0, 0, 10, 0, 0x12), false);  // mflo
    storeWord(mem, 72, encodeI(0x01, 10, 0x00, 1), false);  // bltz? 10 > 0, not taken
    c2.setPc(64);
    CHECK(stepOk(c2, mem));
    CHECK(stepOk(c2, mem));
    CHECK(c2.readGpr(10) == 10);
    CHECK(stepOk(c2, mem));
    CHECK(c2.delayTarget() == 80);  // not taken -> fallthrough
    // Verify BE wire order for a stored word.
    c2.writeGpr(11, 0x12345678);
    storeWord(mem, 96, encodeI(0x2B, 0, 11, 0x200), false);  // sw
    c2.setPc(96);
    CHECK(stepOk(c2, mem));
    uint8_t wire[4] = {0, 0, 0, 0};
    mem.Read(0x200, wire, 4);
    CHECK(wire[0] == 0x12 && wire[1] == 0x34 && wire[2] == 0x56 && wire[3] == 0x78);
}

// -- 23. Count/Compare semantics (interrupt delivery deferred) ---------------------------------------------------
static void testCountCompare() {
    SparseMemory mem;
    storeWord(mem, 0, encodeI(0x09, 0, 8, 1), true);  // addiu (retires)
    Mips64Cpu cpu;
    CHECK(cpu.cp0Count() == 0);
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(cpu.cp0Count() == 10);  // prologue ticked the counter ten times
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Count() == 11);  // one tick per step
    cpu.setPc(0);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Count() == 12);

    // DMFC0 reads the ticking counter; MTC0 Compare arms no interrupt.
    cpu.writeGpr(9, 1);
    storeWord(mem, 8, encodeCp0(5, 9, 11, 0), true);   // dmtc0 Compare = 1
    storeWord(mem, 12, encodeCp0(1, 10, 9, 0), true);  // dmfc0 $10, Count
    cpu.setPc(8);
    CHECK(stepOk(cpu, mem));
    cpu.setPc(12);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.cp0Compare() == 1);
    CHECK(cpu.readGpr(10) == cpu.cp0Count());  // counter kept ticking
    CHECK((cpu.cp0Cause() & 0xFC00ULL) == 0);  // no IP/TI raised: deferred
    for (int i = 0; i < 10; ++i) {
        cpu.setPc(0);
        CHECK(stepOk(cpu, mem));
    }
    CHECK(cpu.cp0Count() == 24);  // 10 prologue + 4 + 10 steps
    CHECK((cpu.cp0Cause() & 0xFC00ULL) == 0);
}

// -- 24. serialization of CP0/HI/LO/delay state ----------------------------------------------------------------------------
static void testSerializationV2() {
    SparseMemory mem;
    Mips64Cpu cpu(Mips64Endianness::Big);
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 7);
    cpu.writeGpr(9, 7);
    cpu.setHi(0x1111);
    cpu.setLo(0x2222);
    cpu.writeGpr(10, 0xDEADBEEF12345678ULL);
    storeWord(mem, 0, encodeI(4, 8, 9, 2), false);  // beq taken
    storeWord(mem, 4, encodeI(9, 0, 10, 1), false);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayArmed());
    const std::vector<uint8_t> blob = cpu.serialize();
    CHECK(blob.size() == Mips64Cpu::kSerializedSize);
    CHECK(blob.size() == 957);
    CHECK(blob[0] == 'M' && blob[1] == 'P' && blob[2] == '6' && blob[3] == '2');
    CHECK(blob[4] == 3);  // MIPS64-3 format (TLB state appended)

    Mips64Cpu restored;
    CHECK(restored.deserialize(blob.data(), blob.size()));
    CHECK(!restored.isLittleEndian());
    CHECK(restored.delayArmed());
    CHECK(restored.delayTarget() == cpu.delayTarget());
    CHECK(restored.delayBranch() == cpu.delayBranch());
    CHECK(restored.hi() == 0x1111 && restored.lo() == 0x2222);
    CHECK(restored.cp0Status() == cpu.cp0Status());
    CHECK(restored.cp0Cause() == cpu.cp0Cause());
    CHECK(restored.cp0Epc() == cpu.cp0Epc());
    CHECK(restored.cp0EBase() == cpu.cp0EBase());

    // Exception-entry state round-trips too.
    SparseMemory mem2;
    storeWord(mem2, 0, 0x0000000CU, true);
    Mips64Cpu c2;
    CHECK(setupKusegIdentityMap(c2, mem2));
    CHECK(stepFault(c2, mem2, Mips64Fault::SyscallTrap));
    const std::vector<uint8_t> blob2 = c2.serialize();
    Mips64Cpu r2;
    CHECK(r2.deserialize(blob2.data(), blob2.size()));
    CHECK(r2.causeExcCode() == 8 && r2.cp0Epc() == 0 && r2.statusExl());
    CHECK(r2.pc() == kBev1Vector);

    // Rejections: v1-size blobs, truncation, bad magic, null.
    std::vector<uint8_t> v1(301, 0);
    v1[0] = 'M';
    v1[1] = 'P';
    v1[2] = '6';
    v1[3] = '1';
    CHECK(!r2.deserialize(v1.data(), v1.size()));
    CHECK(!r2.deserialize(blob2.data(), blob2.size() - 1));
    CHECK(!r2.deserialize(nullptr, blob2.size()));
    std::vector<uint8_t> bad = blob2;
    bad[0] = 'X';
    CHECK(!r2.deserialize(bad.data(), bad.size()));
    CHECK(r2.causeExcCode() == 8);  // failed restore leaves state untouched
}

// -- 25. isolation between two CPU instances -----------------------------------------------------------------------
static void testIsolation() {
    SparseMemory mem;
    fillZeros(mem, 0, 16);
    Mips64Cpu a, b;
    a.writeGpr(8, 1);
    CHECK(setupKusegIdentityMap(a, mem));  // prologue at PA 0x00..0x27; count = 10
    CHECK(stepOk(a, mem));                  // VA 0 -> PA 0 -> prologue 1st insn -> count 11
    CHECK(a.cp0Count() == 11 && b.cp0Count() == 0);
    a.setHi(0xAA);
    a.setLo(0xBB);
    CHECK(b.hi() == 0 && b.lo() == 0);
    // Divergent CP0: fault a into an exception; b stays pristine.
    storeWord(mem, 0, 0x0000000CU, true);  // syscall at PA 0
    a.setPc(0);
    CHECK(stepFault(a, mem, Mips64Fault::SyscallTrap));
    CHECK(a.statusExl() && !b.statusExl());
    CHECK(a.causeExcCode() == 8 && b.causeExcCode() == 0);
    CHECK(a.pc() == kBev1Vector && b.pc() == 0);
    CHECK(a.cp0Epc() == 0 && b.cp0Epc() == 0);
}

// -- 26. real exception-handler demo: main + handler + ERET ------------------------------------------------------------
// Main (at 0x1000):
//   0x1000: lui   $8, 0x40 (Status.BEV image)
//   0x1004: mtc0  $8, $12 (clear reset ERL, keep boot vectors)
//   0x1008: addiu $t0,$0,10
//   0x100C: syscall                      -> handler (EPC=0x100C, BD=0)
//   0x1010: addiu $t1,$0,20
//   0x1014: beq   $t0,$0,1 (not taken)
//   0x1018: break  (delay slot)          -> handler (EPC=0x1014, BD=1)
//   0x101C: addiu $t2,$0,30
//   0x1020: addiu $t3,$0,40
// Handler (at BEV=1 vector): logs (EPC, Cause) pairs into 0x2000 + count*8,
// advances EPC by 4 (BD==0) or 8 (BD==1), ERETs.
static void testHandlerDemo() {
    SparseMemory mem;
    const uint64_t V = kBev1Vector;
    // Main program.
    storeWord(mem, 0x1000, encodeI(0x0F, 0, 8, 0x40), true);  // lui $8, 0x40
    storeWord(mem, 0x1004, encodeCp0(4, 8, 12, 0), true);     // mtc0 Status
    storeWord(mem, 0x1008, encodeI(0x09, 0, 8, 10), true);
    storeWord(mem, 0x100C, 0x0000000CU, true);
    storeWord(mem, 0x1010, encodeI(0x09, 0, 9, 20), true);
    storeWord(mem, 0x1014, encodeI(0x04, 8, 0, 1), true);
    storeWord(mem, 0x1018, 0x0000000DU, true);
    storeWord(mem, 0x101C, encodeI(0x09, 0, 10, 30), true);
    storeWord(mem, 0x1020, encodeI(0x09, 0, 11, 40), true);
    // Handler: $k0=26, $k1=27, $t8=24, $t9=25. The handler lives at the
    // BEV=1 general vector (CKSEG1), so its bytes go at VA & 0x1FFFFFFF.
    storeWordPhys(mem, (V + 0) & kDirectMapMask, encodeCp0(1, 26, 14, 0), true);   // dmfc0 $k0, EPC
    storeWordPhys(mem, (V + 4) & kDirectMapMask, encodeCp0(0, 27, 13, 0), true);   // mfc0  $k1, Cause
    storeWordPhys(mem, (V + 8) & kDirectMapMask, encodeI(0x23, 0, 24, 0x2010), true);  // lw $t8, count
    storeWordPhys(mem, (V + 12) & kDirectMapMask, encodeI(0x09, 0, 25, 0x2000), true);  // $t9 = 0x2000
    storeWordPhys(mem, (V + 16) & kDirectMapMask, encodeR(0, 24, 24, 3, 0x00), true);  // sll $t8, 3
    storeWordPhys(mem, (V + 20) & kDirectMapMask, encodeR(25, 24, 25, 0, 0x2D), true);  // daddu $t9,$t9,$t8
    storeWordPhys(mem, (V + 24) & kDirectMapMask, encodeI(0x2B, 25, 26, 0), true);     // sw $k0, 0($t9)
    storeWordPhys(mem, (V + 28) & kDirectMapMask, encodeI(0x2B, 25, 27, 4), true);     // sw $k1, 4($t9)
    storeWordPhys(mem, (V + 32) & kDirectMapMask, encodeI(0x23, 0, 24, 0x2010), true);  // lw $t8, count
    storeWordPhys(mem, (V + 36) & kDirectMapMask, encodeI(0x09, 24, 24, 1), true);     // $t8++
    storeWordPhys(mem, (V + 40) & kDirectMapMask, encodeI(0x2B, 0, 24, 0x2010), true);  // sw count
    storeWordPhys(mem, (V + 44) & kDirectMapMask, encodeR(0, 27, 27, 31, 0x02), true);  // srl $k1, 31 (BD bit)
    storeWordPhys(mem, (V + 48) & kDirectMapMask, encodeI(0x04, 27, 0, 2), true);      // beq $k1,$0,+2
    storeWordPhys(mem, (V + 52) & kDirectMapMask, encodeI(0x19, 26, 26, 4), true);     // delay: $k0 += 4
    storeWordPhys(mem, (V + 56) & kDirectMapMask, encodeI(0x19, 26, 26, 4), true);     // BD==1: $k0 += 4 more
    storeWordPhys(mem, (V + 60) & kDirectMapMask, encodeCp0(5, 26, 14, 0), true);      // dmtc0 EPC = $k0
    storeWordPhys(mem, (V + 64) & kDirectMapMask, 0x42000018U, true);                  // eret
    fillZeros(mem, 0x2000, 32);

    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.setPc(0x1000);
    int steps = 0;
    while (cpu.pc() != 0x1024 && steps < 200) {
        cpu.step(mem);  // Fault results are expected: they enter the handler
        ++steps;
    }
    CHECK(cpu.pc() == 0x1024);
    CHECK(cpu.readGpr(8) == 10);
    CHECK(cpu.readGpr(9) == 20);
    CHECK(cpu.readGpr(10) == 30);
    CHECK(cpu.readGpr(11) == 40);
    CHECK(!cpu.statusExl() && !cpu.statusErl());
    CHECK(cpu.fault() == Mips64Fault::None);

    // Log slot 0: syscall at 0x100C, ExcCode 8, BD clear.
    uint8_t buf[4];
    mem.Read(0x2000, buf, 4);
    const uint32_t epc0 = static_cast<uint32_t>(buf[0]) |
                          (static_cast<uint32_t>(buf[1]) << 8) |
                          (static_cast<uint32_t>(buf[2]) << 16) |
                          (static_cast<uint32_t>(buf[3]) << 24);
    mem.Read(0x2004, buf, 4);
    const uint32_t cause0 = static_cast<uint32_t>(buf[0]) |
                            (static_cast<uint32_t>(buf[1]) << 8) |
                            (static_cast<uint32_t>(buf[2]) << 16) |
                            (static_cast<uint32_t>(buf[3]) << 24);
    CHECK(epc0 == 0x100C);
    CHECK(((cause0 >> 2) & 0x1F) == 8);
    CHECK((cause0 & 0x80000000U) == 0);
    // Log slot 1: break in delay slot, EPC = branch 0x1014, BD set, ExcCode 9.
    mem.Read(0x2008, buf, 4);
    const uint32_t epc1 = static_cast<uint32_t>(buf[0]) |
                          (static_cast<uint32_t>(buf[1]) << 8) |
                          (static_cast<uint32_t>(buf[2]) << 16) |
                          (static_cast<uint32_t>(buf[3]) << 24);
    mem.Read(0x200C, buf, 4);
    const uint32_t cause1 = static_cast<uint32_t>(buf[0]) |
                            (static_cast<uint32_t>(buf[1]) << 8) |
                            (static_cast<uint32_t>(buf[2]) << 16) |
                            (static_cast<uint32_t>(buf[3]) << 24);
    CHECK(epc1 == 0x1014);
    CHECK(((cause1 >> 2) & 0x1F) == 9);
    CHECK((cause1 & 0x80000000U) != 0);
    // Counter reached 2.
    mem.Read(0x2010, buf, 4);
    const uint32_t count = static_cast<uint32_t>(buf[0]) |
                           (static_cast<uint32_t>(buf[1]) << 8) |
                           (static_cast<uint32_t>(buf[2]) << 16) |
                           (static_cast<uint32_t>(buf[3]) << 24);
    CHECK(count == 2);
}

int main() {
    testCp0ResetValues();
    testCp0WritableFields();
    testCp0TransferWidths();
    testUnsupportedCp0();
    testTrappingOverflow();
    testTrappingOverflow2();
    testSyscallBreak();
    testReservedInstructionEntry();
    testMisalignedEntries();
    testEpcAndBd();
    testVectorSelection();
    testEret();
    testNestedException();
    testMultDiv32();
    testDmult64();
    testDivEdges();
    testHiLoMoves();
    testRegimmBranches();
    testRegimmLink();
    testBranchLikely();
    testBranchLikely2();
    testBranchLikely3();
    testBigEndianCoverage();
    testCountCompare();
    testSerializationV2();
    testIsolation();
    testHandlerDemo();

    std::cout << "test_mips64_cp0_exceptions: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
