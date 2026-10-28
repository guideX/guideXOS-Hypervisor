// PPC64 Phase 5: load-reserve/store-conditional, reservation coherence,
// synchronization, exceptions, and real encoded execution programs.

#include "PPC64ISAPlugin.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

namespace {

constexpr uint64_t kProgram = 0x1000;
constexpr uint64_t kCounter = 0x400;
constexpr uint32_t kInitial32 = 0x11223344U;
constexpr uint64_t kInitial64 = 0x0000000100000000ULL;

void storeBE(IMemory& mem, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
    mem.Write(address, bytes, sizeof(bytes));
}

void storeWords(IMemory& mem, uint64_t address, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        storeBE(mem, address + static_cast<uint64_t>(i) * 4, words[i]);
    }
}

void storeValueBE(IMemory& mem, uint64_t address, uint64_t value, uint32_t width) {
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * (width - i - 1)));
    }
    mem.Write(address, bytes, width);
}

uint64_t readValueBE(IMemory& mem, uint64_t address, uint32_t width) {
    uint8_t bytes[8] = {};
    mem.Read(address, bytes, width);
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t immediate) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt & 0x1F) << 21) |
           (static_cast<uint32_t>(ra & 0x1F) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeX(uint8_t rt, uint8_t ra, uint8_t rb, uint16_t xo, bool bit31 = false) {
    return (31U << 26) | (static_cast<uint32_t>(rt & 0x1F) << 21) |
           (static_cast<uint32_t>(ra & 0x1F) << 16) |
           (static_cast<uint32_t>(rb & 0x1F) << 11) |
           (static_cast<uint32_t>(xo & 0x3FFU) << 1) | (bit31 ? 1U : 0U);
}

uint32_t encodeBC(uint8_t bo, uint8_t bi, int32_t displacement) {
    const uint32_t bd = static_cast<uint32_t>(displacement) & 0xFFFCU;
    return (16U << 26) | (static_cast<uint32_t>(bo & 0x1F) << 21) |
           (static_cast<uint32_t>(bi & 0x1F) << 16) | bd;
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t imm) {
    return encodeD(OPCODE_ADDI, rt, ra, imm);
}

uint32_t encodeLwarx(uint8_t rt, uint8_t ra, uint8_t rb, bool eh = false) {
    return encodeX(rt, ra, rb, XO_LWARX, eh);
}

uint32_t encodeLdarx(uint8_t rt, uint8_t ra, uint8_t rb, bool eh = false) {
    return encodeX(rt, ra, rb, XO_LDARX, eh);
}

uint32_t encodeStwcx(uint8_t rs, uint8_t ra, uint8_t rb) {
    return encodeX(rs, ra, rb, XO_STWCX, true);
}

uint32_t encodeStdcx(uint8_t rs, uint8_t ra, uint8_t rb) {
    return encodeX(rs, ra, rb, XO_STDCX, true);
}

uint32_t encodeCmpwi(uint8_t ra, int16_t imm) {
    return encodeD(11, 0, ra, imm);
}

PPC64ISAState& state(PPC64ISAPlugin& cpu) {
    return static_cast<PPC64ISAState&>(cpu.getState());
}

void putAt(IMemory& mem, uint64_t address, uint32_t instruction) {
    storeBE(mem, address, instruction);
}

void stepOK(PPC64ISAPlugin& cpu, IMemory& mem) {
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
}

void testDecodeAndEncodings() {
    std::printf("[decode-and-encodings]\n");
    CHECK(encodeLwarx(3, 4, 5) == 0x7C642828U);
    CHECK(encodeLdarx(3, 4, 5) == 0x7C6428A8U);
    CHECK(encodeStwcx(3, 4, 5) == 0x7C64292DU);
    CHECK(encodeStdcx(3, 4, 5) == 0x7C6429ADU);
    CHECK(ENCODING_SYNC == 0x7C0004ACU);
    CHECK(ENCODING_LWSYNC == 0x7C2004ACU);
    CHECK(ENCODING_ISYNC == 0x4C00012CU);
    CHECK(ENCODING_EIEIO == 0x7C0006ACU);

    DecodedInstruction d = PPC64ISAPlugin::decodeWord(encodeLwarx(3, 4, 5));
    CHECK(d.kind == DecodedKind::LWARX && d.supported);
    d = PPC64ISAPlugin::decodeWord(encodeLwarx(3, 4, 5, true));
    CHECK(d.kind == DecodedKind::LWARX && d.supported);
    d = PPC64ISAPlugin::decodeWord(encodeLdarx(3, 4, 5));
    CHECK(d.kind == DecodedKind::LDARX && d.supported);
    d = PPC64ISAPlugin::decodeWord(encodeStwcx(3, 4, 5));
    CHECK(d.kind == DecodedKind::STWCX && d.supported);
    d = PPC64ISAPlugin::decodeWord(encodeStdcx(3, 4, 5));
    CHECK(d.kind == DecodedKind::STDCX && d.supported);
    d = PPC64ISAPlugin::decodeWord(encodeX(3, 4, 5, XO_STWCX, false));
    CHECK(d.kind == DecodedKind::STWCX && !d.supported);

    CHECK(PPC64ISAPlugin::decodeWord(ENCODING_SYNC).kind == DecodedKind::SYNC);
    CHECK(PPC64ISAPlugin::decodeWord(ENCODING_LWSYNC).kind == DecodedKind::SYNC);
    CHECK(PPC64ISAPlugin::decodeWord(ENCODING_ISYNC).kind == DecodedKind::ISYNC);
    CHECK(PPC64ISAPlugin::decodeWord(ENCODING_EIEIO).kind == DecodedKind::EIEIO);
    d = PPC64ISAPlugin::decodeWord(0x7C4004ACU);  // ptesync, outside the subset
    CHECK(!d.supported && d.kind == DecodedKind::UNSUPPORTED);
    d = PPC64ISAPlugin::decodeWord(ENCODING_SYNC | 1U);  // reserved Rc pattern
    CHECK(!d.supported && d.kind == DecodedKind::UNSUPPORTED);
    d = PPC64ISAPlugin::decodeWord(ENCODING_ISYNC | (1U << 11));
    CHECK(!d.supported && d.kind == DecodedKind::UNSUPPORTED);
    d = PPC64ISAPlugin::decodeWord(encodeX(3, 4, 5, XO_LBARX));
    CHECK(!d.supported && d.kind == DecodedKind::UNSUPPORTED);
}

void testLoadsReservationsAndAlignment() {
    std::printf("[loads-reservations-alignment]\n");
    Memory mem(0x10000);
    storeValueBE(mem, 0x400, 0x89ABCDEFU, 4);
    storeValueBE(mem, 0x408, 0x123456789ABCDEF0ULL, 8);
    storeWords(mem, kProgram,
               {encodeLwarx(5, 0, 3), encodeLdarx(6, 0, 4), ENCODING_NOP});

    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setPC(kProgram);
    cpu.writeGPR(0, 0xDEADBEEFCAFEBABEULL);
    cpu.writeGPR(3, 0x400);
    cpu.writeGPR(4, 0x408);
    cpu.writeGPR(5, 0xAAAAAAAAAAAAAAAAULL);
    stepOK(cpu, mem);
    CHECK(cpu.readGPR(5) == 0x89ABCDEFU);  // lwarx zero extends
    CHECK(cpu.readGPR(0) == 0xDEADBEEFCAFEBABEULL);
    CHECK(cpu.hasReservation());
    CHECK(cpu.getReservationAddress() == 0x400);
    CHECK(cpu.getReservationWidth() == 4);
    stepOK(cpu, mem);
    CHECK(cpu.readGPR(6) == 0x123456789ABCDEF0ULL);
    CHECK(cpu.hasReservation());
    CHECK(cpu.getReservationAddress() == 0x408);  // second larx replaces first
    CHECK(cpu.getReservationWidth() == 8);
    CHECK(cpu.getNIP() == kProgram + 8);

    // RA=0 contributes zero; the RB register remains an ordinary GPR source.
    PPC64ISAPlugin zeroBase;
    zeroBase.reset();
    zeroBase.setPC(kProgram + 0x10);
    putAt(mem, kProgram + 0x10, encodeLwarx(7, 0, 3));
    zeroBase.writeGPR(0, 0x408);
    zeroBase.writeGPR(3, 0x400);
    zeroBase.writeGPR(7, 0xAAAAAAAAAAAAAAAAULL);
    stepOK(zeroBase, mem);
    CHECK(zeroBase.readGPR(7) == 0x89ABCDEFU);
    CHECK(zeroBase.getReservationAddress() == 0x400);

    PPC64ISAPlugin misaligned;
    misaligned.reset();
    misaligned.setPC(kProgram + 0x20);
    putAt(mem, kProgram + 0x20, encodeLwarx(8, 0, 3));
    misaligned.writeGPR(3, 0x401);
    misaligned.writeGPR(8, 0xA5A5A5A5A5A5A5A5ULL);
    CHECK(misaligned.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(misaligned.getLastFault().category == PPC64FaultCategory::ALIGNMENT);
    CHECK(misaligned.readGPR(8) == 0xA5A5A5A5A5A5A5A5ULL);
    CHECK(!misaligned.hasReservation());
    CHECK(misaligned.getNIP() == kProgram + 0x20);

    // A misaligned conditional store faults before it can report CR0 failure.
    PPC64ISAPlugin misalignedSC;
    misalignedSC.reset();
    misalignedSC.setPC(kProgram + 0x28);
    storeValueBE(mem, 0x420, 0x55667788U, 4);
    storeWords(mem, kProgram + 0x28,
               {encodeLwarx(10, 0, 3), encodeStwcx(4, 0, 3)});
    misalignedSC.writeGPR(3, 0x420);
    misalignedSC.writeGPR(4, 0x99);
    state(misalignedSC).cr = PPC64ISAPlugin::crSetField(0x12345678U, 0, 0xDU);
    stepOK(misalignedSC, mem);
    misalignedSC.writeGPR(3, 0x421);
    CHECK(misalignedSC.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(misalignedSC.getLastFault().category == PPC64FaultCategory::ALIGNMENT);
    CHECK(misalignedSC.getNIP() == kProgram + 0x2C);
    CHECK(PPC64ISAPlugin::crGetField(misalignedSC.getCR(), 0) == 0xDU);
    CHECK(!misalignedSC.hasReservation());
    CHECK(readValueBE(mem, 0x420, 4) == 0x55667788U);

    PPC64ISAPlugin badAddress;
    badAddress.reset();
    badAddress.setPC(kProgram + 0x24);
    putAt(mem, kProgram + 0x24, encodeLdarx(9, 0, 3));
    badAddress.writeGPR(3, 0xFFFF000000000000ULL);
    badAddress.writeGPR(9, 0xFEEDFACEFEEDFACEULL);
    CHECK(badAddress.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(badAddress.readGPR(9) == 0xFEEDFACEFEEDFACEULL);
    CHECK(!badAddress.hasReservation());
    CHECK(badAddress.getLastFault().category == PPC64FaultCategory::DATA_LOAD_FAILURE);
}

void testConditionalStoreFlagsAndPreservation() {
    std::printf("[conditional-store-flags]\n");
    Memory mem(0x10000);
    storeValueBE(mem, kCounter, kInitial32, 4);
    storeWords(mem, kProgram, {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});

    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setPC(kProgram);
    cpu.writeGPR(3, kCounter);
    cpu.writeGPR(4, 0xA1B2C3D4U);
    cpu.writeGPR(5, 0xCCCCCCCCCCCCCCCCULL);
    cpu.writeGPR(8, 0x8888888888888888ULL);
    state(cpu).lr = 0x123456789ABCDEF0ULL;
    state(cpu).ctr = 0x0FEDCBA987654321ULL;
    state(cpu).cr = PPC64ISAPlugin::crSetField(0x12345678U, 0, 0xDU);
    state(cpu).xer = XER_SO | XER_CA | XER_OV;
    const uint32_t unrelatedCR = state(cpu).cr & 0x0FFFFFFFU;
    const uint32_t originalXER = state(cpu).xer;
    const uint64_t originalLR = cpu.getLR();
    const uint64_t originalCTR = cpu.getCTR();

    stepOK(cpu, mem);
    CHECK(cpu.readGPR(5) == kInitial32);
    CHECK(cpu.hasReservation());
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 0xA1B2C3D4U);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0x3U);  // EQ + SO
    CHECK((cpu.getCR() & 0x0FFFFFFFU) == unrelatedCR);
    CHECK(cpu.getXER() == originalXER);
    CHECK(cpu.getLR() == originalLR && cpu.getCTR() == originalCTR);
    CHECK(cpu.readGPR(3) == kCounter && cpu.readGPR(4) == 0xA1B2C3D4U);
    CHECK(cpu.readGPR(5) == kInitial32 && cpu.readGPR(8) == 0x8888888888888888ULL);
    CHECK(!cpu.hasReservation());

    // A store-conditional without a reservation is a normal failed result.
    putAt(mem, kProgram + 8, encodeStwcx(4, 0, 3));
    state(cpu).cr = PPC64ISAPlugin::crSetField(cpu.getCR(), 0, 0xDU);
    cpu.setPC(kProgram + 8);
    // setPC intentionally discards any reservation; this tests the no-reservation path.
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(readValueBE(mem, kCounter, 4) == 0xA1B2C3D4U);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0x1U);  // SO only
    CHECK((cpu.getCR() & 0x0FFFFFFFU) == unrelatedCR);
    CHECK(cpu.getXER() == originalXER);
    CHECK(cpu.getNIP() == kProgram + 12);
}

void testProgramA32BitIncrement() {
    std::printf("[program-a-32-bit-increment]\n");
    constexpr int kIterations = 5;
    Memory mem(0x10000);
    storeValueBE(mem, kCounter, kInitial32, 4);
    storeWords(mem, kProgram,
               {encodeAddi(3, 0, static_cast<int16_t>(kCounter)),
                encodeAddi(4, 0, kIterations),
                encodeLwarx(5, 0, 3),
                encodeAddi(5, 5, 1),
                encodeStwcx(5, 0, 3),
                encodeBC(4, 2, -12),             // retry failed stwcx.
                encodeAddi(4, 4, -1),
                encodeCmpwi(4, 0),
                encodeBC(4, 2, -24),             // next increment
                ENCODING_NOP});
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setPC(kProgram);
    size_t steps = 0;
    while (cpu.getNIP() != kProgram + 0x28 && steps < 100) {
        stepOK(cpu, mem);
        ++steps;
    }
    CHECK(cpu.getNIP() == kProgram + 0x28);
    CHECK(steps == static_cast<size_t>(3 + 7 * kIterations));
    CHECK(readValueBE(mem, kCounter, 4) == kInitial32 + kIterations);
    CHECK(cpu.readGPR(3) == kCounter);
    CHECK(cpu.readGPR(4) == 0);
    CHECK(cpu.readGPR(5) == kInitial32 + kIterations);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0x2U);
    CHECK(!cpu.hasReservation());
}

