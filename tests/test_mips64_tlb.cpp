// MIPS64-3 focused test: bounded TLB + CP0 TLB-management registers +
// TLBP/TLBR/TLBWI/TLBWR semantics through genuine encoded MIPS64r2
// instructions (production decoder/executor), plus snapshot coverage.
//
// Memory model: IMemory is PHYSICAL. Test code runs from a CKSEG0 window
// (direct-mapped, no TLB entry needed); kuseg data VAs use explicit TLB
// entries installed by the test programs themselves.

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

// -- sparse physical memory: backs any PA, throws when absent -----------------
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

// Run a straight-line block of encoded instructions from a CKSEG0 window.
// All must retire with Continue. GPR/PC effects are the caller's concern.
// The block length is deduced from the array (no manual counts to mismatch).
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

// -- 1. reset state: bounded, invalid, deterministic ---------------------------
static void testTlbResetState() {
    Mips64Cpu cpu;
    CHECK(cpu.tlbSize() == 16);
    CHECK(cpu.cp0Index() == 0);
    CHECK(cpu.cp0Random() == 15);
    CHECK(cpu.cp0Wired() == 0);
    CHECK(cpu.cp0EntryHi() == 0);
    CHECK(cpu.cp0EntryLo0() == 0);
    CHECK(cpu.cp0EntryLo1() == 0);
    CHECK(cpu.cp0PageMask() == 0);
    CHECK(cpu.cp0Context() == 0);
    CHECK(cpu.cp0XContext() == 0);
    for (unsigned i = 0; i < cpu.tlbSize(); ++i) {
        Mips64TlbEntry e;
        CHECK(cpu.tlbEntry(i, e));
        CHECK(!e.v0 && !e.v1 && !e.d0 && !e.d1 && !e.g && !e.badMask);
    }
    Mips64TlbEntry bad;
    CHECK(!cpu.tlbEntry(16, bad));
    CHECK(!cpu.tlbEntry(999, bad));
    // Reset restores the deterministic state after heavy mutation.
    cpu.writeGpr(8, 0x1234);
    SparseMemory mem;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.reset();
    CHECK(cpu.cp0Random() == 15 && cpu.cp0Wired() == 0 && cpu.cp0EntryHi() == 0);
    Mips64TlbEntry e0;
    CHECK(cpu.tlbEntry(0, e0) && !e0.v0 && !e0.v1);
    // No mapping survives reset: kuseg fetch faults as refill. (VPN2 must be
    // nonzero: all-zero entries would match VPN2 0 with an Invalid result,
    // which is correct hardware behavior for uninitialized entries.)
    const Mips64Probe probe = cpu.probeAddress(0x4000, Mips64Access::Fetch);
    CHECK(!probe.ok && probe.isRefill && probe.fault == Mips64Fault::TlbRefillFetch);
}

