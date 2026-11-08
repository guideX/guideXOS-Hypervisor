// PPC64 Phase 21 RLDICR decode, execution, and preservation regressions.

#include "PPC64ISAPlugin.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                          \
        }                                                                      \
    } while (0)

constexpr uint32_t kLinuxRldicr = 0x798C07C6U;
constexpr uint64_t kPhase20R12 = 0xFFFFFFFF80000000ULL;
constexpr uint64_t kLinuxR12Result = 0x8000000000000000ULL;
constexpr uint32_t kXerSO = 0x80000000U;
constexpr uint32_t kXerOV = 0x40000000U;
constexpr uint32_t kXerCA = 0x20000000U;

void storeBE(IMemory& memory, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word >> 24),
        static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word),
    };
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t encodeRldicr(uint8_t rs, uint8_t ra, uint8_t sh, uint8_t me,
                      bool rc = false, uint8_t operation = 1) {
    return (30U << 26) |
           (static_cast<uint32_t>(rs & 31U) << 21) |
           (static_cast<uint32_t>(ra & 31U) << 16) |
           (static_cast<uint32_t>(sh & 31U) << 11) |
           (static_cast<uint32_t>(me & 31U) << 6) |
           (static_cast<uint32_t>((me >> 5) & 1U) << 5) |
           (static_cast<uint32_t>(operation & 7U) << 2) |
           (static_cast<uint32_t>((sh >> 5) & 1U) << 1) |
           (rc ? 1U : 0U);
}

uint64_t referenceRotateLeft(uint64_t value, unsigned amount) {
    // Independent bit-by-bit model: bit positions here are LSB-numbered.
    amount &= 63U;
    uint64_t result = 0;
    for (unsigned sourceBit = 0; sourceBit < 64; ++sourceBit) {
        if (((value >> sourceBit) & 1U) != 0) {
            const unsigned destinationBit = (sourceBit + amount) & 63U;
            result |= uint64_t{1} << destinationBit;
        }
    }
    return result;
}

uint64_t referenceMask(unsigned end) {
    uint64_t result = 0;
    for (unsigned powerBit = 0; powerBit <= end; ++powerBit) {
        result |= uint64_t{1} << (63U - powerBit);
    }
    return result;
}

uint8_t referenceCr0(uint64_t result, uint32_t xer) {
    uint8_t field = result == 0 ? 0x2U : ((result >> 63) ? 0x8U : 0x4U);
    if ((xer & kXerSO) != 0) field |= 0x1U;
    return field;
}

struct RunResult {
    PPC64ISAState before;
    PPC64ISAState after;
    bool continued = false;
    bool memoryUnchanged = false;
    std::string disassembly;
};

RunResult runInstruction(uint32_t word, uint8_t rs, uint64_t source,
                         uint8_t destination, uint32_t cr = 0xA5B6C7D8U,
                         uint32_t xer = 0xE1234567U) {
    Memory memory(4096);
    storeBE(memory, 0, word);

    PPC64ISAPlugin cpu;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    state.nip = 0;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.cr = cr;
    state.xer = xer;
    state.msr = 0x8000000000000000ULL;
    for (unsigned i = 0; i < state.gpr.size(); ++i) {
        state.gpr[i] = 0x0102030405060708ULL * (i + 1U);
    }
    state.gpr[rs] = source;

    RunResult result;
    result.before = state;
    if (PPC64ISAPlugin::decodeWord(word).ra != destination) {
        result.after = state;
        return result;
    }
    const ISADecodeResult decoded = cpu.decode(memory);
    result.disassembly = decoded.disassembly;
    result.continued = cpu.step(memory) == ISAExecutionResult::CONTINUE;
    result.after = dynamic_cast<const PPC64ISAState&>(cpu.getState());
    uint8_t fetched[4]{};
    memory.Read(0, fetched, sizeof(fetched));
    result.memoryUnchanged = fetched[0] == static_cast<uint8_t>(word >> 24) &&
                             fetched[1] == static_cast<uint8_t>(word >> 16) &&
                             fetched[2] == static_cast<uint8_t>(word >> 8) &&
                             fetched[3] == static_cast<uint8_t>(word);
    return result;
}

