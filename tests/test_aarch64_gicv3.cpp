#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64GICv3.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64MMU.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64PhysicalBus.h"
#include "memory.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aarch64;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, unsigned line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "AARCH64-12 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-12 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

bool observePrivateBytes(uint64_t& privateBytes) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters))) {
        privateBytes = static_cast<uint64_t>(counters.PrivateUsage);
        return true;
    }
#else
    (void)privateBytes;
#endif
    return false;
}

constexpr uint64_t kGuestStart = 0x1000;
constexpr uint64_t kEl0Start = 0x2000;
constexpr uint64_t kVectorBase = 0x4000;
constexpr uint64_t kUartBase = 0x09000000ULL;
constexpr uint64_t kGicdBase = 0x08000000ULL;
constexpr uint64_t kGicrBase = 0x080A0000ULL;
constexpr uint64_t kGicdVa = 0x21000000ULL;
constexpr uint64_t kGicrVa = 0x210A0000ULL;
constexpr uint64_t kUartVa = 0x20000000ULL;
constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kEret = 0xD69F03E0u;

uint16_t syskey(uint8_t op0, uint8_t op1, uint8_t crn,
                uint8_t crm, uint8_t op2) {
    return static_cast<uint16_t>((op0 << 14) | (op1 << 11) |
                                 (crn << 7) | (crm << 3) | op2);
}

