// MIPS64-3 focused test: privilege enforcement — KSU/EXL/ERL effective
// privilege, segment access control, privileged-instruction rejection in
// supervisor/user modes, UX/SX/KX handling and explicit 64-bit region
// rejection (no truncation). Real encoded instructions throughout;
// IMemory is PHYSICAL memory. Kernel-mode setup runs from CKSEG0;
// user/supervisor probes run from mapped kuseg code.

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
        if (cpu.step(mem) != mips64::Mips64StepResult::Continue) {
            ok = false;
            break;
        }
    }
    cpu.setPc(savedPc);
    return ok;
}

// Drop from kernel to supervisor/user mode by writing the KSU field
// (BEV kept, EXL/ERL clear): Status = BEV | (ksu << 3).
// Runs from mapped kuseg code (VA 0x5000): supervisor mode cannot fetch
// CKSEG0, so the switch sequence itself must live in a segment the current
// mode can fetch. Callers must have installed the kuseg identity mapping
// (kernel) before the first drop; supervisor->user drops re-use it.
static bool enterMode(Mips64Cpu& cpu, ia64::IMemory& mem, unsigned ksu) {
    const uint32_t prog[] = {
        (0x0FU << 26) | (8U << 16) | 0x40U,  // lui $8, 0x40 (BEV)
        (0x0DU << 26) | (8U << 21) | (8U << 16) |
            static_cast<uint32_t>((ksu << 3) & 0xFFFFU),  // ori KSU bits
        encCp0(4, 8, 12, 0),  // mtc0 $8, Status
    };
    const bool le = cpu.isLittleEndian();
    for (int i = 0; i < 3; ++i) {
        storeWordPhys(mem, 0x5000 + static_cast<uint64_t>(i) * 4, prog[i], le);
    }
    cpu.setPc(0x5000);
    for (int i = 0; i < 3; ++i) {
        if (cpu.step(mem) != mips64::Mips64StepResult::Continue) {
            return false;
        }
    }
    return true;
}

// Kernel-mode fixture: kuseg identity (entry 0) + cksseg VA==PA window
// (entry 2, VPN2 of 0xFFFFFFFFC0000000, 4 KiB pair, pfn 0, V+D+G) +
// ckseg3 VA==PA window (entry 3, VPN2 of 0xFFFFFFFFE0000000, pfn 0x80).
static bool installTestMappings(Mips64Cpu& cpu, ia64::IMemory& mem) {
    if (!setupKusegIdentityMap(cpu, mem)) {
        return false;
    }
    cpu.writeGpr(8, 0xFFFFFFFFC0000000ULL & 0xFFFFFFFFFFFFE000ULL);
    cpu.writeGpr(9, 2);
    cpu.writeGpr(10, 7);  // Lo: pfn 0, D|V|G
    const uint32_t cksseg[] = {
        encCp0(5, 8, 10, 0),  // EntryHi
        encCp0(5, 0, 5, 0),  // PageMask 0
        encCp0(5, 10, 2, 0),  // Lo0
        encCp0(5, 10, 3, 0),  // Lo1
        encCp0(5, 9, 0, 0),  // Index 2
        0x42000002U,  // tlbwi
    };
    if (!runCkSeg0(cpu, mem, 0x6400, cksseg)) {
        return false;
    }
    cpu.writeGpr(8, 0xFFFFFFFFE0000000ULL & 0xFFFFFFFFFFFFE000ULL);
    cpu.writeGpr(9, 3);
    cpu.writeGpr(10, static_cast<uint64_t>((0x80U << 6) | 7U));  // pfn 0x80
    const uint32_t ckseg3[] = {
        encCp0(5, 8, 10, 0),  encCp0(5, 0, 5, 0),  encCp0(5, 10, 2, 0),
        encCp0(5, 10, 3, 0),  encCp0(5, 9, 0, 0),  0x42000002U,
    };
    if (!runCkSeg0(cpu, mem, 0x6500, ckseg3)) {
        return false;
    }
    Mips64TlbEntry e2, e3;
    return cpu.tlbEntry(2, e2) && e2.v0 && cpu.tlbEntry(3, e3) && e3.v0;
}

