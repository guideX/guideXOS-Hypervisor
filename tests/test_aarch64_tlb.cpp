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
int checks = 0;
int failures = 0;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "AARCH64-7 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual,
             const char* expression, unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-7 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(x) check((x), #x, __LINE__)
#define CHECK_EQ(e, a) checkEq((e), (a), #e " == " #a, __LINE__)

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(size_t size) : bytes_(size, 0) {}
    void Read(uint64_t address, uint8_t* destination, size_t size) const override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address))
            throw std::out_of_range("read outside guest RAM");
        std::memcpy(destination, bytes_.data() + static_cast<size_t>(address), size);
    }
    void Write(uint64_t address, const uint8_t* source, size_t size) override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address))
            throw std::out_of_range("write outside guest RAM");
        std::memcpy(bytes_.data() + static_cast<size_t>(address), source, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* source, size_t size) override {
        Write(address, source, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& source) override {
        Write(address, source.data(), source.size());
    }
    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }
    uint8_t at(size_t index) const { return bytes_.at(index); }
    bool sameBytes(const FlatMemory& other) const { return bytes_ == other.bytes_; }
private:
    std::vector<uint8_t> bytes_;
};

constexpr uint32_t kDsbIsh = 0xD5033B9Fu;
constexpr uint32_t kDsbIshst = 0xD5033A9Fu;
constexpr uint32_t kIsb = 0xD5033FDFu;

uint64_t makeTcr() {
    return 25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32);
}

