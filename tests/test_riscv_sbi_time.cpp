// RISCV-5 executable SBI TIME, firmware timer forwarding, WFI, and Sv39 proofs.
#include "riscv_cpu.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace riscv;

constexpr uint64_t kPhysBase = 0x80000000ULL;
constexpr size_t kImageSize = 0xC000;
constexpr size_t kMachineTrapOffset = 0x400;
constexpr size_t kSupervisorEntryOffset = 0x1000;
constexpr size_t kSupervisorTrapOffset = 0x2000;
constexpr size_t kDataOffset = 0x3000;
constexpr size_t kScratchOffset = 0x3800;
constexpr size_t kRootOffset = 0x8000;
constexpr size_t kLevel1Offset = 0x9000;
constexpr size_t kCodeLevel0Offset = 0xA000;
constexpr size_t kDataLevel0Offset = 0xB000;
constexpr uint64_t kSupervisorVa = 0x40000000ULL;
constexpr uint64_t kSupervisorTrapVa = 0x40001000ULL;
constexpr uint64_t kDataVa = 0x50000000ULL;
constexpr uint64_t kMtip = 1ULL << kInterruptMachineTimer;
constexpr uint64_t kStip = 1ULL << kInterruptSupervisorTimer;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint32_t kCsrMstatus = 0x300;
constexpr uint32_t kCsrMedeleg = 0x302;
constexpr uint32_t kCsrMideleg = 0x303;
constexpr uint32_t kCsrMie = 0x304;
constexpr uint32_t kCsrMtvec = 0x305;
constexpr uint32_t kCsrMscratch = 0x340;
constexpr uint32_t kCsrMepc = 0x341;
constexpr uint32_t kCsrMcause = 0x342;
constexpr uint32_t kCsrStvec = 0x105;
constexpr uint32_t kCsrSie = 0x104;
constexpr uint32_t kCsrSstatus = 0x100;
constexpr uint32_t kCsrScause = 0x142;
constexpr uint32_t kCsrSepc = 0x141;
constexpr uint32_t kCsrSatp = 0x180;

uint64_t g_checks = 0;

void Expect(bool condition, const std::string& what) {
    ++g_checks;
    if (!condition) {
        std::cerr << "SBI TIME TEST FAILED: " << what << '\n';
        std::exit(1);
    }
}

void ExpectEq(const std::string& what, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "SBI TIME TEST FAILED: " << what << " expected=0x"
                  << std::hex << expected << " actual=0x" << actual << std::dec << '\n';
        std::exit(1);
    }
}

uint32_t EncodeI(uint32_t opcode, uint32_t rd, uint32_t f3, uint32_t rs1, int32_t imm) {
    return ((static_cast<uint32_t>(imm) & 0xFFFU) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | opcode;
}

uint32_t EncodeS(uint32_t rs2, uint32_t rs1, int32_t imm) {
    const uint32_t u = static_cast<uint32_t>(imm) & 0xFFFU;
    return (((u >> 5) & 0x7FU) << 25) | (rs2 << 20) | (rs1 << 15) |
           (3U << 12) | ((u & 0x1FU) << 7) | 0x23U;
}

uint32_t EncodeR(uint32_t rd, uint32_t f3, uint32_t rs1, uint32_t rs2,
                 uint32_t f7 = 0) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           (rd << 7) | 0x33U;
}

uint32_t EncodeU(uint32_t opcode, uint32_t rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFFU) << 12) | (rd << 7) | opcode;
}