void testProgramB64BitIncrement() {
    std::printf("[program-b-64-bit-increment]\n");
    constexpr int kIterations = 4;
    constexpr uint64_t target = 0x408;
    Memory mem(0x10000);
    storeValueBE(mem, target, kInitial64, 8);
    storeWords(mem, kProgram,
               {encodeAddi(3, 0, static_cast<int16_t>(target)),
                encodeAddi(4, 0, kIterations),
                encodeLdarx(5, 0, 3),
                encodeAddi(5, 5, 1),
                encodeStdcx(5, 0, 3),
                encodeBC(4, 2, -12),
                encodeAddi(4, 4, -1),
                encodeCmpwi(4, 0),
                encodeBC(4, 2, -24),
                ENCODING_NOP});
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setPC(kProgram);
    size_t steps = 0;
    while (cpu.getNIP() != kProgram + 0x28 && steps < 100) {
        stepOK(cpu, mem);
        ++steps;
    }
    CHECK(cpu.getNIP() == kProgram + 0x28);
    CHECK(steps == static_cast<size_t>(3 + 7 * kIterations));
    CHECK(readValueBE(mem, target, 8) == kInitial64 + kIterations);
    CHECK(cpu.readGPR(5) == kInitial64 + kIterations);
    CHECK((readValueBE(mem, target, 8) >> 32) != 0);  // no 32-bit truncation
    CHECK(cpu.readGPR(4) == 0);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0x2U);
}