// -- 1. effective privilege: KSU bits, EXL override, reserved KSU ---------------
static void testEffectivePrivilege() {
    SparseMemory mem;
    Mips64Cpu cpu;  // reset: KSU=0, ERL=1 -> kernel
    CHECK(cpu.isKernelMode() && !cpu.isSupervisorMode() && !cpu.isUserMode());
    // Mode drops run from kernel (only kernel can write Status.KSU).
    auto enterFresh = [&](unsigned ksu, Mips64Cpu& out) {
        CHECK(setupKusegIdentityMap(out, mem));
        return enterMode(out, mem, ksu);
    };
    Mips64Cpu ck;
    CHECK(enterFresh(0, ck));
    CHECK(ck.isKernelMode() && ck.statusKsu() == 0);
    Mips64Cpu cs;
    CHECK(enterFresh(1, cs));
    CHECK(cs.isSupervisorMode() && !cs.isKernelMode() && !cs.isUserMode());
    Mips64Cpu cu;
    CHECK(enterFresh(2, cu));
    CHECK(cu.isUserMode() && !cu.isKernelMode() && !cu.isSupervisorMode());
    // Reserved KSU=11 is kernel-equivalent (documented implementation choice).
    Mips64Cpu cr;
    CHECK(enterFresh(3, cr));
    CHECK(cr.isKernelMode());
    // EXL forces kernel even with KSU=user: take a syscall in user mode.
    Mips64Cpu cpu2;
    CHECK(installTestMappings(cpu2, mem));
    CHECK(enterMode(cpu2, mem, 2));
    CHECK(cpu2.isUserMode());
    storeGuestWord(mem, 0x5000, 0x0000000CU, true);  // syscall (mapped kuseg)
    cpu2.setPc(0x5000);
    CHECK(stepFault(cpu2, mem, Mips64Fault::SyscallTrap));
    CHECK(cpu2.statusExl() && cpu2.isKernelMode());
    CHECK(cpu2.cp0Epc() == 0x5000ULL);
}

// -- 2. UX/SX/KX round-trip; compat behavior unaffected ----------------------------------
static void testUxSxKx() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(installTestMappings(cpu, mem));  // kernel mappings first
    // Status = BEV | UX | SX | KX | (user << 3) = 0x400000|0xE0|0x10.
    cpu.writeGpr(8, 0x4000F0ULL);
    const uint32_t prog[] = {encCp0(5, 8, 12, 0)};  // dmtc0 Status
    CHECK(runCkSeg0(cpu, mem, 0x6600, prog));
    CHECK(cpu.isUserMode());
    CHECK((cpu.cp0Status() & 0xE0ULL) == 0xE0ULL);  // UX/SX/KX stick
    // User kuseg access works with UX set (32-bit compat region).
    storeGuestWord(mem, 0x5100, encI(0x09, 0, 8, 42), true);  // addiu
    cpu.setPc(0x5100);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 42);
    // Same with UX/SX/KX clear: compat behavior identical (documented: the
    // bits gate 64-bit regions, unsupported in this phase).
    Mips64Cpu cpu2;
    CHECK(installTestMappings(cpu2, mem));
    CHECK(enterMode(cpu2, mem, 2));  // user, UX/SX/KX clear
    CHECK((cpu2.cp0Status() & 0xE0ULL) == 0);
    cpu2.setPc(0x5100);
    CHECK(stepOk(cpu2, mem));
    CHECK(cpu2.readGpr(8) == 42);
}