void testExactLinuxEncoding() {
    std::puts("[phase21-exact-linux-encoding]");
    const auto instruction = PPC64ISAPlugin::decodeWord(kLinuxRldicr);
    CHECK(instruction.opcode == 30);
    CHECK(instruction.kind == DecodedKind::RLDICR);
    CHECK(instruction.supported);
    CHECK(instruction.rt == 12);  // RS
    CHECK(instruction.ra == 12);  // RA
    CHECK(instruction.sh == 32);
    CHECK(instruction.me == 31);
    CHECK(!instruction.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(instruction, 0xD320) ==
          "rldicr r12,r12,32,31");
    CHECK(encodeRldicr(12, 12, 32, 31) == kLinuxRldicr);

    const auto execution = runInstruction(kLinuxRldicr, 12, kPhase20R12, 12);
    CHECK(execution.continued);
    CHECK(execution.after.gpr[12] == kLinuxR12Result);
    CHECK(execution.after.nip == 4);
}

void testImmediateBoundaries() {
    std::puts("[phase21-six-bit-immediates]");
    constexpr std::array<uint8_t, 6> shifts{0, 1, 31, 32, 33, 63};
    for (uint8_t shift : shifts) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeRldicr(7, 19, shift, 63));
        CHECK(decoded.kind == DecodedKind::RLDICR);
        CHECK(decoded.supported);
        CHECK(decoded.sh == shift);
        CHECK(decoded.me == 63);
    }
    for (unsigned maskEnd = 0; maskEnd < 64; ++maskEnd) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeRldicr(3, 4, 32, static_cast<uint8_t>(maskEnd)));
        CHECK(decoded.kind == DecodedKind::RLDICR);
        CHECK(decoded.sh == 32);
        CHECK(decoded.me == maskEnd);
    }
    CHECK(referenceMask(0) == 0x8000000000000000ULL);
    CHECK(referenceMask(31) == 0xFFFFFFFF00000000ULL);
    CHECK(referenceMask(32) == 0xFFFFFFFF80000000ULL);
    CHECK(referenceMask(63) == 0xFFFFFFFFFFFFFFFFULL);
}

void testRotateMaskPatternsAndAliasing() {
    std::puts("[phase21-rotate-mask-patterns-aliasing]");
    constexpr std::array<uint64_t, 9> patterns{
        0,
        0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        1,
        0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000001ULL,
        0x0123456789ABCDEFULL,
    };
    constexpr std::array<uint8_t, 6> shifts{0, 1, 31, 32, 33, 63};
    constexpr std::array<uint8_t, 6> maskEnds{0, 1, 31, 32, 62, 63};
    for (size_t pattern = 0; pattern < patterns.size(); ++pattern) {
        for (size_t index = 0; index < shifts.size(); ++index) {
            const uint8_t shift = shifts[index];
            const uint8_t end = maskEnds[index];
            const uint8_t rs = pattern % 2 == 0 ? 12 : 0;
            const uint8_t ra = pattern % 3 == 0 ? rs : 17;
            const uint32_t word = encodeRldicr(rs, ra, shift, end);
            const uint64_t expected = referenceRotateLeft(patterns[pattern], shift) &
                                      referenceMask(end);
            const RunResult run = runInstruction(word, rs, patterns[pattern], ra);
            CHECK(run.continued);
            CHECK(run.after.gpr[ra] == expected);
            CHECK(run.after.nip == 4);
            CHECK(run.after.cr == run.before.cr);  // Rc=0
            CHECK(run.after.xer == run.before.xer);
            CHECK(run.after.lr == run.before.lr);
            CHECK(run.after.ctr == run.before.ctr);
            CHECK(run.after.msr == run.before.msr);
            CHECK(run.memoryUnchanged);
            for (unsigned reg = 0; reg < 32; ++reg) {
                if (reg != ra) CHECK(run.after.gpr[reg] == run.before.gpr[reg]);
            }
        }
    }
    // GPR0 is an ordinary source/destination for this instruction.
    const RunResult zeroSource = runInstruction(encodeRldicr(0, 5, 0, 63),
                                                 0, 0x8000000000000001ULL, 5);
    CHECK(zeroSource.after.gpr[5] == 0x8000000000000001ULL);
    const RunResult zeroDest = runInstruction(encodeRldicr(6, 0, 1, 63),
                                               6, 3, 0);
    CHECK(zeroDest.after.gpr[0] == 6);
}