uint32_t EncodeCsr(uint32_t rd, uint32_t f3, uint32_t rs1, uint32_t csr) {
    return (csr << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x73U;
}

class Assembler {
public:
    explicit Assembler(uint64_t basePc = 0) : basePc_(basePc) {}

    size_t pcIndex() const { return words_.size(); }
    void emit(uint32_t word) { words_.push_back(word); }
    void label(const std::string& name) { labels_[name] = words_.size(); }
    void addi(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncodeI(0x13, rd, 0, rs1, imm)); }
    void ld(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncodeI(0x03, rd, 3, rs1, imm)); }
    void sd(uint32_t rs2, uint32_t rs1, int32_t imm) { emit(EncodeS(rs2, rs1, imm)); }
    void lui(uint32_t rd, uint32_t imm20) { emit(EncodeU(0x37, rd, imm20)); }
    void auipc(uint32_t rd, uint32_t imm20) { emit(EncodeU(0x17, rd, imm20)); }
    void slli(uint32_t rd, uint32_t rs1, uint32_t shamt) {
        emit(EncodeI(0x13, rd, 1, rs1, static_cast<int32_t>(shamt)));
    }
    void srli(uint32_t rd, uint32_t rs1, uint32_t shamt) {
        emit(EncodeI(0x13, rd, 5, rs1, static_cast<int32_t>(shamt)));
    }
    void csrr(uint32_t rd, uint32_t csr) { emit(EncodeCsr(rd, 2, 0, csr)); }
    void csrw(uint32_t csr, uint32_t rs1) { emit(EncodeCsr(0, 1, rs1, csr)); }
    void csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { emit(EncodeCsr(rd, 1, rs1, csr)); }
    void ecall() { emit(0x00000073U); }
    void ebreak() { emit(0x00100073U); }
    void mret() { emit(0x30200073U); }
    void sret() { emit(0x10200073U); }
    void wfi() { emit(0x10500073U); }
    void branch(uint32_t f3, uint32_t rs1, uint32_t rs2, const std::string& target) {
        const size_t at = words_.size();
        words_.push_back(0);
        fixups_.push_back({at, target, false, 0, f3, rs1, rs2});
    }
    void beq(uint32_t rs1, uint32_t rs2, const std::string& target) {
        branch(0, rs1, rs2, target);
    }
    void bne(uint32_t rs1, uint32_t rs2, const std::string& target) {
        branch(1, rs1, rs2, target);
    }
    void jal(uint32_t rd, const std::string& target) {
        const size_t at = words_.size();
        words_.push_back(0);
        fixups_.push_back({at, target, true, rd, 0, 0, 0});
    }
    void li32(uint32_t rd, uint32_t value) {
        const int64_t signedValue = static_cast<int64_t>(value);
        const int64_t hi = (signedValue + 0x800) >> 12;
        const int64_t lo = signedValue - (hi << 12);
        lui(rd, static_cast<uint32_t>(hi));
        if (lo != 0) addi(rd, rd, static_cast<int32_t>(lo));
    }
    void finish() {
        for (const Fixup& f : fixups_) {
            const auto it = labels_.find(f.label);
            if (it == labels_.end()) {
                std::cerr << "SBI TIME TEST FAILED: unresolved label " << f.label << '\n';
                std::exit(1);
            }
            const int64_t delta = (static_cast<int64_t>(it->second) -
                                   static_cast<int64_t>(f.at)) * 4;
            const uint32_t imm = static_cast<uint32_t>(delta);
            if (f.isJal) {
                const uint32_t x = imm & 0x1FFFFFU;
                words_[f.at] = (((x >> 20) & 1U) << 31) |
                               (((x >> 1) & 0x3FFU) << 21) |
                               (((x >> 11) & 1U) << 20) |
                               (((x >> 12) & 0xFFU) << 12) |
                               (f.rd << 7) | 0x6FU;
            } else {
                const uint32_t x = imm & 0x1FFFU;
                words_[f.at] = (((x >> 12) & 1U) << 31) |
                               (((x >> 5) & 0x3FU) << 25) |
                               (f.rs2 << 20) | (f.rs1 << 15) |
                               (f.f3 << 12) | (((x >> 1) & 0xFU) << 8) |
                               (((x >> 11) & 1U) << 7) | 0x63U;
            }
        }
    }
    const std::vector<uint32_t>& words() const { return words_; }

private:
    struct Fixup {
        size_t at;
        std::string label;
        bool isJal;
        uint32_t rd;
        uint32_t f3;
        uint32_t rs1;
        uint32_t rs2;
    };
    uint64_t basePc_;
    std::vector<uint32_t> words_;
    std::map<std::string, size_t> labels_;
    std::vector<Fixup> fixups_;
};

void CopyWords(std::vector<uint8_t>& image, size_t offset,
               const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        for (size_t byte = 0; byte < 4; ++byte) {
            image[offset + i * 4 + byte] = static_cast<uint8_t>(word >> (8 * byte));
        }
    }
}