void put32(FlatMemory& memory, uint64_t address, uint32_t value) {
    uint8_t bytes[4] = {static_cast<uint8_t>(value),
                        static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value >> 16),
                        static_cast<uint8_t>(value >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

void put64(FlatMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t get64(const FlatMemory& memory, uint64_t address) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}

void putWords(FlatMemory& memory, uint64_t address,
              const std::vector<uint32_t>& words) {
    for (uint32_t word : words) {
        put32(memory, address, word);
        address += 4;
    }
}

uint32_t movz(unsigned rd, uint16_t imm, unsigned hw = 0) {
    return 0xD2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t movk(unsigned rd, uint16_t imm, unsigned hw) {
    return 0xF2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(movz(rd, static_cast<uint16_t>(value), 0));
    for (unsigned hw = 1; hw < 4; ++hw) {
        const uint16_t part = static_cast<uint16_t>(value >> (hw * 16));
        if (part != 0) words.push_back(movk(rd, part, hw));
    }
}

uint32_t ldrX(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xF9400000u | ((offset / 8u) << 10) | ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t strX(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xF9000000u | ((offset / 8u) << 10) | ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t msr(uint8_t crn, uint8_t crm, uint8_t op2, unsigned rt) {
    return 0xD5000000u | (3u << 19) | (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

uint32_t encodeTlbi(uint8_t crm, uint8_t op2, unsigned rt) {
    return 0xD5000000u | (1u << 19) | (8u << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

uint64_t pageDescriptor(uint64_t pa, uint8_t ap = 0, bool uxn = true,
                        bool pxn = true, bool nG = true) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(ap & 3u) << 6) | (3ULL << 8) | (1ULL << 10) |
           (uxn ? 1ULL << 54 : 0) | (pxn ? 1ULL << 53 : 0) |
           (nG ? 1ULL << 11 : 0);
}

void map39(FlatMemory& memory, uint64_t root, uint64_t l2, uint64_t l3,
           uint64_t va, uint64_t pa, uint8_t ap = 0, bool uxn = true,
           bool pxn = true, bool nG = true) {
    put64(memory, root + (((va >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va >> 12) & 0x1FFu) * 8),
          pageDescriptor(pa, ap, uxn, pxn, nG));
}

bool pluginStep(AArch64ISAPlugin& plugin, FlatMemory& memory) {
    const ia64::ISADecodeResult decoded = plugin.decode(memory);
    return plugin.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE;
}

bool runExact(AArch64ISAPlugin& plugin, FlatMemory& memory, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!pluginStep(plugin, memory)) return false;
    }
    return true;
}

void setMmu(Cpu& cpu, uint64_t ttbr0, uint64_t tcr = makeTcr()) {
    cpu.setPstate(0x5u);
    cpu.setTcrEl1(tcr);
    cpu.setTtbr0El1(ttbr0);
    cpu.setMairEl1(0xFFu);
    cpu.setSctlrEl1(1u);
}

void testTcrAsidSelectionAndEightBitProfile() {
    const uint64_t ttbr0 = (0xABULL << 48) | 0x1000u;
    const uint64_t ttbr1 = (0xCDULL << 48) | 0x2000u;
    MMUContext context = AArch64MMU::selectContext(0x4000, 1, makeTcr(),
                                                   ttbr0, ttbr1);
    CHECK(context.valid);
    CHECK_EQ(0xAB, context.asid); // TTBR[55:48] is the supported ASID field.
    context = AArch64MMU::selectContext(0x4000, 1, makeTcr() | (1ULL << 22),
                                        ttbr0, ttbr1);
    CHECK(context.valid);
    CHECK_EQ(0xCD, context.asid); // TCR_EL1.A1 selects TTBR1_EL1.ASID.
    CHECK(!AArch64MMU::validateTcr(makeTcr() | (1ULL << 36))); // ASID16 absent.
    context = AArch64MMU::selectContext(0x4000, 1, makeTcr(),
                                        ttbr0 | (1ULL << 56), ttbr1);
    CHECK(!context.valid);
    CHECK(context.fault == MMUFaultKind::AddressSize);
    context = AArch64MMU::selectContext(0x4000, 1, makeTcr(),
                                        0x1001, ttbr1); // CnP/reserved low bits
    CHECK(!context.valid);
    CHECK(context.fault == MMUFaultKind::AddressSize);

    FlatMemory memory(0x10000);
    map39(memory, 0x1000, 0x2000, 0x3000, 0x4000, 0x8000);
    const MMUResult translated = AArch64MMU::translate(
        memory, 0x4000, 1, MMUAccess::Read, makeTcr(),
        (0xABULL << 48) | 0x1000u, 0, 0xFF, false);
    CHECK(translated.success);
    CHECK_EQ(0x8000, translated.physicalAddress);
}

void testInvalidTtbrControlsCannotHitWarmEntry() {
    FlatMemory memory(0x10000);
    map39(memory, 0x4000, 0x5000, 0x6000,
          0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x4000, 0x5000, 0x6000, 0x4000, 0x8000);
    put32(memory, 0x1000, ldrX(0, 1));
    put64(memory, 0x8000, 0x0123456789ABCDEFULL);

    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    setMmu(guest.cpu(), 0x4000);
    guest.cpu().setVbarEl1(0x2000);
    guest.cpu().writeX(1, 0x4000);
    CHECK(pluginStep(guest, memory));
    CHECK_EQ(0x0123456789ABCDEFULL, guest.cpu().readX(0));
    CHECK(guest.cpu().tlb().validEntryCount() >= 2);

    // A warmed instruction mapping must not conceal CnP/misaligned TTBR bits.
    guest.cpu().setTtbr0El1(0x4001);
    guest.cpu().setPc(0x1000);
    CHECK(pluginStep(guest, memory));
    CHECK_EQ(0x2200, guest.cpu().pc());
    CHECK_EQ(0x1000, guest.cpu().farEl1());
    CHECK_EQ(0x86000001, guest.cpu().esrEl1()); // instruction address-size abort
}

void testSupportedTlbiDecodingAndPrivilege() {
    const uint32_t encodings[] = {
        encodeTlbi(7, 0, 31), encodeTlbi(3, 0, 31),
        encodeTlbi(7, 1, 0), encodeTlbi(3, 1, 1),
        encodeTlbi(7, 2, 2), encodeTlbi(3, 2, 3),
        encodeTlbi(7, 3, 4), encodeTlbi(3, 3, 5),
    };
    for (uint32_t word : encodings) {
        const Decoded decoded = Cpu::decodeWord(word, 0x1000);
        CHECK(decoded.valid);
        CHECK(decoded.op == Op::Tlbi);
    }
    CHECK(!Cpu::decodeWord(encodeTlbi(7, 4, 0), 0x1000).valid);
    CHECK(Cpu::decodeWord(encodeTlbi(7, 4, 0), 0x1000).fault == Fault::UnsupportedFeature);
    const uint32_t stage2Tlbi = encodeTlbi(3, 1, 0) | (4u << 16);
    CHECK(!Cpu::decodeWord(stage2Tlbi, 0x1000).valid);
    CHECK(Cpu::decodeWord(stage2Tlbi, 0x1000).fault == Fault::UnsupportedFeature);
    CHECK(!Cpu::decodeWord(encodeTlbi(7, 0, 0), 0x1000).valid);
    CHECK(Cpu::decodeWord(encodeTlbi(7, 0, 0), 0x1000).fault == Fault::UnallocatedEncoding);
    CHECK(!Cpu::decodeWord(0xD5098000u, 0x1000).valid); // reserved/unsupported SYS group

    FlatMemory memory(0x4000);
    std::vector<uint32_t> maintenance(encodings, encodings + 8);
    putWords(memory, 0x1000, maintenance);
    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    CHECK(runExact(guest, memory, maintenance.size()));
    CHECK_EQ(2, guest.cpu().tlb().stats().vmallInvalidations);
    CHECK_EQ(4, guest.cpu().tlb().stats().vaInvalidations);
    CHECK_EQ(2, guest.cpu().tlb().stats().asidInvalidations);
    CHECK(guest.hasFeature("TLBI"));

    // Operand RES0 violations are deterministic and never mutate cache state.
    put32(memory, 0x1100, encodeTlbi(3, 1, 0));
    AArch64ISAPlugin invalidOperand(0x1100, ExceptionLevel::EL1);
    invalidOperand.cpu().writeX(0, 1ULL << 44);
    CHECK(!pluginStep(invalidOperand, memory));
    CHECK_EQ(0, invalidOperand.cpu().tlb().stats().vaInvalidations);

    put32(memory, 0x1150, encodeTlbi(3, 3, 0));
    AArch64ISAPlugin invalidVaaeOperand(0x1150, ExceptionLevel::EL1);
    invalidVaaeOperand.cpu().writeX(0, 1ULL << 48); // VAAE ASID field is RES0.
    CHECK(!pluginStep(invalidVaaeOperand, memory));
    CHECK_EQ(0, invalidVaaeOperand.cpu().tlb().stats().vaInvalidations);

    put32(memory, 0x1200, encodeTlbi(3, 1, 0));
    AArch64ISAPlugin el0(0x1200, ExceptionLevel::EL0);
    el0.cpu().setVbarEl1(0x2000);
    CHECK(pluginStep(el0, memory)); // Undefined exception is taken to EL1.
    CHECK_EQ(0x2400, el0.cpu().pc());
    CHECK_EQ(0x1200, el0.cpu().elrEl1());
    CHECK_EQ(0, el0.cpu().esrEl1() >> 26);
    CHECK_EQ(0, el0.cpu().tlb().stats().vaInvalidations);

    put32(memory, 0x1300, encodeTlbi(3, 1, 0));
    AArch64ISAPlugin el2(0x1300, ExceptionLevel::EL1);
    el2.cpu().setPstate(0x9u);
    CHECK(!pluginStep(el2, memory));
    CHECK(el2.cpu().lastFault() == Fault::UnsupportedFeature);

    put32(memory, 0x1400, encodeTlbi(3, 1, 0));
    AArch64ISAPlugin el3(0x1400, ExceptionLevel::EL1);
    el3.cpu().setPstate(0xDu);
    CHECK(!pluginStep(el3, memory));
    CHECK(el3.cpu().lastFault() == Fault::UnsupportedFeature);
}

void seedAsidContexts(FlatMemory& memory, uint64_t rootA, uint64_t l2A,
                      uint64_t l3A, uint64_t rootB, uint64_t l2B,
                      uint64_t l3B) {
    map39(memory, rootA, l2A, l3A, 0x1000, 0x5000, 0, false, false, false);
    map39(memory, rootA, l2A, l3A, 0x4000, 0x8000);
    map39(memory, rootA, l2A, l3A, 0x6000, 0xC000, 0, true, true, false);
    map39(memory, rootB, l2B, l3B, 0x1000, 0x5000, 0, false, false, false);
    map39(memory, rootB, l2B, l3B, 0x4000, 0xA000);
    map39(memory, rootB, l2B, l3B, 0x6000, 0xC000, 0, true, true, false);
}

void testGuestAsidSwitchGlobalReuseAndSelectiveAside() {
    FlatMemory memory(0x20000);
    constexpr uint64_t rootA = 0x10000, l2A = 0x11000, l3A = 0x12000;
    constexpr uint64_t rootB = 0x13000, l2B = 0x14000, l3B = 0x15000;
    constexpr uint8_t asidA = 0x12, asidB = 0x34;
    seedAsidContexts(memory, rootA, l2A, l3A, rootB, l2B, l3B);
    put64(memory, 0x8000, 0xAAAAAAAAAAAAAAAAULL);
    put64(memory, 0xA000, 0xBBBBBBBBBBBBBBBBULL);
    put64(memory, 0xC000, 0xCCCCCCCCCCCCCCCCULL);
    put64(memory, 0xD000, 0xDDDDDDDDDDDDDDDDULL);

    std::vector<uint32_t> program = {
        ldrX(0, 1),                    // A: populate non-global translation
        ldrX(4, 11),                   // A: populate global mapping
        msr(2, 0, 0, 2),               // guest MSR TTBR0_EL1, x2 (ASID B)
        kIsb,
        ldrX(3, 1),                    // B: same VA, distinct PA
        ldrX(5, 11),                   // global entry still resolves to PA-C
        msr(2, 0, 0, 14),              // guest switch back to ASID A
        kIsb,
        ldrX(6, 1),                    // A entry is reusable
        encodeTlbi(3, 1, 13),           // VAE1IS for an absent/wrong ASID
        ldrX(12, 1),                   // A entry must survive wrong-ASID VAE
        encodeTlbi(3, 1, 15),           // VAE invalidates a global VA regardless of ASID
        ldrX(16, 11),                  // refill global mapping under A
        encodeTlbi(3, 2, 7),           // TLBI ASIDE1IS, x7
        kDsbIsh,
        ldrX(8, 1),                    // A must walk again
        msr(2, 0, 0, 2),
        kIsb,
        ldrX(9, 1),                    // B entry was preserved by ASIDE1
        ldrX(10, 11),                  // global entry reused across ASIDs
        encodeTlbi(3, 3, 17),           // VAAE1IS, x17 invalidates both data ASIDs
        kDsbIsh,
        ldrX(18, 1),                   // B walks again after VAAE
        msr(2, 0, 0, 14),
        kIsb,
        ldrX(19, 1),                   // A walks again after VAAE
        ldrX(20, 11),                  // unrelated global entry survived VAAE
        encodeTlbi(3, 0, 31),          // VMALLE1IS
        kDsbIsh,
        kNopEncoding,
    };
    putWords(memory, 0x5000, program);
    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    setMmu(guest.cpu(), (static_cast<uint64_t>(asidA) << 48) | rootA);
    guest.cpu().writeX(1, 0x4000);
    guest.cpu().writeX(2, (static_cast<uint64_t>(asidB) << 48) | rootB);
    guest.cpu().writeX(14, (static_cast<uint64_t>(asidA) << 48) | rootA);
    guest.cpu().writeX(7, static_cast<uint64_t>(asidA) << 48);
    guest.cpu().writeX(13, (static_cast<uint64_t>(0x56u) << 48) | 4u);
    guest.cpu().writeX(15, (static_cast<uint64_t>(0x56u) << 48) | 6u);
    guest.cpu().writeX(17, 4u); // VAAE operand has no ASID field.
    guest.cpu().writeX(11, 0x6000);

    for (size_t i = 0; i < program.size(); ++i) {
        CHECK(pluginStep(guest, memory));
        if (i == 9) {
            CHECK_EQ(1, guest.cpu().tlb().stats().vaInvalidations);
            CHECK_EQ(0, guest.cpu().tlb().stats().invalidatedEntries);
            CHECK_EQ(4, guest.cpu().tlb().validEntryCount());
        }
        if (i == 11) {
            CHECK_EQ(2, guest.cpu().tlb().stats().vaInvalidations);
            CHECK_EQ(1, guest.cpu().tlb().stats().invalidatedEntries);
            CHECK_EQ(3, guest.cpu().tlb().validEntryCount());
        }
        if (i == 13) {
            CHECK_EQ(1, guest.cpu().tlb().stats().asidInvalidations);
            CHECK_EQ(2, guest.cpu().tlb().stats().invalidatedEntries);
            CHECK_EQ(3, guest.cpu().tlb().validEntryCount());
        }
        if (i == 20) {
            CHECK_EQ(3, guest.cpu().tlb().stats().vaInvalidations);
            CHECK_EQ(4, guest.cpu().tlb().stats().invalidatedEntries);
            CHECK_EQ(2, guest.cpu().tlb().validEntryCount());
        }
        if (i == 27) {
            CHECK_EQ(1, guest.cpu().tlb().stats().vmallInvalidations);
            CHECK_EQ(0, guest.cpu().tlb().validEntryCount());
            CHECK_EQ(8, guest.cpu().tlb().stats().invalidatedEntries);
        }
    }
    CHECK_EQ(0x1000 + program.size() * 4, guest.cpu().pc());
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, guest.cpu().readX(0));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, guest.cpu().readX(3));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, guest.cpu().readX(4));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, guest.cpu().readX(5));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, guest.cpu().readX(6));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, guest.cpu().readX(12));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, guest.cpu().readX(8));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, guest.cpu().readX(9));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, guest.cpu().readX(10));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, guest.cpu().readX(16));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, guest.cpu().readX(18));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, guest.cpu().readX(19));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, guest.cpu().readX(20));
    CHECK(guest.cpu().tlb().stats().hits > 0);
    CHECK(guest.cpu().tlb().stats().misses > 0);
    CHECK_EQ(guest.cpu().tlb().stats().misses,
             guest.cpu().tlb().stats().tableWalks);
    const auto& stats = guest.cpu().tlb().stats();
    std::cout << "AARCH64-7 Program B: instructions=" << program.size()
              << " final_pc=0x" << std::hex << guest.cpu().pc()
              << " requests=" << std::dec << stats.translationRequests
              << " hits=" << stats.hits
              << " misses=" << stats.misses
              << " walks=" << stats.tableWalks
              << " inserts=" << stats.insertions
              << " evictions=" << stats.evictions
              << " VMALL=" << stats.vmallInvalidations
              << " VA=" << stats.vaInvalidations
              << " ASID=" << stats.asidInvalidations
              << " invalidated=" << stats.invalidatedEntries << '\n';
}

