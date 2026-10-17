// MIPS64-3 focused test: virtual-to-physical translation pipeline,
// direct-mapped segments, physical bounds rejection, TLB exception routing
// and vectors, delay-slot faults, ERET retry and genuine guest TLB
// refill/Modified handler demonstrations through real encoded MIPS64r2
// instructions. IMemory is PHYSICAL memory throughout.

#include <cstdint>
#include <iostream>
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

// -- sparse physical memory ------------------------------------------------------
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

// -- bounded physical memory: fixed size, throws outside --------------------------
class BoundedMemory : public ia64::IMemory {
public:
    explicit BoundedMemory(size_t size) : data_(size, 0) {}
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (size > data_.size() || address > data_.size() - size) {
            throw std::out_of_range("bounded memory: read out of range");
        }
        for (size_t i = 0; i < size; ++i) {
            dest[i] = data_[static_cast<size_t>(address) + i];
        }
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        if (size > data_.size() || address > data_.size() - size) {
            throw std::out_of_range("bounded memory: write out of range");
        }
        for (size_t i = 0; i < size; ++i) {
            data_[static_cast<size_t>(address) + i] = src[i];
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
    size_t GetTotalSize() const override { return data_.size(); }
    void Clear() override {
        std::fill(data_.begin(), data_.end(), 0);
    }
    const uint8_t* GetRawData() const override { return nullptr; }

private:
    std::vector<uint8_t> data_;
};

static bool stepOk(Mips64Cpu& cpu, ia64::IMemory& mem) {
    return cpu.step(mem) == Mips64StepResult::Continue;
}

static bool stepFault(Mips64Cpu& cpu, ia64::IMemory& mem, Mips64Fault expected) {
    return cpu.step(mem) == Mips64StepResult::Fault && cpu.fault() == expected;
}

template <size_t N>
static bool runCkSeg0(Mips64Cpu& cpu, ia64::IMemory& mem, uint64_t physBase,
                      const uint32_t (&words)[N]) {
    const bool le = cpu.isLittleEndian();
    for (size_t i = 0; i < N; ++i) {
        storeWordPhys(mem, physBase + i * 4, words[i], le);
    }
    const uint64_t savedPc = cpu.pc();
    cpu.setPc(0xFFFFFFFF80000000ULL | (physBase & 0x1FFFFFFFULL));
    bool ok = true;
    for (size_t i = 0; i < N; ++i) {
        if (cpu.step(mem) != Mips64StepResult::Continue) {
            ok = false;
            break;
        }
    }
    cpu.setPc(savedPc);
    return ok;
}

static void storeData32(ia64::IMemory& mem, uint64_t phys, uint32_t value, bool le) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[le ? i : 3 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
    }
    mem.Write(phys, bytes, 4);
}

static uint32_t loadData32(ia64::IMemory& mem, uint64_t phys, bool le) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    mem.Read(phys, bytes, 4);
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[le ? i : 3 - i]) << (8 * i);
    }
    return value;
}

// -- 1. pure vector table (both BEV states, all offsets) ------------------------------
static void testVectorTable() {
    CHECK(Mips64Cpu::exceptionVectorFor(true, 0, 0x000) == 0xFFFFFFFFBFC00200ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(true, 0, 0x080) == 0xFFFFFFFFBFC00280ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(true, 0, 0x180) == 0xFFFFFFFFBFC00380ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(false, 0x80000000ULL, 0x000) ==
          0xFFFFFFFF80000000ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(false, 0x80000000ULL, 0x080) ==
          0xFFFFFFFF80000080ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(false, 0x80000000ULL, 0x180) ==
          0xFFFFFFFF80000180ULL);
    CHECK(Mips64Cpu::exceptionVectorFor(false, 0xA0000000ULL, 0x180) ==
          0xFFFFFFFFA0000180ULL);
    Mips64Cpu cpu;  // reset: BEV=1
    CHECK(cpu.exceptionVector() == 0xFFFFFFFFBFC00380ULL);  // corrected general
    CHECK(cpu.refillVector(0x1000) == 0xFFFFFFFFBFC00200ULL);
}