void testRecordFormAndXerPreservation() {
    std::puts("[phase21-record-form-cr0-xer]");
    struct Case { uint64_t source; uint32_t xer; };
    constexpr std::array<Case, 4> cases{{
        {0x8000000000000000ULL, 0}, // LT, no SO
        {1, kXerSO},                // GT and SO
        {0, kXerOV | kXerCA},       // EQ, no SO
        {0x7FFFFFFFFFFFFFFFULL, kXerSO | kXerOV | kXerCA}, // GT and SO
    }};
    for (const Case& item : cases) {
        constexpr uint32_t originalCr = 0x12345678U;
        const RunResult run = runInstruction(encodeRldicr(9, 10, 0, 63, true),
                                              9, item.source, 10, originalCr,
                                              item.xer);
        CHECK(run.continued);
        const uint64_t expected = item.source;
        CHECK(run.after.gpr[10] == expected);
        const uint8_t field = referenceCr0(expected, item.xer);
        const uint32_t expectedCr = (originalCr & 0x0FFFFFFFU) |
                                    (static_cast<uint32_t>(field) << 28);
        CHECK(run.after.cr == expectedCr);
        CHECK((run.after.cr & 0x0FFFFFFFU) == (originalCr & 0x0FFFFFFFU));
        CHECK(run.after.xer == item.xer);
    }
    CHECK(referenceCr0(0x8000000000000000ULL, 0) == 0x8);
    CHECK(referenceCr0(1, 0) == 0x4);
    CHECK(referenceCr0(0, 0) == 0x2);
    CHECK(referenceCr0(1, kXerSO) == 0x5);
}

void testDecodeIsolation() {
    std::puts("[phase21-md-mds-isolation]");
    // RLDICL, RLDICR, RLDIC, and RLDIMI have distinct MD operation values.
    const auto rldicl = PPC64ISAPlugin::decodeWord(
        encodeRldicr(3, 4, 5, 6, false, 0));
    CHECK(rldicl.kind == DecodedKind::RLDICL);
    CHECK(rldicl.supported && rldicl.sh == 5 && rldicl.mb == 6);
    for (uint8_t operation : {uint8_t{2}, uint8_t{3}, uint8_t{4},
                              uint8_t{5}, uint8_t{6}, uint8_t{7}}) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeRldicr(3, 4, 5, 6, false, operation));
        if (operation == 2) {
            CHECK(decoded.kind == DecodedKind::RLDIC);
            CHECK(decoded.supported && decoded.sh == 5 && decoded.mb == 6);
        } else if (operation == 3) {
            CHECK(decoded.kind == DecodedKind::RLDIMI);
            CHECK(decoded.supported && decoded.sh == 5 && decoded.mb == 6);
        } else {
            CHECK(decoded.kind == DecodedKind::UNSUPPORTED);
            CHECK(!decoded.supported);
        }
    }
    // Changing Rc remains the record form of rldicr and does not change its
    // operation classification.
    const auto record = PPC64ISAPlugin::decodeWord(encodeRldicr(3, 4, 5, 6, true));
    CHECK(record.kind == DecodedKind::RLDICR);
    CHECK(record.supported);
    CHECK(record.rc);
}

void testProductionVmStep() {
    std::puts("[phase21-production-vm-step]");
    VirtualMachine vm(2U << 20, 1, "PPC64");
    CHECK(vm.init());
    if (!vm.getPPC64ISAPlugin()) return;

    const uint8_t program[8] = {0x79, 0x8C, 0x07, 0xC6,
                                0x39, 0xA0, 0x00, 0x01}; // rldicr + addi r13,r0,1
    vm.getMemory().Write(0, program, sizeof(program));
    auto& state = dynamic_cast<PPC64ISAState&>(
        vm.getPPC64ISAPlugin()->getState());
    state.nip = 0;
    state.gpr[12] = kPhase20R12;

    CHECK(vm.step());
    CHECK(state.gpr[12] == kLinuxR12Result);
    CHECK(state.nip == 4);
    CHECK(vm.step());
    CHECK(state.gpr[13] == 1);
    CHECK(state.nip == 8);
}

}  // namespace

int main() {
    std::puts("PPC64 Phase 21 RLDICR acceptance suite");
    testExactLinuxEncoding();
    testImmediateBoundaries();
    testRotateMaskPatternsAndAliasing();
    testRecordFormAndXerPreservation();
    testDecodeIsolation();
    testProductionVmStep();
    std::printf("Phase21 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