std::vector<uint32_t> buildGuestRemapProgram(uint64_t root) {
    std::vector<uint32_t> code;
    appendConstant(code, 0, makeTcr());
    code.push_back(msr(2, 0, 2, 0)); // TCR_EL1
    appendConstant(code, 1, root);
    code.push_back(msr(2, 0, 0, 1)); // TTBR0_EL1
    appendConstant(code, 2, 0xFF);
    code.push_back(msr(10, 2, 0, 2)); // MAIR_EL1
    appendConstant(code, 3, 1);
    code.push_back(msr(1, 0, 0, 3)); // SCTLR_EL1.M
    code.push_back(kDsbIshst);
    code.push_back(kIsb);
    appendConstant(code, 4, 0x5000);
    code.push_back(ldrX(5, 4));       // old mapping fills the TLB
    appendConstant(code, 6, 0);       // break: invalid descriptor
    appendConstant(code, 7, 0x7028);  // VA of L3[5]
    code.push_back(strX(6, 7));
    code.push_back(kDsbIshst);
    code.push_back(ldrX(8, 4));       // deterministic emulator stale hit
    appendConstant(code, 9, 5);       // TLBI VA operand for VA 0x5000
    code.push_back(encodeTlbi(3, 1, 9)); // TLBI VAE1IS, x9
    code.push_back(kDsbIsh);
    code.push_back(kIsb);
    appendConstant(code, 10, pageDescriptor(0xA000));
    code.push_back(strX(10, 7));      // make: install replacement descriptor
    code.push_back(kDsbIshst);
    code.push_back(kIsb);
    code.push_back(ldrX(11, 4));      // new mapping is walked and cached
    appendConstant(code, 12, 0x9000);
    code.push_back(strX(11, 12));     // publish verification result in guest RAM
    code.push_back(kNopEncoding);
    return code;
}

