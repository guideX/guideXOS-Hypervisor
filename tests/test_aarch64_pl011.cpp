#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64MMU.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64PhysicalBus.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace aarch64;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, unsigned line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "AARCH64-10 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-10 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint64_t kUartBase = 0x09000000;
constexpr uint64_t kGuestStart = 0x1000;
constexpr uint32_t kNop = 0xD503201Fu;

uint32_t movzW(unsigned rd, uint16_t immediate) {
    return 0x52800000u | (static_cast<uint32_t>(immediate) << 5) | (rd & 31u);
}

uint32_t movzX(unsigned rd, uint16_t immediate, unsigned halfword = 0) {
    return 0xD2800000u | ((halfword & 3u) << 21) |
           (static_cast<uint32_t>(immediate) << 5) | (rd & 31u);
}

uint32_t movkX(unsigned rd, uint16_t immediate, unsigned halfword) {
    return 0xF2800000u | ((halfword & 3u) << 21) |
           (static_cast<uint32_t>(immediate) << 5) | (rd & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(movzX(rd, static_cast<uint16_t>(value), 0));
    for (unsigned halfword = 1; halfword < 4; ++halfword) {
        const auto part = static_cast<uint16_t>(value >> (halfword * 16));
        if (part != 0) words.push_back(movkX(rd, part, halfword));
    }
}

uint32_t strW(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xB9000000u | ((byteOffset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t ldrW(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xB9400000u | ((byteOffset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t strX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9000000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeSys(bool read, uint8_t op0, uint8_t op1, uint8_t crn,
                   uint8_t crm, uint8_t op2, uint8_t rt) {
    return (read ? 0xD5200000u : 0xD5000000u) |
           (static_cast<uint32_t>(op0) << 19) |
           (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | rt;
}

uint32_t cmpWImm(unsigned rn, uint16_t immediate) {
    return 0x7100001Fu | (static_cast<uint32_t>(immediate) << 10) |
           ((rn & 31u) << 5);
}

uint32_t andsWzrImmBit4(unsigned rn) {
    // ANDS WZR, Wn, #0x10 (TST alias), immr=28, imms=0.
    return 0x7200001Fu | (28u << 16) | ((rn & 31u) << 5);
}

uint32_t branchCond(unsigned condition, int32_t currentWord,
                    int32_t targetWord) {
    const uint32_t displacement = static_cast<uint32_t>(targetWord - currentWord) & 0x7FFFFu;
    return 0x54000000u | (displacement << 5) | (condition & 15u);
}

void writeWords(ia64::Memory& ram, uint64_t address,
                const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size() * 4);
    for (std::size_t n = 0; n < words.size(); ++n) {
        const uint32_t word = words[n];
        for (unsigned b = 0; b < 4; ++b) bytes[n * 4 + b] = static_cast<uint8_t>(word >> (8 * b));
    }
    ram.loadBuffer(address, bytes);
}

void set32le(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

class BlockingMmioDevice final : public AArch64StatefulDevice,
                                 public AArch64MMIODevice {
public:
    struct Token final : AArch64DeviceRestoreToken {};
    explicit BlockingMmioDevice(uint64_t id, uint64_t base = kUartBase,
                                uint64_t aperture = 0x1000,
                                uint64_t alignment = 0x1000,
                                uint32_t widths = 1u << 4)
        : id_(id), base_(base), aperture_(aperture), alignment_(alignment), widths_(widths) {}
    uint64_t instanceId() const noexcept override { return id_; }
    std::string typeId() const override { return "test.blocking_mmio"; }
    uint32_t stateSchemaVersion() const noexcept override { return 1; }
    std::size_t maxStateBytes() const noexcept override { return 1; }
    bool supportsSnapshot() const noexcept override { return true; }
    bool reset(std::string* error) override { if (error) error->clear(); return true; }
    bool quiesce(std::string* error) override { if (error) error->clear(); return true; }
    void resume() noexcept override {}
    bool captureState(std::vector<uint8_t>& state, std::string* error) const override {
        state.assign(1, 0); if (error) error->clear(); return true;
    }
    bool stageRestore(const uint8_t* state, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override {
        token.reset();
        if (state == nullptr || size != 1) { if (error) *error = "bad test state"; return false; }
        token = std::make_unique<Token>(); if (error) error->clear(); return true;
    }
    void commitRestore(std::unique_ptr<AArch64DeviceRestoreToken>) noexcept override {}
    uint64_t mmioBase() const noexcept override { return base_; }
    uint64_t mmioApertureBytes() const noexcept override { return aperture_; }
    uint64_t mmioAlignment() const noexcept override { return alignment_; }
    uint32_t supportedMmioWidths() const noexcept override { return widths_; }
    AArch64MmioAccessResult readMmio(uint64_t, uint8_t* bytes, std::size_t size) override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_; });
        std::fill_n(bytes, size, 0);
        return AArch64MmioAccessResult::Success;
    }
    AArch64MmioAccessResult writeMmio(uint64_t, const uint8_t*, std::size_t) override {
        return AArch64MmioAccessResult::Success;
    }
    void waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return entered_; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }
private:
    const uint64_t id_;
    const uint64_t base_;
    const uint64_t aperture_;
    const uint64_t alignment_;
    const uint32_t widths_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

uint32_t readReg(AArch64PhysicalBus& bus, uint64_t base, uint64_t offset) {
    uint8_t bytes[4]{};
    bus.Read(base + offset, bytes, sizeof(bytes));
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void writeReg(AArch64PhysicalBus& bus, uint64_t base, uint64_t offset,
              uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24)};
    bus.Write(base + offset, bytes, sizeof(bytes));
}

bool expectPhysicalFault(const std::function<void()>& action,
                         AArch64PhysicalFaultKind expected) {
    try {
        action();
    } catch (const AArch64PhysicalAccessFault& fault) {
        return fault.kind() == expected;
    } catch (...) {
        return false;
    }
    return false;
}

std::string hexDigest(const std::array<uint8_t, 32>& digest) {
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (uint8_t byte : digest) text << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}

struct Machine {
    ia64::Memory ram;
    AArch64DeviceRegistry devices;
    AArch64PhysicalBus bus;
    AArch64ISAPlugin cpu;
    AArch64PL011* uart = nullptr;

    explicit Machine(uint64_t deviceId = 1, uint64_t base = kUartBase,
                     uint64_t sourceId = 101, size_t ramBytes = 0x40000,
                     uint64_t entry = kGuestStart,
                     ExceptionLevel level = ExceptionLevel::EL0)
        : ram(ramBytes, false), devices(), bus(ram, devices), cpu(entry, level) {
        auto owned = std::make_unique<AArch64PL011>(devices, deviceId, base, sourceId);
        uart = owned.get();
        std::string error;
        if (!devices.registerDevice(std::move(owned), &error)) {
            throw std::runtime_error("PL011 registration failed: " + error);
        }
    }
};

bool guestStep(Machine& machine) {
    return machine.cpu.cpu().step(machine.bus) == Fault::None;
}

bool progressIfEnabled(Machine& machine, std::size_t bytes = 1) {
    const uint32_t control = readReg(machine.bus, machine.uart->physicalBase(), AArch64PL011::UARTCR);
    if ((control & 0x101u) != 0x101u) return true;
    std::size_t transmitted = 0;
    std::string error;
    return machine.uart->advanceTransmit(bytes, transmitted, &error);
}

std::vector<uint32_t> buildConsoleProgram(const std::string& message) {
    std::vector<uint32_t> words;
    appendConstant(words, 1, kUartBase);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x101));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    for (unsigned char ch : message) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    words.push_back(kNop);
    return words;
}

std::vector<uint32_t> buildEchoProgram(std::size_t& pollIndex) {
    std::vector<uint32_t> words;
    appendConstant(words, 1, kUartBase);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    pollIndex = words.size();
    words.push_back(ldrW(2, 1, AArch64PL011::UARTFR));
    words.push_back(andsWzrImmBit4(2));
    const int32_t branchIndex = static_cast<int32_t>(words.size());
    words.push_back(0);
    words.push_back(ldrW(3, 1, AArch64PL011::UARTDR));
    words.push_back(strW(3, 1, AArch64PL011::UARTDR));
    words.push_back(cmpWImm(3, '\n'));
    const int32_t loopBranchIndex = static_cast<int32_t>(words.size());
    words.push_back(0);
    words.push_back(kNop);
    words[static_cast<std::size_t>(branchIndex)] = branchCond(1, branchIndex,
        static_cast<int32_t>(pollIndex)); // B.NE when RXFE is set.
    words[static_cast<std::size_t>(loopBranchIndex)] = branchCond(1, loopBranchIndex,
        static_cast<int32_t>(pollIndex)); // B.NE until the newline is echoed.
    return words;
}

uint64_t pageDescriptor(uint64_t pa, uint8_t attributeIndex) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(attributeIndex) << 2) |
           (3ULL << 8) | (1ULL << 10);
}

void appendGuestMmuSetup(std::vector<uint32_t>& words) {
    // Guest-built 4 KiB L1/L2/L3 tables: VA 0x1000 -> code PA 0x1000,
    // and VA 0x20000000 -> PL011 PA 0x09000000 as Device-nGnRnE.
    appendConstant(words, 20, 0x10000); // L1 table
    appendConstant(words, 21, 0x11000); // L2 table
    appendConstant(words, 22, 0x12000); // code L3 table
    appendConstant(words, 23, 0x13000); // device L3 table
    appendConstant(words, 24, 0x11003);
    words.push_back(strX(24, 20, 0)); // L1[0] -> L2
    appendConstant(words, 24, 0x12003);
    words.push_back(strX(24, 21, 0)); // L2[0] -> code L3
    appendConstant(words, 24, 0x13003);
    words.push_back(strX(24, 21, 256 * 8)); // L2[256] -> UART L3
    appendConstant(words, 24, pageDescriptor(0x1000, 0));
    words.push_back(strX(24, 22, 8));
    appendConstant(words, 24, pageDescriptor(kUartBase, 1));
    words.push_back(strX(24, 23, 0));
    appendConstant(words, 2, 25 | (25ULL << 16) | (2ULL << 30));
    words.push_back(encodeSys(false, 3, 0, 2, 0, 2, 2)); // TCR_EL1
    appendConstant(words, 3, 0x10000);
    words.push_back(encodeSys(false, 3, 0, 2, 0, 0, 3)); // TTBR0_EL1
    appendConstant(words, 4, 0xFF); // Attr0 Normal WB, Attr1 Device-nGnRnE.
    words.push_back(encodeSys(false, 3, 0, 10, 2, 0, 4)); // MAIR_EL1
    appendConstant(words, 5, 1);
    words.push_back(encodeSys(false, 3, 0, 1, 0, 0, 5)); // SCTLR_EL1.M
    words.push_back(0xD5033A9Fu); // DSB ISHST
    words.push_back(0xD5033FDFu); // ISB
}

std::vector<uint32_t> buildTranslatedProgram() {
    std::vector<uint32_t> words;
    appendGuestMmuSetup(words);
    appendConstant(words, 1, 0x20000000);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x101));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    const std::string message = "MMU UART\n";
    for (unsigned char ch : message) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    words.push_back(kNop);
    return words;
}