uint32_t movzX(unsigned rd, uint16_t imm, unsigned half = 0) {
    return 0xD2800000u | ((half & 3u) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t movkX(unsigned rd, uint16_t imm, unsigned half) {
    return 0xF2800000u | ((half & 3u) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(movzX(rd, static_cast<uint16_t>(value), 0));
    for (unsigned half = 1; half < 4; ++half) {
        const uint16_t part = static_cast<uint16_t>(value >> (half * 16));
        if (part != 0) words.push_back(movkX(rd, part, half));
    }
}

uint32_t strW(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xB9000000u | ((offset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t ldrW(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xB9400000u | ((offset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t strB(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0x39000000u | ((offset & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t strX(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xF9000000u | ((offset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeSys(bool read, uint8_t op0, uint8_t op1, uint8_t crn,
                   uint8_t crm, uint8_t op2, uint8_t rt) {
    return (read ? 0xD5200000u : 0xD5000000u) |
           (static_cast<uint32_t>(op0) << 19) |
           (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

uint32_t mrs(uint16_t key, unsigned rt) {
    return encodeSys(true, static_cast<uint8_t>((key >> 14) & 3u),
                     static_cast<uint8_t>((key >> 11) & 7u),
                     static_cast<uint8_t>((key >> 7) & 15u),
                     static_cast<uint8_t>((key >> 3) & 15u),
                     static_cast<uint8_t>(key & 7u), static_cast<uint8_t>(rt));
}

uint32_t msr(uint16_t key, unsigned rt) {
    return encodeSys(false, static_cast<uint8_t>((key >> 14) & 3u),
                     static_cast<uint8_t>((key >> 11) & 7u),
                     static_cast<uint8_t>((key >> 7) & 15u),
                     static_cast<uint8_t>((key >> 3) & 15u),
                     static_cast<uint8_t>(key & 7u), static_cast<uint8_t>(rt));
}

uint32_t movzW(unsigned rd, uint16_t imm) {
    return 0x52800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t cmpWImm(unsigned rn, uint16_t imm) {
    return 0x7100001Fu | (static_cast<uint32_t>(imm) << 10) |
           ((rn & 31u) << 5);
}

uint32_t bCond(unsigned condition, std::size_t instructionIndex,
               std::size_t targetIndex) {
    const int64_t delta = static_cast<int64_t>(targetIndex) -
                          static_cast<int64_t>(instructionIndex);
    return 0x54000000u |
           ((static_cast<uint32_t>(delta) & 0x7FFFFu) << 5) |
           (condition & 15u);
}

uint32_t stpX(unsigned rt, unsigned rt2, unsigned rn, int offset) {
    const uint32_t scaled = static_cast<uint32_t>(offset / 8) & 0x7Fu;
    return 0xA9000000u | (scaled << 15) | ((rt2 & 31u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t ldpX(unsigned rt, unsigned rt2, unsigned rn, int offset) {
    const uint32_t scaled = static_cast<uint32_t>(offset / 8) & 0x7Fu;
    return 0xA9400000u | (scaled << 15) | ((rt2 & 31u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t adjustSp(bool subtract, uint16_t amount) {
    return (subtract ? 0xD1000000u : 0x91000000u) |
           (static_cast<uint32_t>(amount) << 10) | (31u << 5) | 31u;
}

void appendSysWrite(std::vector<uint32_t>& words, uint16_t key,
                    uint64_t value, unsigned valueReg = 11) {
    appendConstant(words, valueReg, value);
    words.push_back(msr(key, valueReg));
}

void appendMmioWrite32(std::vector<uint32_t>& words, uint64_t address,
                       uint32_t value) {
    appendConstant(words, 10, address);
    appendConstant(words, 11, value);
    words.push_back(strW(11, 10));
}

void appendMmioWrite64(std::vector<uint32_t>& words, uint64_t address,
                       uint64_t value) {
    appendConstant(words, 10, address);
    appendConstant(words, 11, value);
    words.push_back(strX(11, 10));
}

void appendMmioWrite8(std::vector<uint32_t>& words, uint64_t address,
                      uint8_t value) {
    appendConstant(words, 10, address);
    words.push_back(movzW(11, value));
    words.push_back(strB(11, 10));
}

void appendMmioRead32(std::vector<uint32_t>& words, uint64_t address,
                      unsigned resultReg) {
    appendConstant(words, 10, address);
    words.push_back(ldrW(resultReg, 10));
}

void writeWords(ia64::Memory& ram, uint64_t address,
                const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size() * 4);
    for (std::size_t n = 0; n < words.size(); ++n)
        for (unsigned b = 0; b < 4; ++b)
            bytes[n * 4 + b] = static_cast<uint8_t>(words[n] >> (8 * b));
    ram.loadBuffer(address, bytes);
}

void writeWord(ia64::Memory& ram, uint64_t address, uint32_t word) {
    writeWords(ram, address, std::vector<uint32_t>{word});
}

void appendGicSetup(std::vector<uint32_t>& words, uint64_t gicd,
                    uint64_t gicr, uint32_t ppiMask,
                    bool configureUart, uint32_t uartSpi = 33,
                    uint8_t ppiPriority = 0x20,
                    uint8_t spiPriority = 0x20) {
    const uint16_t daif = syskey(3, 3, 4, 2, 1);
    const uint16_t vbar = syskey(3, 0, 12, 0, 0);
    const uint16_t iccSre = gicv3_sysreg::kSreEl1;
    const uint16_t iccPmr = gicv3_sysreg::kPmrEl1;
    const uint16_t iccBpr1 = gicv3_sysreg::kBpr1El1;
    const uint16_t iccIgrpEn1 = gicv3_sysreg::kIgrpEn1El1;

    appendSysWrite(words, daif, kPstateI);
    appendSysWrite(words, vbar, kVectorBase);
    appendMmioWrite32(words, gicr + 0x14, 0); // wake the one PE immediately
    appendMmioWrite32(words, gicr + 0x10080, ppiMask); // Group 1 PPIs
    appendMmioWrite32(words, gicr + 0x10100, ppiMask);
    for (uint32_t id : {AArch64GICv3::kVirtualTimerPpi,
                        AArch64GICv3::kPhysicalTimerPpi}) {
        if ((ppiMask & (1u << id)) != 0)
            appendMmioWrite8(words, gicr + 0x10400 + id, ppiPriority);
    }
    if (configureUart) {
        const uint32_t spiBit = 1u << (uartSpi - 32u);
        appendMmioWrite32(words, gicd + 0x84, spiBit); // Group 1 SPI
        appendMmioWrite32(words, gicd + 0x104, spiBit);
        appendMmioWrite8(words, gicd + 0x400 + uartSpi, spiPriority);
        appendMmioWrite64(words, gicd + 0x6000 + 8u * uartSpi, 0); // Aff3:Aff0 = 0
    }
    appendMmioWrite32(words, gicd + 0x00, 0x12); // DS and ARE are RAO/WI; enable Group 1
    appendSysWrite(words, iccSre, 1);
    appendSysWrite(words, iccPmr, 0xFF);
    appendSysWrite(words, iccBpr1, 0);
    appendSysWrite(words, iccIgrpEn1, 1);
}

std::vector<uint32_t> timerHandler(bool virtualTimer = false) {
    const uint16_t timerControl = virtualTimer
        ? timer_sysreg::kCntvCtlEl0 : timer_sysreg::kCntpCtlEl0;
    return {mrs(gicv3_sysreg::kIar1El1, 0), movzX(1, 0),
            msr(timerControl, 1), msr(gicv3_sysreg::kEoir1El1, 0), kEret};
}

bool digestSnapshot(std::vector<uint8_t>& bytes) {
#ifdef _WIN32
    if (bytes.size() < 120) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, resultLength = 0, digestLength = 0;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> digest{};
    std::array<uint8_t, 32> zero{};
    const bool opened = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    if (!opened) return false;
    const bool sized = BCRYPT_SUCCESS(BCryptGetProperty(algorithm,
        BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
        sizeof(objectLength), &resultLength, 0)) &&
        BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
        &resultLength, 0)) && digestLength == digest.size();
    if (sized) object.resize(objectLength);
    const bool created = sized && BCRYPT_SUCCESS(BCryptCreateHash(
        algorithm, &hash, object.data(), objectLength, nullptr, 0, 0));
    bool updated = created && BCRYPT_SUCCESS(BCryptHashData(
        hash, bytes.data(), 88, 0)) && BCRYPT_SUCCESS(BCryptHashData(
        hash, zero.data(), static_cast<ULONG>(zero.size()), 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(hash, bytes.data() + 120,
        static_cast<ULONG>(bytes.size() - 120), 0));
    const bool finished = updated && BCRYPT_SUCCESS(BCryptFinishHash(
        hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!finished) return false;
    std::copy(digest.begin(), digest.end(), bytes.begin() + 88);
    return true;
#else
    (void)bytes;
    return false;
#endif
}

uint32_t getU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t getU64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

void putU32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

void putU64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

std::size_t findDevicePayload(const std::vector<uint8_t>& snapshot,
                              uint64_t instanceId) {
    std::size_t cursor = 128;
    for (unsigned section = 0; section < 3; ++section) {
        if (cursor + 16 > snapshot.size()) return 0;
        const uint32_t type = getU32(snapshot.data() + cursor);
        const uint64_t sectionBytes = getU64(snapshot.data() + cursor + 8);
        const std::size_t payload = cursor + 16;
        if (sectionBytes > snapshot.size() - payload) return 0;
        if (type == 2) {
            std::size_t at = payload;
            const uint32_t count = getU32(snapshot.data() + at);
            at += 4;
            for (uint32_t index = 0; index < count; ++index) {
                if (at + 17 > payload + sectionBytes) return 0;
                const uint64_t id = getU64(snapshot.data() + at);
                const uint8_t typeBytes = snapshot[at + 12];
                const uint32_t stateBytes = getU32(snapshot.data() + at + 13);
                at += 17;
                if (at + typeBytes + stateBytes > payload + sectionBytes) return 0;
                at += typeBytes;
                if (id == instanceId) return at;
                at += stateBytes;
            }
            return 0;
        }
        cursor = payload + static_cast<std::size_t>(sectionBytes);
    }
    return 0;
}

struct Machine {
    ia64::Memory ram;
    AArch64DeviceRegistry devices;
    AArch64PhysicalBus bus;
    AArch64ISAPlugin cpu;
    AArch64GenericTimer* timer = nullptr;
    AArch64GICv3* gic = nullptr;
    AArch64PL011* uart = nullptr;

    explicit Machine(AArch64GICv3::Config gicConfig = {},
                     uint64_t initialCounter = 0)
        : ram(0x80000, false), devices(), bus(ram, devices),
          cpu(kGuestStart, ExceptionLevel::EL1) {
        std::string error;
        auto timerOwned = std::make_unique<AArch64GenericTimer>(devices, 10, 201, 202,
            AArch64GenericTimer::Config{initialCounter,
                AArch64GenericTimer::kDefaultFrequencyHz});
        timer = timerOwned.get();
        if (!devices.registerDevice(std::move(timerOwned), &error) ||
            !timer->registerInterruptSources(&error))
            throw std::runtime_error("Generic Timer setup failed: " + error);

        auto gicOwned = std::make_unique<AArch64GICv3>(devices, 12, gicConfig);
        gic = gicOwned.get();
        if (!devices.registerDevice(std::move(gicOwned), &error))
            throw std::runtime_error("GICv3 registration failed: " + error);
        if (!gic->mapInterruptSource(201, AArch64GICv3::kPhysicalTimerPpi,
                                     AArch64InterruptMode::Level, &error) ||
            !gic->mapInterruptSource(202, AArch64GICv3::kVirtualTimerPpi,
                                     AArch64InterruptMode::Level, &error))
            throw std::runtime_error("timer PPI mapping failed: " + error);
        // Bind the GIC first deliberately; step ordering must still retire the
        // timer before the controller samples its level output.
        if (!gic->attach(cpu.cpu(), &error) || !timer->attach(cpu.cpu(), &error))
            throw std::runtime_error("CPU interrupt endpoint setup failed: " + error);
    }

    void addUart(uint64_t instanceId = 11, uint64_t sourceId = 301) {
        auto uartOwned = std::make_unique<AArch64PL011>(devices, instanceId,
                                                         kUartBase, sourceId);
        uart = uartOwned.get();
        std::string error;
        if (!devices.registerDevice(std::move(uartOwned), &error) ||
            !gic->mapInterruptSource(sourceId, gic->uartSpi(),
                                     AArch64InterruptMode::Level, &error))
            throw std::runtime_error("UART GIC setup failed: " + error);
    }
};

bool guestStep(Machine& machine) {
    const ia64::ISADecodeResult decoded = machine.cpu.decode(machine.bus);
    if (!decoded.valid) {
        std::cerr << "guest decode failed at 0x" << std::hex
                  << machine.cpu.cpu().pc() << ": " << decoded.disassembly
                  << " ESR=0x" << machine.cpu.cpu().esrEl1()
                  << " FAR=0x" << machine.cpu.cpu().farEl1()
                  << std::dec << '\n';
        return false;
    }
    const auto result = machine.cpu.execute(machine.bus, decoded);
    if (result != ia64::ISAExecutionResult::CONTINUE) {
        std::cerr << "guest execution failed at 0x" << std::hex
                  << machine.cpu.cpu().pc() << ": "
                  << machine.cpu.cpu().lastError() << std::dec << '\n';
        return false;
    }
    return true;
}

bool runUntilPc(Machine& machine, uint64_t wantedPc, uint64_t maxSteps = 500) {
    for (uint64_t i = 0; i < maxSteps; ++i) {
        if (machine.cpu.cpu().pc() == wantedPc) return true;
        if (!guestStep(machine)) return false;
    }
    return machine.cpu.cpu().pc() == wantedPc;
}

bool runUntilAck(Machine& machine, uint64_t vector, uint32_t expected,
                 uint64_t maxSteps = 500) {
    for (uint64_t i = 0; i < maxSteps; ++i) {
        if (machine.cpu.cpu().pc() == vector + 4 &&
            machine.cpu.cpu().readX(0) == expected) return true;
        if (!guestStep(machine)) return false;
    }
    return machine.cpu.cpu().pc() == vector + 4 &&
           machine.cpu.cpu().readX(0) == expected;
}

uint64_t pageDescriptor(uint64_t pa, uint8_t attributeIndex) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(attributeIndex) << 2) |
           (3ULL << 8) | (1ULL << 10);
}

uint64_t blockDescriptor(uint64_t pa, uint8_t attributeIndex) {
    return (pa & 0x0000FFFFFFE00000ULL) | 1u |
           (static_cast<uint64_t>(attributeIndex) << 2) |
           (3ULL << 8) | (1ULL << 10);
}

void appendGuestMmuSetup(std::vector<uint32_t>& words) {
    // Guest-built 4 KiB tables map code/vector pages as Normal memory, the
    // UART as Device-nGnRnE, and a 2 MiB GIC window as Device-nGnRnE.
    appendConstant(words, 20, 0x10000);
    appendConstant(words, 21, 0x11000);
    appendConstant(words, 22, 0x12000);
    appendConstant(words, 23, 0x13000);
    appendConstant(words, 24, 0x11003);
    words.push_back(strX(24, 20));
    appendConstant(words, 24, 0x12003);
    words.push_back(strX(24, 21));
    appendConstant(words, 24, 0x13003);
    words.push_back(strX(24, 21, 256 * 8));
    appendConstant(words, 24, blockDescriptor(kGicdBase, 1));
    words.push_back(strX(24, 21, 264 * 8));
    appendConstant(words, 24, pageDescriptor(0x1000, 0));
    words.push_back(strX(24, 22, 1 * 8));
    appendConstant(words, 24, pageDescriptor(kVectorBase, 0));
    words.push_back(strX(24, 22, 4 * 8));
    appendConstant(words, 24, pageDescriptor(kUartBase, 1));
    words.push_back(strX(24, 23));
    appendConstant(words, 2, 25 | (25ULL << 16) | (2ULL << 30));
    words.push_back(msr(syskey(3, 0, 2, 0, 2), 2)); // TCR_EL1
    appendConstant(words, 3, 0x10000);
    words.push_back(msr(syskey(3, 0, 2, 0, 0), 3)); // TTBR0_EL1
    appendConstant(words, 4, 0xFF); // Attr0 Normal WB, Attr1 Device-nGnRnE.
    words.push_back(msr(syskey(3, 0, 10, 2, 0), 4)); // MAIR_EL1
    appendConstant(words, 5, 1);
    words.push_back(msr(syskey(3, 0, 1, 0, 0), 5)); // SCTLR_EL1.M
    words.push_back(0xD5033A9Fu); // DSB ISHST
    words.push_back(0xD5033FDFu); // ISB
}

void installHandler(Machine& machine, uint64_t vectorOffset,
                    const std::vector<uint32_t>& handler) {
    writeWords(machine.ram, kVectorBase + vectorOffset, handler);
}

void testGicRegistersAndNegativeMmio() {
    Machine machine;
    AArch64GICv3::Config invalidConfig{};
    invalidConfig.distributorBase += 1;
    bool rejected = false;
    try {
        AArch64GICv3 invalid(machine.devices, 99, invalidConfig);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
    invalidConfig = AArch64GICv3::Config{};
    invalidConfig.uartSpi = 64;
    rejected = false;
    try {
        AArch64GICv3 invalid(machine.devices, 99, invalidConfig);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
    uint8_t bytes[8]{};
    CHECK(machine.gic->readMmio(0x4, bytes, 4) == AArch64MmioAccessResult::Success);
    CHECK_EQ(1, getU32(bytes)); // 64 INTIDs, ITLinesNumber=1.
    CHECK(machine.gic->readMmio(0x0, bytes, 4) == AArch64MmioAccessResult::Success);
    CHECK_EQ(0x50, getU32(bytes)); // DS=1, ARE=1, Group 1 reset disabled.
    CHECK(machine.gic->readMmio(kGicrBase - kGicdBase + 8, bytes, 8) ==
          AArch64MmioAccessResult::Success);
    CHECK_EQ(1u << 4, getU64(bytes)); // One redistributor, Last=1, affinity 0.

    CHECK(machine.gic->readMmio(1, bytes, 4) == AArch64MmioAccessResult::ExternalAbort);
    CHECK(machine.gic->readMmio(0x20, bytes, 4) == AArch64MmioAccessResult::Unsupported);
    CHECK(machine.gic->readMmio(0x0, bytes, 3) == AArch64MmioAccessResult::ExternalAbort);
    putU32(bytes, 1);
    CHECK(machine.gic->writeMmio(0x108, bytes, 4) == AArch64MmioAccessResult::Unsupported);
    putU64(bytes, 1ULL << 40); // Reserved IROUTER bit.
    CHECK(machine.gic->writeMmio(0x6100, bytes, 8) == AArch64MmioAccessResult::Unsupported);
    putU64(bytes, 1); // Valid route to an unsupported affinity: stays undeliverable.
    CHECK(machine.gic->writeMmio(0x6100, bytes, 8) == AArch64MmioAccessResult::Success);
    putU32(bytes, 1);
    CHECK(machine.gic->writeMmio(0x104, bytes, 4) == AArch64MmioAccessResult::Success);
    putU32(bytes, 1);
    CHECK(machine.gic->writeMmio(0x84, bytes, 4) == AArch64MmioAccessResult::Success);
    putU32(bytes, 1);
    CHECK(machine.gic->writeMmio(0x0, bytes, 4) == AArch64MmioAccessResult::Unsupported);
    putU32(bytes, 0x12);
    CHECK(machine.gic->writeMmio(0, bytes, 4) == AArch64MmioAccessResult::Success);
    CHECK(!machine.gic->mapInterruptSource(201, 30, AArch64InterruptMode::Level));
    CHECK(!machine.gic->mapInterruptSource(201, 31, AArch64InterruptMode::Level));
    CHECK(!machine.gic->mapInterruptSource(201, 32, AArch64InterruptMode::Edge));
}

void testSourceOwnerRemovalAndSnapshotTopology() {
    Machine machine;
    machine.addUart();
    std::string error;
    CHECK(machine.devices.setLevel(301, true, &error));
    CHECK(machine.gic->pending(33));
    std::vector<uint8_t> withUartSnapshot;
    CHECK(AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
        machine.devices, withUartSnapshot, &error));
    Machine missingUart;
    const auto missingUartCpu = missingUart.cpu.serialize_state();
    const std::vector<uint8_t> missingUartRam(missingUart.ram.GetRawData(),
        missingUart.ram.GetRawData() + missingUart.ram.GetTotalSize());
    const uint64_t missingUartTimer = missingUart.timer->counter();
    CHECK(!AArch64DeviceSnapshot::restore(missingUart.cpu, missingUart.ram,
        missingUart.devices, withUartSnapshot, &error));
    CHECK(missingUart.cpu.serialize_state() == missingUartCpu);
    CHECK(std::equal(missingUartRam.begin(), missingUartRam.end(),
                     missingUart.ram.GetRawData()));
    CHECK_EQ(missingUartTimer, missingUart.timer->counter());
    CHECK(!missingUart.gic->pending(33));
    CHECK(machine.devices.unregisterDevice(11, &error));
    CHECK(!machine.gic->pending(33)); // A removed source cannot leave a stale IRQ level.
    CHECK_EQ(2, machine.devices.deviceCount());
    CHECK_EQ(2, machine.devices.interruptSourceCount());

    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
        machine.devices, snapshot, &error));
    Machine target;
    CHECK(AArch64DeviceSnapshot::restore(target.cpu, target.ram,
        target.devices, snapshot, &error));
    CHECK(!target.gic->pending(33));
    CHECK_EQ(2, target.devices.deviceCount());
    CHECK_EQ(2, target.devices.interruptSourceCount());

    AArch64GICv3::Config mismatchedConfig{};
    mismatchedConfig.uartSpi = 34;
    Machine wrongGicConfig(mismatchedConfig);
    const auto wrongConfigCpu = wrongGicConfig.cpu.serialize_state();
    const uint64_t wrongConfigTimer = wrongGicConfig.timer->counter();
    CHECK(!AArch64DeviceSnapshot::restore(wrongGicConfig.cpu, wrongGicConfig.ram,
        wrongGicConfig.devices, snapshot, &error));
    CHECK(wrongGicConfig.cpu.serialize_state() == wrongConfigCpu);
    CHECK_EQ(wrongConfigTimer, wrongGicConfig.timer->counter());
    CHECK(!wrongGicConfig.gic->pending(33) && !wrongGicConfig.gic->pending(34));
}

void testCpuInterfaceSpuriousAndInvalidEoi() {
    Machine machine;
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase, 0, false);
    appendSysWrite(words, gicv3_sysreg::kCtlrEl1, 4u << 8);
    const std::size_t ctlrRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kCtlrEl1, 0));
    const std::size_t iarRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kIar1El1, 1));
    const std::size_t hppirRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 2));
    appendConstant(words, 3, 33);
    const std::size_t invalidEoi = words.size();
    words.push_back(msr(gicv3_sysreg::kEoir1El1, 3));
    writeWords(machine.ram, kGuestStart, words);

    CHECK(runUntilPc(machine, kGuestStart + ctlrRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(4u << 8, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + iarRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(AArch64GICv3::kSpuriousIntid, machine.cpu.cpu().readX(1));
    CHECK(runUntilPc(machine, kGuestStart + hppirRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(AArch64GICv3::kSpuriousIntid, machine.cpu.cpu().readX(2));
    CHECK(runUntilPc(machine, kGuestStart + invalidEoi * 4));
    const uint64_t eoiPc = machine.cpu.cpu().pc();
    const ia64::ISADecodeResult decoded = machine.cpu.decode(machine.bus);
    CHECK(decoded.valid);
    CHECK(machine.cpu.execute(machine.bus, decoded) != ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(eoiPc, machine.cpu.cpu().pc());
    CHECK(!machine.gic->active(33));
}

void testProgramAPhysicalTimerPpi() {
    Machine machine;
    machine.cpu.cpu().setSpEl1(0x8000);
    machine.cpu.cpu().setEsrEl1(0x12345678u);
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
                   1u << AArch64GICv3::kPhysicalTimerPpi, false);
    const std::size_t tvalMsrIndex = words.size() + 1;
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 5);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    words.insert(words.end(), 8, kNop); // expiry occurs while PSTATE.I is set
    const std::size_t unmaskMsrIndex = words.size() + 1;
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 24, kNop);
    writeWords(machine.ram, kGuestStart, words);
    installHandler(machine, 0x280, timerHandler(false));

    const uint64_t programmedCompare = tvalMsrIndex + 5;
    const uint64_t unmaskPc = kGuestStart + unmaskMsrIndex * 4;
    CHECK(runUntilPc(machine, unmaskPc));
    CHECK_EQ(programmedCompare, machine.timer->physicalComparator());
    CHECK(machine.timer->counter() >= programmedCompare);
    CHECK(machine.cpu.cpu().pc() != kVectorBase + 0x280);
    CHECK((machine.cpu.cpu().pstate() & kPstateI) != 0);
    CHECK(machine.gic->pending(30));
    CHECK(!machine.gic->active(30));

    const uint64_t interruptedPc = unmaskPc + 4;
    CHECK(guestStep(machine)); // DAIF clear itself retires one tick.
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK(runUntilAck(machine, kVectorBase + 0x280, 30));
    CHECK_EQ(interruptedPc, machine.cpu.cpu().elrEl1());
    CHECK_EQ(5, machine.cpu.cpu().spsrEl1());
    CHECK_EQ(0x12345678u, machine.cpu.cpu().esrEl1()); // asynchronous IRQ has no syndrome
    CHECK((machine.cpu.cpu().pstate() & kPstateI) != 0);
    CHECK_EQ(30, machine.cpu.cpu().readX(0));
    CHECK(machine.gic->active(30));
    CHECK(machine.gic->pending(30)); // active+pending while the timer line remains asserted
    CHECK_EQ(0x20, machine.gic->runningPriority());
    CHECK_EQ(unmaskMsrIndex + 2, machine.timer->counter());

    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(machine));
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK_EQ(0, machine.timer->physicalControl());
    CHECK(!machine.timer->physicalInterruptAsserted());
    CHECK(!machine.gic->pending(30));
    CHECK(!machine.gic->active(30));
    CHECK_EQ(0xFF, machine.gic->runningPriority());
    CHECK(guestStep(machine)); // no spurious repeat after ERET
    CHECK_EQ(interruptedPc + 4, machine.cpu.cpu().pc());
}

void testProgramBVirtualAndSimultaneousPpis() {
    Machine virtualMachine;
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
                   1u << AArch64GICv3::kVirtualTimerPpi, false);
    appendSysWrite(words, timer_sysreg::kCntvTvalEl0, 7);
    appendSysWrite(words, timer_sysreg::kCntvCtlEl0, 1);
    words.insert(words.end(), 12, kNop);
    const std::size_t unmaskIndex = words.size() + 1;
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 16, kNop);
    writeWords(virtualMachine.ram, kGuestStart, words);
    installHandler(virtualMachine, 0x280, timerHandler(true));
    const uint64_t interruptedPc = kGuestStart + (unmaskIndex + 1) * 4;
    CHECK(runUntilPc(virtualMachine, kGuestStart + unmaskIndex * 4));
    CHECK(guestStep(virtualMachine));
    CHECK(runUntilAck(virtualMachine, kVectorBase + 0x280, 27));
    CHECK_EQ(interruptedPc, virtualMachine.cpu.cpu().elrEl1());
    CHECK(virtualMachine.gic->active(27));
    CHECK(!virtualMachine.gic->active(30));
    CHECK(virtualMachine.gic->pending(27));
    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(virtualMachine));
    CHECK_EQ(interruptedPc, virtualMachine.cpu.cpu().pc());
    CHECK_EQ(0, virtualMachine.timer->virtualControl());
    CHECK(!virtualMachine.gic->pending(27));

    Machine simultaneous;
    std::vector<uint32_t> both;
    appendGicSetup(both, kGicdBase, kGicrBase,
        (1u << AArch64GICv3::kPhysicalTimerPpi) |
        (1u << AArch64GICv3::kVirtualTimerPpi), false, 33, 0x20);
    appendMmioWrite8(both, kGicrBase + 0x10400 + 27, 0x40);
    appendMmioWrite8(both, kGicrBase + 0x10400 + 30, 0x20);
    const uint64_t bothCompare = both.size() + 50;
    appendSysWrite(both, timer_sysreg::kCntpCvalEl0, bothCompare);
    appendSysWrite(both, timer_sysreg::kCntvCvalEl0, bothCompare);
    appendSysWrite(both, timer_sysreg::kCntpCtlEl0, 1);
    appendSysWrite(both, timer_sysreg::kCntvCtlEl0, 1);
    both.insert(both.end(), 55, kNop);
    const std::size_t bothUnmask = both.size() + 1;
    appendSysWrite(both, syskey(3, 3, 4, 2, 1), 0);
    both.insert(both.end(), 20, kNop);
    writeWords(simultaneous.ram, kGuestStart, both);
    installHandler(simultaneous, 0x280, {mrs(gicv3_sysreg::kIar1El1, 0), kNop});
    CHECK(runUntilPc(simultaneous, kGuestStart + bothUnmask * 4));
    CHECK_EQ(bothCompare, simultaneous.timer->physicalComparator());
    CHECK_EQ(bothCompare, simultaneous.timer->virtualComparator());
    CHECK(simultaneous.gic->pending(27));
    CHECK(simultaneous.gic->pending(30));
    CHECK(guestStep(simultaneous));
    CHECK(runUntilAck(simultaneous, kVectorBase + 0x280, 30));
    CHECK(simultaneous.gic->active(30));
    CHECK(!simultaneous.gic->active(27));
    CHECK_EQ(0x20, simultaneous.gic->runningPriority());
}

void testProgramCUartSpiThroughGuestMmu() {
    AArch64GICv3::Config gicConfig{};
    gicConfig.uartSpi = 34;
    Machine machine(gicConfig);
    machine.addUart();
    std::vector<uint32_t> words;
    appendGuestMmuSetup(words);
    appendGicSetup(words, kGicdVa, kGicrVa,
                   0, true, 34, 0x20, 0x30);
    appendConstant(words, 20, kUartVa);
    words.push_back(movzW(2, 0x60)); // 8-bit mode with the one-byte FIFO path.
    words.push_back(strW(2, 20, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 20, AArch64PL011::UARTCR));
    words.push_back(movzW(2, 0));
    words.push_back(strW(2, 20, AArch64PL011::UARTIFLS)); // One-byte RX threshold.
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 20, AArch64PL011::UARTIMSC));
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0); // Unmask IRQ at EL1.
    const std::size_t receiveWait = words.size();
    words.insert(words.end(), 32, kNop);
    writeWords(machine.ram, kGuestStart, words);
    installHandler(machine, 0x280,
        {mrs(gicv3_sysreg::kIar1El1, 0), ldrW(1, 20),
         msr(gicv3_sysreg::kEoir1El1, 0), kEret});

    const std::size_t programBytes = words.size() * 4;
    CHECK(runUntilPc(machine, kGuestStart + receiveWait * 4));
    std::size_t accepted = 0;
    std::string error;
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>{'Z'}, accepted, &error));
    CHECK_EQ(1, accepted);
    const uint64_t interruptedPc = machine.cpu.cpu().pc();
    CHECK(runUntilAck(machine, kVectorBase + 0x280, 34));
    CHECK_EQ(interruptedPc, machine.cpu.cpu().elrEl1());
    CHECK(machine.gic->active(34));
    CHECK(guestStep(machine)); // Read the UART data register.
    CHECK_EQ('Z', machine.cpu.cpu().readW(1));
    CHECK(guestStep(machine)); // EOIR
    CHECK(guestStep(machine)); // ERET
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK(machine.uart->pendingReceiveBytes().empty());
    CHECK(!machine.gic->pending(34));
    AArch64InterruptSourceState uartSource;
    CHECK(machine.devices.queryInterruptSource(301, uartSource, &error));
    CHECK(!uartSource.asserted);

    const MMUResult gicTranslation = AArch64MMU::translate(
        machine.bus, kGicdVa, 1, MMUAccess::Read,
        machine.cpu.cpu().effectiveTcrEl1(),
        machine.cpu.cpu().effectiveTtbr0El1(), machine.cpu.cpu().effectiveTtbr1El1(),
        machine.cpu.cpu().effectiveMairEl1(),
        (machine.cpu.cpu().effectiveSctlrEl1() & (1u << 19)) != 0);
    CHECK(gicTranslation.success);
    CHECK_EQ(kGicdBase, gicTranslation.physicalAddress);
    CHECK_EQ(1, gicTranslation.attributes.attributeIndex);
    CHECK_EQ(0x00, gicTranslation.attributes.memoryAttribute);
    CHECK(programBytes >= receiveWait * 4);
}

void testProgramDEl0IrqVectorAndPrivilege() {
    Machine machine;
    machine.cpu.cpu().setSpEl1(0x8000);
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
                   1u << AArch64GICv3::kPhysicalTimerPpi, false);
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 40);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    appendSysWrite(words, syskey(3, 0, 4, 1, 0), 0x7000); // SP_EL0
    appendSysWrite(words, syskey(3, 0, 4, 0, 1), kEl0Start); // ELR_EL1
    appendSysWrite(words, syskey(3, 0, 4, 0, 0), 0); // SPSR_EL1 => EL0t, IRQ unmasked
    words.push_back(kEret);
    writeWords(machine.ram, kGuestStart, words);
    std::vector<uint32_t> el0Code(40, kNop);
    writeWords(machine.ram, kEl0Start, el0Code);
    installHandler(machine, 0x480, timerHandler(false));

    CHECK(runUntilPc(machine, kEl0Start));
    CHECK_EQ(0, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
    CHECK_EQ(0x7000, machine.cpu.cpu().sp());
    const uint64_t initialSpEl1 = machine.cpu.cpu().spEl1();
    CHECK(runUntilAck(machine, kVectorBase + 0x480, 30));
    const uint64_t interruptedPc = machine.cpu.cpu().elrEl1();
    CHECK_EQ(0, machine.cpu.cpu().spsrEl1());
    CHECK_EQ(1, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
    CHECK_EQ(0x8000, machine.cpu.cpu().sp());
    CHECK_EQ(0x7000, machine.cpu.cpu().spEl0());
    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(machine));
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK_EQ(0, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
    CHECK_EQ(0x7000, machine.cpu.cpu().sp());
    CHECK_EQ(initialSpEl1, machine.cpu.cpu().spEl1());

    Machine privilege;
    writeWord(privilege.ram, kGuestStart, mrs(gicv3_sysreg::kPmrEl1, 0));
    privilege.cpu.cpu().setVbarEl1(kVectorBase);
    privilege.cpu.cpu().setPstate(0);
    CHECK(guestStep(privilege)); // unauthorized EL0 ICC read enters a sync trap
    CHECK_EQ(1, static_cast<uint64_t>(privilege.cpu.cpu().exceptionLevel()));
    CHECK_EQ(kVectorBase + 0x400, privilege.cpu.cpu().pc());
    CHECK_EQ(0x18, privilege.cpu.cpu().esrEl1() >> 26);
}

void testEl1SpEl0IrqVectorAndUnmappedVector() {
    Machine threadMode;
    threadMode.cpu.cpu().setSpEl0(0x7000);
    threadMode.cpu.cpu().setSpEl1(0x8000);
    threadMode.cpu.cpu().setPstate(4); // EL1t uses SP_EL0.
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
        1u << AArch64GICv3::kPhysicalTimerPpi, false);
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 5);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    const std::size_t unmaskMsr = words.size() + 1;
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 24, kNop);
    writeWords(threadMode.ram, kGuestStart, words);
    installHandler(threadMode, 0x080, timerHandler(false));

    CHECK(runUntilPc(threadMode, kGuestStart + unmaskMsr * 4));
    CHECK(guestStep(threadMode));
    CHECK(runUntilAck(threadMode, kVectorBase + 0x080, 30));
    CHECK_EQ(4, threadMode.cpu.cpu().spsrEl1());
    CHECK_EQ(1, static_cast<uint64_t>(threadMode.cpu.cpu().exceptionLevel()));
    CHECK_EQ(0x8000, threadMode.cpu.cpu().sp()); // IRQ entry selects EL1h.
    CHECK_EQ(0x7000, threadMode.cpu.cpu().spEl0());
    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(threadMode));
    CHECK_EQ(1, static_cast<uint64_t>(threadMode.cpu.cpu().exceptionLevel()));
    CHECK_EQ(0x7000, threadMode.cpu.cpu().sp()); // ERET restores EL1t's SP_EL0.

    Machine missingVector;
    std::vector<uint32_t> missing;
    appendGicSetup(missing, kGicdBase, kGicrBase,
        1u << AArch64GICv3::kPhysicalTimerPpi, false);
    appendSysWrite(missing, timer_sysreg::kCntpTvalEl0, 1);
    appendSysWrite(missing, timer_sysreg::kCntpCtlEl0, 1);
    const std::size_t missingUnmaskMsr = missing.size() + 1;
    appendSysWrite(missing, syskey(3, 3, 4, 2, 1), 0);
    missing.insert(missing.end(), 8, kNop);
    writeWords(missingVector.ram, kGuestStart, missing);
    CHECK(runUntilPc(missingVector, kGuestStart + missingUnmaskMsr * 4));
    CHECK(guestStep(missingVector));
    CHECK(missingVector.gic->pending(30));
    const ia64::ISADecodeResult vectorDecode = missingVector.cpu.decode(missingVector.bus);
    CHECK(!vectorDecode.valid);
    CHECK_EQ(kVectorBase + 0x280, missingVector.cpu.cpu().pc());
    CHECK_EQ(kGuestStart + (missingUnmaskMsr + 1) * 4,
             missingVector.cpu.cpu().elrEl1());
    CHECK_EQ(0, missingVector.cpu.cpu().esrEl1()); // IRQ entry does not invent an ESR.
}

void testProgramEMaskingPriorityAndLevelReassertion() {
    Machine machine;
    machine.addUart();
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
        1u << AArch64GICv3::kPhysicalTimerPpi, true, 33, 0x40, 0x20);
    appendSysWrite(words, gicv3_sysreg::kIgrpEn1El1, 0); // Group 1 disabled.
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 7);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    appendConstant(words, 20, kUartBase);
    words.push_back(movzW(2, 0x60));
    words.push_back(strW(2, 20, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 20, AArch64PL011::UARTCR));
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 20, AArch64PL011::UARTIMSC));
    const uint64_t maskedWaitPc = kGuestStart + words.size() * 4;
    words.insert(words.end(), 24, kNop);

    const std::size_t groupDisabledRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendSysWrite(words, gicv3_sysreg::kIgrpEn1El1, 1);
    appendSysWrite(words, gicv3_sysreg::kPmrEl1, 0x20);
    const std::size_t pmrBlockedRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendSysWrite(words, gicv3_sysreg::kPmrEl1, 0xFF);
    const std::size_t priorityRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendMmioWrite32(words, kGicdBase + 0x184, 1u << 1); // Disable SPI 33.
    const std::size_t spiDisabledRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendMmioWrite32(words, kGicdBase + 0x104, 1u << 1);
    const std::size_t spiEnabledRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendMmioWrite32(words, kGicrBase + 0x10180,
                      1u << AArch64GICv3::kPhysicalTimerPpi); // Disable PPI 30.
    const std::size_t ppiDisabledRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendMmioWrite32(words, kGicrBase + 0x10100,
                      1u << AArch64GICv3::kPhysicalTimerPpi);
    const std::size_t bothEnabledRead = words.size();
    words.push_back(mrs(gicv3_sysreg::kHppir1El1, 0));
    appendMmioRead32(words, kGicdBase + 0x204, 0);
    const std::size_t distributorPendingReadEnd = words.size();
    appendMmioRead32(words, kGicrBase + 0x10200, 0);
    const std::size_t redistributorPendingReadEnd = words.size();
    const std::size_t unmask = words.size();
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 24, kNop);
    writeWords(machine.ram, kGuestStart, words);
    installHandler(machine, 0x280,
        {mrs(gicv3_sysreg::kIar1El1, 0), msr(gicv3_sysreg::kEoir1El1, 0),
         movzX(1, 0), msr(timer_sysreg::kCntpCtlEl0, 1), ldrW(2, 20), kEret});

    CHECK(runUntilPc(machine, maskedWaitPc));
    std::size_t accepted = 0;
    std::string error;
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>{'E'}, accepted, &error));
    CHECK_EQ(1, accepted);
    for (unsigned i = 0; i < 24; ++i) CHECK(guestStep(machine));
    CHECK(machine.gic->pending(30));
    CHECK(machine.gic->pending(33));
    CHECK(!machine.gic->active(30) && !machine.gic->active(33));
    CHECK((machine.cpu.cpu().pstate() & kPstateI) != 0);

    CHECK(runUntilPc(machine, kGuestStart + groupDisabledRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(AArch64GICv3::kSpuriousIntid, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + pmrBlockedRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(AArch64GICv3::kSpuriousIntid, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + priorityRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(33, machine.cpu.cpu().readX(0)); // SPI priority 0x20 wins over PPI 0x40.
    CHECK(runUntilPc(machine, kGuestStart + spiDisabledRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(30, machine.cpu.cpu().readX(0)); // Disabled SPI leaves the PPI eligible.
    CHECK(runUntilPc(machine, kGuestStart + spiEnabledRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(33, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + ppiDisabledRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(33, machine.cpu.cpu().readX(0)); // Disabled PPI leaves the SPI eligible.
    CHECK(runUntilPc(machine, kGuestStart + bothEnabledRead * 4));
    CHECK(guestStep(machine));
    CHECK_EQ(33, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + distributorPendingReadEnd * 4));
    CHECK_EQ(1u << 1, machine.cpu.cpu().readX(0));
    CHECK(runUntilPc(machine, kGuestStart + redistributorPendingReadEnd * 4));
    CHECK_EQ(1u << AArch64GICv3::kPhysicalTimerPpi,
             machine.cpu.cpu().readX(0));
    CHECK_EQ(0x20, machine.gic->priority(33));

    CHECK(runUntilPc(machine, kGuestStart + unmask * 4));
    const uint64_t interruptedPc = kGuestStart + (unmask + 2) * 4;
    CHECK(guestStep(machine)); // Load the DAIF clear value.
    CHECK_EQ(kGuestStart + (unmask + 1) * 4, machine.cpu.cpu().pc());
    CHECK(guestStep(machine)); // Clear PSTATE.I; IRQ is sampled before the next fetch.
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK(runUntilAck(machine, kVectorBase + 0x280, 33));
    CHECK(machine.gic->active(33));
    CHECK(machine.gic->pending(33)); // Level remains asserted until UART DR is read.
    CHECK(machine.gic->pending(30));
    CHECK_EQ(0x20, machine.gic->runningPriority());
    CHECK(guestStep(machine)); // EOIR while the UART level is still asserted.
    CHECK(!machine.gic->active(33));
    CHECK(machine.gic->pending(33)); // Level re-pends after deactivation.
    CHECK(guestStep(machine)); // X1 = 0
    CHECK(guestStep(machine)); // Disable timer; clears PPI 30 level.
    CHECK(guestStep(machine)); // Read UART DR; clears SPI 33 level.
    CHECK_EQ('E', machine.cpu.cpu().readW(2));
    CHECK(guestStep(machine)); // ERET
    CHECK_EQ(interruptedPc, machine.cpu.cpu().pc());
    CHECK(!machine.gic->pending(30));
    CHECK(!machine.gic->pending(33));
    CHECK(!machine.gic->active(30) && !machine.gic->active(33));
}

void testHigherPriorityNestedPreemptionGuestSavedReturnState() {
    Machine machine;
    machine.addUart();
    machine.cpu.cpu().setSpEl1(0x8000);
    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase,
        1u << AArch64GICv3::kPhysicalTimerPpi, true, 33, 0x40, 0x20);
    appendConstant(words, 20, kUartBase);
    words.push_back(movzW(2, 0x60));
    words.push_back(strW(2, 20, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 20, AArch64PL011::UARTCR));
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 20, AArch64PL011::UARTIMSC));
    appendConstant(words, 0, 0xA5A5);
    appendConstant(words, 1, 0xBEEF);
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 5);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    const std::size_t unmaskMsr = words.size() + 1;
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 32, kNop);
    writeWords(machine.ram, kGuestStart, words);

    const uint16_t elrKey = syskey(3, 0, 4, 0, 1);
    const uint16_t spsrKey = syskey(3, 0, 4, 0, 0);
    const uint16_t daifKey = syskey(3, 3, 4, 2, 1);
    std::vector<uint32_t> handler;
    handler.push_back(adjustSp(true, 64));
    handler.push_back(stpX(0, 1, 31, 0));
    handler.push_back(stpX(10, 11, 31, 16));
    handler.push_back(stpX(20, 31, 31, 32));
    handler.push_back(mrs(elrKey, 10));
    handler.push_back(mrs(spsrKey, 11));
    handler.push_back(stpX(10, 11, 31, 48));
    const std::size_t iarIndex = handler.size();
    handler.push_back(mrs(gicv3_sysreg::kIar1El1, 0));
    handler.push_back(cmpWImm(0, 33));
    const std::size_t branchIndex = handler.size();
    handler.push_back(0); // Patched to the nested SPI path below.

    const std::size_t outerStart = handler.size();
    handler.push_back(movzX(1, 0));
    const std::size_t outerUnmask = handler.size();
    handler.push_back(msr(daifKey, 1));
    const std::size_t outerResume = handler.size();
    handler.push_back(movzX(1, 0));
    handler.push_back(msr(timer_sysreg::kCntpCtlEl0, 1));
    handler.push_back(msr(gicv3_sysreg::kEoir1El1, 0));
    handler.push_back(ldpX(10, 11, 31, 48));
    handler.push_back(msr(elrKey, 10));
    handler.push_back(msr(spsrKey, 11));
    handler.push_back(ldpX(0, 1, 31, 0));
    handler.push_back(ldpX(20, 31, 31, 32));
    handler.push_back(ldpX(10, 11, 31, 16));
    handler.push_back(adjustSp(false, 64));
    const std::size_t outerEret = handler.size();
    handler.push_back(kEret);

    const std::size_t nestedStart = handler.size();
    handler.push_back(ldrW(1, 20, AArch64PL011::UARTDR));
    handler.push_back(msr(gicv3_sysreg::kEoir1El1, 0));
    handler.push_back(ldpX(10, 11, 31, 48));
    handler.push_back(msr(elrKey, 10));
    handler.push_back(msr(spsrKey, 11));
    handler.push_back(ldpX(0, 1, 31, 0));
    handler.push_back(ldpX(20, 31, 31, 32));
    handler.push_back(ldpX(10, 11, 31, 16));
    handler.push_back(adjustSp(false, 64));
    const std::size_t nestedEret = handler.size();
    handler.push_back(kEret);
    handler[branchIndex] = bCond(0, branchIndex, nestedStart);
    installHandler(machine, 0x280, handler);

    CHECK(outerStart < outerUnmask && outerUnmask + 1 == outerResume);
    CHECK(nestedEret + 1 == handler.size());
    CHECK(runUntilPc(machine, kGuestStart + unmaskMsr * 4));
    CHECK(guestStep(machine));
    CHECK(runUntilPc(machine, kVectorBase + 0x280 + iarIndex * 4));
    CHECK(guestStep(machine)); // Acknowledge the lower-priority physical timer.
    CHECK_EQ(30, machine.cpu.cpu().readX(0));
    CHECK(machine.gic->active(30));

    std::size_t accepted = 0;
    std::string error;
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>{'N'}, accepted, &error));
    CHECK_EQ(1, accepted);
    CHECK(machine.gic->pending(33));
    CHECK(machine.gic->irqOutputAsserted());
    const uint64_t outerClearPc = kVectorBase + 0x280 + outerUnmask * 4;
    CHECK(runUntilPc(machine, outerClearPc));
    CHECK(guestStep(machine)); // Re-enable IRQs after saving ELR/SPSR and GPRs.
    CHECK_EQ(kVectorBase + 0x280 + outerResume * 4,
             machine.cpu.cpu().pc());
    const uint64_t nestedIarPc = kVectorBase + 0x280 + iarIndex * 4;
    CHECK(runUntilPc(machine, nestedIarPc)); // Higher-priority SPI preempts the timer handler.
    CHECK_EQ(kVectorBase + 0x280 + outerResume * 4,
             machine.cpu.cpu().elrEl1());
    CHECK_EQ(0x80000005u, machine.cpu.cpu().spsrEl1());
    CHECK(machine.gic->active(30));
    CHECK(!machine.gic->active(33));
    CHECK(machine.gic->pending(33));
    CHECK(guestStep(machine));
    CHECK_EQ(33, machine.cpu.cpu().readX(0));
    CHECK(machine.gic->active(33));
    CHECK_EQ(0x20, machine.gic->runningPriority());

    const uint64_t outerResumePc = kVectorBase + 0x280 + outerResume * 4;
    CHECK(runUntilPc(machine, outerResumePc)); // Nested handler services UART, EOIs, restores, and ERETs.
    CHECK_EQ(outerResumePc, machine.cpu.cpu().pc());
    CHECK_EQ(30, machine.cpu.cpu().readX(0));
    CHECK_EQ(0, machine.cpu.cpu().readX(1)); // Outer handler's temporary after the nested return.
    CHECK_EQ(kUartBase, machine.cpu.cpu().readX(20));
    CHECK_EQ(0x7FC0, machine.cpu.cpu().sp()); // Outer IRQ frame remains allocated.
    CHECK_EQ(0x40, machine.gic->runningPriority());
    CHECK(machine.gic->active(30));
    CHECK(!machine.gic->active(33));
    CHECK(!machine.gic->pending(33));
    CHECK(machine.uart->pendingReceiveBytes().empty());

    const uint64_t outerEretPc = kVectorBase + 0x280 + outerEret * 4;
    CHECK(runUntilPc(machine, outerEretPc));
    CHECK_EQ(0, machine.timer->physicalControl());
    CHECK(!machine.gic->active(30));
    CHECK(!machine.gic->pending(30));
    CHECK_EQ(0x8000, machine.cpu.cpu().sp());
    CHECK_EQ(kGuestStart + (unmaskMsr + 1) * 4,
             machine.cpu.cpu().elrEl1());
    CHECK_EQ(5, machine.cpu.cpu().spsrEl1());
    CHECK(guestStep(machine)); // Outer ERET resumes the original guest context.
    CHECK_EQ(kGuestStart + (unmaskMsr + 1) * 4,
             machine.cpu.cpu().pc());
    CHECK_EQ(0xA5A5, machine.cpu.cpu().readX(0));
    CHECK_EQ(0xBEEF, machine.cpu.cpu().readX(1));
    CHECK_EQ(kUartBase, machine.cpu.cpu().readX(20));
    CHECK_EQ(0x8000, machine.cpu.cpu().spEl1());
    CHECK(guestStep(machine));
    CHECK_EQ(kGuestStart + (unmaskMsr + 2) * 4, machine.cpu.cpu().pc());
}