// -- 2. segment classification + privilege at probe level ---------------------------------
static void testSegmentProbe() {
    Mips64Cpu cpu;  // kernel (reset), ASID 0, no TLB entries
    // kuseg without entry: refill (mapped, privilege OK). VPN2 nonzero so
    // the all-zero entries cannot shadow the probe with an Invalid result.
    Mips64Probe k = cpu.probeAddress(0x4000, Mips64Access::Load);
    CHECK(!k.ok && k.isRefill && k.fault == Mips64Fault::TlbRefillLoad &&
          k.code == Mips64ExcCode::TLBL);
    // ckseg0/ckseg1: direct, kernel OK.
    Mips64Probe c0 = cpu.probeAddress(0xFFFFFFFF80001000ULL, Mips64Access::Load);
    CHECK(c0.ok && c0.phys == 0x1000ULL);
    Mips64Probe c1 = cpu.probeAddress(0xFFFFFFFFBFC00380ULL, Mips64Access::Fetch);
    CHECK(c1.ok && c1.phys == (0xBFC00380ULL & 0x1FFFFFFFULL));
    // cksseg/ckseg3: mapped, kernel OK, no entry -> refill.
    Mips64Probe s = cpu.probeAddress(0xFFFFFFFFC0001000ULL, Mips64Access::Load);
    CHECK(!s.ok && s.isRefill);
    Mips64Probe k3 = cpu.probeAddress(0xFFFFFFFFE0001000ULL, Mips64Access::Store);
    CHECK(!k3.ok && k3.isRefill && k3.fault == Mips64Fault::TlbRefillStore &&
          k3.code == Mips64ExcCode::TLBS);
    // Zero-extended kernel-looking addresses are NOT ckseg*: address error.
    Mips64Probe z = cpu.probeAddress(0x0000000080000000ULL, Mips64Access::Load);
    CHECK(!z.ok && !z.isRefill && z.fault == Mips64Fault::AddressErrorLoad);
    // 64-bit regions are explicit address errors, never truncated.
    const uint64_t big[] = {0x8000000000000000ULL,  // XKPHYS region
                            0xC000000000000000ULL,  // XKSEG region
                            0x4000000000000001ULL, 0xFFFFFFFF00000000ULL};
    for (uint64_t va : big) {
        Mips64Probe p = cpu.probeAddress(va, Mips64Access::Load);
        CHECK(!p.ok && !p.isRefill && p.fault == Mips64Fault::AddressErrorLoad &&
              p.code == Mips64ExcCode::AdEL);
        Mips64Probe pf = cpu.probeAddress(va, Mips64Access::Fetch);
        CHECK(!pf.ok && pf.fault == Mips64Fault::AddressErrorFetch);
        Mips64Probe ps = cpu.probeAddress(va, Mips64Access::Store);
        CHECK(!ps.ok && ps.fault == Mips64Fault::AddressErrorStore &&
              ps.code == Mips64ExcCode::AdES);
    }
}

// -- 3. direct-mapped execution through CKSEG0 ----------------------------------------------
static void testDirectMappingExecution() {
    SparseMemory mem;
    Mips64Cpu cpu;  // kernel: CKSEG0 accessible, no TLB entry needed
    const bool le = true;
    const uint64_t codePhys = 0x1000ULL;
    const uint64_t codeVirt = 0xFFFFFFFF80001000ULL;
    const uint64_t dataPhys = 0x2000ULL;

    storeData32(mem, dataPhys, 0xDEADBEEF, le);
    // addiu $8,$0,0x2000 (data low VA) ; lw $9,0($8)... but $8 holds only
    // the low 32 bits; dataVirt needs full 64-bit construction:
    // lui $8, 0x8000 -> 0xFFFFFFFF80000000 ; ori $8, $8, 0x2000.
    const uint32_t prog[] = {
        (0x0FU << 26) | (8U << 16) | 0x8000U,  // lui $8, 0x8000
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x2000U,  // ori $8,$8,0x2000
        encI(0x23, 8, 9, 0),  // lw $9, 0($8)
        encI(0x2B, 8, 9, 4),  // sw $9, 4($8)
    };
    for (int i = 0; i < 4; ++i) {
        storeWordPhys(mem, codePhys + static_cast<uint64_t>(i) * 4, prog[i], le);
    }
    cpu.setPc(codeVirt);
    for (int i = 0; i < 4; ++i) {
        CHECK(stepOk(cpu, mem));
    }
    CHECK(cpu.readGpr(9) == 0xFFFFFFFFDEADBEEFULL);  // lw sign-extends
    CHECK(loadData32(mem, dataPhys + 4, le) == 0xDEADBEEF);
    CHECK(cpu.fault() == Mips64Fault::None);
}