void testProgramCInterferenceAndVMIsolation() {
    std::printf("[program-c-interference-and-vm-isolation]\n");
    Memory shared(0x10000);
    storeValueBE(shared, kCounter, 5, 4);
    storeWords(shared, kProgram,
               {encodeLwarx(5, 0, 3), encodeAddi(5, 5, 1), encodeStwcx(5, 0, 3),
                encodeBC(4, 2, -12), ENCODING_NOP});
    storeWords(shared, 0x2000,
               {encodeD(OPCODE_STW, 4, 3, 0), ENCODING_NOP});

    PPC64ISAPlugin a, b;
    a.reset();
    b.reset();
    a.setPC(kProgram);
    b.setPC(0x2000);
    a.writeGPR(3, kCounter);
    b.writeGPR(3, kCounter);
    b.writeGPR(4, 20);

    stepOK(a, shared);                       // A reserves old value 5
    CHECK(a.hasReservation());
    stepOK(b, shared);                       // B ordinary stw invalidates it
    CHECK(!a.hasReservation());
    stepOK(a, shared);                       // prepare stale value + 1
    stepOK(a, shared);                       // stwcx. fails
    CHECK(PPC64ISAPlugin::crGetField(a.getCR(), 0) == 0);
    CHECK(readValueBE(shared, kCounter, 4) == 20);
    stepOK(a, shared);                       // bne takes retry path
    CHECK(a.getNIP() == kProgram);
    stepOK(a, shared);                       // reload 20, then retry succeeds
    stepOK(a, shared);
    stepOK(a, shared);
    CHECK(PPC64ISAPlugin::crGetField(a.getCR(), 0) == 2);
    stepOK(a, shared);                       // bne falls through
    CHECK(a.getNIP() == kProgram + 0x10);
    stepOK(a, shared);
    CHECK(readValueBE(shared, kCounter, 4) == 21);
    CHECK(a.readGPR(5) == 21);
    CHECK(a.getNIP() == kProgram + 0x14);

    // Equal addresses in separate Memory backends belong to separate VMs.
    Memory vmA(0x10000), vmB(0x10000);
    storeValueBE(vmA, kCounter, 7, 4);
    storeValueBE(vmB, kCounter, 7, 4);
    storeWords(vmA, 0x3000, {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
    storeWords(vmB, 0x3000, {encodeD(OPCODE_STW, 4, 3, 0)});
    PPC64ISAPlugin vmCpuA, vmCpuB;
    vmCpuA.reset(); vmCpuB.reset();
    vmCpuA.setPC(0x3000); vmCpuB.setPC(0x3000);
    vmCpuA.writeGPR(3, kCounter); vmCpuA.writeGPR(4, 8);
    vmCpuB.writeGPR(3, kCounter); vmCpuB.writeGPR(4, 99);
    stepOK(vmCpuA, vmA);
    stepOK(vmCpuB, vmB);
    CHECK(vmCpuA.hasReservation());
    stepOK(vmCpuA, vmA);
    CHECK(readValueBE(vmA, kCounter, 4) == 8);
    CHECK(readValueBE(vmB, kCounter, 4) == 99);
}

void testProgramDReservationBoundariesAndLifetime() {
    std::printf("[program-d-reservation-boundaries]\n");

    // Ordinary loads do not explicitly discard a reservation in this CPU model.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeValueBE(mem, 0x500, 2, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeD(OPCODE_LWZ, 6, 7, 0),
                    encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 3); cpu.writeGPR(7, 0x500);
        stepOK(cpu, mem);
        stepOK(cpu, mem);
        CHECK(cpu.hasReservation());
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 3);
    }

    // A write anywhere in the chosen 128-byte granule loses the reservation.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 3);
        stepOK(cpu, mem);
        const uint8_t changed = 0xAA;
        mem.Write(0x47F, &changed, 1);  // last byte of [0x400,0x480)
        CHECK(!cpu.hasReservation());
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 1);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }

    // A write in the next granule does not needlessly lose the reservation.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 3);
        stepOK(cpu, mem);
        const uint8_t changed = 0xAA;
        mem.Write(0x480, &changed, 1);
        CHECK(cpu.hasReservation());
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 3);
    }

    // An ordinary store by the reserving CPU invalidates its own reservation.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeAddi(4, 0, 7),
                    encodeD(OPCODE_STW, 4, 3, 0), encodeAddi(4, 0, 9),
                    encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400);
        stepOK(cpu, mem); stepOK(cpu, mem); stepOK(cpu, mem);
        CHECK(!cpu.hasReservation());
        CHECK(readValueBE(mem, 0x400, 4) == 7);
        stepOK(cpu, mem); stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 7);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }

    // A second load-reserve replaces the first address; a failed stwcx. is one-shot.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4); storeValueBE(mem, 0x408, 2, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeAddi(3, 0, 0x408),
                    encodeLwarx(6, 0, 3), encodeAddi(3, 0, 0x400),
                    encodeStwcx(4, 0, 3), encodeAddi(3, 0, 0x408),
                    encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem); stepOK(cpu, mem); stepOK(cpu, mem); stepOK(cpu, mem);
        CHECK(cpu.getReservationAddress() == 0x408);
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 1);
        CHECK(!cpu.hasReservation());
        stepOK(cpu, mem);
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x408, 4) == 2);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }

    // A word reservation does not authorize a doubleword conditional store.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 0x1122334455667788ULL, 8);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStdcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 0xAABBCCDDEEFF0011ULL);
        stepOK(cpu, mem);
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 8) == 0x1122334455667788ULL);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }

    // Memory::write<T>, loadBuffer, clear, MMU mapping changes, and snapshots
    // are mutation paths that must invalidate or conservatively drop a record.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem);
        mem.write<uint32_t>(0x400, 0x88776655U);
        CHECK(!cpu.hasReservation());
        stepOK(cpu, mem);
        const uint32_t expectedTyped = 0x88776655U;
        CHECK(std::memcmp(mem.GetRawData() + 0x400, &expectedTyped,
                          sizeof(expectedTyped)) == 0);
    }
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem);
        mem.Clear();
        storeBE(mem, kProgram + 4, encodeStwcx(4, 0, 3));
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 0);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem);
        const uint8_t replacement[4] = {0, 0, 0, 2};
        mem.loadBuffer(0x400, replacement, sizeof(replacement));
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 2);
    }
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem);
        MemorySnapshot snapshot = mem.CreateSnapshot();
        mem.GetMMU().MapPage(0x400, 0x400, PermissionFlags::READ_WRITE_EXECUTE);
        CHECK(cpu.hasReservation());  // mapping generation checked at next atomic op
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 1);
        (void)snapshot;
    }
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 1, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        stepOK(cpu, mem);
        MemorySnapshot snapshot = mem.CreateSnapshot();
        const uint8_t outside = 0xAA;
        mem.Write(0x480, &outside, 1);
        mem.RestoreSnapshot(snapshot);
        CHECK(!cpu.hasReservation());
        stepOK(cpu, mem);
        CHECK(readValueBE(mem, 0x400, 4) == 1);
    }
}