void testEdgeTriggeredSpiRetainedWhileMasked() {
    Machine machine;
    std::string error;
    constexpr uint64_t kEdgeSourceId = 401;
    constexpr uint32_t kEdgeSpi = 34;
    CHECK(machine.devices.registerInterruptSource(
        kEdgeSourceId, 12, AArch64InterruptMode::Edge, &error));
    CHECK(machine.gic->mapInterruptSource(kEdgeSourceId, kEdgeSpi,
        AArch64InterruptMode::Edge, &error));

    std::vector<uint32_t> words;
    appendGicSetup(words, kGicdBase, kGicrBase, 0, false);
    appendMmioWrite32(words, kGicdBase + 0x84, 1u << 2);
    appendMmioWrite32(words, kGicdBase + 0x104, 1u << 2);
    appendMmioWrite8(words, kGicdBase + 0x400 + kEdgeSpi, 0x20);
    appendMmioWrite64(words, kGicdBase + 0x6000 + 8u * kEdgeSpi, 0);
    appendMmioWrite32(words, kGicdBase + 0x0C08, 1u << 5); // SPI 34 edge-triggered.
    const uint64_t waitPc = kGuestStart + words.size() * 4;
    words.push_back(kNop);
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 8, kNop);
    writeWords(machine.ram, kGuestStart, words);
    installHandler(machine, 0x280,
        {mrs(gicv3_sysreg::kIar1El1, 0), msr(gicv3_sysreg::kEoir1El1, 0), kEret});

    CHECK(runUntilPc(machine, waitPc));
    CHECK(machine.devices.signalEdge(kEdgeSourceId, &error));
    CHECK(machine.gic->pending(kEdgeSpi));
    CHECK((machine.cpu.cpu().pstate() & kPstateI) != 0);
    CHECK(guestStep(machine)); // The masked guest instruction retires normally.
    CHECK(machine.gic->pending(kEdgeSpi));
    CHECK(guestStep(machine)); // Unmask; the edge is delivered at the next boundary.
    CHECK(runUntilAck(machine, kVectorBase + 0x280, kEdgeSpi));
    CHECK(machine.gic->active(kEdgeSpi));
    CHECK(!machine.gic->pending(kEdgeSpi));
    CHECK(guestStep(machine)); // EOI
    CHECK(guestStep(machine)); // ERET
    CHECK(!machine.gic->active(kEdgeSpi));
    CHECK(!machine.gic->pending(kEdgeSpi));
}