std::vector<uint32_t> buildReplayProgram(std::size_t& savedWord,
                                         std::size_t& endWord) {
    std::vector<uint32_t> words;
    appendGuestMmuSetup(words);
    appendConstant(words, 1, 0x20000000);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    words.push_back(movzW(2, 0x0A));
    words.push_back(strW(2, 1, AArch64PL011::UARTIFLS));
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 1, AArch64PL011::UARTIMSC));
    for (unsigned char ch : std::string("PRE")) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    for (unsigned char ch : std::string("SNAP")) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    savedWord = words.size();
    for (unsigned char ch : std::string("SHOT")) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    for (unsigned reg = 4; reg < 12; ++reg) words.push_back(ldrW(reg, 1, AArch64PL011::UARTDR));
    words.push_back(kNop);
    endWord = words.size();
    return words;
}

bool executeRange(Machine& machine, uint64_t count, bool progress) {
    for (uint64_t i = 0; i < count; ++i) {
        if (!guestStep(machine)) return false;
        if (progress && !progressIfEnabled(machine)) return false;
    }
    return true;
}

void testRegistryAndAccessValidation() {
    Machine machine;
    CHECK_EQ(1, machine.devices.deviceCount());
    CHECK_EQ(1, machine.devices.interruptSourceCount());
    CHECK_EQ(0x90, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR));
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTDR));
    CHECK_EQ(0x300, readReg(machine.bus, kUartBase, AArch64PL011::UARTCR));
    CHECK_EQ(0x12, readReg(machine.bus, kUartBase, AArch64PL011::UARTIFLS));
    CHECK_EQ(0x11, readReg(machine.bus, kUartBase, AArch64PL011::UARTPeriphID0));
    CHECK_EQ(0x10, readReg(machine.bus, kUartBase, AArch64PL011::UARTPeriphID0 + 4));
    CHECK_EQ(0x34, readReg(machine.bus, kUartBase, AArch64PL011::UARTPeriphID0 + 8));
    CHECK_EQ(0x00, readReg(machine.bus, kUartBase, AArch64PL011::UARTPeriphID0 + 12));
    CHECK_EQ(0x0D, readReg(machine.bus, kUartBase, AArch64PL011::UARTPCellID0));
    CHECK_EQ(0xF0, readReg(machine.bus, kUartBase, AArch64PL011::UARTPCellID0 + 4));
    CHECK_EQ(0x05, readReg(machine.bus, kUartBase, AArch64PL011::UARTPCellID0 + 8));
    CHECK_EQ(0xB1, readReg(machine.bus, kUartBase, AArch64PL011::UARTPCellID0 + 12));

    uint8_t data[8]{};
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase + 1, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase, data, 2); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase, data, 8); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase - 4, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase + 0x1000, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(UINT64_MAX - 1, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(0x0A000000, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase + AArch64PL011::UARTICR, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Write(kUartBase + AArch64PL011::UARTFR, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Write(kUartBase + AArch64PL011::UARTPeriphID0, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(expectPhysicalFault([&] { machine.bus.Write(kUartBase + 0x10, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));

    std::string error;
    CHECK(!machine.devices.removeInterruptSource(101, &error));
    CHECK_EQ(1, machine.devices.interruptSourceCount());
    CHECK(!machine.devices.registerInterruptSource(700, 999, AArch64InterruptMode::Level, &error));

    auto misaligned = std::make_unique<AArch64PL011>(machine.devices, 10, kUartBase + 4, 110);
    CHECK(!machine.devices.registerDevice(std::move(misaligned), &error));
    auto wrapping = std::make_unique<AArch64PL011>(machine.devices, 11,
        0xFFFFFFFFFFFFF000ULL, 111);
    CHECK(!machine.devices.registerDevice(std::move(wrapping), &error));
    auto duplicateSource = std::make_unique<AArch64PL011>(machine.devices, 12,
        kUartBase + 0x1000, 101);
    CHECK(!machine.devices.registerDevice(std::move(duplicateSource), &error));
    CHECK_EQ(1, machine.devices.deviceCount());

    auto zeroAperture = std::make_unique<BlockingMmioDevice>(13, 0x09100000, 0, 0x1000);
    CHECK(!machine.devices.registerDevice(std::move(zeroAperture), &error));
    auto oversizedAperture = std::make_unique<BlockingMmioDevice>(14, 0x09100000,
        kMaxAArch64MmioRegionBytes + 1, 0x1000);
    CHECK(!machine.devices.registerDevice(std::move(oversizedAperture), &error));
    auto invalidAlignment = std::make_unique<BlockingMmioDevice>(15, 0x09100000,
        0x1000, 3);
    CHECK(!machine.devices.registerDevice(std::move(invalidAlignment), &error));
    auto noWidths = std::make_unique<BlockingMmioDevice>(16, 0x09100000,
        0x1000, 0x1000, 0);
    CHECK(!machine.devices.registerDevice(std::move(noWidths), &error));
    auto zeroId = std::make_unique<BlockingMmioDevice>(0, 0x09100000);
    CHECK(!machine.devices.registerDevice(std::move(zeroId), &error));
    auto duplicateId = std::make_unique<BlockingMmioDevice>(1, 0x09100000);
    CHECK(!machine.devices.registerDevice(std::move(duplicateId), &error));
    auto missingUartSource = std::make_unique<AArch64PL011>(machine.devices,
        17, 0x09100000, 0);
    CHECK(!machine.devices.registerDevice(std::move(missingUartSource), &error));
    CHECK_EQ(1, machine.devices.deviceCount());

    bool differentRamRejected = false;
    try {
        ia64::Memory otherRam(0x40000, false);
        AArch64PhysicalBus invalidBus(otherRam, machine.devices);
    } catch (const std::invalid_argument&) {
        differentRamRejected = true;
    }
    CHECK(differentRamRejected);

    CHECK(machine.uart->quiesce(&error));
    std::vector<uint8_t> goodState;
    CHECK(machine.uart->captureState(goodState, &error));
    machine.uart->resume();
    std::unique_ptr<AArch64DeviceRestoreToken> token;
    auto badSchema = goodState;
    set32le(badSchema, 4, 2);
    CHECK(!machine.uart->stageRestore(badSchema.data(), badSchema.size(), token, &error));
    CHECK(token == nullptr);
    auto badRegisterState = goodState;
    set32le(badRegisterState, 20, 1); // Unsupported BRK bit in LCR_H.
    CHECK(!machine.uart->stageRestore(badRegisterState.data(), badRegisterState.size(), token, &error));
    CHECK(token == nullptr);
    auto badCount = goodState;
    set32le(badCount, 44, static_cast<uint32_t>(AArch64PL011::kFifoDepth + 1));
    CHECK(!machine.uart->stageRestore(badCount.data(), badCount.size(), token, &error));
    CHECK(token == nullptr);

    auto overlaps = std::make_unique<AArch64PL011>(machine.devices, 2, 0x8000, 102);
    CHECK(!machine.devices.registerDevice(std::move(overlaps), &error));
    CHECK(machine.devices.deviceCount() == 1);
    auto duplicateRegion = std::make_unique<AArch64PL011>(machine.devices, 3, kUartBase, 103);
    CHECK(!machine.devices.registerDevice(std::move(duplicateRegion), &error));
    CHECK(machine.devices.deviceCount() == 1);

    std::vector<uint8_t> malformed(56, 0);
    CHECK(!machine.uart->stageRestore(malformed.data(), malformed.size(), token, &error));
    CHECK(token == nullptr);

    CHECK(machine.devices.unregisterDevice(1, &error));
    CHECK_EQ(0, machine.devices.deviceCount());
    CHECK_EQ(0, machine.devices.interruptSourceCount());
    CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase, data, 4); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    auto replacement = std::make_unique<AArch64PL011>(machine.devices, 1, kUartBase, 101);
    CHECK(machine.devices.registerDevice(std::move(replacement), &error));
    CHECK_EQ(1, machine.devices.deviceCount());
    CHECK_EQ(1, machine.devices.interruptSourceCount());
}

void testControlFifosAndInterrupts() {
    Machine machine;
    std::string error;
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70); // FEN + 8 data bits.
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x101);   // UART + TX only.
    for (uint32_t i = 0; i < AArch64PL011::kFifoDepth; ++i) {
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'a' + i);
    }
    CHECK_EQ(0x28, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR) & 0x28u);
    const auto fullTx = machine.uart->pendingTransmitBytes();
    CHECK_EQ(AArch64PL011::kFifoDepth, fullTx.size());
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'z'); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK(machine.uart->pendingTransmitBytes() == fullTx);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 1); // Hold data with TX disabled.
    std::size_t moved = 0;
    CHECK(!machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0, moved);
    CHECK(machine.uart->pendingTransmitBytes() == fullTx);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x101);
    CHECK(machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(1, moved);
    CHECK_EQ('a', machine.uart->outputBytes().front());
    CHECK_EQ(AArch64PL011::kFifoDepth - 1, machine.uart->pendingTransmitBytes().size());
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0);
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK_EQ(AArch64PL011::kFifoDepth - 1, machine.uart->pendingTransmitBytes().size());
    CHECK(machine.devices.resetAll(&error));
    CHECK_EQ(0x300, readReg(machine.bus, kUartBase, AArch64PL011::UARTCR));
    CHECK_EQ(0x90, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR));
    CHECK(machine.uart->pendingTransmitBytes().empty());
    CHECK_EQ(1, machine.uart->outputBytes().size()); // Reset retains local output history.

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIFLS, 0x0A); // RX threshold 8/32.
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x10);
    const std::vector<uint8_t> input(8, 'R');
    std::size_t accepted = 0;
    CHECK(machine.uart->injectReceive(input, accepted, &error));
    CHECK_EQ(8, accepted);
    CHECK_EQ(0x10, readReg(machine.bus, kUartBase, AArch64PL011::UARTRIS) & 0x10u);
    CHECK_EQ(0x10, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS) & 0x10u);
    AArch64InterruptSourceState source;
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(source.asserted);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0);
    CHECK_EQ(0x10, readReg(machine.bus, kUartBase, AArch64PL011::UARTRIS) & 0x10u);
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS));
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(!source.asserted);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x10);
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(source.asserted);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTICR, 0x10);
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS));
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(!source.asserted);

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x400);
    std::vector<uint8_t> tooMany(AArch64PL011::kFifoDepth - 8 + 1, 'X');
    CHECK(!machine.uart->injectReceive(tooMany, accepted, &error));
    CHECK_EQ(AArch64PL011::kFifoDepth - 8, accepted);
    CHECK_EQ(0x8, readReg(machine.bus, kUartBase, AArch64PL011::UARTRSR));
    CHECK_EQ(0x400, readReg(machine.bus, kUartBase, AArch64PL011::UARTRIS) & 0x400u);
    CHECK_EQ(0x400, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS) & 0x400u);
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(source.asserted);

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x101); // RX off; buffered bytes remain.
    CHECK_EQ(AArch64PL011::kFifoDepth, machine.uart->pendingReceiveBytes().size());
    CHECK(!machine.uart->injectReceive(input, accepted, &error));
    CHECK_EQ(0, accepted);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    for (std::size_t i = 0; i < AArch64PL011::kFifoDepth; ++i)
        (void)readReg(machine.bus, kUartBase, AArch64PL011::UARTDR);
    CHECK(machine.uart->pendingReceiveBytes().empty());
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTICR, 0x400);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTRSR, 0);
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTRSR));
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS));

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x20);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'T');
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);
    CHECK(machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0x20, readReg(machine.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(source.asserted);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTICR, 0x20);
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTMIS));
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(!source.asserted);

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0);
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTILPR, 1); },
                              AArch64PhysicalFaultKind::UnsupportedOperation));
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTDMACR, 1); },
                              AArch64PhysicalFaultKind::UnsupportedOperation));
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x181); },
                              AArch64PhysicalFaultKind::UnsupportedOperation));
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x71); },
                              AArch64PhysicalFaultKind::UnsupportedOperation));

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIFLS, 0x0A);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x10);
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>(8, 'Q'), accepted, &error));
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(source.asserted);
    CHECK(machine.devices.resetAll(&error));
    CHECK(machine.devices.queryInterruptSource(101, source, &error));
    CHECK(!source.asserted);
    CHECK(machine.uart->pendingReceiveBytes().empty());
    CHECK(machine.uart->pendingTransmitBytes().empty());
    CHECK_EQ(0x90, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR));
}