// -- 2. CP0 TLB register transfers + writable masks ----------------------------
static void testCp0TlbRegisters() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // EntryHi keeps VPN2 (bits 63:13) + ASID, drops bits 12:8.
    // VPN2 = 0x12345, ASID = 0xAB: value = (0x12345 << 13) | 0xAB.
    const uint32_t block1[] = {
        (0x0FU << 26) | (8U << 16) | 0x91A2U,  // lui $8, 0x91A2
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x80ABU,  // ori $8,$8,0x80AB
        encCp0(5, 8, 10, 0),  // dmtc0 $8, EntryHi
    };
    CHECK(runCkSeg0(cpu, mem, 0x10000, block1));
    // $8 = 0xFFFFFFFF91A280AB (lui sign-extends); bits 12:8 already zero.
    CHECK(cpu.cp0EntryHi() == 0xFFFFFFFF91A280ABULL);
    // MTC0 sign-extends the low word, then masks apply.
    const uint32_t block2[] = {
        (0x0FU << 26) | (8U << 16) | 0xFFFFU,  // lui $8, 0xFFFF
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xFFFFU,  // ori -> 0xFFFFFFFF
        encCp0(4, 8, 10, 0),  // mtc0 $8, EntryHi (low32 = 0xFFFFFFFF)
    };
    CHECK(runCkSeg0(cpu, mem, 0x10100, block2));
    CHECK(cpu.cp0EntryHi() == 0xFFFFFFFFFFFFE0FFULL);

    // PageMask keeps only bits 24:13.
    const uint32_t block3[] = {
        (0x0FU << 26) | (8U << 16) | 0xFFFFU,  // lui $8, 0xFFFF
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xFFFFU,  // ori -> all ones
        encCp0(5, 8, 5, 0),  // dmtc0 $8, PageMask
    };
    CHECK(runCkSeg0(cpu, mem, 0x10200, block3));
    CHECK(cpu.cp0PageMask() == 0x01FFE000ULL);

    // Index keeps P + low 4 bits.
    const uint32_t block4[] = {
        (0x0FU << 26) | (8U << 16) | 0x8000U,  // lui $8, 0x8000 (negative)
        encCp0(4, 8, 0, 0),  // mtc0 $8, Index -> P set, field = 0
    };
    CHECK(runCkSeg0(cpu, mem, 0x10300, block4));
    CHECK(cpu.cp0Index() == 0x80000000ULL);

    // EntryLo drops bits 31:30 (36-bit physical cap, documented).
    const uint32_t block5[] = {
        (0x0FU << 26) | (8U << 16) | 0xFFFFU,  // lui
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xFFFFU,  // ori -> all ones
        encCp0(5, 8, 2, 0),  // dmtc0 $8, EntryLo0
        encCp0(5, 8, 3, 0),  // dmtc0 $8, EntryLo1
    };
    CHECK(runCkSeg0(cpu, mem, 0x10400, block5));
    CHECK(cpu.cp0EntryLo0() == 0x3FFFFFFFULL);
    CHECK(cpu.cp0EntryLo1() == 0x3FFFFFFFULL);

    // Context: only PTEBase (bits 22:4) is software-writable.
    const uint32_t block6[] = {
        (0x0FU << 26) | (8U << 16) | 0xFFFFU,  // lui
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xFFFFU,  // ori -> all ones
        encCp0(5, 8, 4, 0),  // dmtc0 $8, Context
    };
    CHECK(runCkSeg0(cpu, mem, 0x10500, block6));
    CHECK(cpu.cp0Context() == 0x007FFFF0ULL);

    // XContext: only PTEBase (bits 51:34) is software-writable.
    const uint32_t block7[] = {
        (0x0FU << 26) | (8U << 16) | 0xFFFFU,  // lui -> low32 all ones
        encCp0(5, 8, 20, 0),  // dmtc0 $8, XContext (upper 32 of GPR are 0)
    };
    CHECK(runCkSeg0(cpu, mem, 0x10600, block7));
    // lui sign-extends, so PTEBase (bits 51:34) reads all ones here.
    CHECK(cpu.cp0XContext() == 0x000FFFFC00000000ULL);

    // Wired clamps to [0, 16]; Random writes are ignored (read-only).
    const uint32_t block8[] = {
        (0x0FU << 26) | (8U << 16) | 0x00FFU,  // lui $8, 0xFF
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xFFFFU,  // ori -> 0xFFFFFF
        encCp0(5, 8, 6, 0),  // dmtc0 $8, Wired -> clamp 16
        encCp0(5, 8, 1, 0),  // dmtc0 $8, Random -> ignored
    };
    CHECK(runCkSeg0(cpu, mem, 0x10700, block8));
    CHECK(cpu.cp0Wired() == 16);
    CHECK(cpu.cp0Random() == 15);  // reset value, write had no effect

    // MFC0/DMFC0 read back the TLB registers (kernel mode here).
    const uint32_t block9[] = {
        encCp0(0, 9, 10, 0),   // mfc0 $9, EntryHi (low word sign-extended)
        encCp0(1, 10, 10, 0),  // dmfc0 $10, EntryHi (full 64)
        encCp0(0, 11, 0, 0),   // mfc0 $11, Index
        encCp0(1, 12, 1, 0),   // dmfc0 $12, Random
    };
    CHECK(runCkSeg0(cpu, mem, 0x10800, block9));
    CHECK(cpu.readGpr(10) == cpu.cp0EntryHi());
    CHECK(cpu.readGpr(12) == 15);
}