class UntrackedMemory final : public IMemory {
public:
    UntrackedMemory() : backing_(0x10000) {}
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        backing_.Read(address, dest, size);
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        backing_.Write(address, src, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* src, size_t size) override {
        backing_.loadBuffer(address, src, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& bytes) override {
        backing_.loadBuffer(address, bytes);
    }
    size_t GetTotalSize() const override { return backing_.GetTotalSize(); }
    void Clear() override { backing_.Clear(); }
    const uint8_t* GetRawData() const override { return backing_.GetRawData(); }
private:
    Memory backing_;
};

class TestDevice final : public IMemoryMappedDevice {
public:
    uint64_t GetBaseAddress() const override { return 0x602; }
    size_t GetSize() const override { return 16; }
    bool Read(uint64_t, uint8_t*, size_t) const override { return false; }
    bool Write(uint64_t, const uint8_t*, size_t) override { return false; }
};

void testUnsupportedMemoryAndDevices() {
    std::printf("[unsupported-memory-and-device-ranges]\n");
    {
        Memory mem(0x10000);
        storeBE(mem, kProgram, 0x7C4004ACU);  // valid ptesync, unsupported here
        PPC64ISAPlugin syncCpu; syncCpu.reset();
        syncCpu.setGuestDeliveryEnabled(true); syncCpu.setPC(kProgram);
        CHECK(syncCpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(syncCpu.getNIP() == kProgram);
        CHECK(syncCpu.getLastFault().category == PPC64FaultCategory::INVALID_INSTRUCTION);
    }

    UntrackedMemory untracked;
    storeBE(untracked, kProgram, encodeLwarx(5, 0, 3));
    PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram); cpu.writeGPR(3, 0x400);
    CHECK(cpu.step(untracked) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(!cpu.hasReservation());

    Memory mem(0x10000);
    TestDevice device;
    mem.RegisterDevice(&device);
    storeBE(mem, kProgram, encodeLwarx(5, 0, 3));
    PPC64ISAPlugin deviceCpu; deviceCpu.reset(); deviceCpu.setPC(kProgram);
    deviceCpu.writeGPR(3, 0x600);  // aligned 4-byte access overlaps at 0x602
    CHECK(deviceCpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(deviceCpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(!deviceCpu.hasReservation());
}

void testProgramEExceptionsAndRecovery() {
    std::printf("[program-e-exceptions-and-recovery]\n");
    // Legacy mode retains Phase 4's host exception result and precise NIP.
    {
        Memory mem(0x10000);
        storeBE(mem, kProgram, encodeLwarx(5, 0, 3));
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
        cpu.writeGPR(3, 0x401);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getNIP() == kProgram);
        CHECK(cpu.getLastFault().category == PPC64FaultCategory::ALIGNMENT);
        CHECK(!cpu.hasReservation());
    }

    // Guest Alignment delivery can repair the EA and retry the exact instruction.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 0xABCDEF01U, 4);
        storeWords(mem, kProgram, {encodeLwarx(6, 0, 3), encodeStwcx(4, 0, 3)});
        storeBE(mem, VEC_ALIGN, ENCODING_RFID);
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setGuestDeliveryEnabled(true);
        cpu.setPC(kProgram); cpu.writeGPR(3, 0x401);
        cpu.writeGPR(6, 0xDEADBEEFDEADBEEFULL); cpu.writeGPR(4, 7);
        stepOK(cpu, mem);
        CHECK(cpu.getNIP() == VEC_ALIGN);
        CHECK(cpu.getSRR0() == kProgram);
        CHECK(cpu.getDAR() == 0x401 && cpu.getDSISR() == 0);
        CHECK(cpu.readGPR(6) == 0xDEADBEEFDEADBEEFULL);
        CHECK(!cpu.hasReservation());
        cpu.writeGPR(3, 0x400);  // the handler's repair to the guest context
        stepOK(cpu, mem);         // rfid retries lwarx
        CHECK(cpu.getNIP() == kProgram);
        stepOK(cpu, mem);
        CHECK(cpu.hasReservation() && cpu.getReservationAddress() == 0x400);
        CHECK(cpu.readGPR(6) == 0xABCDEF01U);
    }

    // An unrelated DSI entered after lwarx must also discard the reservation.
    {
        Memory mem(0x10000);
        storeValueBE(mem, 0x400, 5, 4);
        storeValueBE(mem, 0x4000, 0xCAFEBABE, 4);
        storeWords(mem, kProgram,
                   {encodeLwarx(6, 0, 3), encodeD(OPCODE_STW, 4, 5, 0),
                    encodeStwcx(4, 0, 3)});
        storeBE(mem, VEC_DSI, ENCODING_RFID);
        mem.GetMMU().SetPagePermissions(0x4000, PermissionFlags::READ);
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setGuestDeliveryEnabled(true);
        cpu.setPC(kProgram); cpu.writeGPR(3, 0x400); cpu.writeGPR(4, 9);
        cpu.writeGPR(5, 0x4000);
        stepOK(cpu, mem);
        CHECK(cpu.hasReservation());
        stepOK(cpu, mem);  // protected ordinary store takes DSI
        CHECK(cpu.getNIP() == VEC_DSI);
        CHECK(cpu.getSRR0() == kProgram + 4);
        CHECK(cpu.getDAR() == 0x4000);
        CHECK(cpu.getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
        CHECK(readValueBE(mem, 0x4000, 4) == 0xCAFEBABE);
        CHECK(!cpu.hasReservation());
        cpu.setSRR0(kProgram + 8);  // exception handler skips the protected store
        stepOK(cpu, mem);            // rfid
        CHECK(cpu.getNIP() == kProgram + 8);
        stepOK(cpu, mem);            // stale stwcx. fails normally
        CHECK(readValueBE(mem, 0x400, 4) == 5);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);
    }

    // A protection fault on stwcx. is a DSI, preserves memory/CR, and can be
    // recovered by repairing permissions and restarting at lwarx.
    {
        constexpr uint64_t protectedWord = 0x5000;
        Memory mem(0x10000);
        storeValueBE(mem, protectedWord, 0x01020304U, 4);
        storeWords(mem, 0x2000,
                   {encodeLwarx(6, 0, 3), encodeStwcx(4, 0, 3)});
        storeBE(mem, VEC_DSI, ENCODING_RFID);
        mem.GetMMU().SetPagePermissions(protectedWord, PermissionFlags::READ);
        PPC64ISAPlugin cpu; cpu.reset(); cpu.setGuestDeliveryEnabled(true);
        cpu.setPC(0x2000); cpu.writeGPR(3, protectedWord); cpu.writeGPR(4, 0xAABBCCDD);
        state(cpu).cr = PPC64ISAPlugin::crSetField(0x12345678U, 0, 0xDU);
        const uint32_t oldOtherCR = cpu.getCR() & 0x0FFFFFFFU;
        stepOK(cpu, mem);
        CHECK(cpu.hasReservation());
        stepOK(cpu, mem);  // write permission fault, never a normal CR0 failure
        CHECK(cpu.getNIP() == VEC_DSI);
        CHECK(cpu.getSRR0() == 0x2004 && cpu.getDAR() == protectedWord);
        CHECK(cpu.getDSISR() == (DSISR_ISSTORE | DSISR_PROTFAULT));
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0xDU);
        CHECK((cpu.getCR() & 0x0FFFFFFFU) == oldOtherCR);
        CHECK(readValueBE(mem, protectedWord, 4) == 0x01020304U);
        CHECK(!cpu.hasReservation());
        mem.GetMMU().SetPagePermissions(protectedWord,
                                        PermissionFlags::READ_WRITE_EXECUTE);
        cpu.setSRR0(0x2000);  // guest handler retries with a fresh reservation
        stepOK(cpu, mem);     // rfid
        stepOK(cpu, mem);     // lwarx
        stepOK(cpu, mem);     // stwcx. succeeds
        CHECK(readValueBE(mem, protectedWord, 4) == 0xAABBCCDDU);
        CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 2);
        CHECK(!cpu.hasReservation());
    }

}