void WriteImageU64(std::vector<uint8_t>& image, size_t offset, uint64_t value) {
    for (size_t byte = 0; byte < 8; ++byte) {
        image[offset + byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
}

uint64_t MakePte(uint64_t physical, uint64_t flags) {
    return ((physical >> 12) << 10) | flags;
}

enum class GuestKind { Registration, Recurring, CounterDenied };

std::vector<uint32_t> BuildMachineBoot(bool sv39, bool enableMtimer,
                                       bool enableCounter) {
    Assembler a(kPhysBase);
    a.auipc(5, 0);
    a.addi(5, 5, static_cast<int32_t>(kMachineTrapOffset));
    a.csrw(kCsrMtvec, 5);

    a.addi(5, 0, static_cast<int32_t>(kStip));
    a.csrw(kCsrMideleg, 5);
    a.addi(5, 0, static_cast<int32_t>(kStip | (enableMtimer ? kMtip : 0)));
    a.csrw(kCsrMie, 5);

    const size_t scratchAuipc = a.pcIndex();
    a.auipc(5, 3);
    const int32_t scratchAdjust = static_cast<int32_t>(0x800 - scratchAuipc * 4);
    a.addi(5, 5, scratchAdjust);
    a.csrw(kCsrMscratch, 5);

    a.addi(5, 0, enableCounter ? static_cast<int32_t>(kCounterenTime) : 0);
    a.csrw(kCsrMcounteren, 5);

    if (sv39) {
        // satp = Sv39 | PPN(base + root offset) = 0x8000000000080008.
        a.lui(5, 0x80000);
        a.slli(5, 5, 32);
        a.lui(6, 0x80);
        a.addi(6, 6, 8);
        a.emit(EncodeR(5, 6, 5, 6)); // or x5,x5,x6
        a.csrw(kCsrSatp, 5);
        a.lui(5, 0x40000);
    } else {
        const size_t mepcAuipc = a.pcIndex();
        a.auipc(5, 1);
        a.addi(5, 5, -static_cast<int32_t>(mepcAuipc * 4));
    }
    a.csrw(kCsrMepc, 5);
    a.addi(5, 0, 1);
    a.slli(5, 5, 11);
    a.addi(5, 5, 8); // MPP=S and MIE=1 (MRET restores from MPIE).
    a.csrw(kCsrMstatus, 5);
    a.mret();
    a.finish();
    return a.words();
}

std::vector<uint32_t> BuildMachineTrapHandler() {
    Assembler a(kPhysBase + kMachineTrapOffset);
    a.csrrw(5, kCsrMscratch, 5);
    a.sd(6, 5, 0);
    a.sd(7, 5, 8);
    a.csrr(6, kCsrMcause);
    a.slli(6, 6, 1);
    a.srli(6, 6, 1);
    a.addi(7, 0, static_cast<int32_t>(kInterruptMachineTimer));
    a.bne(6, 7, "ecall");
    a.label("restore");
    a.ld(6, 5, 0);
    a.ld(7, 5, 8);
    a.csrrw(5, kCsrMscratch, 5);
    a.mret();
    a.label("ecall");
    a.csrr(6, kCsrMepc);
    a.addi(6, 6, 4);
    a.csrw(kCsrMepc, 6);
    a.jal(0, "restore");
    a.finish();
    return a.words();
}

std::vector<uint32_t> BuildSupervisorTrapHandler(uint64_t targetCount) {
    Assembler a;
    // Preserve every integer register used by the handler or SBI TIME call.
    a.sd(5, 9, 64);
    a.sd(6, 9, 72);
    a.sd(7, 9, 80);
    a.sd(10, 9, 88);
    a.sd(11, 9, 96);
    a.sd(16, 9, 104);
    a.sd(17, 9, 112);

    a.ld(5, 9, 0);
    a.addi(5, 5, 1);
    a.sd(5, 9, 0);
    a.csrr(6, kCsrScause);
    a.sd(6, 9, 8);
    a.csrr(7, kCsrSepc);
    a.sd(7, 9, 16);
    a.addi(7, 0, static_cast<int32_t>(targetCount));
    a.bne(5, 7, "rearm");
    a.addi(10, 0, -1); // UINT64_MAX disables practical future delivery.
    a.jal(0, "set_timer");
    a.label("rearm");
    a.csrr(10, kCsrTime);
    a.addi(10, 10, 100);
    a.label("set_timer");
    a.li32(17, static_cast<uint32_t>(kSbiTimeEid));
    a.addi(16, 0, 0);
    a.ecall();
    a.sd(10, 9, 120);

    a.ld(17, 9, 112);
    a.ld(16, 9, 104);
    a.ld(11, 9, 96);
    a.ld(10, 9, 88);
    a.ld(7, 9, 80);
    a.ld(6, 9, 72);
    a.ld(5, 9, 64);
    a.sret();
    a.finish();
    return a.words();
}

std::vector<uint32_t> BuildSupervisorEntry(GuestKind kind, bool sv39,
                                           bool useWfi, uint64_t targetCount) {
    Assembler a;
    if (sv39) {
        a.lui(9, 0x50000); // Non-identity data VA.
        a.lui(5, 0x40001); // Non-identity handler VA.
    } else {
        a.auipc(9, 2);     // Physical data page, from entry page + 8 KiB.
        a.auipc(5, 1);
        a.addi(5, 5, -4);  // Handler begins at the next page.
    }
    a.csrw(kCsrStvec, 5);
    a.addi(5, 0, static_cast<int32_t>(kStip));
    a.csrw(kCsrSie, 5);
    a.addi(5, 0, 2);
    a.csrw(kCsrSstatus, 5);

    if (kind == GuestKind::Registration) {
        a.addi(18, 0, 123);
        a.addi(19, 0, 234);
        a.addi(28, 0, 321);
        a.addi(17, 0, static_cast<int32_t>(kSbiBaseEid));
        a.addi(16, 0, 0);
        a.ecall();
        a.sd(10, 9, 16);
        a.sd(11, 9, 24);

        a.addi(16, 0, 3);
        a.li32(10, static_cast<uint32_t>(kSbiTimeEid));
        a.ecall();
        a.sd(10, 9, 32);
        a.sd(11, 9, 40);

        a.csrr(5, kCsrTime);
        a.sd(5, 9, 48);
        a.addi(10, 5, 1000);
        a.sd(10, 9, 56);

        a.li32(17, static_cast<uint32_t>(kSbiTimeEid));
        a.addi(16, 0, 1); // Unsupported TIME function ID.
        a.addi(10, 0, 77);
        a.ecall();
        a.sd(10, 9, 64);

        a.addi(17, 0, 0x123);
        a.addi(16, 0, 0);
        a.ecall(); // Unsupported extension ID.
        a.sd(10, 9, 72);

        a.ld(10, 9, 56);
        a.li32(17, static_cast<uint32_t>(kSbiTimeEid));
        a.addi(16, 0, 0);
        a.ecall();
        a.sd(10, 9, 80);
        a.sd(11, 9, 96);
        a.addi(5, 0, 1);
        a.sd(5, 9, 88);
        a.ebreak();
    } else if (kind == GuestKind::Recurring) {
        a.csrr(10, kCsrTime);
        a.addi(10, 10, 100);
        a.li32(17, static_cast<uint32_t>(kSbiTimeEid));
        a.addi(16, 0, 0);
        a.ecall();
        a.label("wait_loop");
        if (useWfi) a.wfi();
        else a.addi(0, 0, 0);
        a.ld(5, 9, 0);
        a.addi(6, 0, static_cast<int32_t>(targetCount));
        a.bne(5, 6, "wait_loop");
        a.ebreak();
    } else { // time CSR access must trap when mcounteren.TM is clear.
        a.csrr(5, kCsrTime);
        a.ebreak();
    }
    a.finish();
    return a.words();
}

std::vector<uint8_t> BuildGuestImage(GuestKind kind, bool sv39 = false,
                                     bool useWfi = false, uint64_t targetCount = 1,
                                     bool enableMtimer = true,
                                     bool enableCounter = true) {
    std::vector<uint8_t> image(kImageSize, 0);
    CopyWords(image, 0, BuildMachineBoot(sv39, enableMtimer, enableCounter));
    CopyWords(image, kMachineTrapOffset, BuildMachineTrapHandler());
    CopyWords(image, kSupervisorEntryOffset,
              BuildSupervisorEntry(kind, sv39, useWfi, targetCount));
    if (kind == GuestKind::Recurring) {
        CopyWords(image, kSupervisorTrapOffset, BuildSupervisorTrapHandler(targetCount));
    }
    if (sv39) {
        const uint64_t rootPa = kPhysBase + kRootOffset;
        const uint64_t level1Pa = kPhysBase + kLevel1Offset;
        const uint64_t code0Pa = kPhysBase + kCodeLevel0Offset;
        const uint64_t data0Pa = kPhysBase + kDataLevel0Offset;
        WriteImageU64(image, kRootOffset + 8, MakePte(level1Pa, 1));
        WriteImageU64(image, kLevel1Offset + 0, MakePte(code0Pa, 1));
        WriteImageU64(image, kLevel1Offset + 128 * 8, MakePte(data0Pa, 1));
        WriteImageU64(image, kCodeLevel0Offset + 0,
                      MakePte(kPhysBase + kSupervisorEntryOffset, 0x4B));
        WriteImageU64(image, kCodeLevel0Offset + 8,
                      MakePte(kPhysBase + kSupervisorTrapOffset, 0x4B));
        WriteImageU64(image, kDataLevel0Offset + 0,
                      MakePte(kPhysBase + kDataOffset, 0xC7));
        (void)rootPa;
    }
    return image;
}

struct RunSummary {
    uint64_t steps = 0;
    uint64_t ecalls = 0;
    uint64_t machineTimerTraps = 0;
    uint64_t supervisorTimerTraps = 0;
    uint64_t wfiInstructions = 0;
    uint64_t waitingSteps = 0;
    uint64_t wfiWakeups = 0;
    bool halted = false;
};

RunSummary RunGuest(RV64CPU& cpu, uint64_t maxSteps, bool advanceWhileWaiting = true) {
    RunSummary summary;
    for (; summary.steps < maxSteps; ++summary.steps) {
        const StepResult r = cpu.step();
        if (r.wfiExecuted) ++summary.wfiInstructions;
        if (r.wokeFromWfi) ++summary.wfiWakeups;
        if (r.outcome == StepOutcome::Waiting) {
            ++summary.waitingSteps;
            if (advanceWhileWaiting) cpu.advanceVirtualTime(1);
            continue;
        }
        if (r.outcome == StepOutcome::Trap) {
            if (r.trapCause == TrapCause::EcallFromS) ++summary.ecalls;
            if (r.interruptTaken && r.interruptCause == kInterruptMachineTimer) {
                ++summary.machineTimerTraps;
            }
            if (r.interruptTaken && r.interruptCause == kInterruptSupervisorTimer) {
                ++summary.supervisorTimerTraps;
            }
            continue;
        }
        if (r.outcome == StepOutcome::Halt) {
            summary.halted = true;
            ++summary.steps;
            break;
        }
        if (r.outcome != StepOutcome::Ok) {
            std::cerr << "SBI TIME TEST FAILED: unexpected guest outcome "
                      << StepOutcomeToString(r.outcome) << " PC=0x" << std::hex
                      << cpu.getPC() << std::dec << '\n';
            std::exit(1);
        }
    }
    if (!summary.halted && summary.steps >= maxSteps) {
        uint64_t mcause = 0;
        uint64_t mepc = 0;
        (void)cpu.readCsr(kCsrMcause, mcause);
        (void)cpu.readCsr(kCsrMepc, mepc);
        std::cerr << "SBI TIME guest exhausted bound at PC=0x" << std::hex
                  << cpu.getPC() << " mcause=0x" << mcause << " mepc=0x" << mepc
                  << std::dec << " privilege=" << PrivilegeModeToString(cpu.privilege())
                  << " mtime=" << cpu.mtime() << '\n';
    }
    return summary;
}

uint64_t ReadPhysicalU64(RV64CPU& cpu, uint64_t offset) {
    uint64_t value = 0;
    Expect(cpu.readU64(kPhysBase + kDataOffset + offset, value), "read guest proof memory");
    return value;
}

void TestGuestRegistrationAndDiscovery() {
    RV64CPU cpu;
    cpu.loadProgram(kPhysBase, BuildGuestImage(GuestKind::Registration));
    SbiFirmwareProvider firmware(cpu);
    cpu.setSbiProvider(&firmware);
    const RunSummary run = RunGuest(cpu, 400);
    Expect(run.halted, "registration guest completes");
    Expect(run.ecalls >= 5, "registration guest issued at least five Base/TIME calls");
    ExpectEq("BASE GetSpecVersion error", kSbiSuccess, ReadPhysicalU64(cpu, 16));
    ExpectEq("BASE SBI version", kSbiSpecVersion, ReadPhysicalU64(cpu, 24));
    ExpectEq("TIME probe error", kSbiSuccess, ReadPhysicalU64(cpu, 32));
    ExpectEq("TIME is advertised", 1, ReadPhysicalU64(cpu, 40));
    Expect(ReadPhysicalU64(cpu, 48) > 0, "S-mode time CSR returns virtual time");
    const uint64_t deadline = ReadPhysicalU64(cpu, 56);
    ExpectEq("unsupported TIME FID", kSbiErrNotSupported, ReadPhysicalU64(cpu, 64));
    ExpectEq("unsupported EID", kSbiErrNotSupported, ReadPhysicalU64(cpu, 72));
    ExpectEq("TIME set_timer result", kSbiSuccess, ReadPhysicalU64(cpu, 80));
    ExpectEq("TIME return value", 0, ReadPhysicalU64(cpu, 96));
    ExpectEq("registration marker", 1, ReadPhysicalU64(cpu, 88));
    ExpectEq("SBI preserves s2", 123, cpu.readGR(18));
    ExpectEq("SBI preserves s3", 234, cpu.readGR(19));
    ExpectEq("firmware preserves unrelated integer register", 321, cpu.readGR(28));
    ExpectEq("provider deadline", deadline, firmware.scheduledDeadline());
    ExpectEq("machine comparator deadline", deadline, cpu.mtimecmp());
    Expect(cpu.supervisorTimerArmed(), "future absolute deadline is armed");
    Expect(firmware.supports(kSbiBaseEid), "firmware advertises SBI Base");
    Expect(firmware.supports(kSbiTimeEid, cpu), "firmware advertises connected SBI TIME");
    Expect(!firmware.supports(0x123ULL, cpu), "unknown EID is not advertised");
    std::cout << "PASS guest SBI Base probe, TIME discovery, absolute deadline registration\n";
}

void TestRecurringGuest(bool useWfi, bool sv39, uint64_t targetCount) {
    RV64CPU cpu;
    cpu.loadProgram(kPhysBase,
                    BuildGuestImage(GuestKind::Recurring, sv39, useWfi, targetCount));
    SbiFirmwareProvider firmware(cpu);
    cpu.setSbiProvider(&firmware);
    const RunSummary run = RunGuest(cpu, 30000);
    Expect(run.halted, "recurring supervisor timer guest completes");
    Expect(run.machineTimerTraps >= targetCount, "MTIP reaches M firmware service boundary");
    Expect(run.supervisorTimerTraps >= targetCount, "STIP reaches S-mode handler");
    ExpectEq("guest handled target timer events", targetCount, ReadPhysicalU64(cpu, 0));
    ExpectEq("last guest scause has STIP identity",
             kInterruptFlag | kInterruptSupervisorTimer, ReadPhysicalU64(cpu, 8));
    const uint64_t interruptedPc = ReadPhysicalU64(cpu, 16);
    Expect((interruptedPc & 3ULL) == 0, "S-mode interrupted PC remains aligned");
    if (sv39) {
        Expect(interruptedPc >= kSupervisorVa && interruptedPc < kSupervisorVa + 0x1000,
               "Sv39 interrupt preserves translated virtual EPC");
    }
    ExpectEq("guest final SBI TIME call succeeds", kSbiSuccess, ReadPhysicalU64(cpu, 120));
    ExpectEq("firmware retains disabled deadline", UINT64_MAX,
             firmware.scheduledDeadline());
    ExpectEq("UINT64_MAX disables comparator", UINT64_MAX, cpu.mtimecmp());
    Expect(!cpu.supervisorTimerArmed(), "final timer is disarmed");
    if (useWfi) {
        Expect(run.wfiInstructions >= targetCount, "guest executed WFI between events");
        Expect(run.waitingSteps > 0, "guest remained in WFI while virtual time advanced");
        Expect(run.wfiWakeups >= targetCount, "timer events woke guest WFI");
    }
    if (sv39) {
        Expect(cpu.pagingEnabled(), "Sv39 remains enabled for translated guest");
        const Sv39Translation handler = cpu.translateFetch(kSupervisorTrapVa, 4);
        Expect(handler.ok, "virtual timer handler translates");
        ExpectEq("handler uses non-identity physical page",
                 kPhysBase + kSupervisorTrapOffset, handler.paddr);
        ExpectEq("translated handler writes guest memory", targetCount,
                 ReadPhysicalU64(cpu, 0));
    }
    std::cout << "PASS " << (useWfi ? "WFI" : "busy-loop") << " timer guest ("
              << targetCount << " events, Sv39=" << (sv39 ? "on" : "off") << ")\n";
}

void TestTimerProviderSemanticsAndIsolation() {
    RV64CPU first;
    RV64CPU second;
    first.loadProgram(kPhysBase, std::vector<uint8_t>(64, 0));
    second.loadProgram(kPhysBase, std::vector<uint8_t>(64, 0));
    SbiFirmwareProvider firstFirmware(first);
    SbiFirmwareProvider secondFirmware(second);
    first.setSbiProvider(&firstFirmware);
    second.setSbiProvider(&secondFirmware);

    uint64_t args[6] = {20, 0, 0, 0, 0, 0};
    ExpectEq("TIME supports standard extension ID", true, firstFirmware.supports(kSbiTimeEid));
    ExpectEq("TIME FID 0 succeeds", kSbiSuccess,
             firstFirmware.handle(kSbiTimeEid, kSbiTimeSetTimerFid, args).error);
    ExpectEq("absolute deadline is stored", 20, firstFirmware.scheduledDeadline());
    ExpectEq("absolute deadline programs mtimecmp", 20, first.mtimecmp());
    ExpectEq("unknown TIME FID is unsupported", kSbiErrNotSupported,
             firstFirmware.handle(kSbiTimeEid, 1, args).error);
    ExpectEq("unknown EID is unsupported", kSbiErrNotSupported,
             firstFirmware.handle(0x1234, 0, args).error);

    args[0] = 50;
    ExpectEq("second VM TIME succeeds", kSbiSuccess,
             secondFirmware.handle(kSbiTimeEid, 0, args).error);
    first.advanceVirtualTime(20);
    uint64_t pending = 0;
    Expect(first.readCsr(0x344, pending), "read first VM pending state");
    Expect((pending & kMtip) != 0, "first MTIP asserts at its absolute deadline");
    ExpectEq("second clock remains isolated", 0, second.mtime());
    Expect(second.readCsr(kCsrTime, pending), "read second VM time CSR");
    ExpectEq("second time CSR is isolated", 0, pending);
    ExpectEq("second comparator remains isolated", 50, second.mtimecmp());
    Expect(first.serviceSupervisorTimer(), "first due timer is serviced");
    Expect(first.readCsr(0x344, pending), "read first VM after service");
    ExpectEq("MTIP clears after firmware comparator disarm", 0, pending & kMtip);
    ExpectEq("firmware presents separate STIP", kStip, pending & kStip);
    Expect(second.readCsr(0x344, pending), "read second VM pending state");
    ExpectEq("second VM receives no STIP", 0, pending & kStip);

    args[0] = 19; // already passed on first hart
    ExpectEq("past deadline call succeeds", kSbiSuccess,
             firstFirmware.handle(kSbiTimeEid, 0, args).error);
    Expect(first.serviceSupervisorTimer(), "past deadline becomes immediately serviceable");
    Expect(first.readCsr(0x344, pending), "read immediate pending state");
    ExpectEq("immediate event presents STIP", kStip, pending & kStip);

    args[0] = first.mtime() + 5;
    firstFirmware.handle(kSbiTimeEid, 0, args);
    Expect(first.readCsr(0x344, pending), "read reprogrammed pending state");
    ExpectEq("future reprogram clears old STIP", 0, pending & kStip);
    first.advanceVirtualTime(5);
    Expect(first.serviceSupervisorTimer(), "equal-to-now deadline fires");
    args[0] = UINT64_MAX;
    ExpectEq("UINT64_MAX set_timer succeeds", kSbiSuccess,
             firstFirmware.handle(kSbiTimeEid, 0, args).error);
    ExpectEq("UINT64_MAX deadline retained", UINT64_MAX, firstFirmware.scheduledDeadline());
    ExpectEq("UINT64_MAX comparator suppresses practical delivery", UINT64_MAX, first.mtimecmp());
    Expect(!first.supervisorTimerArmed(), "UINT64_MAX disarms future event");
    Expect(first.readCsr(0x344, pending), "read disabled pending state");
    ExpectEq("disabled timer clears STIP", 0, pending & kStip);
    Expect(!firstFirmware.supports(kSbiTimeEid, second),
           "provider refuses another VM context");
    ExpectEq("cross-VM TIME call is unsupported", kSbiErrNotSupported,
             firstFirmware.handle(kSbiTimeEid, 0, args, second).error);
    std::cout << "PASS absolute deadline, MTIP/STIP ownership, reprogramming, and VM isolation\n";
}

void TestUnsupportedAndNoProviderBehavior() {
    SbiBaseProvider base;
    uint64_t args[6] = {kSbiTimeEid, 0, 0, 0, 0, 0};
    Expect(base.supports(kSbiBaseEid), "Base-only provider keeps BASE support");
    Expect(!base.supports(kSbiTimeEid), "Base-only provider does not advertise TIME");
    ExpectEq("Base-only TIME probe returns zero", 0,
             base.handle(kSbiBaseEid, 3, args).value);
    ExpectEq("Base-only TIME call fails", kSbiErrNotSupported,
             base.handle(kSbiTimeEid, 0, args).error);

    RV64CPU unavailable;
    unavailable.loadProgram(kPhysBase, BuildGuestImage(GuestKind::Registration));
    unavailable.setSbiProvider(&base);
    const RunSummary unavailableRun = RunGuest(unavailable, 400);
    Expect(unavailableRun.halted, "guest completes with Base-only firmware");
    ExpectEq("Base-only firmware reports TIME unavailable", 0,
             ReadPhysicalU64(unavailable, 40));
    ExpectEq("Base-only firmware rejects TIME scheduling", kSbiErrNotSupported,
             ReadPhysicalU64(unavailable, 80));
    ExpectEq("unavailable TIME does not arm machine timer", UINT64_MAX,
             unavailable.mtimecmp());

    RV64CPU cpu;
    cpu.loadProgram(kPhysBase, BuildGuestImage(GuestKind::Registration));
    // No provider is attached: run until the first S-mode ECALL and verify the
    // architectural M trap is delivered without altering the guest result regs.
    bool sawBareEcall = false;
    for (uint64_t step = 0; step < 80; ++step) {
        const StepResult r = cpu.step();
        if (r.outcome == StepOutcome::Trap && r.trapCause == TrapCause::EcallFromS) {
            Expect(!r.sbiHandled, "no provider leaves architectural ECALL unhandled");
            ExpectEq("no-provider a0 remains unchanged", 0, cpu.readGR(10));
            Expect(cpu.privilege() == PrivilegeMode::Machine,
                   "no-provider ECALL enters M mode");
            sawBareEcall = true;
            break;
        }
    }
    Expect(sawBareEcall, "no-provider guest reaches S-mode ECALL");
    std::cout << "PASS unsupported TIME and no-provider architectural ECALL behavior\n";
}

void TestMachineTimerEnableGate() {
    RV64CPU cpu;
    cpu.loadProgram(kPhysBase, BuildGuestImage(GuestKind::CounterDenied, false, false,
                                               1, false, true));
    SbiFirmwareProvider firmware(cpu);
    cpu.setSbiProvider(&firmware);
    uint64_t args[6] = {1, 0, 0, 0, 0, 0};
    ExpectEq("disabled-MTIE test schedules TIME", kSbiSuccess,
             firmware.handle(kSbiTimeEid, 0, args).error);
    const RunSummary run = RunGuest(cpu, 100);
    Expect(run.halted, "guest continues with MTIE disabled");
    ExpectEq("MTIE-disabled timer never enters M service", 0, run.machineTimerTraps);
    ExpectEq("MTIE-disabled timer never presents STIP", 0, run.supervisorTimerTraps);
    uint64_t pending = 0;
    Expect(cpu.readCsr(0x344, pending), "read pending state with MTIE disabled");
    Expect((pending & kMtip) != 0, "MTIP remains pending while machine timer is disabled");
    ExpectEq("STIP remains clear until firmware service", 0, pending & kStip);

    RV64CPU machineMasked;
    std::vector<uint8_t> nops(16, 0);
    for (size_t i = 0; i < nops.size(); i += 4) nops[i] = 0x13;
    machineMasked.loadProgram(kPhysBase, nops);
    machineMasked.debugWriteCsr(kCsrMie, kMtip);
    machineMasked.debugWriteCsr(kCsrMstatus, 0); // MTIE set, global MIE clear.
    SbiFirmwareProvider machineFirmware(machineMasked);
    machineMasked.setSbiProvider(&machineFirmware);
    uint64_t due[6] = {0, 0, 0, 0, 0, 0};
    machineFirmware.handle(kSbiTimeEid, 0, due);
    const StepResult machineStep = machineMasked.step();
    Expect(machineStep.outcome == StepOutcome::Ok,
           "pending MTIP remains masked by MIE while executing in M mode");
    Expect(machineMasked.readCsr(0x344, pending), "read MIE-masked pending state");
    Expect((pending & kMtip) != 0, "MIE masking does not clear MTIP");
    std::cout << "PASS MTIE gates timer service without losing pending MTIP\n";
}

void TestCounterPermissionAndMaskedInterrupt() {
    RV64CPU denied;
    denied.loadProgram(kPhysBase,
                       BuildGuestImage(GuestKind::CounterDenied, false, false, 1,
                                       true, false));
    const RunSummary deniedRun = RunGuest(denied, 100);
    Expect(deniedRun.halted, "time CSR denial guest returns and halts");
    uint64_t cause = 0;
    Expect(denied.readCsr(kCsrMcause, cause), "read machine illegal-instruction cause");
    ExpectEq("mcounteren.TM denies S-mode time read", 2, cause);

    RV64CPU masked;
    masked.loadProgram(kPhysBase, BuildGuestImage(GuestKind::CounterDenied));
    SbiFirmwareProvider firmware(masked);
    masked.setSbiProvider(&firmware);
    // The program reaches S mode, then STIP is pending while SIE is globally
    // masked. Delegation and source-enable state alone must not trap it.
    bool enteredS = false;
    for (uint64_t i = 0; i < 30 && !enteredS; ++i) {
        const StepResult r = masked.step();
        enteredS = masked.privilege() == PrivilegeMode::Supervisor &&
                   r.outcome == StepOutcome::Ok;
    }
    Expect(enteredS, "masked-interrupt guest enters S mode");
    masked.debugWriteCsr(kCsrMideleg, kStip);
    masked.debugWriteCsr(kCsrSie, kStip);
    masked.debugWriteCsr(kCsrMstatus, 0); // SIE remains clear.
    Expect(masked.setInterruptPending(kInterruptSupervisorTimer, true),
           "assert STIP independently of enable state");
    const uint64_t before = masked.getPC();
    const StepResult maskedStep = masked.step();
    Expect(maskedStep.outcome == StepOutcome::Ok,
           "pending STIP with SIE clear remains masked");
    Expect(masked.getPC() != before, "masked pending interrupt does not block guest progress");
    Expect(masked.readCsr(0x344, cause), "read pending supervisor timer state");
    Expect((cause & kStip) != 0, "STIP remains pending while globally masked");

    RV64CPU locallyMasked;
    locallyMasked.loadProgram(kPhysBase, BuildGuestImage(GuestKind::CounterDenied));
    bool enteredLocalS = false;
    for (uint64_t i = 0; i < 30 && !enteredLocalS; ++i) {
        const StepResult r = locallyMasked.step();
        enteredLocalS = locallyMasked.privilege() == PrivilegeMode::Supervisor &&
                        r.outcome == StepOutcome::Ok;
    }
    Expect(enteredLocalS, "STIE-masked guest enters S mode");
    locallyMasked.debugWriteCsr(kCsrMideleg, kStip);
    locallyMasked.debugWriteCsr(kCsrSie, 0); // STIE clear.
    locallyMasked.debugWriteCsr(kCsrMstatus, 2); // SIE set.
    Expect(locallyMasked.setInterruptPending(kInterruptSupervisorTimer, true),
           "assert locally masked STIP");
    const StepResult localStep = locallyMasked.step();
    Expect(localStep.outcome == StepOutcome::Ok,
           "pending STIP remains masked by STIE with SIE enabled");
    Expect(locallyMasked.readCsr(0x344, cause), "read STIE-masked state");
    Expect((cause & kStip) != 0, "STIE masking leaves STIP pending");
    std::cout << "PASS time counter permission and pending-but-masked STIP\n";
}

} // namespace

int main() {
    std::cout << "RISCV-5 SBI TIME integration\n";
    TestGuestRegistrationAndDiscovery();
    TestRecurringGuest(false, false, 1); // Program B: S handler, rearm, SRET.
    TestRecurringGuest(true, false, 3);  // Program C: WFI and three separate events.
    TestRecurringGuest(true, true, 3);   // Program D: translated code, handler, and data.
    TestTimerProviderSemanticsAndIsolation();
    TestUnsupportedAndNoProviderBehavior();
    TestMachineTimerEnableGate();
    TestCounterPermissionAndMaskedInterrupt();
    std::cout << "RISCV-5 SBI TIME tests passed (" << g_checks << " runtime checks)\n";
    return 0;
}