// -- 3. TLBWI/TLBR round trip incl. Global-bit joining --------------------------
static void testTlbwiTlbrRoundTrip() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // EntryHi: VPN2 = 0xABC, ASID = 0x42. EntryHi value = (0xABC << 13) | 0x42.
    // 0xABC << 13 = 0x1578000. lui 0x157 -> 0x1570000, ori 0x8042 -> 0x1578042.
    // PageMask = 0 (4 KiB). EntryLo0: pfn 0x123, C=2, D=1, V=1, G=1.
    //   (0x123 << 6) | (2 << 3) | 7 = 0x48C0 | 0x10 | 7 = 0x48D7.
    // EntryLo1: pfn 0x124, C=2, D=1, V=1, G=0 -> 0x4900|0x10|6 = 0x4916.
    const uint32_t block[] = {
        (0x0FU << 26) | (8U << 16) | 0x157U,  // lui $8, 0x157
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x8042U,  // ori -> EntryHi
        encCp0(5, 8, 10, 0),  // dmtc0 EntryHi
        encCp0(5, 0, 5, 0),  // dmtc0 $0, PageMask
        (0x0FU << 26) | (8U << 16) | 0x0U,  // lui $8, 0
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x48D7U,  // ori -> Lo0
        encCp0(5, 8, 2, 0),  // dmtc0 EntryLo0
        (0x0FU << 26) | (8U << 16) | 0x0U,  // lui $8, 0
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x4916U,  // ori -> Lo1
        encCp0(5, 8, 3, 0),  // dmtc0 EntryLo1
        (0x19U << 26) | (8U << 16) | 5U,  // daddiu $8, $0, 5
        encCp0(5, 8, 0, 0),  // dmtc0 Index = 5
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x11000, block));
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(5, e));
    CHECK(e.vpn2 == 0xABCULL && e.asid == 0x42);
    CHECK(e.mask == 0 && !e.badMask);
    CHECK(e.pfn0 == 0x123 && e.pfn1 == 0x124);
    CHECK(e.c0 == 2 && e.c1 == 2);
    CHECK(e.v0 && e.d0 && e.v1 && e.d1);
    CHECK(!e.g);  // G joins as AND: Lo0.G=1, Lo1.G=0 -> non-global
    // Round trip back through TLBR: clobber regs, read entry 5.
    const uint32_t block2[] = {
        encCp0(5, 0, 10, 0),  // dmtc0 $0, EntryHi
        encCp0(5, 0, 2, 0),  // dmtc0 $0, EntryLo0
        encCp0(5, 0, 3, 0),  // dmtc0 $0, EntryLo1
        encCp0(5, 0, 5, 0),  // dmtc0 $0, PageMask
        0x42000001U,  // tlbr (Index still 5)
    };
    CHECK(runCkSeg0(cpu, mem, 0x11100, block2));
    CHECK(cpu.cp0EntryHi() == ((0xABCULL << 13) | 0x42ULL));
    CHECK(cpu.cp0PageMask() == 0);
    CHECK(cpu.cp0EntryLo0() == 0x48D6ULL);  // G cleared (entry non-global)
    CHECK(cpu.cp0EntryLo1() == 0x4916ULL);
}

// -- 4. TLBP hit/miss + P-bit semantics ------------------------------------------
static void testTlbpHitMiss() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));  // entry 0: VPN2 0, ASID 0, 16M
    // Hit: EntryHi VPN2 = 0x100 (inside 16M pair), ASID 0.
    // EntryHi = 0x100 << 13 = 0x200000. lui 0x20 -> 0x200000.
    const uint32_t hit[] = {
        (0x0FU << 26) | (8U << 16) | 0x20U,  // lui $8, 0x20
        encCp0(5, 8, 10, 0),  // dmtc0 EntryHi
        0x42000008U,  // tlbp
    };
    CHECK(runCkSeg0(cpu, mem, 0x12000, hit));
    CHECK(cpu.cp0Index() == 0);  // hit at 0, P clear
    // Miss: VPN2 far away; preset Index field 7, P clear -> P set, field kept.
    const uint32_t miss[] = {
        (0x0FU << 26) | (8U << 16) | 0xBEEFU,  // lui $8, 0xBEEF
        encCp0(5, 8, 10, 0),  // dmtc0 EntryHi (VPN2 huge, ASID low bits)
        (0x19U << 26) | (8U << 16) | 7U,  // daddiu $8, $0, 7
        encCp0(5, 8, 0, 0),  // dmtc0 Index = 7
        0x42000008U,  // tlbp -> miss
    };
    CHECK(runCkSeg0(cpu, mem, 0x12100, miss));
    CHECK(cpu.cp0Index() == 0x80000007ULL);
}

// -- 5. ASID isolation ------------------------------------------------------------
static void testAsidIsolation() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // Entry 3: VPN2 = 0x40, ASID = 5, 4 KiB, both pages V+D, global clear.
    // EntryHi = (0x40 << 13) | 5 = 0x80005. Lo = (pfn<<6)|7.
    const uint32_t setup[] = {
        (0x0FU << 26) | (8U << 16) | 0x8U,  // lui $8, 8 -> 0x80000
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x5U,  // ori -> 0x80005
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x40, ASID 5)
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x19U << 26) | (8U << 16) | 0x87U,  // daddiu $8, $0, 0x87 (pfn2 D|V|G)
        encCp0(5, 8, 2, 0),  // Lo0
        (0x19U << 26) | (8U << 16) | 0x86U,  // daddiu $8, $0, 0x86 (pfn2 D|V)
        encCp0(5, 8, 3, 0),  // Lo1 (G clear -> non-global)
        (0x19U << 26) | (8U << 16) | 3U,  // daddiu $8, 3
        encCp0(5, 8, 0, 0),  // Index 3
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x13000, setup));
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(3, e) && !e.g && e.asid == 5);
    // Same ASID hits; different ASID misses (non-global).
    Mips64Probe hit = cpu.probeAddress(0x80000ULL, Mips64Access::Load);
    CHECK(hit.ok && hit.phys == 0x2000ULL);  // even page: pfn 2 -> PA 0x2000
    // Switch requester ASID to 6 (EntryHi ASID field): miss expected.
    const uint32_t asid6[] = {
        (0x0FU << 26) | (8U << 16) | 0x8U,  // lui $8, 8
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x6U,  // ori -> 0x80006
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x40, ASID 6)
        0x42000008U,  // tlbp -> miss
    };
    CHECK(runCkSeg0(cpu, mem, 0x13200, asid6));
    CHECK((cpu.cp0Index() & 0x80000000ULL) != 0);  // P set
    Mips64Probe miss = cpu.probeAddress(0x80000ULL, Mips64Access::Load);
    CHECK(!miss.ok && miss.isRefill);
}