void seedRemapImage(FlatMemory& memory, const std::vector<uint32_t>& code) {
    memory.Clear();
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x5000, 0x8000);
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x7000, 0x12000, 0, true, true, true);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x9000, 0xD000);
    putWords(memory, 0x1000, code);
    put64(memory, 0x8000, 0x1111111111111111ULL);
    put64(memory, 0xA000, 0x2222222222222222ULL);
}

void testGuestBreakBeforeMakeRemapAndRepeatability() {
    const std::vector<uint32_t> code = buildGuestRemapProgram(0x10000);
    FlatMemory memoryA(0x20000), memoryB(0x20000);
    seedRemapImage(memoryA, code);
    seedRemapImage(memoryB, code);
    AArch64ISAPlugin guestA(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin guestB(0x1000, ExceptionLevel::EL1);
    CHECK(runExact(guestA, memoryA, code.size()));
    CHECK(runExact(guestB, memoryB, code.size()));
    CHECK_EQ(0x1000 + code.size() * 4, guestA.cpu().pc());
    CHECK_EQ(guestA.cpu().pc(), guestB.cpu().pc());
    CHECK_EQ(0x1111111111111111ULL, guestA.cpu().readX(5));
    CHECK_EQ(0x1111111111111111ULL, guestA.cpu().readX(8));
    CHECK_EQ(0x2222222222222222ULL, guestA.cpu().readX(11));
    CHECK_EQ(0x2222222222222222ULL, get64(memoryA, 0xD000));
    CHECK_EQ(pageDescriptor(0xA000), get64(memoryA, 0x12028));
    CHECK_EQ(1, guestA.cpu().tlb().stats().vaInvalidations);
    CHECK(guestA.cpu().tlb().stats().hits > 0);
    CHECK(guestA.cpu().tlb().stats().misses > 0);
    CHECK(guestA.cpu().tlb().stats().tableWalks > 0);
    CHECK_EQ(guestA.cpu().tlb().stats().hits, guestB.cpu().tlb().stats().hits);
    CHECK_EQ(guestA.cpu().tlb().stats().misses, guestB.cpu().tlb().stats().misses);
    CHECK(memoryA.sameBytes(memoryB));
    CHECK(guestA.serialize_state() == guestB.serialize_state());
    const auto& stats = guestA.cpu().tlb().stats();
    std::cout << "AARCH64-7 Program A: instructions=" << code.size()
              << " final_pc=0x" << std::hex << guestA.cpu().pc()
              << " requests=" << std::dec << stats.translationRequests
              << " hits=" << stats.hits
              << " misses=" << stats.misses
              << " walks=" << stats.tableWalks
              << " inserts=" << stats.insertions
              << " evictions=" << stats.evictions
              << " VA=" << stats.vaInvalidations
              << " invalidated=" << stats.invalidatedEntries << '\n';
}

void testColdCheckpointRestoreAndCpuIsolation() {
    FlatMemory memory(0x10000);
    map39(memory, 0x4000, 0x5000, 0x6000,
          0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x4000, 0x5000, 0x6000, 0x4000, 0x8000);
    putWords(memory, 0x1000, {ldrX(0, 1), ldrX(0, 1), ldrX(0, 1), kNopEncoding});
    put64(memory, 0x8000, 0x0123456789ABCDEFULL);
    AArch64ISAPlugin writer(0x1000, ExceptionLevel::EL1);
    setMmu(writer.cpu(), 0x4000);
    writer.cpu().writeX(1, 0x4000);
    CHECK(pluginStep(writer, memory));
    CHECK_EQ(0x0123456789ABCDEFULL, writer.cpu().readX(0));
    CHECK(writer.cpu().tlb().validEntryCount() >= 2);
    const uint64_t writerHits = writer.cpu().tlb().stats().hits;
    const std::vector<uint8_t> checkpoint = writer.serialize_state();
    CHECK_EQ(400, checkpoint.size());

    AArch64ISAPlugin restored(0, ExceptionLevel::EL0);
    CHECK(restored.deserialize_state(checkpoint));
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK_EQ(0, restored.cpu().tlb().stats().translationRequests);
    CHECK(pluginStep(restored, memory));
    CHECK_EQ(0x0123456789ABCDEFULL, restored.cpu().readX(0));
    CHECK(restored.cpu().tlb().stats().misses >= 2); // code and data are cold
    CHECK(restored.cpu().tlb().stats().tableWalks >= 2);
    CHECK(pluginStep(restored, memory));
    CHECK(restored.cpu().tlb().stats().hits >= 2);
    CHECK_EQ(writerHits, writer.cpu().tlb().stats().hits); // independent instances

    restored.reset();
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK_EQ(0, restored.cpu().tlb().stats().translationRequests);
}

void testBlockMappingHitAndGuestVaInvalidation() {
    FlatMemory memory(0x10000);
    constexpr uint64_t va = 0x201000;
    put64(memory, 0x4000, 0x5000 | 3u);
    const uint64_t block = 1u | (3ULL << 8) | (1ULL << 10); // 2 MiB L2 block
    put64(memory, 0x5008, block);
    putWords(memory, 0x1000, {ldrX(0, 1), ldrX(2, 1),
                              encodeTlbi(3, 1, 3), // wrong VA: outside the block
                              ldrX(4, 1),
                              encodeTlbi(3, 1, 5), // matching VA inside the block
                              ldrX(6, 1), kNopEncoding});
    put64(memory, 0x1234, 0xFEDCBA9876543210ULL);
    AArch64ISAPlugin guest(va, ExceptionLevel::EL1);
    setMmu(guest.cpu(), 0x4000);
    guest.cpu().writeX(1, va + 0x234);
    guest.cpu().writeX(3, (va + 0x200000) >> 12);
    guest.cpu().writeX(5, (va + 0x1000) >> 12);
    CHECK(runExact(guest, memory, 7));
    CHECK_EQ(0xFEDCBA9876543210ULL, guest.cpu().readX(0));
    CHECK_EQ(0xFEDCBA9876543210ULL, guest.cpu().readX(2));
    CHECK_EQ(0xFEDCBA9876543210ULL, guest.cpu().readX(4));
    CHECK_EQ(0xFEDCBA9876543210ULL, guest.cpu().readX(6));
    CHECK_EQ(2, guest.cpu().tlb().stats().vaInvalidations);
    CHECK(guest.cpu().tlb().stats().misses >= 2); // cold block plus refetch after hit
    CHECK_EQ(1, guest.cpu().tlb().stats().invalidatedEntries);
    CHECK_EQ(va + 28, guest.cpu().pc());
}

std::vector<uint32_t> buildPermissionUpdateProgram() {
    std::vector<uint32_t> code;
    appendConstant(code, 1, 0x5000);
    appendConstant(code, 2, 0xDEADBEEFCAFEBABEULL);
    appendConstant(code, 3, 0x7028);
    code.push_back(ldrX(0, 1));
    appendConstant(code, 4, 0); // BBM break: clear the old descriptor
    code.push_back(strX(4, 3));
    code.push_back(kDsbIshst);
    appendConstant(code, 5, 5);
    code.push_back(encodeTlbi(3, 1, 5));
    code.push_back(kDsbIsh);
    code.push_back(kIsb);
    appendConstant(code, 4, pageDescriptor(0x8000, 2)); // AP=10: EL1 read-only
    code.push_back(strX(4, 3));
    code.push_back(kDsbIshst);
    code.push_back(kIsb);
    code.push_back(strX(2, 1)); // rejected write enters the real EL1 handler
    code.push_back(kNopEncoding);
    return code;
}

std::vector<uint32_t> dataAbortHandler() {
    return {
        0xD538600Fu,                   // MRS x15,FAR_EL1
        0xD5385210u,                   // MRS x16,ESR_EL1
        0xD5384031u,                   // MRS x17,ELR_EL1
        0x91001231u,                   // ADD x17,x17,#4
        0xD5184031u,                   // MSR ELR_EL1,x17
        0xD69F03E0u,                   // ERET
    };
}

void testGuestPermissionInvalidationAndAbortRecovery() {
    FlatMemory memory(0x20000);
    const std::vector<uint32_t> code = buildPermissionUpdateProgram();
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x2000, 0x6000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x5000, 0x8000);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x7000, 0x12000);
    putWords(memory, 0x1000, code);
    putWords(memory, 0x6200, dataAbortHandler());
    put64(memory, 0x8000, 0x1122334455667788ULL);

    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    setMmu(guest.cpu(), 0x10000);
    guest.cpu().setVbarEl1(0x2000);
    const size_t faultingStoreIndex = code.size() - 2;
    const uint64_t faultingStorePc = 0x1000 + faultingStoreIndex * 4;
    uint64_t executed = 0;
    while (guest.cpu().pc() != 0x1000 + code.size() * 4 && executed < 64) {
        CHECK(pluginStep(guest, memory));
        ++executed;
    }
    CHECK_EQ(code.size() + dataAbortHandler().size(), executed);
    CHECK_EQ(0x1000 + code.size() * 4, guest.cpu().pc());
    CHECK_EQ(0x1122334455667788ULL, guest.cpu().readX(0));
    CHECK_EQ(0x5000, guest.cpu().farEl1());
    CHECK_EQ(0x9600004F, guest.cpu().esrEl1()); // permission, same-EL write
    CHECK_EQ(faultingStorePc + 4, guest.cpu().elrEl1());
    CHECK_EQ(0x5000, guest.cpu().readX(15));
    CHECK_EQ(0x9600004F, guest.cpu().readX(16));
    CHECK_EQ(faultingStorePc + 4, guest.cpu().readX(17));
    CHECK_EQ(0x1122334455667788ULL, get64(memory, 0x8000));
    CHECK_EQ(1, guest.cpu().tlb().stats().vaInvalidations);
    CHECK(guest.cpu().tlb().stats().invalidatedEntries >= 1);
    const auto& stats = guest.cpu().tlb().stats();
    std::cout << "AARCH64-7 Program C: guest_instructions=" << executed
              << " final_pc=0x" << std::hex << guest.cpu().pc()
              << " requests=" << std::dec << stats.translationRequests
              << " hits=" << stats.hits
              << " misses=" << stats.misses
              << " walks=" << stats.tableWalks
              << " inserts=" << stats.insertions
              << " evictions=" << stats.evictions
              << " VA=" << stats.vaInvalidations
              << " invalidated=" << stats.invalidatedEntries << '\n';
}