void testHoldingRegisterModeAndBaudRegisters() {
    Machine machine(20, kUartBase, 120);
    std::string error;
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIBRD, 0x30);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTFBRD, 0x15);
    CHECK_EQ(0x30, readReg(machine.bus, kUartBase, AArch64PL011::UARTIBRD));
    CHECK_EQ(0x15, readReg(machine.bus, kUartBase, AArch64PL011::UARTFBRD));
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x60); // 8-bit, FIFO off.
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTIBRD, 0x40); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    CHECK_EQ(0x30, readReg(machine.bus, kUartBase, AArch64PL011::UARTIBRD));
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR) & 0x28u);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'A');
    CHECK_EQ(0x28, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR) & 0x28u);
    CHECK(expectPhysicalFault([&] { writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'B'); },
                              AArch64PhysicalFaultKind::ExternalAbort));
    std::size_t moved = 0;
    CHECK(machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(1, moved);
    CHECK(machine.uart->outputBytes() == std::vector<uint8_t>({'A'}));
    CHECK_EQ(0x90, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR));

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 1); // UART enabled, TX disabled.
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 'T');
    CHECK(!machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0, moved);
    CHECK(machine.uart->outputBytes() == std::vector<uint8_t>({'A'}));
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0); // UART disabled.
    CHECK(!machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0, moved);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x101);
    CHECK(machine.uart->advanceTransmit(1, moved, &error));
    CHECK(machine.uart->outputBytes() == std::vector<uint8_t>({'A', 'T'}));

    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    std::size_t accepted = 0;
    CHECK(!machine.uart->injectReceive(std::vector<uint8_t>{'R', 'S'}, accepted, &error));
    CHECK_EQ(1, accepted);
    CHECK_EQ(0x40, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR) & 0x40u);
    CHECK_EQ('R', readReg(machine.bus, kUartBase, AArch64PL011::UARTDR) & 0xFFu);
    CHECK_EQ(0x8, readReg(machine.bus, kUartBase, AArch64PL011::UARTRSR));
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTRSR, 0);
    CHECK_EQ(0, readReg(machine.bus, kUartBase, AArch64PL011::UARTRSR));
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>{'Z'}, accepted, &error));
    CHECK_EQ(1, machine.uart->pendingReceiveBytes().size());
    CHECK(machine.devices.resetAll(&error));
    CHECK(machine.uart->pendingReceiveBytes().empty());
    CHECK(machine.uart->outputBytes() == std::vector<uint8_t>({'A', 'T'}));
}