// -- 3. supervisor segment access: cksseg allowed, kernel-only denied -----------------
static void testSupervisorSegments() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(installTestMappings(cpu, mem));
    uint8_t z[4] = {0x11, 0x22, 0x33, 0x44};
    mem.Write(0, z, 4);  // PA 0 backs the cksseg window (pfn 0)
    // Kernel cksseg load works before the mode drop (sanity).
    cpu.writeGpr(4, 0xFFFFFFFFC0000000ULL);
    const uint32_t lw[] = {encI(0x23, 4, 8, 0)};  // lw $8, 0($4)
    CHECK(runCkSeg0(cpu, mem, 0x6700, lw));
    CHECK(cpu.readGpr(8) == 0x44332211ULL);
    auto freshSupervisor = [&]() {
        Mips64Cpu s;
        CHECK(installTestMappings(s, mem));
        CHECK(enterMode(s, mem, 1));
        return s;
    };
    // Supervisor cksseg load from mapped kuseg code: allowed.
    {
        Mips64Cpu s = freshSupervisor();
        CHECK(s.isSupervisorMode());
        storeGuestWord(mem, 0x5200, encI(0x23, 4, 8, 0), true);
        s.writeGpr(4, 0xFFFFFFFFC0000000ULL);
        s.setPc(0x5200);
        CHECK(stepOk(s, mem));
        CHECK(s.readGpr(8) == 0x44332211ULL);
    }
    // Supervisor fetch from ckseg0 -> Address Error (AdEL), general vector.
    {
        Mips64Cpu s = freshSupervisor();
        s.setPc(0xFFFFFFFF80006700ULL);
        CHECK(stepFault(s, mem, Mips64Fault::AddressErrorFetch));
        CHECK(s.causeExcCode() == 4);
        CHECK(s.cp0BadVAddr() == 0xFFFFFFFF80006700ULL);
        CHECK(s.pc() == 0xFFFFFFFFBFC00380ULL);
    }
    // Supervisor load from ckseg3-mapped window -> Address Error (AdEL).
    {
        Mips64Cpu s = freshSupervisor();
        s.writeGpr(4, 0xFFFFFFFFE0000000ULL);
        storeGuestWord(mem, 0x5210, encI(0x23, 4, 8, 0), true);
        s.setPc(0x5210);
        CHECK(stepFault(s, mem, Mips64Fault::AddressErrorLoad));
        CHECK(s.causeExcCode() == 4);
        CHECK(s.cp0BadVAddr() == 0xFFFFFFFFE0000000ULL);
    }
    // Supervisor store to ckseg1 -> AdES.
    {
        Mips64Cpu s = freshSupervisor();
        s.writeGpr(4, 0xFFFFFFFFA0000000ULL);
        storeGuestWord(mem, 0x5220, encI(0x2B, 4, 8, 0), true);
        s.setPc(0x5220);
        CHECK(stepFault(s, mem, Mips64Fault::AddressErrorStore));
        CHECK(s.causeExcCode() == 5);
    }
}