// -- 6. global mappings ignore ASID --------------------------------------------------
static void testGlobalMapping() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // Entry 2: VPN2 = 0x60, ASID = 9, global (both G set), 4 KiB.
    const uint32_t setup[] = {
        (0x0FU << 26) | (8U << 16) | 0xCU,  // lui $8, 0xC -> 0xC0000
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x9U,  // ori -> 0xC0009
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x60, ASID 9)
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x19U << 26) | (8U << 16) | 0x47U,  // daddiu $8,$0,0x47 (pfn1 D|V|G)
        encCp0(5, 8, 2, 0),  // Lo0
        encCp0(5, 8, 3, 0),  // Lo1 (both G set -> global)
        (0x19U << 26) | (8U << 16) | 2U,  // daddiu $8, $0, 2
        encCp0(5, 8, 0, 0),  // dmtc0 Index = 2
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x14000, setup));
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(2, e) && e.g);
    // Requester ASID 9 hits...
    CHECK(cpu.probeAddress(0xC0000ULL, Mips64Access::Load).ok);
    // ...and so does an unrelated ASID (global ignores it).
    const uint32_t asid7[] = {
        (0x0FU << 26) | (8U << 16) | 0xCU,  // lui $8, 0xC
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x7U,  // ori -> 0xC0007
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x60, ASID 7)
    };
    CHECK(runCkSeg0(cpu, mem, 0x14100, asid7));
    Mips64Probe p = cpu.probeAddress(0xC0000ULL, Mips64Access::Load);
    CHECK(p.ok && p.phys == 0x1000ULL);
}

// -- 7. even/odd page selection -------------------------------------------------------
static void testEvenOddSelection() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // Entry 4: VPN2 = 0x02 (pair covers VA 0x4000/0x5000), 4 KiB,
    // even pfn 0xA, odd pfn 0xB, both V+D, non-global (ASID 0).
    // Lo0 = (0xA<<6)|2 = 0x282 (V set, D/G clear), Lo1 = (0xB<<6)|7 = 0x2C7
    // (D|V|G; G join needs both Lo G bits, so Lo0.G=0 keeps it non-global).
    const uint32_t setup[] = {
        (0x19U << 26) | (8U << 16) | 0x4000U,  // daddiu $8, $0, 0x4000
        encCp0(5, 8, 10, 0),  // EntryHi = 0x4000 (VPN2 2, ASID 0)
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x19U << 26) | (8U << 16) | 0x282U,  // daddiu $8, $0, 0x282
        encCp0(5, 8, 2, 0),  // Lo0
        (0x19U << 26) | (8U << 16) | 0x2C7U,  // daddiu $8, $0, 0x2C7
        encCp0(5, 8, 3, 0),  // Lo1
        (0x19U << 26) | (8U << 16) | 4U,  // Index 4
        encCp0(5, 8, 0, 0),  // dmtc0 Index (rt=8 holds 4)
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x15000, setup));
    Mips64Probe even = cpu.probeAddress(0x4000ULL, Mips64Access::Load);
    Mips64Probe evenEnd = cpu.probeAddress(0x4FFFULL, Mips64Access::Load);
    Mips64Probe odd = cpu.probeAddress(0x5000ULL, Mips64Access::Load);
    Mips64Probe oddMid = cpu.probeAddress(0x5ABCULL, Mips64Access::Load);
    CHECK(even.ok && even.phys == 0xA000ULL);
    CHECK(evenEnd.ok && evenEnd.phys == 0xAFFFULL);
    CHECK(odd.ok && odd.phys == 0xB000ULL);
    CHECK(oddMid.ok && oddMid.phys == 0xBABCULL);
    // Store to the even (non-dirty) page is Modified; odd (dirty) stores fine.
    Mips64Probe modProbe = cpu.probeAddress(0x4000ULL, Mips64Access::Store);
    CHECK(!modProbe.ok && modProbe.fault == Mips64Fault::TlbModified);
    Mips64Probe storeOdd = cpu.probeAddress(0x5000ULL, Mips64Access::Store);
    CHECK(storeOdd.ok && storeOdd.phys == 0xB000ULL);
}