void testFifoInterruptTransitionsAndOutputBound() {
    Machine threshold(21, kUartBase, 121);
    std::string error;
    writeReg(threshold.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
    writeReg(threshold.bus, kUartBase, AArch64PL011::UARTCR, 0x101);
    writeReg(threshold.bus, kUartBase, AArch64PL011::UARTIMSC, 0x20);
    writeReg(threshold.bus, kUartBase, AArch64PL011::UARTIFLS, 0x12); // TX threshold 16/32.
    for (std::size_t i = 0; i < AArch64PL011::kFifoDepth; ++i) {
        writeReg(threshold.bus, kUartBase, AArch64PL011::UARTDR, static_cast<uint32_t>('a' + i));
    }
    CHECK_EQ(0, readReg(threshold.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);
    std::size_t moved = 0;
    CHECK(threshold.uart->advanceTransmit(AArch64PL011::kFifoDepth / 2, moved, &error));
    CHECK_EQ(AArch64PL011::kFifoDepth / 2, moved);
    CHECK_EQ(0x20, readReg(threshold.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);
    CHECK_EQ(0x20, readReg(threshold.bus, kUartBase, AArch64PL011::UARTMIS) & 0x20u);
    writeReg(threshold.bus, kUartBase, AArch64PL011::UARTDR, 'x');
    CHECK_EQ(0, readReg(threshold.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);
    CHECK(threshold.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0x20, readReg(threshold.bus, kUartBase, AArch64PL011::UARTRIS) & 0x20u);

    Machine bounded(22, kUartBase, 122);
    writeReg(bounded.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
    writeReg(bounded.bus, kUartBase, AArch64PL011::UARTCR, 0x101);
    for (std::size_t block = 0; block < AArch64PL011::kMaxOutputBytes /
                                      AArch64PL011::kFifoDepth; ++block) {
        for (std::size_t i = 0; i < AArch64PL011::kFifoDepth; ++i) {
            writeReg(bounded.bus, kUartBase, AArch64PL011::UARTDR,
                     static_cast<uint32_t>(block & 0xFFu));
        }
        CHECK(bounded.uart->advanceTransmit(AArch64PL011::kFifoDepth, moved, &error));
        CHECK_EQ(AArch64PL011::kFifoDepth, moved);
    }
    CHECK_EQ(AArch64PL011::kMaxOutputBytes, bounded.uart->outputBytes().size());
    writeReg(bounded.bus, kUartBase, AArch64PL011::UARTDR, 'Z');
    CHECK(!bounded.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(0, moved);
    CHECK(bounded.uart->pendingTransmitBytes() == std::vector<uint8_t>({'Z'}));
    CHECK_EQ(AArch64PL011::kMaxOutputBytes, bounded.uart->outputBytes().size());
}

void testProgramAConsoleOutput() {
    Machine machine;
    const std::string expected = "guideXOS AArch64 UART OK\n";
    const auto words = buildConsoleProgram(expected);
    writeWords(machine.ram, kGuestStart, words);
    const auto initialRam = std::vector<uint8_t>(machine.ram.GetRawData(),
        machine.ram.GetRawData() + machine.ram.GetTotalSize());
    uint64_t executed = 0;
    for (; executed < words.size(); ++executed) {
        CHECK(guestStep(machine));
        CHECK(progressIfEnabled(machine));
    }
    CHECK_EQ(words.size(), executed);
    CHECK_EQ(kGuestStart + words.size() * 4, machine.cpu.getPC());
    const auto output = machine.uart->outputBytes();
    CHECK(output == std::vector<uint8_t>(expected.begin(), expected.end()));
    CHECK_EQ(expected.size(), output.size());
    CHECK_EQ(1u, readReg(machine.bus, kUartBase, AArch64PL011::UARTCR) & 1u);
    CHECK_EQ(0x80, readReg(machine.bus, kUartBase, AArch64PL011::UARTFR) & 0x80u);
    CHECK(std::equal(initialRam.begin(), initialRam.end(), machine.ram.GetRawData()));
    std::cout << "AARCH64-10 Program A: guest instructions=" << executed
              << " output=guideXOS AArch64 UART OK\\n\n";
}

void testProgramBEcho() {
    Machine machine;
    std::size_t pollIndex = 0;
    const auto words = buildEchoProgram(pollIndex);
    writeWords(machine.ram, kGuestStart, words);
    for (std::size_t i = 0; i <= pollIndex - 1; ++i) {
        CHECK(guestStep(machine));
        CHECK(progressIfEnabled(machine));
    }
    const std::string input = "ARM64 ready\n";
    std::size_t accepted = 0;
    std::string error;
    CHECK(machine.uart->injectReceive(
        reinterpret_cast<const uint8_t*>(input.data()), input.size(), accepted, &error));
    CHECK_EQ(input.size(), accepted);
    uint64_t guestCount = pollIndex;
    const uint64_t maximumSteps = 256;
    while (machine.cpu.getPC() < kGuestStart + words.size() * 4 && guestCount < maximumSteps) {
        CHECK(guestStep(machine));
        CHECK(progressIfEnabled(machine));
        ++guestCount;
    }
    CHECK_EQ(kGuestStart + words.size() * 4, machine.cpu.getPC());
    CHECK_EQ(6 + input.size() * 7 + 1, guestCount);
    const auto output = machine.uart->outputBytes();
    CHECK(output == std::vector<uint8_t>(input.begin(), input.end()));
    CHECK(machine.uart->pendingReceiveBytes().empty());
    CHECK(machine.uart->pendingTransmitBytes().empty());
    std::cout << "AARCH64-10 Program B: guest instructions=" << guestCount
              << " input/echo bytes=" << input.size()
              << " output=ARM64 ready\\n\n";
}

void testProgramCTranslatedMmio() {
    Machine machine(4, kUartBase, 104, 0x40000, kGuestStart, ExceptionLevel::EL1);
    const auto words = buildTranslatedProgram();
    writeWords(machine.ram, kGuestStart, words);
    std::size_t transmitted = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
        CHECK(guestStep(machine));
        CHECK(progressIfEnabled(machine));
        ++transmitted;
    }
    CHECK_EQ(words.size(), transmitted);
    CHECK_EQ(kGuestStart + words.size() * 4, machine.cpu.getPC());
    const std::string expected = "MMU UART\n";
    CHECK(machine.uart->outputBytes() == std::vector<uint8_t>(expected.begin(), expected.end()));
    CHECK_EQ(1, machine.cpu.cpu().effectiveSctlrEl1() & 1u);
    const MMUResult mapping = AArch64MMU::translate(machine.bus, 0x20000000, 1,
        MMUAccess::Write, machine.cpu.cpu().effectiveTcrEl1(),
        machine.cpu.cpu().effectiveTtbr0El1(), machine.cpu.cpu().effectiveTtbr1El1(),
        machine.cpu.cpu().effectiveMairEl1(), false);
    CHECK(mapping.success);
    CHECK_EQ(kUartBase, mapping.physicalAddress);
    CHECK_EQ(0, mapping.attributes.memoryAttribute);
    CHECK(machine.cpu.cpu().tlb().stats().tableWalks > 0);
    CHECK_EQ(0, machine.ram.read<uint64_t>(0x8000));
    std::cout << "AARCH64-10 Program C: guest instructions=" << transmitted
              << " UART VA=0x20000000 -> PA=0x" << std::hex << mapping.physicalAddress
              << " MAIR=0x" << static_cast<unsigned>(mapping.attributes.memoryAttribute)
              << std::dec << " output=MMU UART\\n\n";
}

void testProgramDInterruptSource() {
    Machine machine(5, kUartBase, 105);
    std::vector<uint32_t> words;
    appendConstant(words, 1, kUartBase);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x301));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    words.push_back(movzW(2, 0x0A));
    words.push_back(strW(2, 1, AArch64PL011::UARTIFLS));
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 1, AArch64PL011::UARTIMSC));
    words.push_back(ldrW(4, 1, AArch64PL011::UARTRIS));
    words.push_back(ldrW(5, 1, AArch64PL011::UARTMIS));
    words.push_back(movzW(2, 0x10));
    words.push_back(strW(2, 1, AArch64PL011::UARTICR));
    words.push_back(ldrW(6, 1, AArch64PL011::UARTRIS));
    words.push_back(ldrW(7, 1, AArch64PL011::UARTMIS));
    words.push_back(kNop);
    writeWords(machine.ram, kGuestStart, words);
    for (std::size_t i = 0; i < 10; ++i) CHECK(guestStep(machine));
    std::size_t accepted = 0;
    std::string error;
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>(8, 'I'), accepted, &error));
    CHECK_EQ(8, accepted);
    AArch64InterruptSourceState source;
    CHECK(machine.devices.queryInterruptSource(105, source, &error));
    CHECK(source.ownerDeviceId == 5 && source.mode == AArch64InterruptMode::Level && source.asserted);
    for (std::size_t i = 10; i < words.size(); ++i) CHECK(guestStep(machine));
    CHECK_EQ(0x10, machine.cpu.cpu().readX(4) & 0x10u);
    CHECK_EQ(0x10, machine.cpu.cpu().readX(5) & 0x10u);
    CHECK_EQ(0, machine.cpu.cpu().readX(6));
    CHECK_EQ(0, machine.cpu.cpu().readX(7));
    CHECK(machine.devices.queryInterruptSource(105, source, &error));
    CHECK(!source.asserted);
    CHECK_EQ(kGuestStart + words.size() * 4, machine.cpu.getPC());
    CHECK(machine.cpu.cpu().exceptionLevel() == ExceptionLevel::EL0); // No CPU IRQ delivery was introduced.
    std::cout << "AARCH64-10 Program D: guest instructions=" << words.size()
              << " RX threshold=8 RIS/MIS=0x10, ICR-cleared RIS/MIS=0x0"
              << " owned level source asserted then deasserted; CPU IRQ delivery=none\n";
}