// -- 4. physical memory bounds rejection (DBE/IBE via translation) -------------------------
static void testPhysicalBoundsRejection() {
    BoundedMemory mem(64 * 1024);  // 64 KiB physical
    Mips64Cpu cpu;
    // Map kuseg VA 0x10000 -> PA 0x20000 (beyond 64 KiB) with a 4 KiB entry.
    // EntryHi = 0x10000 (VPN2 8, ASID 0); Lo pfn = 0x2000 (PA 0x20000).
    const uint32_t install[] = {
        (0x0FU << 26) | (8U << 16) | 1U,  // lui $8, 1 -> 0x10000
        encCp0(5, 8, 10, 0),  // EntryHi
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x0FU << 26) | (8U << 16) | 0x8U,  // lui $8, 8 -> 0x80000
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x7U,  // ori -> 0x80007
        encCp0(5, 8, 2, 0),  // Lo0: pfn 0x2000? (0x80007>>6 = 0x2000), D|V|G
        encCp0(5, 8, 3, 0),  // Lo1: same
        encCp0(5, 0, 0, 0),  // Index 0
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x1000, install));
    // VA 0x10000 -> PA 0x20000 (beyond 64 KiB): load -> DBE, store -> DBE.
    const uint32_t access[] = {
        (0x0FU << 26) | (8U << 16) | 0x1U,  // lui $8, 1 -> 0x10000
        encI(0x23, 8, 9, 0),  // lw $9, 0($8)
    };
    for (int i = 0; i < 2; ++i) {
        storeWordPhys(mem, 0x2000 + static_cast<uint64_t>(i) * 4, access[i], true);
    }
    cpu.setPc(0xFFFFFFFF80002000ULL);
    CHECK(stepOk(cpu, mem));  // lui retires (direct-mapped code)
    CHECK(stepFault(cpu, mem, Mips64Fault::MemoryFaultLoad));
    CHECK(cpu.causeExcCode() == 7);  // DBE, BadVAddr untouched by bus errors
    CHECK(cpu.cp0BadVAddr() == 0);
    // Fetch beyond physical memory -> IBE (CKSEG0 VA for PA 0x20000).
    Mips64Cpu cpu2;
    cpu2.setPc(0xFFFFFFFF80020000ULL);  // PA 0x20000, beyond 64 KiB
    CHECK(stepFault(cpu2, mem, Mips64Fault::MemoryFaultFetch));
    CHECK(cpu2.causeExcCode() == 6);  // IBE
}

// -- 5. TLB refill routing: BEV=1/BEV=0, fetch/load/store, nested -----------------------
// Test code runs from CKSEG0 (direct-mapped, no TLB entry) so the kuseg
// data VAs under test are genuinely unmapped (fresh CPUs, no entries).
static void testRefillVectors() {
    const uint64_t kCodePhys = 0x4000ULL;
    const uint64_t kCodeVirt = 0xFFFFFFFF80004000ULL;
    // BEV=1 (reset): load miss uses the refill vector.
    {
        SparseMemory mem;
        // lui $4, 2 ($4 = 0x20000); lw $8, 0x120($4) -> VA 0x20120, no entry.
        storeWordPhys(mem, kCodePhys, (0x0FU << 26) | (4U << 16) | 2U, true);
        storeWordPhys(mem, kCodePhys + 4, encI(0x23, 4, 8, 0x120), true);
        Mips64Cpu cpu;
        cpu.setPc(kCodeVirt);
        CHECK(stepOk(cpu, mem));  // lui retires
        CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));
        CHECK(cpu.causeExcCode() == 2);  // TLBL
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00200ULL);
        CHECK(cpu.cp0Epc() == kCodeVirt + 4);
        CHECK(cpu.cp0BadVAddr() == 0x20120ULL);
        CHECK(cpu.statusExl() && !cpu.causeBd());
        // EntryHi.VPN2 = fault VPN, ASID preserved (0); Context/XContext set.
        CHECK(cpu.cp0EntryHi() == (0x20120ULL & 0xFFFFFFFFFFFFE000ULL));
        CHECK((cpu.cp0Context() >> 23) == ((0x20120ULL >> 23) & 0x1FFULL));
        CHECK(((cpu.cp0XContext() >> 13) & 0x3FFFFULL) ==
              ((0x20120ULL >> 13) & 0x3FFFFULL));
        CHECK(((cpu.cp0XContext() >> 31) & 0x7ULL) == 0);  // R = VA[63:61]
    }
    // Store miss -> TLBS, still refill vector when EXL was clear.
    {
        SparseMemory mem;
        storeWordPhys(mem, kCodePhys, (0x0FU << 26) | (4U << 16) | 2U, true);
        storeWordPhys(mem, kCodePhys + 4, encI(0x2B, 4, 8, 0x120), true);
        Mips64Cpu cpu;
        cpu.setPc(kCodeVirt);
        CHECK(stepOk(cpu, mem));
        CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillStore));
        CHECK(cpu.causeExcCode() == 3);  // TLBS
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00200ULL);
        CHECK(cpu.cp0BadVAddr() == 0x20120ULL);
    }
    // Fetch miss -> TLBL on the refill vector.
    {
        SparseMemory mem;  // empty: VA 0x4000 has no mapping
        Mips64Cpu cpu;
        cpu.setPc(0x4000);
        CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillFetch));
        CHECK(cpu.causeExcCode() == 2);
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00200ULL);
        CHECK(cpu.cp0Epc() == 0x4000ULL);
        CHECK(cpu.cp0BadVAddr() == 0x4000ULL);
    }
    // BEV=0: refill vector derives from EBase (reset 0x80000000). mtc0 clears
    // BEV from CKSEG0 (direct-mapped), then a load from unmapped kuseg VA 0
    // faults with EXL clear -> EBase refill vector.
    {
        SparseMemory mem;
        const uint64_t kScratchPhys = 0x4000ULL;
        const uint32_t code[] = {
            encCp0(4, 0, 12, 0),  // mtc0 $0, Status  (BEV clear)
            encI(0x23, 4, 8, 0x2000),  // lw $8, 0x2000($4) -> VA 0x2000, no mapping (VPN2=1)
        };
        for (size_t i = 0; i < 2; ++i)
            storeWordPhys(mem, kScratchPhys + i * 4, code[i], true);
        Mips64Cpu cpu;
        cpu.setPc(0xFFFFFFFF80004000ULL);
        CHECK(stepOk(cpu, mem));  // BEV cleared (still kernel, KSU=0)
        CHECK(cpu.exceptionVector() == 0xFFFFFFFF80000180ULL);
        CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));
        CHECK(cpu.pc() == 0xFFFFFFFF80000000ULL);  // EBase refill vector
        CHECK(cpu.causeExcCode() == 2);
        CHECK(cpu.cp0BadVAddr() == 0x2000ULL);
    }
    // Nested (EXL already set): miss uses the GENERAL vector, EPC preserved.
    {
        SparseMemory mem;
        const uint64_t kScratchPhys = 0x4010ULL;
        const uint32_t code[] = {
            0x0000000CU,            // syscall  -> EXL set, EPC = this PC
            encI(0x23, 4, 8, 0x2000),  // lw $8, 0x2000($4) -> VA 0x2000, no mapping (VPN2=1)
        };
        for (size_t i = 0; i < 2; ++i)
            storeWordPhys(mem, kScratchPhys + i * 4, code[i], true);
        Mips64Cpu cpu;
        cpu.setPc(0xFFFFFFFF80004010ULL);
        CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
        CHECK(cpu.statusExl());
        const uint64_t outerEpc = cpu.cp0Epc();
        cpu.setPc(0xFFFFFFFF80004014ULL);  // the faulting load, EXL still set
        CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));
        CHECK(cpu.causeExcCode() == 2);
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);  // general, not refill
        CHECK(cpu.cp0Epc() == outerEpc);  // outer restart address intact
    }
    // SYSCALL/BREAK still use the corrected BEV=1 general vector.
    {
        SparseMemory mem;
        storeWordPhys(mem, kCodePhys, 0x0000000CU, true);
        storeWordPhys(mem, kCodePhys + 4, 0x0000000DU, true);
        Mips64Cpu cpu;
        cpu.setPc(kCodeVirt);
        CHECK(stepFault(cpu, mem, Mips64Fault::SyscallTrap));
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);
        cpu.setPc(kCodeVirt + 4);
        CHECK(stepFault(cpu, mem, Mips64Fault::BreakTrap));
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);
    }
}