std::vector<uint8_t> copyRam(const ia64::Memory& ram) {
    return std::vector<uint8_t>(ram.GetRawData(),
                                ram.GetRawData() + ram.GetTotalSize());
}

std::string hexDigest(const std::array<uint8_t, 32>& digest) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (uint8_t byte : digest) stream << std::setw(2) << unsigned(byte);
    return stream.str();
}

void testProgramFActiveHandlerSnapshotReplay() {
    Machine source;
    source.addUart();
    std::vector<uint32_t> words;
    appendGuestMmuSetup(words);
    appendGicSetup(words, kGicdVa, kGicrVa,
        1u << AArch64GICv3::kPhysicalTimerPpi, true, 33, 0x20, 0x30);
    appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 12);
    appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
    const std::size_t unmask = words.size();
    appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
    words.insert(words.end(), 48, kNop);
    writeWords(source.ram, kGuestStart, words);
    installHandler(source, 0x280, timerHandler(false));

    CHECK(runUntilPc(source, kGuestStart + unmask * 4));
    const uint64_t unmaskNextPc = kGuestStart + (unmask + 1) * 4;
    CHECK(guestStep(source));
    CHECK_EQ(unmaskNextPc, source.cpu.cpu().pc());
    CHECK(runUntilAck(source, kVectorBase + 0x280, 30));
    const uint64_t interruptedPc = source.cpu.cpu().elrEl1();
    CHECK(interruptedPc >= unmaskNextPc);
    CHECK_EQ(5, source.cpu.cpu().spsrEl1());
    CHECK(source.gic->active(30));
    CHECK(source.gic->pending(30));
    CHECK_EQ(0x20, source.gic->runningPriority());
    const uint64_t timerCounter = source.timer->counter();
    const uint64_t timerComparator = source.timer->physicalComparator();
    CHECK_EQ(timerComparator + 1, timerCounter);

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram,
        source.devices, snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(kVectorBase + 0x284, info.savedPc);
    CHECK_EQ(info.savedPc, source.cpu.cpu().pc());
    const std::size_t snapshotSize = snapshot.size();
    CHECK(snapshotSize > 0);

    Machine target;
    target.addUart();
    CHECK(AArch64DeviceSnapshot::restore(target.cpu, target.ram,
        target.devices, snapshot, &error));
    CHECK_EQ(info.savedPc, target.cpu.cpu().pc());
    CHECK_EQ(interruptedPc, target.cpu.cpu().elrEl1());
    CHECK_EQ(5, target.cpu.cpu().spsrEl1());
    CHECK(target.gic->active(30));
    CHECK(target.gic->pending(30));
    CHECK_EQ(0x20, target.gic->runningPriority());
    CHECK_EQ(timerCounter, target.timer->counter());
    CHECK_EQ(timerComparator, target.timer->physicalComparator());
    CHECK_EQ(0, target.cpu.cpu().tlb().validEntryCount());
    CHECK_EQ(info.savedPc, target.cpu.cpu().pc()); // Restore itself did not inject an IRQ.

    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(source));
    for (unsigned i = 0; i < 4; ++i) CHECK(guestStep(target));
    CHECK_EQ(interruptedPc, source.cpu.cpu().pc());
    CHECK_EQ(interruptedPc, target.cpu.cpu().pc());
    CHECK(source.cpu.serialize_state() == target.cpu.serialize_state());
    CHECK(copyRam(source.ram) == copyRam(target.ram));
    CHECK(!source.gic->active(30) && !target.gic->active(30));
    CHECK(!source.gic->pending(30) && !target.gic->pending(30));
    CHECK_EQ(source.timer->counter(), target.timer->counter());
    CHECK_EQ(source.timer->physicalControl(), target.timer->physicalControl());
    CHECK(source.uart->pendingReceiveBytes().empty());
    CHECK(target.uart->pendingReceiveBytes().empty());

    std::vector<uint8_t> malformed = snapshot;
    const std::size_t gicPayload = findDevicePayload(malformed, 12);
    CHECK(gicPayload != 0 && gicPayload + 48 < malformed.size());
    malformed[gicPayload + 47] = 1; // Reserved GIC payload byte; keep the envelope digest valid.
    CHECK(digestSnapshot(malformed));
    CHECK(AArch64DeviceSnapshot::validate(malformed, nullptr, &error));
    Machine failureTarget(AArch64GICv3::Config{}, 19);
    failureTarget.addUart();
    failureTarget.cpu.cpu().writeX(7, 0x12345678);
    failureTarget.cpu.cpu().setPc(0x1800);
    const uint8_t marker[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    failureTarget.ram.Write(0x7000, marker, sizeof(marker));
    const auto baselineCpu = failureTarget.cpu.serialize_state();
    const auto baselineRam = copyRam(failureTarget.ram);
    const uint64_t baselineTimer = failureTarget.timer->counter();
    const bool baselineEnabled = failureTarget.gic->enabled(30);
    CHECK(!AArch64DeviceSnapshot::restore(failureTarget.cpu, failureTarget.ram,
        failureTarget.devices, malformed, &error));
    CHECK(failureTarget.cpu.serialize_state() == baselineCpu);
    CHECK(copyRam(failureTarget.ram) == baselineRam);
    CHECK_EQ(baselineTimer, failureTarget.timer->counter());
    CHECK_EQ(baselineEnabled, failureTarget.gic->enabled(30));
    CHECK(!failureTarget.gic->active(30));
    CHECK(failureTarget.uart->pendingReceiveBytes().empty());

    std::cout << "AARCH64-12 Program F: snapshot_bytes=" << snapshotSize
              << " sha256=" << hexDigest(info.sha256)
              << " interrupted_pc=0x" << std::hex << interruptedPc
              << " vector_pc=0x" << kVectorBase + 0x284
              << " restored_pc=0x" << info.savedPc
              << " final_pc=0x" << target.cpu.cpu().pc() << std::dec << '\n';
}