void testProgramEDeviceSnapshotReplay() {
    Machine source(6, kUartBase, 106, 0x40000, kGuestStart, ExceptionLevel::EL1);
    std::size_t savedWord = 0, endWord = 0;
    const auto words = buildReplayProgram(savedWord, endWord);
    writeWords(source.ram, kGuestStart, words);
    std::string error;
    CHECK(executeRange(source, savedWord - 8, true)); // Configure and emit PRE.
    std::size_t accepted = 0;
    CHECK(source.uart->injectReceive(std::vector<uint8_t>{'A','B','C','D','E','F','G','H'},
                                     accepted, &error));
    CHECK_EQ(8, accepted);
    CHECK(executeRange(source, 8, false)); // Four pending TX bytes; stop at saved boundary.
    CHECK_EQ(kGuestStart + savedWord * 4, source.cpu.getPC());
    CHECK(source.uart->outputBytes() == std::vector<uint8_t>({'P','R','E'}));
    CHECK(source.uart->pendingTransmitBytes() == std::vector<uint8_t>({'S','N','A','P'}));
    CHECK(source.uart->pendingReceiveBytes() == std::vector<uint8_t>({'A','B','C','D','E','F','G','H'}));
    AArch64InterruptSourceState sourceIrq;
    CHECK(source.devices.queryInterruptSource(106, sourceIrq, &error));
    CHECK(sourceIrq.asserted);

    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices,
                                         snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(2, info.envelopeVersion);
    CHECK_EQ(1, info.deviceCount);
    CHECK_EQ(1, info.interruptSourceCount);
    CHECK_EQ(79, info.totalDeviceStateBytes);
    const uint64_t savedPc = info.savedPc;
    CHECK_EQ(kGuestStart + savedWord * 4, savedPc);

    CHECK(executeRange(source, endWord - savedWord, false));
    std::size_t sourceMoved = 0;
    CHECK(source.uart->advanceTransmit(32, sourceMoved, &error));
    CHECK_EQ(8, sourceMoved);
    const auto sourceOutput = source.uart->outputBytes();
    const uint64_t finalPc = source.cpu.getPC();
    const auto sourceCpu = source.cpu.serialize_state();
    const std::vector<uint8_t> sourceRam(source.ram.GetRawData(),
        source.ram.GetRawData() + source.ram.GetTotalSize());

    Machine restored(6, kUartBase, 106, 0x40000, kGuestStart, ExceptionLevel::EL1);
    CHECK(AArch64DeviceSnapshot::restore(restored.cpu, restored.ram,
                                         restored.devices, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu.getPC());
    CHECK(restored.uart->outputBytes() == std::vector<uint8_t>({'P','R','E'}));
    CHECK(restored.uart->pendingTransmitBytes() == std::vector<uint8_t>({'S','N','A','P'}));
    CHECK(restored.uart->pendingReceiveBytes() == std::vector<uint8_t>({'A','B','C','D','E','F','G','H'}));
    CHECK_EQ(1u, restored.cpu.cpu().effectiveSctlrEl1() & 1u);
    CHECK_EQ(0x70, readReg(restored.bus, kUartBase, AArch64PL011::UARTLCR_H));
    CHECK_EQ(0x301, readReg(restored.bus, kUartBase, AArch64PL011::UARTCR));
    CHECK_EQ(0x0A, readReg(restored.bus, kUartBase, AArch64PL011::UARTIFLS));
    CHECK_EQ(0x10, readReg(restored.bus, kUartBase, AArch64PL011::UARTIMSC));
    CHECK_EQ(0x30, readReg(restored.bus, kUartBase, AArch64PL011::UARTRIS));
    CHECK_EQ(0x10, readReg(restored.bus, kUartBase, AArch64PL011::UARTMIS));
    CHECK(restored.devices.queryInterruptSource(106, sourceIrq, &error));
    CHECK(sourceIrq.asserted);
    CHECK(executeRange(restored, endWord - savedWord, false));
    std::size_t restoredMoved = 0;
    CHECK(restored.uart->advanceTransmit(32, restoredMoved, &error));
    CHECK_EQ(sourceMoved, restoredMoved);
    CHECK_EQ(finalPc, restored.cpu.getPC());
    CHECK(restored.uart->outputBytes() == sourceOutput);
    CHECK(restored.cpu.serialize_state() == sourceCpu);
    CHECK(std::equal(sourceRam.begin(), sourceRam.end(), restored.ram.GetRawData()));

    std::vector<uint8_t> finalSource, finalRestored;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices,
                                         finalSource, &error));
    CHECK(AArch64DeviceSnapshot::capture(restored.cpu, restored.ram, restored.devices,
                                         finalRestored, &error));
    CHECK(finalSource == finalRestored);
    const MMUResult replayMapping = AArch64MMU::translate(restored.bus, 0x20000000, 1,
        MMUAccess::Write, restored.cpu.cpu().effectiveTcrEl1(),
        restored.cpu.cpu().effectiveTtbr0El1(), restored.cpu.cpu().effectiveTtbr1El1(),
        restored.cpu.cpu().effectiveMairEl1(), false);
    CHECK(replayMapping.success);
    CHECK_EQ(kUartBase, replayMapping.physicalAddress);

    Machine wrongIdentity(7, kUartBase, 107, 0x40000, kGuestStart, ExceptionLevel::EL1);
    const auto beforeWrongRestoreCpu = wrongIdentity.cpu.serialize_state();
    const std::vector<uint8_t> beforeWrongRestoreRam(wrongIdentity.ram.GetRawData(),
        wrongIdentity.ram.GetRawData() + wrongIdentity.ram.GetTotalSize());
    const auto beforeWrongRestoreOutput = wrongIdentity.uart->outputBytes();
    const auto beforeWrongRestoreTx = wrongIdentity.uart->pendingTransmitBytes();
    const auto beforeWrongRestoreRx = wrongIdentity.uart->pendingReceiveBytes();
    CHECK(!AArch64DeviceSnapshot::restore(wrongIdentity.cpu, wrongIdentity.ram,
        wrongIdentity.devices, snapshot, &error));
    CHECK(wrongIdentity.cpu.serialize_state() == beforeWrongRestoreCpu);
    CHECK(std::equal(beforeWrongRestoreRam.begin(), beforeWrongRestoreRam.end(),
                     wrongIdentity.ram.GetRawData()));
    CHECK(wrongIdentity.uart->outputBytes() == beforeWrongRestoreOutput);
    CHECK(wrongIdentity.uart->pendingTransmitBytes() == beforeWrongRestoreTx);
    CHECK(wrongIdentity.uart->pendingReceiveBytes() == beforeWrongRestoreRx);
    AArch64InterruptSourceState wrongIdentitySource;
    CHECK(wrongIdentity.devices.queryInterruptSource(107, wrongIdentitySource, &error));
    CHECK(!wrongIdentitySource.asserted && wrongIdentitySource.ownerDeviceId == 7);

    Machine wrongSource(6, kUartBase, 107, 0x40000, kGuestStart, ExceptionLevel::EL1);
    const auto beforeWrongSourceCpu = wrongSource.cpu.serialize_state();
    const std::vector<uint8_t> beforeWrongSourceRam(wrongSource.ram.GetRawData(),
        wrongSource.ram.GetRawData() + wrongSource.ram.GetTotalSize());
    CHECK(!AArch64DeviceSnapshot::restore(wrongSource.cpu, wrongSource.ram,
        wrongSource.devices, snapshot, &error));
    CHECK(wrongSource.cpu.serialize_state() == beforeWrongSourceCpu);
    CHECK(std::equal(beforeWrongSourceRam.begin(), beforeWrongSourceRam.end(),
                     wrongSource.ram.GetRawData()));
    CHECK(wrongSource.uart->outputBytes().empty());
    AArch64InterruptSourceState wrongSourceState;
    CHECK(wrongSource.devices.queryInterruptSource(107, wrongSourceState, &error));
    CHECK(!wrongSourceState.asserted && wrongSourceState.ownerDeviceId == 6);

    std::cout << "AARCH64-10 Program E: saved/restored PC=0x" << std::hex << savedPc
              << " final PC=0x" << finalPc << std::dec << " snapshotBytes="
              << snapshot.size() << " SHA-256=" << hexDigest(info.sha256)
              << " output=PRESNAPSHOT\n";
    std::cout << "AARCH64-10 Program E: pre-RX guest instructions=" << savedWord - 8
              << " input-to-snapshot instructions=8 post-snapshot continuation="
              << endWord - savedWord << " saved/restored/final PCs=0x" << std::hex << savedPc << "/0x"
              << savedPc << "/0x" << finalPc << std::dec
              << " MMU=enabled; table walks are device-snapshot state in guest RAM\n";
}