// -- 6. Invalid vs Modified distinction ------------------------------------------------------
// Test code runs from CKSEG0; the kuseg data VAs use one explicit entry
// (no identity map, so nothing shadows the entry under test).
static void testInvalidVsModified() {
    SparseMemory mem;
    const uint64_t kCodePhys = 0x3000ULL;
    const uint64_t kCodeVirt = 0xFFFFFFFF80003000ULL;
    // Entry 1: VPN2 0x10 (VA 0x20000 even / 0x21000 odd), 4 KiB,
    // even V=0, odd V=1/D=0 (pfn 1 -> PA 0x1000).
    const uint32_t install[] = {
        (0x0FU << 26) | (8U << 16) | 0x2U,  // lui $8, 2 -> 0x20000
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x10, ASID 0)
        encCp0(5, 0, 5, 0),  // PageMask 0
        encCp0(5, 0, 2, 0),  // Lo0 = 0 (invalid)
        (0x19U << 26) | (8U << 16) | 0x42U,  // daddiu $8,$0,0x42 (pfn1,V; no D)
        encCp0(5, 8, 3, 0),  // Lo1 = 0x42: pfn 1, V=1, D=0, G=0
        (0x19U << 26) | (8U << 16) | 1U,  // Index 1
        encCp0(5, 8, 0, 0),
        0x42000002U,  // tlbwi
    };
    // lui $4, 2 ($4 = 0x20000); lw/sw even+odd.
    const uint32_t code[] = {
        (0x0FU << 26) | (4U << 16) | 0x2U,  // lui $4, 2
        encI(0x23, 4, 8, 0),  // lw $8, 0($4) (even invalid)
        encI(0x2B, 4, 8, 0),  // sw $8, 0($4) (even invalid)
        encI(0x23, 4, 8, 0x1000),  // lw $8, 0x1000($4) (odd valid RO)
        encI(0x2B, 4, 8, 0x1000),  // sw $8, 0x1000($4) (odd modified)
    };
    for (int i = 0; i < 5; ++i) {
        storeWordPhys(mem, kCodePhys + static_cast<uint64_t>(i) * 4, code[i], true);
    }
    Mips64Cpu cpu;
    CHECK(runCkSeg0(cpu, mem, 0x10000, install));
    // Prime $4 = 0x20000 first (the faulting PCs below are entered directly).
    cpu.setPc(kCodeVirt);
    CHECK(stepOk(cpu, mem));  // lui $4
    // Load from the invalid even page -> TLBL Invalid on the GENERAL vector.
    cpu.setPc(kCodeVirt + 4);
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbInvalidLoad));
    CHECK(cpu.causeExcCode() == 2);
    CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);  // general even with EXL=0
    CHECK(cpu.cp0BadVAddr() == 0x20000ULL);
    cpu.setPc(kCodeVirt + 8);
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbInvalidStore));
    CHECK(cpu.causeExcCode() == 3);
    CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);
    // Load from the valid read-only odd page succeeds...
    storeData32(mem, 0x1000, 0x12345678, true);  // PA 0x1000 (pfn 1)
    Mips64Cpu cpu2;
    CHECK(runCkSeg0(cpu2, mem, 0x10000, install));
    cpu2.setPc(kCodeVirt);
    CHECK(stepOk(cpu2, mem));  // lui $4
    cpu2.setPc(kCodeVirt + 12);
    CHECK(stepOk(cpu2, mem));
    CHECK(cpu2.readGpr(8) == 0x12345678ULL);
    // ...but storing there raises Modified (code 1), general vector.
    cpu2.setPc(kCodeVirt + 16);
    CHECK(stepFault(cpu2, mem, Mips64Fault::TlbModified));
    CHECK(cpu2.causeExcCode() == 1);
    CHECK(cpu2.pc() == 0xFFFFFFFFBFC00380ULL);
    CHECK(cpu2.cp0BadVAddr() == 0x21000ULL);
    CHECK(cpu2.cp0EntryHi() == (0x21000ULL & 0xFFFFFFFFFFFFE000ULL));
}