// -- 8. Wired/Random behavior ------------------------------------------------------------
static void testWiredRandom() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(cpu.cp0Random() == 15);
    // Wired = 14 keeps Random 15 (in range); two TLBWRs cycle 15 -> 14 -> 15.
    const uint32_t wire[] = {
        (0x19U << 26) | (8U << 16) | 14U,  // daddiu $8, $0, 14
        encCp0(5, 8, 6, 0),  // dmtc0 Wired
    };
    CHECK(runCkSeg0(cpu, mem, 0x16000, wire));
    CHECK(cpu.cp0Wired() == 14 && cpu.cp0Random() == 15);
    // Program a recognizable entry then TLBWR twice.
    const uint32_t prep[] = {
        (0x19U << 26) | (8U << 16) | 0x6000U,  // $8 = 0x6000 (VPN2 3)
        encCp0(5, 8, 10, 0),  // EntryHi
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x19U << 26) | (8U << 16) | 7U,  // $8 = 7
        encCp0(5, 8, 2, 0),  // Lo0
        encCp0(5, 8, 3, 0),  // Lo1
        0x42000006U,  // tlbwr -> index 15
    };
    CHECK(runCkSeg0(cpu, mem, 0x16100, prep));
    CHECK(cpu.cp0Random() == 14);
    Mips64TlbEntry e15;
    CHECK(cpu.tlbEntry(15, e15) && e15.vpn2 == 3);
    const uint32_t wr2[] = {0x42000006U};  // tlbwr -> index 14, wraps to 15
    CHECK(runCkSeg0(cpu, mem, 0x16200, wr2));
    CHECK(cpu.cp0Random() == 15);
    Mips64TlbEntry e14;
    CHECK(cpu.tlbEntry(14, e14) && e14.vpn2 == 3);
    // Wired below Random reseeds Random to the top: Wired 15 with Random 15
    // stays; drive Random down then raise Wired above it.
    const uint32_t wr3[] = {0x42000006U};  // tlbwr at 15 -> random 14
    CHECK(runCkSeg0(cpu, mem, 0x16300, wr3));
    CHECK(cpu.cp0Random() == 14);
    const uint32_t wireUp[] = {
        (0x19U << 26) | (8U << 16) | 15U,  // $8 = 15
        encCp0(5, 8, 6, 0),  // Wired = 15 > Random 14 -> reseed 15
    };
    CHECK(runCkSeg0(cpu, mem, 0x16400, wireUp));
    CHECK(cpu.cp0Wired() == 15 && cpu.cp0Random() == 15);
}

// -- 9. supported PageMask variations -------------------------------------------------------
static void testPageMaskVariations() {
    struct Case {
        uint16_t mask;
        uint64_t span;  // pair span in bytes
    };
    const Case cases[] = {
        {0x000, 0x2000}, {0x003, 0x8000}, {0x00F, 0x20000}, {0x03F, 0x80000},
        {0x0FF, 0x200000}, {0x3FF, 0x800000}, {0xFFF, 0x2000000},
    };
    for (const Case& c : cases) {
        SparseMemory mem;
        Mips64Cpu cpu;
        // VPN2 0x1100 keeps every pair base nonzero (all-zero entries would
        // otherwise shadow the base-0 pairs of the large page sizes).
        const uint64_t vpn2 = 0x1100ULL;  // aligned for every supported size
        const uint64_t entryHiVal = (vpn2 << 13);
        const uint32_t hiHi = static_cast<uint32_t>(entryHiVal >> 16);
        const uint32_t hiLo = static_cast<uint32_t>(entryHiVal & 0xFFFFULL);
        const uint32_t pmVal = static_cast<uint32_t>(c.mask) << 13;
        const uint32_t pmHi = pmVal >> 16;
        const uint32_t pmLo = pmVal & 0xFFFFU;
        const uint32_t setup[] = {
            (0x0FU << 26) | (8U << 16) | hiHi,  // lui EntryHi high
            (0x0DU << 26) | (8U << 21) | (8U << 16) | hiLo,  // ori EntryHi low
            encCp0(5, 8, 10, 0),
            (0x0FU << 26) | (8U << 16) | pmHi,  // lui PageMask high
            (0x0DU << 26) | (8U << 21) | (8U << 16) | pmLo,  // ori low
            encCp0(5, 8, 5, 0),
            (0x19U << 26) | (8U << 16) | 7U,  // Lo = D|V|G, pfn 0
            encCp0(5, 8, 2, 0),
            encCp0(5, 8, 3, 0),
            (0x19U << 26) | (8U << 16) | 1U,  // Index 1
            encCp0(5, 8, 0, 0),
            0x42000002U,  // tlbwi
        };
        CHECK(runCkSeg0(cpu, mem, 0x17000, setup));
        Mips64TlbEntry e;
        CHECK(cpu.tlbEntry(1, e) && e.mask == c.mask && !e.badMask);
        const uint64_t base = (vpn2 & ~static_cast<uint64_t>(c.mask)) << 13;
        Mips64Probe inside = cpu.probeAddress(base, Mips64Access::Load);
        Mips64Probe top = cpu.probeAddress(base + c.span - 1, Mips64Access::Load);
        Mips64Probe outside = cpu.probeAddress(base + c.span, Mips64Access::Load);
        CHECK(inside.ok && inside.phys == 0);
        CHECK(top.ok);
        if (base + c.span <= kKusegEnd) {
            CHECK(!outside.ok && outside.isRefill);
        } else {
            // Pair top reaches outside kuseg: the next VA is an address error.
            CHECK(!outside.ok && !outside.isRefill);
        }
    }
}