void testArchitecturalExternalAbort() {
    Machine machine(8, kUartBase, 108);
    writeWords(machine.ram, 0x400, {kNop});
    writeWords(machine.ram, kGuestStart, {strW(2, 1)});
    machine.cpu.cpu().writeX(1, 0x0A000000);
    machine.cpu.cpu().writeX(2, 0xDEADBEEF);
    CHECK(guestStep(machine));
    CHECK_EQ(0x400, machine.cpu.getPC());
    CHECK_EQ(0x24, (machine.cpu.cpu().esrEl1() >> 26) & 0x3Fu);
    CHECK_EQ(0x10, machine.cpu.cpu().esrEl1() & 0x3Fu);
    CHECK((machine.cpu.cpu().esrEl1() & (1u << 9)) != 0); // Synchronous external abort.
    CHECK((machine.cpu.cpu().esrEl1() & (1u << 6)) != 0); // Write access.
    CHECK_EQ(0x0A000000, machine.cpu.cpu().farEl1());
}

void testRegistryOperationLease() {
    ia64::Memory ram(0x40000, false);
    AArch64DeviceRegistry registry;
    AArch64PhysicalBus bus(ram, registry);
    AArch64ISAPlugin cpu(kGuestStart, ExceptionLevel::EL0);
    auto candidate = std::make_unique<BlockingMmioDevice>(77);
    BlockingMmioDevice* blocking = candidate.get();
    std::string error;
    CHECK(registry.registerDevice(std::move(candidate), &error));

    uint8_t value[4]{};
    bool readSucceeded = false;
    std::thread reader([&] {
        try { bus.Read(kUartBase, value, sizeof(value)); readSucceeded = true; }
        catch (...) { readSucceeded = false; }
    });
    blocking->waitUntilEntered();
    CHECK(!registry.unregisterDevice(77, &error));
    std::vector<uint8_t> snapshot;
    CHECK(!AArch64DeviceSnapshot::capture(cpu, ram, registry, snapshot, &error));
    blocking->release();
    reader.join();
    CHECK(readSucceeded);
    CHECK(registry.unregisterDevice(77, &error));
    CHECK_EQ(0, registry.deviceCount());
    CHECK(expectPhysicalFault([&] { bus.Read(kUartBase, value, sizeof(value)); },
                              AArch64PhysicalFaultKind::ExternalAbort));
}