// -- 7. TLB faults in branch delay slots (EPC = branch, BD = 1) ---------------------------
static void testDelaySlotTlbFault() {
    SparseMemory mem;
    const uint64_t kCodePhys = 0x4000ULL;
    const uint64_t kCodeVirt = 0xFFFFFFFF80004000ULL;
    // lui $4, 5 ($4 = 0x50000, unmapped); beq taken; delay slot lw faults.
    storeWordPhys(mem, kCodePhys, (0x0FU << 26) | (4U << 16) | 5U, true);
    storeWordPhys(mem, kCodePhys + 4, encI(0x04, 0, 0, 1), true);  // beq $0,$0,1
    storeWordPhys(mem, kCodePhys + 8, encI(0x23, 4, 8, 0), true);  // lw (no entry)
    Mips64Cpu cpu;
    cpu.setPc(kCodeVirt);
    CHECK(stepOk(cpu, mem));  // lui $4
    CHECK(stepOk(cpu, mem));  // branch arms the slot
    CHECK(cpu.delayArmed());
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));
    CHECK(cpu.causeBd());
    CHECK(cpu.cp0Epc() == kCodeVirt + 4);  // originating branch
    CHECK(cpu.cp0BadVAddr() == 0x50000ULL);
    CHECK(cpu.pc() == 0xFFFFFFFFBFC00200ULL);  // EXL was 0 -> refill vector
}

// -- 8. decode has no side effects on unmapped VAs -----------------------------------------------
static void testDecodeSideEffectFree() {
    SparseMemory mem;
    Mips64Cpu cpu;  // fresh: no TLB entries at all
    Mips64Decoded d = cpu.decodeAt(mem, 0x60000);  // unmapped kuseg VA
    CHECK(!d.valid);
    CHECK(!cpu.statusExl() && cpu.cp0Epc() == 0 && cpu.cp0BadVAddr() == 0);
    CHECK(cpu.cp0EntryHi() == 0);  // failed decode touches no TLB state
}