void testExecutePermissionAfterGuestTlbi() {
    FlatMemory memory(0x20000);
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x2000, 0x6000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000,
          0x5000, 0x8000, 0, false, false, true);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x7000, 0x12000);
    put32(memory, 0x8000, kNopEncoding);
    std::vector<uint32_t> maintenance;
    appendConstant(maintenance, 1, 0x7028);
    appendConstant(maintenance, 2, 5);
    appendConstant(maintenance, 3, 0); // BBM break: clear old executable leaf
    maintenance.push_back(strX(3, 1));
    maintenance.push_back(kDsbIshst);
    maintenance.push_back(encodeTlbi(3, 1, 2));
    maintenance.push_back(kDsbIsh);
    maintenance.push_back(kIsb);
    appendConstant(maintenance, 3, pageDescriptor(0x8000, 0, true, true));
    maintenance.push_back(strX(3, 1));
    maintenance.push_back(kDsbIshst);
    maintenance.push_back(kIsb);
    maintenance.push_back(kNopEncoding);
    putWords(memory, 0x1000, maintenance);
    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    setMmu(guest.cpu(), 0x10000);
    guest.cpu().setVbarEl1(0x2000);
    CHECK(guest.cpu().fetchDecodeAt(memory, 0x5000).valid); // warm executable entry
    CHECK(runExact(guest, memory, maintenance.size()));
    const Decoded rejectedFetch = guest.cpu().fetchDecodeAt(memory, 0x5000);
    CHECK(!rejectedFetch.valid);
    CHECK(rejectedFetch.hasMemoryAbort);
    CHECK_EQ(0x0F, rejectedFetch.memoryAbortStatus);
    const uint64_t returnPc = guest.cpu().pc();
    CHECK(guest.cpu().executeDecoded(rejectedFetch, memory) == Fault::None);
    CHECK_EQ(0x5000, guest.cpu().farEl1());
    CHECK_EQ(0x8600000F, guest.cpu().esrEl1()); // same-EL instruction permission abort
    CHECK_EQ(returnPc, guest.cpu().elrEl1());
}