void testConcurrentInterruptLevelPublication() {
    Machine machine(90, kUartBase, 190);
    std::string error;
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
    writeReg(machine.bus, kUartBase, AArch64PL011::UARTIFLS, 0x0A);
    std::size_t accepted = 0;
    CHECK(machine.uart->injectReceive(std::vector<uint8_t>(8, 'C'), accepted, &error));
    CHECK_EQ(8, accepted);

    for (unsigned round = 0; round < 8; ++round) {
        std::thread enable([&] {
            for (unsigned i = 0; i < 500; ++i)
                writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x10);
        });
        std::thread disable([&] {
            for (unsigned i = 0; i < 500; ++i)
                writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0);
        });
        enable.join();
        disable.join();

        const bool masked = (readReg(machine.bus, kUartBase,
                                     AArch64PL011::UARTMIS) & 0x10u) != 0;
        AArch64InterruptSourceState source;
        CHECK(machine.devices.queryInterruptSource(190, source, &error));
        CHECK(source.asserted == masked);
    }
}

void testLifecycleStress() {
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        Machine machine(1000 + cycle, kUartBase, 2000 + cycle, 0x10000);
        std::string error;
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTLCR_H, 0x70);
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTCR, 0x301);
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTIFLS, 0x0A);
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTIMSC, 0x10);
        std::size_t accepted = 0;
        CHECK(machine.uart->injectReceive(std::vector<uint8_t>(8, static_cast<uint8_t>(cycle)),
                                          accepted, &error));
        CHECK_EQ(8, accepted);
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, static_cast<uint32_t>(cycle));
        std::size_t moved = 0;
        CHECK(machine.uart->advanceTransmit(1, moved, &error));
        CHECK_EQ(1, moved);
        std::vector<uint8_t> snapshot;
        CHECK(AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
            machine.devices, snapshot, &error));
        writeReg(machine.bus, kUartBase, AArch64PL011::UARTDR, 0x55);
        CHECK(AArch64DeviceSnapshot::restore(machine.cpu, machine.ram,
            machine.devices, snapshot, &error));
        CHECK_EQ(8, machine.uart->pendingReceiveBytes().size());
        CHECK_EQ(1, machine.uart->outputBytes().size());
        CHECK(machine.devices.resetAll(&error));
        CHECK(machine.uart->pendingReceiveBytes().empty());
        CHECK(machine.devices.unregisterDevice(1000 + cycle, &error));
        CHECK_EQ(0, machine.devices.deviceCount());
        CHECK_EQ(0, machine.devices.interruptSourceCount());
        uint8_t value[4]{};
        CHECK(expectPhysicalFault([&] { machine.bus.Read(kUartBase, value, sizeof(value)); },
                                  AArch64PhysicalFaultKind::ExternalAbort));
    }
}

} // namespace

int main() {
    try {
        testRegistryAndAccessValidation();
        testControlFifosAndInterrupts();
        testHoldingRegisterModeAndBaudRegisters();
        testFifoInterruptTransitionsAndOutputBound();
        testProgramAConsoleOutput();
        testProgramBEcho();
        testProgramCTranslatedMmio();
        testProgramDInterruptSource();
        testProgramEDeviceSnapshotReplay();
        testArchitecturalExternalAbort();
        testRegistryOperationLease();
        testConcurrentInterruptLevelPublication();
        testLifecycleStress();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "AARCH64-10 unexpected exception: " << exception.what() << '\n';
    } catch (...) {
        ++failures;
        std::cerr << "AARCH64-10 unexpected non-standard exception\n";
    }
    std::cout << "AARCH64-10 PL011 checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