// -- 9. genuine guest TLB-refill handler demonstration ----------------------------------------------
// Main (CKSEG0, kernel): clears ERL (keeps BEV=1), loads $4 = U, lw faults,
// handler (at the BEV=1 refill vector) installs the mapping with TLBWI and
// ERETs; the lw is retried and succeeds. No host-side TLB injection.
static void testGuestRefillDemo() {
    SparseMemory mem;
    const bool le = true;
    const uint64_t kMainPhys = 0x1000ULL;
    const uint64_t kMainVirt = 0xFFFFFFFF80001000ULL;
    const uint64_t kRefillVec = 0xFFFFFFFFBFC00200ULL;
    const uint64_t kDataPhys = 0x5000ULL;
    const uint64_t kDataVirt = 0x00002000ULL;  // kuseg, initially unmapped
    const uint64_t kCountPhys = 0x3000ULL;

    const uint32_t kExpected = 0xDEADBEEF;
    storeData32(mem, kDataPhys, kExpected, le);
    storeData32(mem, kCountPhys, 0, le);
    // Main: lui $8,0x40 ; mtc0 $8,Status (BEV only: clear ERL/EXL) ;
    //       daddiu $4,$0,U ; lw $8,0($4) ; lui $6,0x8000 ; ori $6,$6,0x3004 ;
    //       sw $8,0($6) (publish loaded value to a CKSEG0 marker).
    const uint32_t main[] = {
        (0x0FU << 26) | (8U << 16) | 0x40U,  // lui $8, 0x40
        encCp0(4, 8, 12, 0),  // mtc0 $8, Status
        (0x19U << 26) | (4U << 16) | 0x2000U,  // daddiu $4, $0, 0x2000 (U)
        encI(0x23, 4, 8, 0),  // lw $8, 0($4)  <- refill fault, EPC here
        (0x0FU << 26) | (6U << 16) | 0x8000U,  // lui $6, 0x8000
        (0x0DU << 26) | (6U << 21) | (6U << 16) | 0x3004U,  // ori $6,$6,0x3004
        encI(0x2B, 6, 8, 0),  // sw $8, 0($6)
    };
    for (int i = 0; i < 7; ++i) {
        storeWordPhys(mem, kMainPhys + static_cast<uint64_t>(i) * 4, main[i], le);
    }
    // Handler at the refill vector (all predetermined immediates):
    // daddiu $k0,$0,U ; dmtc0 $k0,EntryHi ;
    // U = 0x2000 has bit 12 clear, so it is the EVEN page of the pair:
    // daddiu $k0,$0,0x147 ; dmtc0 $k0,EntryLo0 (even pfn 0x5 -> PA 0x5000) ;
    // daddiu $k0,$0,0x107 ; dmtc0 $k0,EntryLo1 (odd pfn 0x4) ;
    // dmtc0 $0,PageMask ; dmtc0 $0,Index ; tlbwi ;
    // lui $k0,0x8000 ; ori $k0,$k0,0x3000 ; lw $k1,0($k0) ; daddiu $k1,$k1,1 ;
    // sw $k1,0($k0) (handler-run counter) ; eret.
    const uint32_t handler[] = {
        (0x19U << 26) | (26U << 16) | 0x2000U,  // daddiu $k0,$0,0x2000
        encCp0(5, 26, 10, 0),  // dmtc0 EntryHi
        (0x19U << 26) | (26U << 16) | 0x147U,  // daddiu $k0,$0,0x147
        encCp0(5, 26, 2, 0),  // dmtc0 EntryLo0
        (0x19U << 26) | (26U << 16) | 0x107U,  // daddiu $k0,$0,0x107
        encCp0(5, 26, 3, 0),  // dmtc0 EntryLo1
        encCp0(5, 0, 5, 0),  // dmtc0 $0, PageMask
        encCp0(5, 0, 0, 0),  // dmtc0 $0, Index
        0x42000002U,  // tlbwi
        (0x0FU << 26) | (26U << 16) | 0x8000U,  // lui $k0, 0x8000
        (0x0DU << 26) | (26U << 21) | (26U << 16) | 0x3000U,  // ori
        encI(0x23, 26, 27, 0),  // lw $k1, 0($k0)
        (0x19U << 26) | (27U << 21) | (27U << 16) | 1U,  // daddiu $k1,$k1,1
        encI(0x2B, 26, 27, 0),  // sw $k1, 0($k0)
        0x42000018U,  // eret
    };
    for (int i = 0; i < 15; ++i) {
        CHECK(storeGuestWord(mem, kRefillVec + static_cast<uint64_t>(i) * 4,
                             handler[i], le));
    }

    Mips64Cpu cpu;
    cpu.setPc(kMainVirt);
    CHECK(stepOk(cpu, mem));  // lui
    CHECK(stepOk(cpu, mem));  // mtc0 Status (ERL cleared, BEV kept)
    CHECK(!cpu.statusErl() && cpu.statusBev());
    CHECK(stepOk(cpu, mem));  // daddiu $4
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));  // lw faults
    CHECK(cpu.pc() == kRefillVec);
    CHECK(cpu.causeExcCode() == 2);
    CHECK(cpu.cp0Epc() == kMainVirt + 12);
    CHECK(cpu.cp0BadVAddr() == kDataVirt);
    // Run the guest handler to its ERET (bounded).
    int handlerSteps = 0;
    while (cpu.pc() != kMainVirt + 12 && handlerSteps < 64) {
        cpu.step(mem);
        ++handlerSteps;
    }
    CHECK(cpu.pc() == kMainVirt + 12);  // ERET returned to the faulting lw
    CHECK(!cpu.statusExl());
    CHECK(loadData32(mem, kCountPhys, le) == 1);  // handler ran exactly once
    // The mapping is genuinely installed: even page pfn 0x5, V+D.
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(0, e) && e.pfn0 == 5 && e.v0 && e.d0);
    // Retry: the original lw now succeeds with the expected data.
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 0xFFFFFFFFDEADBEEFULL);
    CHECK(stepOk(cpu, mem));  // lui $6
    CHECK(stepOk(cpu, mem));  // ori $6
    CHECK(stepOk(cpu, mem));  // sw publishes the value
    CHECK(loadData32(mem, 0x3004, le) == kExpected);
    CHECK(cpu.fault() == Mips64Fault::None);
}