// -- 10. unsupported PageMask encodings never match ----------------------------------------------
static void testUnsupportedPageMask() {
    SparseMemory mem;
    Mips64Cpu cpu;
    // Mask 0x001 is not a ones-pattern: entry stored but flagged never-match.
    const uint32_t fixed[] = {
        (0x19U << 26) | (8U << 16) | 0x2000U,  // $8 = 0x2000 (VPN2 1)
        encCp0(5, 8, 10, 0),  // EntryHi
        (0x0FU << 26) | (8U << 16) | 0x0U,  // lui $8, 0
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x2000U,  // ori -> 0x2000
        encCp0(5, 8, 5, 0),  // PageMask = 0x2000 (mask field 0x001)
        (0x19U << 26) | (8U << 16) | 7U,  // Lo = D|V|G
        encCp0(5, 8, 2, 0),
        encCp0(5, 8, 3, 0),
        encCp0(5, 0, 0, 0),  // dmtc0 $0, Index
        0x42000002U,  // tlbwi
    };
    CHECK(runCkSeg0(cpu, mem, 0x18000, fixed));
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(0, e) && e.mask == 0x001 && e.badMask);
    // The covered VA does NOT translate (deterministic miss, not 4 KiB alias).
    Mips64Probe p = cpu.probeAddress(0x2000ULL, Mips64Access::Load);
    CHECK(!p.ok && p.isRefill && p.fault == Mips64Fault::TlbRefillLoad);
    // TLBP misses too, and TLBR still returns the raw stored mask.
    const uint32_t probe[] = {
        (0x19U << 26) | (8U << 16) | 0x2000U,  // $8 = 0x2000
        encCp0(5, 8, 10, 0),  // EntryHi VPN2 1
        0x42000008U,  // tlbp -> miss, P set
        encCp0(5, 0, 2, 0),  // clear Lo0
        encCp0(5, 0, 0, 0),  // Index = 0 (P clear)
        0x42000001U,  // tlbr -> loads entry 0 incl. raw mask
    };
    CHECK(runCkSeg0(cpu, mem, 0x18100, probe));
    CHECK(cpu.cp0PageMask() == 0x2000ULL);
    CHECK(cpu.cp0EntryLo0() == 7ULL);
}

// -- 11. invalid Index handling ----------------------------------------------------------------------
static void testInvalidIndex() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));  // entry 0 known-good
    // TLBWI with P set is a deterministic no-op.
    const uint32_t noopWr[] = {
        (0x19U << 26) | (8U << 16) | 0x4000U,  // $8 = 0x4000 (VPN2 2)
        encCp0(5, 8, 10, 0),  // EntryHi (would change entry 0 if written)
        (0x0FU << 26) | (8U << 16) | 0x8000U,  // lui $8, 0x8000
        encCp0(4, 8, 0, 0),  // mtc0 Index = P|0 (probe-failed shape)
        0x42000002U,  // tlbwi -> no-op
    };
    CHECK(runCkSeg0(cpu, mem, 0x19000, noopWr));
    Mips64TlbEntry e0;
    CHECK(cpu.tlbEntry(0, e0) && e0.vpn2 == 0);  // unchanged
    // TLBR with P set leaves the CP0 registers untouched.
    const uint32_t noopRd[] = {
        encCp0(5, 0, 2, 0),  // Lo0 = 0 sentinel... (Lo0 already 7; use 0x5A)
        (0x19U << 26) | (8U << 16) | 0x5AU,  // $8 = 0x5A
        encCp0(5, 8, 2, 0),  // Lo0 = 0x5A sentinel
        0x42000001U,  // tlbr with P set -> no-op
    };
    CHECK(runCkSeg0(cpu, mem, 0x19100, noopRd));
    CHECK(cpu.cp0EntryLo0() == 0x5AULL);
}