void testDeterministicCapacityAndInstanceIsolation() {
    AArch64TLB tlb;
    MMUContext context;
    context.valid = true;
    context.region = MMURegion::Lower;
    context.vaBits = 39;
    context.asid = 7;
    context.configurationKey = makeTcr();
    for (size_t i = 0; i <= AArch64TLB::kCapacity; ++i) {
        MMUResult::Attributes attributes;
        attributes.virtualBase = static_cast<uint64_t>(i) << 12;
        attributes.physicalBase = 0x100000 + (static_cast<uint64_t>(i) << 12);
        attributes.mappingSize = 4096;
        attributes.accessFlag = true;
        attributes.global = false;
        tlb.insert(context, 0xFF, attributes);
    }
    CHECK_EQ(AArch64TLB::kCapacity, tlb.validEntryCount());
    CHECK_EQ(1, tlb.stats().evictions);
    MMUResult::Attributes output;
    CHECK(!tlb.lookup(context, 0, 0xFF, output)); // FIFO evicted entry zero.
    CHECK(tlb.lookup(context, 0x1000, 0xFF, output));
    CHECK_EQ(0x101000, output.physicalBase);

    AArch64TLB other;
    CHECK_EQ(0, other.validEntryCount());
    CHECK_EQ(0, other.stats().translationRequests);
    tlb.reset();
    CHECK_EQ(0, tlb.validEntryCount());
    CHECK_EQ(0, tlb.stats().insertions);
}

} // namespace

int main() {
    testTcrAsidSelectionAndEightBitProfile();
    testInvalidTtbrControlsCannotHitWarmEntry();
    testSupportedTlbiDecodingAndPrivilege();
    testGuestAsidSwitchGlobalReuseAndSelectiveAside();
    testGuestBreakBeforeMakeRemapAndRepeatability();
    testColdCheckpointRestoreAndCpuIsolation();
    testBlockMappingHitAndGuestVaInvalidation();
    testGuestPermissionInvalidationAndAbortRecovery();
    testExecutePermissionAfterGuestTlbi();
    testDeterministicCapacityAndInstanceIsolation();
    std::cout << "AArch64TLBTests: " << checks << " checks, " << failures
              << " failures\n";
    return failures == 0 ? 0 : 1;
}