// -- 10. genuine guest Modified handler demonstration -----------------------------------------------
// Main installs a valid read-only mapping, loads fine, then stores: the
// guest handler (general vector) sets D via TLBP/TLBR/modify/TLBWI + ERET;
// the store is retried and succeeds.
static void testGuestModifiedDemo() {
    SparseMemory mem;
    const bool le = true;
    const uint64_t kMainPhys = 0x8000ULL;
    const uint64_t kMainVirt = 0xFFFFFFFF80008000ULL;
    const uint64_t kGenVec = 0xFFFFFFFFBFC00380ULL;
    const uint64_t kDataPhys = 0x7000ULL;
    // kDataVirt = 0x00004000 (even page of pair VPN2 2).
    const uint32_t kExpected = 0x12345678;
    storeData32(mem, kDataPhys, 0, le);
    // Main: clear ERL; install entry 1 mapping V->PA read-only (D=0):
    // EntryHi=V, PageMask=0, Lo0 = (0x7<<6)|V|G = 0x1C3 (D clear),
    // Lo1=0, Index=1, tlbwi; $4 = V; lw $8,0($4) (ok); sw (Modified fault).
    const uint32_t main[] = {
        (0x0FU << 26) | (8U << 16) | 0x40U,  // lui $8, 0x40
        encCp0(4, 8, 12, 0),  // mtc0 Status (BEV only)
        (0x19U << 26) | (8U << 16) | 0x4000U,  // daddiu $8,$0,0x4000 (EntryHi)
        encCp0(5, 8, 10, 0),  // dmtc0 EntryHi
        encCp0(5, 0, 5, 0),  // dmtc0 $0, PageMask
        (0x0FU << 26) | (8U << 16) | 0x0U,  // lui $8, 0
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x1C3U,  // ori -> Lo0
        encCp0(5, 8, 2, 0),  // dmtc0 EntryLo0 (pfn 7 -> PA 0x7000, V, no D)
        encCp0(5, 0, 3, 0),  // dmtc0 $0, EntryLo1 (invalid odd)
        (0x19U << 26) | (8U << 16) | 1U,  // daddiu $8,$0,1
        encCp0(5, 8, 0, 0),  // dmtc0 Index = 1
        0x42000002U,  // tlbwi
        (0x19U << 26) | (4U << 16) | 0x4000U,  // daddiu $4,$0,V
        encI(0x23, 4, 8, 0),  // lw $8, 0($4)
        encI(0x2B, 4, 8, 0),  // sw $8, 0($4) <- Modified fault, EPC here
        encI(0x2B, 4, 8, 4),  // sw $8, 4($4) (post-handler marker)
    };
    for (int i = 0; i < 16; ++i) {
        storeWordPhys(mem, kMainPhys + static_cast<uint64_t>(i) * 4, main[i], le);
    }
    // Handler at the general vector: TLBP (EntryHi already holds the fault
    // VPN); TLBR; dmfc0 $k0,EntryLo0; ori D; dmtc0; tlbwi; eret.
    const uint32_t handler[] = {
        0x42000008U,  // tlbp
        0x42000001U,  // tlbr
        encCp0(1, 26, 2, 0),  // dmfc0 $k0, EntryLo0
        (0x0DU << 26) | (26U << 21) | (26U << 16) | 0x4U,  // ori $k0,$k0,D
        encCp0(5, 26, 2, 0),  // dmtc0 EntryLo0
        0x42000002U,  // tlbwi (Index from TLBP)
        0x42000018U,  // eret
    };
    for (int i = 0; i < 7; ++i) {
        CHECK(storeGuestWord(mem, kGenVec + static_cast<uint64_t>(i) * 4,
                             handler[i], le));
    }
    storeData32(mem, kDataPhys, kExpected, le);

    Mips64Cpu cpu;
    cpu.setPc(kMainVirt);
    for (int i = 0; i < 14; ++i) {
        CHECK(stepOk(cpu, mem));  // prologue + install + daddiu + lw
    }
    CHECK(cpu.readGpr(8) == 0x12345678ULL);  // read-only load succeeded
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbModified));  // sw faults
    CHECK(cpu.causeExcCode() == 1);
    CHECK(cpu.pc() == kGenVec);
    CHECK(cpu.cp0Epc() == kMainVirt + 14 * 4);
    int handlerSteps = 0;
    while (cpu.pc() != kMainVirt + 14 * 4 && handlerSteps < 64) {
        cpu.step(mem);
        ++handlerSteps;
    }
    CHECK(cpu.pc() == kMainVirt + 14 * 4);
    CHECK(!cpu.statusExl());
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(1, e) && e.d0);  // handler set Dirty
    CHECK(stepOk(cpu, mem));  // retried store succeeds
    CHECK(loadData32(mem, kDataPhys, le) == 0x12345678);
    CHECK(stepOk(cpu, mem));  // marker store
    CHECK(loadData32(mem, kDataPhys + 4, le) == 0x12345678);
}