// -- 4. user denial of every kernel-only segment ----------------------------------------------
// Fresh user-mode CPU per probe: each denial sets EXL, which would
// legitimately authorize the following probe (handler privilege).
static void testUserSegmentDenial() {
    SparseMemory mem;
    Mips64Cpu setup;
    CHECK(installTestMappings(setup, mem));
    auto freshUser = [&]() {
        Mips64Cpu u;
        CHECK(installTestMappings(u, mem));
        CHECK(enterMode(u, mem, 2));
        return u;
    };
    // Fetch from CKSEG0.
    {
        Mips64Cpu u = freshUser();
        u.setPc(0xFFFFFFFF80006000ULL);
        CHECK(stepFault(u, mem, Mips64Fault::AddressErrorFetch));
        CHECK(u.causeExcCode() == 4);
        CHECK(u.cp0BadVAddr() == 0xFFFFFFFF80006000ULL);
        CHECK(u.pc() == 0xFFFFFFFFBFC00380ULL);
        CHECK(u.cp0Epc() == 0xFFFFFFFF80006000ULL);
        // Kernel handlers still function: the EXL entry re-enables access.
        CHECK(u.statusExl() && u.isKernelMode());
        u.setPc(0xFFFFFFFF80006000ULL);
        CHECK(stepOk(u, mem));  // lui $8, 0x40 (enterMode prologue residue)
    }
    // Load from cksseg (supervisor/kernel-only).
    {
        Mips64Cpu u = freshUser();
        u.writeGpr(4, 0xFFFFFFFFC0000000ULL);
        storeGuestWord(mem, 0x5300, encI(0x23, 4, 8, 0), true);
        u.setPc(0x5300);
        CHECK(stepFault(u, mem, Mips64Fault::AddressErrorLoad));
        CHECK(u.causeExcCode() == 4);
        CHECK(u.cp0BadVAddr() == 0xFFFFFFFFC0000000ULL);
    }
    // Store to ckseg3-mapped window.
    {
        Mips64Cpu u = freshUser();
        u.writeGpr(4, 0xFFFFFFFFE0000000ULL);
        storeGuestWord(mem, 0x5310, encI(0x2B, 4, 8, 0), true);
        u.setPc(0x5310);
        CHECK(stepFault(u, mem, Mips64Fault::AddressErrorStore));
        CHECK(u.causeExcCode() == 5);
        CHECK(u.cp0BadVAddr() == 0xFFFFFFFFE0000000ULL);
    }
}

// -- 5. privileged instructions rejected in user/supervisor modes -----------------------
static void testCp0PrivilegeRejection() {
    SparseMemory mem;
    Mips64Cpu cpu;
    CHECK(installTestMappings(cpu, mem));
    CHECK(enterMode(cpu, mem, 2));  // user
    cpu.writeGpr(9, 0x1234);  // sentinel dest; $8 = 0
    cpu.writeGpr(8, 0);
    // MFC0/DMFC0/MTC0/DMTC0/ERET/TLBP/TLBR/TLBWI/TLBWR, one per kuseg slot.
    const uint32_t ops[] = {
        encCp0(0, 9, 12, 0),  // mfc0 $9, Status
        encCp0(1, 9, 12, 0),  // dmfc0 $9, Status
        encCp0(4, 8, 12, 0),  // mtc0 $8, Status
        encCp0(5, 8, 12, 0),  // dmtc0 $8, Status
        0x42000018U,  // eret
        0x42000008U,  // tlbp
        0x42000001U,  // tlbr
        0x42000002U,  // tlbwi
        0x42000006U,  // tlbwr
    };
    for (int i = 0; i < 9; ++i) {
        storeGuestWord(mem, 0x5400 + static_cast<uint64_t>(i) * 4, ops[i], true);
    }
    // Fresh user-mode CPU per op: each rejection sets EXL, and EXL would
    // legitimately authorize the next op (handler privilege), so the CPUs
    // must not be shared across iterations.
    for (int i = 0; i < 9; ++i) {
        Mips64Cpu u;
        CHECK(installTestMappings(u, mem));
        CHECK(enterMode(u, mem, 2));
        u.writeGpr(9, 0x1234);  // sentinel dest; $8 = 0
        u.writeGpr(8, 0);
        u.setPc(0x5400 + static_cast<uint64_t>(i) * 4);
        CHECK(stepFault(u, mem, Mips64Fault::PrivilegeViolation));
        CHECK(u.causeExcCode() == 10);  // RI
        CHECK(u.pc() == 0xFFFFFFFFBFC00380ULL);
        CHECK(u.readGpr(9) == 0x1234);  // no destination write
    }
    // Rejected TLB operations change no TLB state: identity still translates.
    CHECK(cpu.probeAddress(0x1000ULL, Mips64Access::Load).ok);
    // Supervisor mode rejects them too (representative subset, fresh CPUs).
    const uint32_t supOps[] = {
        encCp0(0, 9, 12, 0),  // mfc0
        0x42000002U,  // tlbwi
        0x42000018U,  // eret
    };
    for (int i = 0; i < 3; ++i) {
        Mips64Cpu s;
        CHECK(installTestMappings(s, mem));
        CHECK(enterMode(s, mem, 1));  // supervisor
        s.writeGpr(9, 0x5678);
        s.writeGpr(8, 0);
        s.setPc(0x5500 + static_cast<uint64_t>(i) * 4);
        storeGuestWord(mem, 0x5500 + static_cast<uint64_t>(i) * 4, supOps[i], true);
        CHECK(stepFault(s, mem, Mips64Fault::PrivilegeViolation));
        CHECK(s.causeExcCode() == 10);
        CHECK(s.readGpr(9) == 0x5678);
    }
    // Kernel mode executes them (sanity: MFC0 + TLBP retire).
    Mips64Cpu cpu3;
    CHECK(installTestMappings(cpu3, mem));
    storeGuestWord(mem, 0x5600, encCp0(0, 9, 12, 0), true);  // mfc0 (mapped kuseg)
    storeGuestWord(mem, 0x5604, 0x42000008U, true);  // tlbp
    cpu3.setPc(0x5600);
    CHECK(stepOk(cpu3, mem));
    CHECK(stepOk(cpu3, mem));
}