void testSerializationAndStateReplacement() {
    std::printf("[serialization-and-state-replacement]\n");
    Memory mem(0x10000);
    storeValueBE(mem, kCounter, 4, 4);
    storeWords(mem, kProgram, {encodeLwarx(5, 0, 3), encodeStwcx(4, 0, 3)});
    PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
    cpu.writeGPR(3, kCounter); cpu.writeGPR(4, 8);
    stepOK(cpu, mem);
    CHECK(cpu.hasReservation());
    const std::vector<uint8_t> snapshot = cpu.serialize_state();
    CHECK(snapshot.size() == STATE_SIZE);
    CHECK(snapshot.size() == 304);
    CHECK(cpu.deserialize_state(snapshot));
    CHECK(!cpu.hasReservation());
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 4);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 0);

    cpu.setPC(kProgram);
    stepOK(cpu, mem);
    CHECK(cpu.hasReservation());
    PPC64ISAState replacement = static_cast<const PPC64ISAState&>(cpu.getState());
    cpu.setState(replacement);
    CHECK(!cpu.hasReservation());
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 4);

    cpu.setPC(kProgram);
    stepOK(cpu, mem);
    cpu.setSRR0(0x1234);  // nonzero Book III-S state selects v2 serialization
    const std::vector<uint8_t> snapshotV2 = cpu.serialize_state();
    CHECK(snapshotV2.size() == STATE_SIZE_V2);
    const std::vector<uint8_t> forcedV2 = cpu.serialize_state_v2();
    CHECK(forcedV2.size() == STATE_SIZE_V2);
    CHECK(cpu.deserialize_state_v2(forcedV2));
    CHECK(!cpu.hasReservation());
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 4);

    cpu.setPC(kProgram);
    stepOK(cpu, mem);
    cpu.reset();
    CHECK(!cpu.hasReservation());
    cpu.writeGPR(3, kCounter); cpu.writeGPR(4, 8);
    cpu.setPC(kProgram + 4);
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 4);

    // A full memory checkpoint restore invalidates all outstanding records.
    cpu.setPC(kProgram);
    stepOK(cpu, mem);
    CHECK(cpu.hasReservation());
    MemorySnapshot memorySnapshot = mem.CreateSnapshot();
    mem.RestoreSnapshot(memorySnapshot);
    CHECK(!cpu.hasReservation());
    stepOK(cpu, mem);
    CHECK(readValueBE(mem, kCounter, 4) == 4);
}