// -- 11. snapshot/restore immediately before and after a TLB exception ---------------------
static void testSnapshotAroundException() {
    SparseMemory mem;
    const bool le = true;
    // Reuse the refill-demo program shape (smaller): main faults on lw.
    const uint64_t kMainPhys = 0x9000ULL;
    const uint64_t kMainVirt = 0xFFFFFFFF80009000ULL;
    const uint32_t main[] = {
        (0x0FU << 26) | (8U << 16) | 0x40U,  // lui $8, 0x40
        encCp0(4, 8, 12, 0),  // mtc0 Status
        (0x19U << 26) | (4U << 16) | 0x2000U,  // daddiu $4,$0,0x2000
        encI(0x23, 4, 8, 0),  // lw $8, 0($4) <- refill
    };
    for (int i = 0; i < 4; ++i) {
        storeWordPhys(mem, kMainPhys + static_cast<uint64_t>(i) * 4, main[i], le);
    }
    Mips64Cpu cpu;
    cpu.setPc(kMainVirt);
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    const std::vector<uint8_t> before = cpu.serialize();  // before the fault
    CHECK(stepFault(cpu, mem, Mips64Fault::TlbRefillLoad));
    const std::vector<uint8_t> after = cpu.serialize();  // after entry
    // Restore "before" elsewhere and fault identically.
    Mips64Cpu c1;
    CHECK(c1.deserialize(before.data(), before.size()));
    CHECK(stepFault(c1, mem, Mips64Fault::TlbRefillLoad));
    CHECK(c1.serialize() == after);
    // Restore "after" elsewhere: identical architectural state.
    Mips64Cpu c2;
    CHECK(c2.deserialize(after.data(), after.size()));
    CHECK(c2.serialize() == after);
    CHECK(c2.pc() == cpu.pc() && c2.cp0Epc() == cpu.cp0Epc());
    CHECK(c2.cp0BadVAddr() == cpu.cp0BadVAddr());
    CHECK(c2.cp0EntryHi() == cpu.cp0EntryHi());
    CHECK(c2.cp0Context() == cpu.cp0Context());
    CHECK(c2.causeExcCode() == cpu.causeExcCode());
}

// -- 12. big-endian loads/stores through translation -----------------------------------------------
static void testBigEndianMapped() {
    SparseMemory mem;
    Mips64Cpu cpu(Mips64Endianness::Big);
    CHECK(setupKusegIdentityMap(cpu, mem));  // BE-aware prologue
    // BE store then load at kuseg VA 0x800 (identity-mapped PA 0x800).
    cpu.writeGpr(9, 0xAABBCCDDULL);
    CHECK(storeGuestWord(mem, 0x200, encI(0x2B, 0, 9, 0x800), false));  // sw
    CHECK(storeGuestWord(mem, 0x204, encI(0x23, 0, 10, 0x800), false));  // lw
    cpu.setPc(0x200);
    CHECK(stepOk(cpu, mem));
    uint8_t wire[4] = {0, 0, 0, 0};
    mem.Read(0x800, wire, 4);  // physical image: BE byte order on the wire
    CHECK(wire[0] == 0xAA && wire[1] == 0xBB && wire[2] == 0xCC && wire[3] == 0xDD);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFAABBCCDDULL);  // lw sign-extends
}

int main() {
    std::cout << std::unitbuf;  // unbuffered: failures stay ordered on abort
    testVectorTable();
    testSegmentProbe();
    testDirectMappingExecution();
    testPhysicalBoundsRejection();
    testRefillVectors();
    testInvalidVsModified();
    testDelaySlotTlbFault();
    testDecodeSideEffectFree();
    testGuestRefillDemo();
    testGuestModifiedDemo();
    testSnapshotAroundException();
    testBigEndianMapped();

    std::cout << "test_mips64_mmu: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