// -- 12. decoder: exact TLB encodings, malformed variants, WAIT/DERET -----------------------
static void testTlbDecode() {
    CHECK(decodeInstruction(0x42000001U).valid &&
          decodeInstruction(0x42000001U).op == Mips64Op::Tlbr);
    CHECK(decodeInstruction(0x42000002U).valid &&
          decodeInstruction(0x42000002U).op == Mips64Op::Tlbwi);
    CHECK(decodeInstruction(0x42000006U).valid &&
          decodeInstruction(0x42000006U).op == Mips64Op::Tlbwr);
    CHECK(decodeInstruction(0x42000008U).valid &&
          decodeInstruction(0x42000008U).op == Mips64Op::Tlbp);
    // Malformed (rt != 0) TLB encodings are reserved (-> RI entry).
    Mips64Decoded bad = decodeInstruction(0x42010002U);
    CHECK(!bad.valid && bad.fault == Mips64Fault::ReservedInstruction);
    // WAIT (0x20) and DERET (0x1F) are valid but unimplemented.
    CHECK(decodeInstruction(0x42000020U).fault == Mips64Fault::UnsupportedInstruction);
    CHECK(decodeInstruction(0x4200001FU).fault == Mips64Fault::UnsupportedInstruction);
    // Other CO functions are reserved.
    CHECK(decodeInstruction(0x42000003U).fault == Mips64Fault::ReservedInstruction);
    // ERET still decodes exactly.
    CHECK(decodeInstruction(0x42000018U).op == Mips64Op::Eret);
}

// -- 13. per-CPU TLB isolation -------------------------------------------------------------------
static void testCpuIsolation() {
    SparseMemory mem;
    Mips64Cpu a, b;
    CHECK(setupKusegIdentityMap(a, mem));
    // b has no entries: same VA misses there while hitting in a.
    // (VPN2 nonzero to avoid matching b's all-zero entries with Invalid.)
    CHECK(a.probeAddress(0x4000ULL, Mips64Access::Load).ok);
    Mips64Probe pb = b.probeAddress(0x4000ULL, Mips64Access::Load);
    CHECK(!pb.ok && pb.isRefill);
    CHECK(b.cp0Random() == 15 && b.cp0Wired() == 0);
}

// -- 14. snapshot round trip incl. TLB state --------------------------------------------------
static void testSerializationRoundTrip() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    // Diverge management registers: EntryHi ASID 3, Wired 2, Context PTEBase.
    const uint32_t setup[] = {
        (0x19U << 26) | (8U << 16) | 0x6003U,  // $8 = 0x6003 (VPN2 3, ASID 3)
        encCp0(5, 8, 10, 0),  // EntryHi
        (0x19U << 26) | (8U << 16) | 2U,  // $8 = 2
        encCp0(5, 8, 6, 0),  // Wired = 2
        (0x0FU << 26) | (8U << 16) | 0x1234U,  // lui PTEBase-ish
        encCp0(5, 8, 4, 0),  // Context PTEBase
    };
    CHECK(runCkSeg0(cpu, mem, 0x1A000, setup));
    const std::vector<uint8_t> blob = cpu.serialize();
    CHECK(blob.size() == Mips64Cpu::kSerializedSize);
    CHECK(blob.size() == 957);
    CHECK(blob[4] == 3);
    Mips64Cpu restored;
    CHECK(restored.deserialize(blob.data(), blob.size()));
    CHECK(restored.cp0EntryHi() == cpu.cp0EntryHi());
    CHECK(restored.cp0Wired() == 2);
    CHECK(restored.cp0Random() == cpu.cp0Random());
    CHECK(restored.cp0Context() == cpu.cp0Context());
    CHECK(restored.probeAddress(0x1000ULL, Mips64Access::Load).ok);
    Mips64TlbEntry e0a, e0b;
    CHECK(cpu.tlbEntry(0, e0a) && restored.tlbEntry(0, e0b));
    CHECK(e0a.vpn2 == e0b.vpn2 && e0a.pfn0 == e0b.pfn0 && e0a.mask == e0b.mask);
    CHECK(e0a.v0 == e0b.v0 && e0a.g == e0b.g && e0a.badMask == e0b.badMask);
}