void testHundredLifecycleCyclesAndVmIsolation() {
#ifdef _WIN32
    uint64_t baselinePrivate = 0;
    uint64_t warmPrivate = 0;
    uint64_t previousBatchPrivate = 0;
    std::array<uint64_t, 3> batchPrivate{};
    CHECK(observePrivateBytes(baselinePrivate));
#endif
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        {
            Machine source(AArch64GICv3::Config{}, cycle * 100);
            source.addUart();
            Machine target(AArch64GICv3::Config{}, cycle * 100);
            target.addUart();

            std::vector<uint32_t> words;
            appendGicSetup(words, kGicdBase, kGicrBase,
                1u << AArch64GICv3::kPhysicalTimerPpi, true, 33, 0x20, 0x30);
            appendSysWrite(words, timer_sysreg::kCntpTvalEl0, 3);
            appendSysWrite(words, timer_sysreg::kCntpCtlEl0, 1);
            appendConstant(words, 20, kUartBase);
            words.push_back(movzW(2, 0x60));
            words.push_back(strW(2, 20, AArch64PL011::UARTLCR_H));
            words.push_back(movzW(2, 0x301));
            words.push_back(strW(2, 20, AArch64PL011::UARTCR));
            words.push_back(movzW(2, 0x10));
            words.push_back(strW(2, 20, AArch64PL011::UARTIMSC));
            const std::size_t unmaskMsr = words.size() + 1;
            appendSysWrite(words, syskey(3, 3, 4, 2, 1), 0);
            words.insert(words.end(), 16, kNop);
            writeWords(source.ram, kGuestStart, words);
            installHandler(source, 0x280,
                {mrs(gicv3_sysreg::kIar1El1, 0), msr(gicv3_sysreg::kEoir1El1, 0),
                 movzX(1, 0), msr(timer_sysreg::kCntpCtlEl0, 1), ldrW(2, 20), kEret});

            CHECK(runUntilPc(source, kGuestStart + unmaskMsr * 4));
            std::size_t accepted = 0;
            std::string error;
            CHECK(source.uart->injectReceive(std::vector<uint8_t>{'L'}, accepted, &error));
            CHECK_EQ(1, accepted);
            CHECK(guestStep(source)); // Clear DAIF.I.
            CHECK(runUntilAck(source, kVectorBase + 0x280, 30));
            const uint64_t interruptedPc = source.cpu.cpu().elrEl1();
            CHECK(source.gic->active(30));
            CHECK(source.gic->pending(30));
            CHECK(source.gic->pending(33));

            std::vector<uint8_t> snapshot;
            CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram,
                source.devices, snapshot, &error));
            const bool restored = AArch64DeviceSnapshot::restore(target.cpu, target.ram,
                target.devices, snapshot, &error);
            CHECK(restored);
            if (!restored) {
                std::cerr << "lifecycle restore failed at cycle " << cycle << ": "
                          << error << '\n';
                break;
            }
            CHECK_EQ(source.cpu.cpu().pc(), target.cpu.cpu().pc());
            CHECK_EQ(source.cpu.cpu().elrEl1(), target.cpu.cpu().elrEl1());
            CHECK(target.gic->active(30) && target.gic->pending(30));
            CHECK(target.gic->pending(33));

            for (unsigned i = 0; i < 5; ++i) CHECK(guestStep(source));
            for (unsigned i = 0; i < 5; ++i) CHECK(guestStep(target));
            CHECK_EQ(interruptedPc, source.cpu.cpu().pc());
            CHECK_EQ(interruptedPc, target.cpu.cpu().pc());
            CHECK(source.cpu.serialize_state() == target.cpu.serialize_state());
            CHECK(copyRam(source.ram) == copyRam(target.ram));
            CHECK(!source.gic->active(30) && !source.gic->pending(30));
            CHECK(!source.gic->pending(33) && !target.gic->pending(33));
            CHECK(source.uart->pendingReceiveBytes().empty());
            CHECK(target.uart->pendingReceiveBytes().empty());

            CHECK(target.timer->reset(&error));
            CHECK(target.gic->reset(&error));
            CHECK(target.uart->reset(&error));
            CHECK(target.devices.unregisterDevice(12, &error));
            CHECK(target.devices.unregisterDevice(11, &error));
            CHECK(target.devices.unregisterDevice(10, &error));
            CHECK_EQ(0, target.devices.deviceCount());
            CHECK_EQ(0, target.devices.interruptSourceCount());

            CHECK(source.timer->reset(&error));
            CHECK(source.gic->reset(&error));
            CHECK(source.uart->reset(&error));
            CHECK(source.devices.unregisterDevice(12, &error));
            CHECK(source.devices.unregisterDevice(11, &error));
            CHECK(source.devices.unregisterDevice(10, &error));
            CHECK_EQ(0, source.devices.deviceCount());
            CHECK_EQ(0, source.devices.interruptSourceCount());
        }