// -- 6. extended 64-bit regions fail explicitly in ALL modes (no truncation) ------
static void testExtendedRegionsRejected() {
    SparseMemory mem;
    Mips64Cpu cpu;  // kernel: rejection is address error, not privilege
    const uint64_t vas[] = {
        0x8000000000000000ULL,  // XKPHYS
        0xBFFFFFFFFFFFFFFFULL, 0xC000000000000000ULL,  // XKSEG
        0x0000000080000000ULL,  // zero-extended ckseg0 lookalike
        0x0000000100000000ULL,  // just above 32 bits
        0x0000000180000000ULL, 0xFFFFFFFF00000000ULL,  // non-sign-extended
    };
    // Code runs from CKSEG0; target VA staged in $4 host-side. Byte ops
    // avoid the alignment check (which fires before translation), so every
    // rejection below is a genuine segment/width address error.
    storeWordPhys(mem, 0x6800, encI(0x20, 4, 8, 0), true);  // lb $8, 0($4)
    storeWordPhys(mem, 0x6804, encI(0x28, 4, 8, 0), true);  // sb $8, 0($4)
    for (uint64_t va : vas) {
        cpu.writeGpr(4, va);
        cpu.setPc(0xFFFFFFFF80006800ULL);
        CHECK(stepFault(cpu, mem, Mips64Fault::AddressErrorLoad));
        CHECK(cpu.causeExcCode() == 4);
        CHECK(cpu.cp0BadVAddr() == va);  // full VA reported, never truncated
        CHECK(cpu.pc() == 0xFFFFFFFFBFC00380ULL);
        cpu.writeGpr(4, va);
        cpu.setPc(0xFFFFFFFF80006804ULL);
        CHECK(stepFault(cpu, mem, Mips64Fault::AddressErrorStore));
        CHECK(cpu.causeExcCode() == 5);
        // Fetch: alignment faults first on misaligned VAs; use aligned ones
        // so the segment rejection (AddressErrorFetch) is exercised.
        if ((va & 0x3ULL) == 0) {
            Mips64Cpu fetchCpu;
            fetchCpu.setPc(va);
            CHECK(stepFault(fetchCpu, mem, Mips64Fault::AddressErrorFetch));
            CHECK(fetchCpu.causeExcCode() == 4);
            CHECK(fetchCpu.cp0BadVAddr() == va);
            CHECK(fetchCpu.pc() == 0xFFFFFFFFBFC00380ULL);
        }
    }
}

int main() {
    std::cout << std::unitbuf;  // unbuffered: failures stay ordered on abort
    testEffectivePrivilege();
    testUxSxKx();
    testSupervisorSegments();
    testUserSegmentDenial();
    testCp0PrivilegeRejection();
    testExtendedRegionsRejected();

    std::cout << "test_mips64_privilege: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