// -- 15. invalid restore data fails safely with state untouched -----------------------
static void testInvalidRestoreRollback() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    const std::vector<uint8_t> good = cpu.serialize();
    Mips64Cpu live;
    CHECK(live.deserialize(good.data(), good.size()));
    const std::vector<uint8_t> before = live.serialize();
    auto tryBad = [&](std::vector<uint8_t> bad) {
        CHECK(!live.deserialize(bad.data(), bad.size()));
        CHECK(live.serialize() == before);  // untouched
    };
    // Truncation / extension / bad magic / bad version / null.
    tryBad(std::vector<uint8_t>(good.begin(), good.end() - 1));
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    tryBad(longer);
    std::vector<uint8_t> badMagic = good;
    badMagic[0] = 'X';
    tryBad(badMagic);
    std::vector<uint8_t> badVersion = good;
    badVersion[4] = 2;
    tryBad(badVersion);
    CHECK(!live.deserialize(nullptr, good.size()));
    CHECK(live.serialize() == before);
    // v2-size blob rejected.
    tryBad(std::vector<uint8_t>(373, 0));
    // Out-of-range fields: Random = 16 (offset 373+8), Wired = 17,
    // Index upper bits, PageMask bit 28, EntryHi bit 12, Context low nibble,
    // XContext low 12 bits, fault code 26.
    std::vector<uint8_t> badRandom = good;
    badRandom[373 + 8] = 16;
    tryBad(badRandom);
    std::vector<uint8_t> badWired = good;
    badWired[373 + 48] = 17;
    tryBad(badWired);
    std::vector<uint8_t> badIndex = good;
    badIndex[373 + 1] = 0x40;  // bit outside P|field
    tryBad(badIndex);
    std::vector<uint8_t> badPm = good;
    badPm[373 + 40 + 3] |= 0x10;  // PageMask bit 28
    tryBad(badPm);
    std::vector<uint8_t> badHi = good;
    badHi[373 + 16 + 1] |= 0x10;  // EntryHi bit 12
    tryBad(badHi);
    std::vector<uint8_t> badCtx = good;
    badCtx[373 + 56] |= 0x0F;  // Context bits 3:0
    tryBad(badCtx);
    std::vector<uint8_t> badXCtx = good;
    badXCtx[373 + 64] |= 0x01;  // XContext bit 0
    tryBad(badXCtx);
    std::vector<uint8_t> badFault = good;
    badFault[305] = 26;
    badFault[306] = 0;
    badFault[307] = 0;
    badFault[308] = 0;
    tryBad(badFault);
    // Corrupt TLB entry flags (bit 6 reserved) and reserved padding.
    std::vector<uint8_t> badFlags = good;
    badFlags[445 + 21] |= 0x40;
    tryBad(badFlags);
    std::vector<uint8_t> badPad = good;
    badPad[445 + 31] = 1;
    tryBad(badPad);
    // badMask flag disagreeing with a supported mask.
    std::vector<uint8_t> badMaskFlag = good;
    badMaskFlag[445 + 21] |= 0x20;  // entry 0 has supported 16M mask
    tryBad(badMaskFlag);
}

// -- 16. big-endian TLB management -------------------------------------------------------------------
static void testBigEndianTlb() {
    SparseMemory mem;
    Mips64Cpu cpu(Mips64Endianness::Big);
    // EntryHi VPN2 = 0x70, ASID 1; 4 KiB; pfn 0x33; TLBWI at 6; TLBP; TLBR.
    const uint32_t setup[] = {
        (0x0FU << 26) | (8U << 16) | 0xEU,  // lui $8, 0xE -> 0xE0000
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0x1U,  // ori -> 0xE0001
        encCp0(5, 8, 10, 0),  // EntryHi (VPN2 0x70, ASID 1)
        encCp0(5, 0, 5, 0),  // PageMask 0
        (0x0FU << 26) | (8U << 16) | 0x0U,  // lui $8, 0
        (0x0DU << 26) | (8U << 21) | (8U << 16) | 0xCC7U,  // ori -> 0xCC7
        encCp0(5, 8, 2, 0),  // Lo0: pfn 0x33, D|V|G
        encCp0(5, 8, 3, 0),  // Lo1: same
        (0x19U << 26) | (8U << 16) | 6U,  // Index 6
        encCp0(5, 8, 0, 0),
        0x42000002U,  // tlbwi
        0x42000008U,  // tlbp -> hit 6
    };
    CHECK(runCkSeg0(cpu, mem, 0x1B000, setup));
    CHECK(cpu.cp0Index() == 6);
    Mips64TlbEntry e;
    CHECK(cpu.tlbEntry(6, e) && e.asid == 1 && e.vpn2 == 0x70 && e.g);
    Mips64Probe p = cpu.probeAddress(0xE0000ULL, Mips64Access::Load);
    CHECK(p.ok && p.phys == 0x33000ULL);
}

int main() {
    std::cout << std::unitbuf;  // unbuffered: failures stay ordered on abort
    testTlbResetState();
    testCp0TlbRegisters();
    testTlbwiTlbrRoundTrip();
    testTlbpHitMiss();
    testAsidIsolation();
    testGlobalMapping();
    testEvenOddSelection();
    testWiredRandom();
    testPageMaskVariations();
    testUnsupportedPageMask();
    testInvalidIndex();
    testTlbDecode();
    testCpuIsolation();
    testSerializationRoundTrip();
    testInvalidRestoreRollback();
    testBigEndianTlb();

    std::cout << "test_mips64_tlb: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