#ifdef _WIN32
        if (cycle == 9) {
            CHECK(observePrivateBytes(warmPrivate));
            previousBatchPrivate = warmPrivate;
        }
        if (cycle == 39 || cycle == 69 || cycle == 99) {
            const std::size_t batch = static_cast<std::size_t>((cycle - 39) / 30);
            CHECK(observePrivateBytes(batchPrivate[batch]));
            CHECK(batchPrivate[batch] <= previousBatchPrivate + 16ULL * 1024ULL * 1024ULL);
            previousBatchPrivate = batchPrivate[batch];
        }
#endif
    }
#ifdef _WIN32
    uint64_t finalPrivate = 0;
    CHECK(observePrivateBytes(finalPrivate));
    CHECK(finalPrivate <= warmPrivate + 32ULL * 1024ULL * 1024ULL);
    std::cout << "AARCH64-12 lifecycle: 100 isolated register/configure/IRQ/snapshot/restore/reset/unregister cycles passed"
              << " private_bytes=" << baselinePrivate << '/' << warmPrivate << '/'
              << batchPrivate[0] << ',' << batchPrivate[1] << ',' << batchPrivate[2]
              << '/' << finalPrivate << " bounded_growth_per_batch=16777216\n";
#else
    std::cout << "AARCH64-12 lifecycle: 100 isolated register/configure/IRQ/snapshot/restore/reset/unregister cycles passed"
              << " (process memory measurement unavailable on this platform)\n";