void testSynchronizationRegisterPreservation() {
    std::printf("[synchronization-register-preservation]\n");
    Memory mem(0x10000);
    storeWords(mem, kProgram,
               {ENCODING_SYNC, ENCODING_LWSYNC, ENCODING_ISYNC, ENCODING_EIEIO});
    PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
    std::array<uint64_t, NUM_GPR> before{};
    for (size_t i = 0; i < NUM_GPR; ++i) {
        before[i] = (0x0123456789ABCDEFULL ^ (static_cast<uint64_t>(i) << 40));
        cpu.writeGPR(i, before[i]);
    }
    state(cpu).cr = 0x89ABCDEFU;
    state(cpu).xer = XER_SO | XER_OV | XER_CA;
    state(cpu).lr = 0xFEDCBA9876543210ULL;
    state(cpu).ctr = 0x1020304050607080ULL;
    const uint32_t beforeCR = cpu.getCR();
    const uint32_t beforeXER = cpu.getXER();
    const uint64_t beforeLR = cpu.getLR();
    const uint64_t beforeCTR = cpu.getCTR();
    for (unsigned i = 0; i < 4; ++i) {
        stepOK(cpu, mem);
        for (size_t r = 0; r < NUM_GPR; ++r) {
            CHECK(cpu.readGPR(r) == before[r]);
        }
        CHECK(cpu.getCR() == beforeCR);
        CHECK(cpu.getXER() == beforeXER);
        CHECK(cpu.getLR() == beforeLR);
        CHECK(cpu.getCTR() == beforeCTR);
        CHECK(cpu.getNIP() == kProgram + (i + 1) * 4);
    }
}

void testProgramFSynchronizationProtocol() {
    std::printf("[program-f-synchronization-protocol]\n");
    Memory mem(0x10000);
    storeValueBE(mem, kCounter, 0, 4);  // unlocked word
    storeWords(mem, kProgram,
               {ENCODING_SYNC,
                encodeLwarx(4, 0, 3),
                encodeCmpwi(4, 0),
                encodeBC(4, 2, -8),       // bne retry
                encodeAddi(4, 0, 1),
                encodeStwcx(4, 0, 3),
                encodeBC(4, 2, -20),      // retry failed stwcx.
                ENCODING_ISYNC,
                ENCODING_LWSYNC,
                encodeD(OPCODE_STW, 0, 3, 0),
                ENCODING_EIEIO,
                ENCODING_NOP});
    PPC64ISAPlugin cpu; cpu.reset(); cpu.setPC(kProgram);
    cpu.writeGPR(3, kCounter);
    state(cpu).cr = 0x89ABCDEFU;
    state(cpu).xer = XER_CA;
    state(cpu).lr = 0x123456789ABCDEF0ULL;
    state(cpu).ctr = 0x1020304050607080ULL;
    const uint32_t initialCR = cpu.getCR();
    const uint32_t initialXER = cpu.getXER();
    const uint64_t initialLR = cpu.getLR();
    const uint64_t initialCTR = cpu.getCTR();

    size_t steps = 0;
    while (cpu.getNIP() != kProgram + 0x30 && steps < 40) {
        stepOK(cpu, mem);
        ++steps;
    }
    CHECK(cpu.getNIP() == kProgram + 0x30);
    CHECK(steps == 12);
    CHECK(readValueBE(mem, kCounter, 4) == 0);  // acquire/store/release protocol
    CHECK(cpu.readGPR(4) == 1);
    CHECK(PPC64ISAPlugin::crGetField(cpu.getCR(), 0) == 2);
    CHECK((cpu.getCR() & 0x0FFFFFFFU) == (initialCR & 0x0FFFFFFFU));
    CHECK(cpu.getXER() == initialXER);
    CHECK(cpu.getLR() == initialLR && cpu.getCTR() == initialCTR);
    CHECK(!cpu.hasReservation());
}

}  // namespace

int main() {
    testDecodeAndEncodings();
    testLoadsReservationsAndAlignment();
    testConditionalStoreFlagsAndPreservation();
    testProgramA32BitIncrement();
    testProgramB64BitIncrement();
    testProgramCInterferenceAndVMIsolation();
    testProgramDReservationBoundariesAndLifetime();
    testUnsupportedMemoryAndDevices();
    testProgramEExceptionsAndRecovery();
    testSerializationAndStateReplacement();
    testSynchronizationRegisterPreservation();
    testProgramFSynchronizationProtocol();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