#endif

    Machine vmA(AArch64GICv3::Config{}, 10);
    Machine vmB(AArch64GICv3::Config{}, 900);
    std::string error;
    CHECK(vmA.devices.setLevel(201, true, &error));
    CHECK(vmA.gic->pending(30));
    CHECK(!vmB.gic->pending(30));
    CHECK_EQ(10, vmA.timer->counter());
    CHECK_EQ(900, vmB.timer->counter());
    CHECK(vmB.devices.setLevel(202, true, &error));
    CHECK(!vmA.gic->pending(27));
    CHECK(vmB.gic->pending(27));
}

} // namespace

int main() {
    try {
        testGicRegistersAndNegativeMmio();
        testSourceOwnerRemovalAndSnapshotTopology();
        testCpuInterfaceSpuriousAndInvalidEoi();
        testProgramAPhysicalTimerPpi();
        testProgramBVirtualAndSimultaneousPpis();
        testProgramCUartSpiThroughGuestMmu();
        testProgramDEl0IrqVectorAndPrivilege();
        testEl1SpEl0IrqVectorAndUnmappedVector();
        testProgramEMaskingPriorityAndLevelReassertion();
        testHigherPriorityNestedPreemptionGuestSavedReturnState();
        testEdgeTriggeredSpiRetainedWhileMasked();
        testProgramFActiveHandlerSnapshotReplay();
        testHundredLifecycleCyclesAndVmIsolation();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "AARCH64-12 UNCAUGHT: " << error.what() << '\n';
    } catch (...) {
        ++failures;
        std::cerr << "AARCH64-12 UNCAUGHT non-standard exception\n";
    }
    std::cout << "AARCH64-12 GICv3: " << checks << " assertions, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
