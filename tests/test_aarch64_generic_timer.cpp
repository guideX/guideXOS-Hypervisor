#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64PhysicalBus.h"
#include "memory.h"

#include <algorithm>
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
        std::cerr << "AARCH64-11 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-11 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint64_t kGuestStart = 0x1000;
constexpr uint64_t kDataAddress = 0x3000;
constexpr uint64_t kUartBase = 0x09000000;
constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kEret = 0xD69F03E0u;

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

uint32_t addXImm(unsigned rd, unsigned rn, uint16_t imm) {
    return 0x91000000u | (static_cast<uint32_t>(imm) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t subXImm(unsigned rd, unsigned rn, uint16_t imm) {
    return 0xD1000000u | (static_cast<uint32_t>(imm) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t strX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9000000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t movzW(unsigned rd, uint16_t imm) {
    return 0x52800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t strW(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xB9000000u | ((byteOffset / 4u) << 10) |
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
    const uint8_t op0 = static_cast<uint8_t>((key >> 14) & 3u);
    const uint8_t op1 = static_cast<uint8_t>((key >> 11) & 7u);
    const uint8_t crn = static_cast<uint8_t>((key >> 7) & 15u);
    const uint8_t crm = static_cast<uint8_t>((key >> 3) & 15u);
    const uint8_t op2 = static_cast<uint8_t>(key & 7u);
    return encodeSys(true, op0, op1, crn, crm, op2, static_cast<uint8_t>(rt));
}

uint32_t msr(uint16_t key, unsigned rt) {
    const uint8_t op0 = static_cast<uint8_t>((key >> 14) & 3u);
    const uint8_t op1 = static_cast<uint8_t>((key >> 11) & 7u);
    const uint8_t crn = static_cast<uint8_t>((key >> 7) & 15u);
    const uint8_t crm = static_cast<uint8_t>((key >> 3) & 15u);
    const uint8_t op2 = static_cast<uint8_t>(key & 7u);
    return encodeSys(false, op0, op1, crn, crm, op2, static_cast<uint8_t>(rt));
}

void writeWords(ia64::Memory& ram, uint64_t address,
                const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size() * 4);
    for (std::size_t n = 0; n < words.size(); ++n)
        for (unsigned b = 0; b < 4; ++b)
            bytes[n * 4 + b] = static_cast<uint8_t>(words[n] >> (8 * b));
    ram.loadBuffer(address, bytes);
}

uint64_t read64(const ia64::Memory& ram, uint64_t address) {
    uint8_t bytes[8]{};
    ram.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}

std::string hexDigest(const std::array<uint8_t, 32>& digest) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : digest) out << std::setw(2) << unsigned(byte);
    return out.str();
}

struct Machine {
    ia64::Memory ram;
    AArch64DeviceRegistry devices;
    AArch64PhysicalBus bus;
    AArch64ISAPlugin cpu;
    AArch64GenericTimer* timer = nullptr;
    AArch64PL011* uart = nullptr;

    explicit Machine(size_t ramBytes = 0x40000,
                     ExceptionLevel level = ExceptionLevel::EL1,
                     uint64_t initialCounter = 0,
                     uint32_t frequency = AArch64GenericTimer::kDefaultFrequencyHz,
                     uint64_t timerId = 10,
                     uint64_t physicalSource = 201,
                     uint64_t virtualSource = 202)
        : ram(ramBytes, false), devices(), bus(ram, devices), cpu(kGuestStart, level) {
        auto owned = std::make_unique<AArch64GenericTimer>(
            devices, timerId, physicalSource, virtualSource,
            AArch64GenericTimer::Config{initialCounter, frequency});
        timer = owned.get();
        std::string error;
        if (!devices.registerDevice(std::move(owned), &error))
            throw std::runtime_error("Generic Timer registration failed: " + error);
        if (!timer->registerInterruptSources(&error))
            throw std::runtime_error("Generic Timer source registration failed: " + error);
        if (!timer->attach(cpu.cpu(), &error))
            throw std::runtime_error("Generic Timer CPU attachment failed: " + error);
    }

    bool addUart(uint64_t deviceId = 11, uint64_t sourceId = 301) {
        auto owned = std::make_unique<AArch64PL011>(devices, deviceId, kUartBase, sourceId);
        uart = owned.get();
        std::string error;
        return devices.registerDevice(std::move(owned), &error);
    }
};

class QuiescenceToken final : public AArch64DeviceRestoreToken {
public:
    uint8_t value = 0;
};

class QuiescenceProbe final : public AArch64StatefulDevice {
public:
    explicit QuiescenceProbe(uint64_t id, bool allowQuiescence)
        : id_(id), allowQuiescence_(allowQuiescence) {}

    uint64_t instanceId() const noexcept override { return id_; }
    std::string typeId() const override { return "test.quiescence_probe"; }
    uint32_t stateSchemaVersion() const noexcept override { return 1; }
    std::size_t maxStateBytes() const noexcept override { return 1; }
    bool supportsSnapshot() const noexcept override { return true; }
    bool reset(std::string* error) override {
        value_ = 0;
        if (error) error->clear();
        return true;
    }
    bool quiesce(std::string* error) override {
        if (!allowQuiescence_) {
            if (error) *error = "test device intentionally refused quiescence";
            return false;
        }
        quiesced_ = true;
        if (error) error->clear();
        return true;
    }
    void resume() noexcept override { quiesced_ = false; }
    bool captureState(std::vector<uint8_t>& state, std::string* error) const override {
        state.assign(1, value_);
        if (error) error->clear();
        return true;
    }
    bool stageRestore(const uint8_t* state, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override {
        token.reset();
        if (!state || size != 1) {
            if (error) *error = "test device state length is invalid";
            return false;
        }
        auto staged = std::make_unique<QuiescenceToken>();
        staged->value = state[0];
        token = std::move(staged);
        if (error) error->clear();
        return true;
    }
    void commitRestore(std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept override {
        auto* staged = dynamic_cast<QuiescenceToken*>(token.get());
        if (staged) value_ = staged->value;
    }

    uint8_t value() const noexcept { return value_; }
    void setValue(uint8_t value) noexcept { value_ = value; }

private:
    const uint64_t id_;
    const bool allowQuiescence_;
    bool quiesced_ = false;
    uint8_t value_ = 0;
};

bool guestStep(Machine& machine) {
    const auto decoded = machine.cpu.decode(machine.bus);
    return machine.cpu.execute(machine.bus, decoded) == ia64::ISAExecutionResult::CONTINUE;
}

bool runSteps(Machine& machine, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) if (!guestStep(machine)) return false;
    return true;
}

void appendMmuSetup(std::vector<uint32_t>& words) {
    // L1/L2/L3 tables map VA 0x1000 -> PA 0x1000 and, when a UART is present,
    // VA 0x20000000 -> PA 0x09000000 Device-nGnRnE.
    auto page = [](uint64_t pa, uint8_t attr) {
        return (pa & 0x0000FFFFFFFFF000ull) | 3u |
               (static_cast<uint64_t>(attr) << 2) | (3ull << 8) | (1ull << 10);
    };
    appendConstant(words, 20, 0x10000);
    appendConstant(words, 21, 0x11000);
    appendConstant(words, 22, 0x12000);
    appendConstant(words, 23, 0x13000);
    appendConstant(words, 24, 0x11003); words.push_back(strX(24, 20, 0));
    appendConstant(words, 24, 0x12003); words.push_back(strX(24, 21, 0));
    appendConstant(words, 24, 0x13003); words.push_back(strX(24, 21, 256 * 8));
    appendConstant(words, 24, page(0x1000, 0)); words.push_back(strX(24, 22, 8));
    appendConstant(words, 24, page(kUartBase, 1)); words.push_back(strX(24, 23, 0));
    appendConstant(words, 2, 25 | (25ull << 16) | (2ull << 30));
    words.push_back(msr((3u << 14) | (2u << 7) | 2u, 2)); // TCR_EL1
    appendConstant(words, 3, 0x10000);
    words.push_back(msr((3u << 14) | (2u << 7), 3)); // TTBR0_EL1
    appendConstant(words, 4, 0xFF);
    words.push_back(msr((3u << 14) | (10u << 7) | (2u << 3), 4)); // MAIR_EL1
    appendConstant(words, 5, 1);
    words.push_back(msr((3u << 14) | (1u << 7), 5)); // SCTLR_EL1.M
    words.push_back(0xD5033A9Fu); // DSB ISHST
    words.push_back(0xD5033FDFu); // ISB
}

uint32_t readSource(AArch64DeviceRegistry& registry, uint64_t sourceId) {
    AArch64InterruptSourceState source;
    std::string error;
    CHECK(registry.queryInterruptSource(sourceId, source, &error));
    return (source.asserted ? 1u : 0u) | (source.pending ? 2u : 0u);
}

uint64_t esrSysreg(uint16_t key, unsigned rt, bool read) {
    const uint32_t op0 = (key >> 14) & 3u;
    const uint32_t op1 = (key >> 11) & 7u;
    const uint32_t crn = (key >> 7) & 15u;
    const uint32_t crm = (key >> 3) & 15u;
    const uint32_t op2 = key & 7u;
    const uint32_t iss = (op0 << 20) | (op2 << 17) | (op1 << 14) |
                         (crn << 10) | ((rt & 31u) << 5) | (crm << 1) |
                         (read ? 0u : 1u);
    return (uint64_t{0x18} << 26) | (1ull << 25) | iss;
}

std::vector<uint32_t> el1El0Setup(uint64_t cntkctl) {
    std::vector<uint32_t> words;
    appendConstant(words, 0, cntkctl);
    words.push_back(msr(timer_sysreg::kCntkctlEl1, 0));
    appendConstant(words, 1, 0x2000);
    words.push_back(msr((3u << 14) | (12u << 7), 1)); // VBAR_EL1
    appendConstant(words, 2, 0x1100);
    words.push_back(msr((3u << 14) | (4u << 7) | 1u, 2)); // ELR_EL1
    appendConstant(words, 3, 0); // EL0t SPSR
    words.push_back(msr((3u << 14) | (4u << 7), 3)); // SPSR_EL1
    appendConstant(words, 20, kDataAddress);
    appendConstant(words, 12, 5); // EL0 system-register write operand
    words.push_back(kEret);
    return words;
}

std::vector<uint32_t> exceptionHandler() {
    return {
        mrs((3u << 14) | (5u << 7) | (2u << 3), 10), // ESR_EL1
        mrs((3u << 14) | (4u << 7) | 1u, 11),         // ELR_EL1
        strX(10, 20, 0),
        strX(11, 20, 8),
        addXImm(11, 11, 4),
        msr((3u << 14) | (4u << 7) | 1u, 11),         // ELR_EL1 += 4
        kEret,
    };
}

struct El0Attempt {
    uint16_t key;
    unsigned rt;
    bool permitted;
    bool isRead = true;
};

void runEl0Scenario(uint64_t cntkctl, const std::vector<El0Attempt>& attempts) {
    Machine machine;
    std::vector<uint32_t> words = el1El0Setup(cntkctl);
    while (words.size() < (0x100 / 4)) words.push_back(kNop);
    for (const auto& attempt : attempts) words.push_back(mrs(attempt.key, attempt.rt));
    words.push_back(kNop);
    writeWords(machine.ram, kGuestStart, words);
    writeWords(machine.ram, 0x2400, exceptionHandler());

    CHECK(runSteps(machine, el1El0Setup(cntkctl).size()));
    CHECK_EQ(0x1100, machine.cpu.cpu().pc());
    CHECK_EQ(0, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
    for (const auto& attempt : attempts) {
        const uint64_t faultPc = machine.cpu.cpu().pc();
        const uint64_t oldPhysicalComparator = machine.timer->physicalComparator();
        const uint64_t oldVirtualComparator = machine.timer->virtualComparator();
        const uint32_t instruction = attempt.isRead
            ? mrs(attempt.key, attempt.rt) : msr(attempt.key, attempt.rt);
        writeWords(machine.ram, faultPc, {instruction});
        CHECK(guestStep(machine));
        if (attempt.permitted) {
            CHECK_EQ(0, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
            CHECK_EQ(faultPc + 4, machine.cpu.cpu().pc());
        } else {
            CHECK_EQ(1, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
            CHECK_EQ(0x2400, machine.cpu.cpu().pc());
            CHECK_EQ(faultPc, machine.cpu.cpu().elrEl1());
            CHECK_EQ(esrSysreg(attempt.key, attempt.rt, attempt.isRead), machine.cpu.cpu().esrEl1());
            CHECK(runSteps(machine, exceptionHandler().size()));
            CHECK_EQ(0, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
            CHECK_EQ(faultPc + 4, machine.cpu.cpu().pc());
            CHECK_EQ(esrSysreg(attempt.key, attempt.rt, attempt.isRead), read64(machine.ram, kDataAddress));
            CHECK_EQ(faultPc, read64(machine.ram, kDataAddress + 8));
            CHECK_EQ(oldPhysicalComparator, machine.timer->physicalComparator());
            CHECK_EQ(oldVirtualComparator, machine.timer->virtualComparator());
        }
    }
    CHECK(guestStep(machine)); // known final NOP
    CHECK_EQ(0x1100 + 4 * (attempts.size() + 1), machine.cpu.cpu().pc());
}

void testProgramA_CounterVerification() {
    Machine machine;
    std::vector<uint32_t> words = {
        mrs(timer_sysreg::kCntfrqEl0, 0),
        mrs(timer_sysreg::kCntpctEl0, 1),
        addXImm(2, 2, 1), addXImm(2, 2, 1), addXImm(2, 2, 1),
        mrs(timer_sysreg::kCntpctEl0, 3),
        mrs(timer_sysreg::kCntvctEl0, 4),
        subXImm(5, 3, 1),
        movzX(10, static_cast<uint16_t>(kDataAddress)),
        strX(0, 10, 0), strX(1, 10, 8), strX(3, 10, 16),
        strX(4, 10, 24), strX(5, 10, 32),
    };
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, words.size()));
    CHECK_EQ(14, words.size());
    CHECK_EQ(1'000'000, machine.cpu.cpu().readX(0));
    CHECK_EQ(1, machine.cpu.cpu().readX(1));
    CHECK_EQ(3, machine.cpu.cpu().readX(2));
    CHECK_EQ(5, machine.cpu.cpu().readX(3));
    CHECK_EQ(6, machine.cpu.cpu().readX(4));
    CHECK_EQ(4, machine.cpu.cpu().readX(5));
    CHECK_EQ(0x1038, machine.cpu.cpu().pc());
    CHECK_EQ(words.size(), machine.timer->retiredInstructionCount());
    CHECK_EQ(words.size(), machine.timer->counter());
    CHECK_EQ(1'000'000, read64(machine.ram, kDataAddress));
    CHECK_EQ(1, read64(machine.ram, kDataAddress + 8));
    CHECK_EQ(5, read64(machine.ram, kDataAddress + 16));
    CHECK_EQ(6, read64(machine.ram, kDataAddress + 24));
    CHECK_EQ(4, read64(machine.ram, kDataAddress + 32));

    const auto firstCpu = machine.cpu.serialize_state();
    const uint64_t firstCounter = machine.timer->counter();
    machine.cpu.reset();
    std::string error;
    CHECK(machine.devices.resetAll(&error));
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, words.size()));
    CHECK_EQ(firstCounter, machine.timer->counter());
    CHECK(firstCpu == machine.cpu.serialize_state());
    CHECK_EQ(1, static_cast<uint64_t>(machine.cpu.cpu().exceptionLevel()));
}

void testProgramB_VirtualTimer() {
    Machine machine;
    const std::vector<uint32_t> words = {
        mrs(timer_sysreg::kCntvctEl0, 0),
        movzX(1, 3),
        msr(timer_sysreg::kCntvTvalEl0, 1),
        movzX(1, 1),
        msr(timer_sysreg::kCntvCtlEl0, 1),
        mrs(timer_sysreg::kCntvCtlEl0, 2),
        movzX(1, 3),
        msr(timer_sysreg::kCntvCtlEl0, 1), // mask while expired
        mrs(timer_sysreg::kCntvCtlEl0, 3),
        movzX(1, 1),
        msr(timer_sysreg::kCntvCtlEl0, 1), // unmask; output reasserts
        mrs(timer_sysreg::kCntvctEl0, 4),
        movzX(1, 10),
        msr(timer_sysreg::kCntvTvalEl0, 1), // move comparator into future
        kNop,
    };
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, 5));
    CHECK_EQ(5, machine.timer->counter());
    CHECK_EQ(5, machine.timer->virtualComparator());
    CHECK_EQ(1, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(5, machine.cpu.cpu().readX(2)); // guest-observed ENABLE|ISTATUS
    CHECK(runSteps(machine, 2));
    CHECK_EQ(3, machine.timer->virtualControl());
    CHECK_EQ(0, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(7, machine.cpu.cpu().readX(3)); // ENABLE|IMASK|ISTATUS
    CHECK(runSteps(machine, 2));
    CHECK_EQ(1, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(11, machine.cpu.cpu().readX(4));
    CHECK(runSteps(machine, 3));
    CHECK_EQ(23, machine.timer->virtualComparator());
    CHECK_EQ(0, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK_EQ(15, words.size());
    CHECK_EQ(0x103C, machine.cpu.cpu().pc());
    CHECK_EQ(words.size(), machine.timer->retiredInstructionCount());
    CHECK_EQ(words.size(), machine.timer->counter());
}

void testProgramC_PhysicalTimerAndSimultaneousExpiry() {
    Machine machine;
    const std::vector<uint32_t> words = {
        mrs(timer_sysreg::kCntpctEl0, 0),
        movzX(1, 4),
        msr(timer_sysreg::kCntpTvalEl0, 1),
        movzX(1, 1),
        msr(timer_sysreg::kCntpCtlEl0, 1),
        kNop,
        mrs(timer_sysreg::kCntpCtlEl0, 2),
        movzX(1, 3),
        msr(timer_sysreg::kCntpCtlEl0, 1),
        mrs(timer_sysreg::kCntpCtlEl0, 3),
        movzX(1, 0),
        msr(timer_sysreg::kCntpCtlEl0, 1),
        mrs(timer_sysreg::kCntpCtlEl0, 4),
        kNop,
    };
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, 6));
    CHECK_EQ(6, machine.timer->counter());
    CHECK_EQ(6, machine.timer->physicalComparator());
    CHECK_EQ(1, readSource(machine.devices, machine.timer->physicalSourceId()));
    CHECK_EQ(0, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(5, machine.cpu.cpu().readX(2));
    CHECK(runSteps(machine, 2));
    CHECK_EQ(3, machine.timer->physicalControl());
    CHECK_EQ(0, readSource(machine.devices, machine.timer->physicalSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(7, machine.cpu.cpu().readX(3));
    CHECK(runSteps(machine, 2));
    CHECK_EQ(0, machine.timer->physicalControl());
    CHECK_EQ(0, readSource(machine.devices, machine.timer->physicalSourceId()));
    CHECK(guestStep(machine));
    CHECK_EQ(0, machine.cpu.cpu().readX(4)); // deterministic disabled ISTATUS
    CHECK(guestStep(machine));
    CHECK_EQ(kGuestStart + words.size() * 4, machine.cpu.cpu().pc());
    CHECK_EQ(14, words.size());
    CHECK_EQ(0x1038, machine.cpu.cpu().pc());

    Machine simultaneous;
    const std::vector<uint32_t> both = {
        movzX(0, 10),
        msr(timer_sysreg::kCntpCvalEl0, 0),
        msr(timer_sysreg::kCntvCvalEl0, 0),
        movzX(0, 1),
        msr(timer_sysreg::kCntpCtlEl0, 0),
        msr(timer_sysreg::kCntvCtlEl0, 0),
        kNop, kNop, kNop, kNop,
    };
    writeWords(simultaneous.ram, kGuestStart, both);
    CHECK(runSteps(simultaneous, 10));
    CHECK_EQ(10, simultaneous.timer->counter());
    CHECK_EQ(10, simultaneous.timer->physicalComparator());
    CHECK_EQ(10, simultaneous.timer->virtualComparator());
    CHECK_EQ(1, readSource(simultaneous.devices, simultaneous.timer->physicalSourceId()));
    CHECK_EQ(1, readSource(simultaneous.devices, simultaneous.timer->virtualSourceId()));
}

void testProgramD_El0Controls() {
    runEl0Scenario(1, {
        {timer_sysreg::kCntfrqEl0, 4, true},
        {timer_sysreg::kCntpctEl0, 5, true},
        {timer_sysreg::kCntvctEl0, 6, false},
        {timer_sysreg::kCntvTvalEl0, 12, false, false},
    });
    runEl0Scenario(2, {
        {timer_sysreg::kCntfrqEl0, 4, true},
        {timer_sysreg::kCntpctEl0, 5, false},
        {timer_sysreg::kCntvctEl0, 6, true},
        {timer_sysreg::kCntpTvalEl0, 12, false, false},
    });
    runEl0Scenario((1ull << 0) | (1ull << 8), {
        {timer_sysreg::kCntpctEl0, 4, true},
        {timer_sysreg::kCntvctEl0, 5, false},
        {timer_sysreg::kCntvTvalEl0, 6, true},
        {timer_sysreg::kCntpCtlEl0, 7, false},
    });
    runEl0Scenario((1ull << 1) | (1ull << 9), {
        {timer_sysreg::kCntpctEl0, 4, false},
        {timer_sysreg::kCntvctEl0, 5, true},
        {timer_sysreg::kCntpTvalEl0, 6, true},
        {timer_sysreg::kCntvCtlEl0, 7, false},
    });
    runEl0Scenario(0, {
        {timer_sysreg::kCntfrqEl0, 4, false},
        {timer_sysreg::kCntpctEl0, 5, false},
        {timer_sysreg::kCntvctEl0, 6, false},
    });
    runEl0Scenario(0x303, {
        {timer_sysreg::kCntfrqEl0, 4, true},
        {timer_sysreg::kCntpctEl0, 5, true},
        {timer_sysreg::kCntvctEl0, 6, true},
        {timer_sysreg::kCntvCtlEl0, 7, true},
        {timer_sysreg::kCntpCtlEl0, 8, true},
        {timer_sysreg::kCntvTvalEl0, 12, true, false},
    });
}

void testProgramE_TimerSnapshotReplay() {
    Machine original;
    std::vector<uint32_t> words;
    appendMmuSetup(words);
    words.push_back(mrs(timer_sysreg::kCntvctEl0, 6));
    words.push_back(movzX(7, 50));
    words.push_back(msr(timer_sysreg::kCntvTvalEl0, 7));
    words.push_back(movzX(7, 1));
    words.push_back(msr(timer_sysreg::kCntvCtlEl0, 7));
    for (unsigned i = 0; i < 80; ++i) words.push_back(kNop);
    words.push_back(mrs(timer_sysreg::kCntvCtlEl0, 8));
    words.push_back(kNop);
    writeWords(original.ram, kGuestStart, words);

    const std::size_t setupCount = words.size() - 87;
    CHECK(runSteps(original, setupCount + 10));
    const uint64_t savedPc = original.cpu.cpu().pc();
    const uint64_t savedCounter = original.timer->counter();
    const uint64_t savedComparator = original.timer->virtualComparator();
    CHECK_EQ(0x10B0, savedPc);
    CHECK_EQ(44, savedCounter);
    CHECK_EQ(86, savedComparator);
    CHECK_EQ(42, savedComparator - savedCounter);
    CHECK_EQ(0, readSource(original.devices, original.timer->virtualSourceId()));

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64DeviceSnapshot::capture(original.cpu, original.ram,
                                         original.devices, snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(1, info.deviceCount);
    CHECK_EQ(2, info.interruptSourceCount);
    CHECK_EQ(AArch64GenericTimer::kStateBytes, info.totalDeviceStateBytes);
    std::cout << "Program E snapshot bytes=" << snapshot.size()
              << " sha256=" << hexDigest(info.sha256)
              << " pc=0x" << std::hex << savedPc << std::dec << '\n';

    Machine restored;
    writeWords(restored.ram, kGuestStart, words);
    CHECK(AArch64DeviceSnapshot::restore(restored.cpu, restored.ram,
                                         restored.devices, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu.cpu().pc());
    CHECK_EQ(savedCounter, restored.timer->counter());
    CHECK_EQ(savedComparator, restored.timer->virtualComparator());
    CHECK_EQ(0, readSource(restored.devices, restored.timer->virtualSourceId()));

    const uint64_t stepsToExpiry = savedComparator - savedCounter;
    CHECK(runSteps(original, stepsToExpiry));
    const uint64_t originalExpiry = original.timer->counter();
    CHECK_EQ(86, originalExpiry);
    CHECK_EQ(savedComparator, originalExpiry);
    CHECK_EQ(1, readSource(original.devices, original.timer->virtualSourceId()));
    const uint64_t postExpiryNops = (setupCount + 5 + 80) -
                                    (setupCount + 10 + stepsToExpiry);
    CHECK(runSteps(original, postExpiryNops));
    CHECK(guestStep(original)); // Guest observes ISTATUS after expiry.
    CHECK_EQ(5, original.cpu.cpu().readX(8));
    const uint64_t finalPc = original.cpu.cpu().pc();
    CHECK_EQ(0x11E0, finalPc);
    const auto finalCpu = original.cpu.serialize_state();
    std::vector<uint8_t> finalRam(original.ram.GetRawData(),
                                  original.ram.GetRawData() + original.ram.GetTotalSize());

    CHECK(runSteps(restored, stepsToExpiry));
    CHECK_EQ(originalExpiry, restored.timer->counter());
    CHECK_EQ(1, readSource(restored.devices, restored.timer->virtualSourceId()));
    CHECK(runSteps(restored, postExpiryNops));
    CHECK(guestStep(restored));
    CHECK_EQ(finalPc, restored.cpu.cpu().pc());
    CHECK_EQ(5, restored.cpu.cpu().readX(8));
    CHECK(finalCpu == restored.cpu.serialize_state());
    CHECK(finalRam == std::vector<uint8_t>(restored.ram.GetRawData(),
                                           restored.ram.GetRawData() + restored.ram.GetTotalSize()));
    CHECK_EQ(0, original.timer->physicalComparator());
    CHECK_EQ(0, restored.timer->physicalComparator());
}

void testProgramF_Pl011TimerCoexistence() {
    Machine original;
    CHECK(original.addUart());
    std::vector<uint32_t> words;
    appendMmuSetup(words);
    appendConstant(words, 1, 0x20000000);
    words.push_back(movzW(2, 0x70));
    words.push_back(strW(2, 1, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(2, 0x101));
    words.push_back(strW(2, 1, AArch64PL011::UARTCR));
    const std::string message = "GT11\n";
    for (unsigned char ch : message) {
        words.push_back(movzW(2, ch));
        words.push_back(strW(2, 1, AArch64PL011::UARTDR));
    }
    words.push_back(mrs(timer_sysreg::kCntvctEl0, 8));
    words.push_back(movzX(9, 10));
    words.push_back(msr(timer_sysreg::kCntvTvalEl0, 9));
    words.push_back(movzX(9, 1));
    words.push_back(msr(timer_sysreg::kCntvCtlEl0, 9));
    for (unsigned i = 0; i < 5; ++i) words.push_back(kNop);
    const std::size_t snapshotStepCount = words.size();
    words.push_back(kNop);
    words.push_back(kNop);
    words.push_back(mrs(timer_sysreg::kCntvCtlEl0, 10));
    words.push_back(kNop);
    writeWords(original.ram, kGuestStart, words);
    CHECK(runSteps(original, snapshotStepCount));
    CHECK_EQ(60, snapshotStepCount);
    CHECK_EQ(0x10F0, original.cpu.cpu().pc());
    std::size_t moved = 0;
    std::string error;
    CHECK(original.uart->advanceTransmit(32, moved, &error));
    CHECK_EQ(message.size(), moved);
    CHECK(original.uart->outputBytes() == std::vector<uint8_t>(message.begin(), message.end()));
    CHECK_EQ(snapshotStepCount, original.timer->retiredInstructionCount());
    CHECK_EQ(snapshotStepCount, original.timer->counter());

    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(original.cpu, original.ram,
                                         original.devices, snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(2, info.deviceCount);
    CHECK_EQ(3, info.interruptSourceCount);
    CHECK_EQ(149, info.totalDeviceStateBytes);
    Machine restored;
    CHECK(restored.addUart());
    writeWords(restored.ram, kGuestStart, words);
    CHECK(AArch64DeviceSnapshot::restore(restored.cpu, restored.ram,
                                         restored.devices, snapshot, &error));
    CHECK_EQ(original.cpu.cpu().pc(), restored.cpu.cpu().pc());
    CHECK_EQ(original.timer->counter(), restored.timer->counter());
    CHECK_EQ(original.timer->virtualComparator(), restored.timer->virtualComparator());
    CHECK_EQ(62, original.timer->virtualComparator());
    CHECK_EQ(2, original.timer->virtualComparator() - original.timer->counter());
    CHECK(original.uart->outputBytes() == restored.uart->outputBytes());
    CHECK_EQ(0, readSource(restored.devices, restored.timer->physicalSourceId()));
    CHECK_EQ(0, readSource(restored.devices, restored.timer->virtualSourceId()));

    const uint64_t remaining = original.timer->virtualComparator() - original.timer->counter();
    CHECK(runSteps(original, remaining));
    CHECK(runSteps(restored, remaining));
    CHECK_EQ(62, original.timer->counter());
    CHECK_EQ(original.timer->counter(), restored.timer->counter());
    CHECK_EQ(1, readSource(original.devices, original.timer->virtualSourceId()));
    CHECK_EQ(1, readSource(restored.devices, restored.timer->virtualSourceId()));
    CHECK_EQ(0, readSource(original.devices, original.timer->physicalSourceId()));
    CHECK_EQ(0, readSource(restored.devices, restored.timer->physicalSourceId()));
    CHECK(guestStep(original));
    CHECK(guestStep(restored));
    CHECK_EQ(0x10FC, original.cpu.cpu().pc());
    CHECK_EQ(0x10FC, restored.cpu.cpu().pc());
    CHECK_EQ(5, original.cpu.cpu().readX(10));
    CHECK_EQ(5, restored.cpu.cpu().readX(10));
    CHECK(original.cpu.serialize_state() == restored.cpu.serialize_state());
    CHECK(std::equal(original.ram.GetRawData(),
                     original.ram.GetRawData() + original.ram.GetTotalSize(),
                     restored.ram.GetRawData()));
    CHECK(original.uart->outputBytes() == restored.uart->outputBytes());
}

void testCounterWrapTvalCvalAndControls() {
    constexpr uint64_t start = UINT64_MAX - 2;
    Machine machine(0x40000, ExceptionLevel::EL1, start, 1234);
    std::vector<uint32_t> nops(5, kNop);
    writeWords(machine.ram, kGuestStart, nops);
    CHECK(runSteps(machine, nops.size()));
    CHECK_EQ(2, machine.timer->counter());
    CHECK_EQ(5, machine.timer->retiredInstructionCount());
    CHECK_EQ(1234, machine.timer->frequencyHz());

    Machine wrapping(0x40000, ExceptionLevel::EL1, UINT64_MAX - 2, 1'000'000);
    const std::vector<uint32_t> wrapTimer = {
        movzX(0, 3),
        msr(timer_sysreg::kCntpTvalEl0, 0),
        movzX(0, 1),
        msr(timer_sysreg::kCntpCtlEl0, 0),
        mrs(timer_sysreg::kCntpCtlEl0, 1),
    };
    writeWords(wrapping.ram, kGuestStart, wrapTimer);
    CHECK(runSteps(wrapping, 4));
    CHECK_EQ(1, wrapping.timer->physicalComparator());
    CHECK_EQ(1, wrapping.timer->counter());
    CHECK_EQ(1, readSource(wrapping.devices, wrapping.timer->physicalSourceId()));
    CHECK(guestStep(wrapping));
    CHECK_EQ(5, wrapping.cpu.cpu().readX(1));

    Machine values;
    const std::vector<uint32_t> tval = {
        movzX(0, 10),
        msr(timer_sysreg::kCntpTvalEl0, 0),
        movzX(0, 1),
        msr(timer_sysreg::kCntpCtlEl0, 0),
        mrs(timer_sysreg::kCntpTvalEl0, 1),
        movzX(2, 0xFFFF), movkX(2, 0xFFFF, 1),
        msr(timer_sysreg::kCntvTvalEl0, 2), // signed -1
        mrs(timer_sysreg::kCntvCvalEl0, 3),
        movzX(4, 0x8000, 1), // INT32_MIN low word
        msr(timer_sysreg::kCntpTvalEl0, 4),
        mrs(timer_sysreg::kCntpCvalEl0, 5),
    };
    writeWords(values.ram, kGuestStart, tval);
    CHECK(runSteps(values, tval.size()));
    CHECK_EQ(7, values.cpu.cpu().readX(1)); // low 32-bit TVAL read view
    CHECK_EQ(static_cast<uint64_t>(7), values.cpu.cpu().readX(1) & 0xFFFFFFFFu);
    CHECK_EQ(6, values.cpu.cpu().readX(3));
    CHECK_EQ(0xFFFFFFFF8000000Aull, values.timer->physicalComparator());
    CHECK_EQ(values.timer->physicalComparator(), values.cpu.cpu().readX(5));

    Machine reserved;
    const std::vector<uint32_t> reservedWords = {
        movzX(0, 0xFFFF), movkX(0, 0xFFFF, 3),
        msr(timer_sysreg::kCntpCvalEl0, 0),
        movzX(1, 5), movkX(1, 0xFFFF, 3),
        msr(timer_sysreg::kCntpCtlEl0, 1),
    };
    writeWords(reserved.ram, kGuestStart, reservedWords);
    CHECK(runSteps(reserved, reservedWords.size()));
    CHECK_EQ(0xFFFF00000000FFFFull, reserved.timer->physicalComparator());
    CHECK_EQ(1, reserved.timer->physicalControl()); // reserved control writes are ignored
}

void testFrequencyTvalBoundariesAndIndependentComparators() {
    Machine frequency;
    const std::vector<uint32_t> frequencyProgram = {
        movzX(0, 0x1234),
        msr(timer_sysreg::kCntfrqEl0, 0),
        mrs(timer_sysreg::kCntfrqEl0, 1),
        mrs(timer_sysreg::kCntpctEl0, 2),
    };
    writeWords(frequency.ram, kGuestStart, frequencyProgram);
    CHECK(runSteps(frequency, frequencyProgram.size()));
    CHECK_EQ(0x1234, frequency.cpu.cpu().readX(1));
    CHECK_EQ(3, frequency.cpu.cpu().readX(2));
    CHECK_EQ(0x1234, frequency.timer->frequencyHz());
    CHECK_EQ(4, frequency.timer->counter());

    // CNTFRQ_EL0 is writable at EL1 because EL1 is the highest implemented
    // exception level in this profile. EL0 writes are UNDEFINED and cannot
    // change the frequency, even when EL0 timer reads are enabled.
    Machine el0Write(0x40000, ExceptionLevel::EL0);
    writeWords(el0Write.ram, kGuestStart,
               {msr(timer_sysreg::kCntfrqEl0, 0)});
    CHECK(guestStep(el0Write));
    CHECK_EQ(1, static_cast<uint64_t>(el0Write.cpu.cpu().exceptionLevel()));
    CHECK_EQ(kGuestStart, el0Write.cpu.cpu().elrEl1());
    CHECK_EQ(0, el0Write.cpu.cpu().esrEl1() >> 26);
    CHECK_EQ(AArch64GenericTimer::kDefaultFrequencyHz,
             el0Write.timer->frequencyHz());
    CHECK_EQ(1, el0Write.timer->counter()); // the completed UNDEFINED step retires once

    Machine zeroTval;
    const std::vector<uint32_t> zeroProgram = {
        movzX(0, 0), msr(timer_sysreg::kCntpTvalEl0, 0),
        movzX(0, 1), msr(timer_sysreg::kCntpCtlEl0, 0),
        mrs(timer_sysreg::kCntpCtlEl0, 1),
        mrs(timer_sysreg::kCntpTvalEl0, 2),
    };
    writeWords(zeroTval.ram, kGuestStart, zeroProgram);
    CHECK(runSteps(zeroTval, zeroProgram.size()));
    CHECK_EQ(1, zeroTval.timer->physicalComparator());
    CHECK_EQ(5, zeroTval.cpu.cpu().readX(1)); // ENABLE | ISTATUS at equality/expiry
    CHECK_EQ(0xFFFFFFFCu, zeroTval.cpu.cpu().readX(2));
    CHECK_EQ(1, readSource(zeroTval.devices,
                           zeroTval.timer->physicalSourceId()));

    Machine positiveMax;
    const std::vector<uint32_t> maxProgram = {
        movzX(0, 0xFFFF), movkX(0, 0x7FFF, 1),
        msr(timer_sysreg::kCntpTvalEl0, 0),
    };
    writeWords(positiveMax.ram, kGuestStart, maxProgram);
    CHECK(runSteps(positiveMax, maxProgram.size()));
    CHECK_EQ(0x80000001ull, positiveMax.timer->physicalComparator());

    Machine independent;
    const std::vector<uint32_t> independentProgram = {
        movzX(0, 2), msr(timer_sysreg::kCntpCvalEl0, 0),
        movzX(0, 4), msr(timer_sysreg::kCntvCvalEl0, 0),
        movzX(0, 1), msr(timer_sysreg::kCntpCtlEl0, 0),
        movzX(0, 1), msr(timer_sysreg::kCntvCtlEl0, 0),
    };
    writeWords(independent.ram, kGuestStart, independentProgram);
    CHECK(runSteps(independent, independentProgram.size()));
    CHECK_EQ(2, independent.timer->physicalComparator());
    CHECK_EQ(4, independent.timer->virtualComparator());
    CHECK_EQ(1, readSource(independent.devices,
                           independent.timer->physicalSourceId()));
    CHECK_EQ(1, readSource(independent.devices,
                           independent.timer->virtualSourceId()));
    CHECK(independent.timer->physicalCondition());
    CHECK(independent.timer->virtualCondition());
}

void testQuiescenceAndTimerResetIsolation() {
    Machine machine;
    CHECK(machine.addUart());
    const std::vector<uint32_t> program = {kNop, kNop};
    writeWords(machine.ram, kGuestStart, program);
    CHECK(guestStep(machine));
    const uint64_t pausedCounter = machine.timer->counter();
    std::string error;
    CHECK(machine.timer->quiesce(&error));
    CHECK(!guestStep(machine));
    CHECK_EQ(pausedCounter, machine.timer->counter());
    machine.timer->resume();
    CHECK(guestStep(machine));
    CHECK_EQ(pausedCounter + 1, machine.timer->counter());

    const uint8_t lcr[4] = {0x70, 0, 0, 0};
    const uint8_t control[4] = {0x01, 0x01, 0, 0};
    const uint8_t byte[4] = {'R', 0, 0, 0};
    CHECK(machine.uart->writeMmio(AArch64PL011::UARTLCR_H, lcr, sizeof(lcr)) ==
          AArch64MmioAccessResult::Success);
    CHECK(machine.uart->writeMmio(AArch64PL011::UARTCR, control, sizeof(control)) ==
          AArch64MmioAccessResult::Success);
    CHECK(machine.uart->writeMmio(AArch64PL011::UARTDR, byte, sizeof(byte)) ==
          AArch64MmioAccessResult::Success);
    std::size_t moved = 0;
    CHECK(machine.uart->advanceTransmit(1, moved, &error));
    CHECK_EQ(1, moved);
    const auto uartBeforeReset = machine.uart->outputBytes();
    CHECK(uartBeforeReset == std::vector<uint8_t>{'R'});
    CHECK(machine.timer->reset(&error));
    CHECK_EQ(0, machine.timer->counter());
    CHECK_EQ(0, readSource(machine.devices, machine.timer->physicalSourceId()));
    CHECK_EQ(0, readSource(machine.devices, machine.timer->virtualSourceId()));
    CHECK(uartBeforeReset == machine.uart->outputBytes());
}

void testSystemRegisterNegativeAndSnapshots() {
    Machine machine;
    std::vector<uint32_t> words = {
        movzX(0, 1), msr(timer_sysreg::kCntkctlEl1, 0),
        movzX(1, 1), msr(timer_sysreg::kCntvTvalEl0, 1),
        movzX(1, 1), msr(timer_sysreg::kCntvCtlEl0, 1),
        kNop,
    };
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, 7));
    const uint64_t beforeCounter = machine.timer->counter();
    const uint64_t beforeComparator = machine.timer->virtualComparator();
    const uint32_t beforeCtl = machine.timer->virtualControl();
    std::vector<uint8_t> timerState;
    std::string error;
    CHECK(machine.timer->captureState(timerState, &error));
    CHECK_EQ(AArch64GenericTimer::kStateBytes, timerState.size());

    auto corrupt = timerState;
    corrupt[4] = 99;
    std::unique_ptr<AArch64DeviceRestoreToken> token;
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    CHECK(!token);
    corrupt = timerState;
    corrupt[76] = 0x80; // reserved virtual CTL bit
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[44] = 0; corrupt[45] = 0; corrupt[46] = 0; corrupt[47] = 0;
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[24] ^= 1; // counter no longer agrees with initial value plus retired ticks
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[72] = 0x80; // reserved physical CTL bit
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[48] = 0x04; // unsupported event-stream enable in CNTKCTL
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[80] ^= 1; // saved output disagrees with timer condition
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    corrupt = timerState;
    corrupt[84] = 1; // reserved payload tail
    CHECK(!machine.timer->stageRestore(corrupt.data(), corrupt.size(), token, &error));
    CHECK_EQ(beforeCounter, machine.timer->counter());
    CHECK_EQ(beforeComparator, machine.timer->virtualComparator());
    CHECK_EQ(beforeCtl, machine.timer->virtualControl());

    std::vector<uint8_t> legacy;
    CHECK(AArch64ExecutionSnapshot::capture(machine.cpu, machine.ram, legacy, &error));
    CHECK(!AArch64DeviceSnapshot::restore(machine.cpu, machine.ram,
                                          machine.devices, legacy, &error));
    CHECK_EQ(beforeCounter, machine.timer->counter());

    // Source assertion is a transactionally validated part of the timer state.
    CHECK(machine.devices.setLevel(machine.timer->physicalSourceId(), true, &error));
    std::vector<uint8_t> rejected;
    CHECK(!AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                                          machine.devices, rejected, &error));
    CHECK_EQ(beforeCounter, machine.timer->counter());
    CHECK(machine.devices.setLevel(machine.timer->physicalSourceId(), false, &error));

    // A timer-aware snapshot cannot restore over a different device identity.
    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                                         machine.devices, snapshot, &error));
    Machine wrongId(0x40000, ExceptionLevel::EL1, 0, 1'000'000, 88, 901, 902);
    const uint64_t wrongCounter = wrongId.timer->counter();
    CHECK(!AArch64DeviceSnapshot::restore(wrongId.cpu, wrongId.ram,
                                          wrongId.devices, snapshot, &error));
    CHECK_EQ(wrongCounter, wrongId.timer->counter());

    auto cpuBytes = [](Machine& m) { return m.cpu.serialize_state(); };
    auto ramBytes = [](Machine& m) {
        return std::vector<uint8_t>(m.ram.GetRawData(),
                                   m.ram.GetRawData() + m.ram.GetTotalSize());
    };
    auto timerBytes = [&](Machine& m) {
        std::vector<uint8_t> bytes;
        CHECK(m.timer->captureState(bytes, &error));
        return bytes;
    };

    for (const uint64_t missingId : {machine.timer->physicalSourceId(),
                                     machine.timer->virtualSourceId()}) {
        Machine missing;
        const auto savedCpu = cpuBytes(missing);
        const auto savedRam = ramBytes(missing);
        const auto savedTimer = timerBytes(missing);
        const std::size_t savedDeviceCount = missing.devices.deviceCount();
        CHECK(missing.devices.removeInterruptSource(missingId, &error));
        const std::size_t savedSourceCount = missing.devices.interruptSourceCount();
        CHECK(!AArch64DeviceSnapshot::restore(missing.cpu, missing.ram,
                                              missing.devices, snapshot, &error));
        CHECK(savedCpu == cpuBytes(missing));
        CHECK(savedRam == ramBytes(missing));
        CHECK(savedTimer == timerBytes(missing));
        CHECK_EQ(savedDeviceCount, missing.devices.deviceCount());
        CHECK_EQ(savedSourceCount, missing.devices.interruptSourceCount());
    }

    // A destination with the right source ID and owner but the wrong trigger
    // mode must reject before changing CPU, RAM, timer state, or inventory.
    Machine wrongMode;
    const auto modeCpu = cpuBytes(wrongMode);
    const auto modeRam = ramBytes(wrongMode);
    const auto modeTimer = timerBytes(wrongMode);
    CHECK(wrongMode.devices.removeInterruptSource(
        wrongMode.timer->physicalSourceId(), &error));
    CHECK(wrongMode.devices.registerInterruptSource(
        wrongMode.timer->physicalSourceId(), wrongMode.timer->instanceId(),
        AArch64InterruptMode::Edge, &error));
    const std::size_t modeDeviceCount = wrongMode.devices.deviceCount();
    const std::size_t modeSourceCount = wrongMode.devices.interruptSourceCount();
    CHECK(!AArch64DeviceSnapshot::restore(wrongMode.cpu, wrongMode.ram,
                                          wrongMode.devices, snapshot, &error));
    CHECK(modeCpu == cpuBytes(wrongMode));
    CHECK(modeRam == ramBytes(wrongMode));
    CHECK(modeTimer == timerBytes(wrongMode));
    CHECK_EQ(modeDeviceCount, wrongMode.devices.deviceCount());
    CHECK_EQ(modeSourceCount, wrongMode.devices.interruptSourceCount());

    // The timer snapshot cannot restore into a device-aware envelope topology
    // with no timer owner or timer sources.
    ia64::Memory plainRam(0x40000, false);
    AArch64DeviceRegistry plainDevices;
    AArch64PhysicalBus plainBus(plainRam, plainDevices);
    AArch64ISAPlugin plainCpu(kGuestStart, ExceptionLevel::EL1);
    const auto plainCpuBefore = plainCpu.serialize_state();
    const auto plainRamBefore = std::vector<uint8_t>(
        plainRam.GetRawData(), plainRam.GetRawData() + plainRam.GetTotalSize());
    CHECK(!AArch64DeviceSnapshot::restore(plainCpu, plainRam, plainDevices,
                                          snapshot, &error));
    CHECK(plainCpuBefore == plainCpu.serialize_state());
    CHECK(plainRamBefore == std::vector<uint8_t>(
        plainRam.GetRawData(), plainRam.GetRawData() + plainRam.GetTotalSize()));
    CHECK_EQ(0, plainDevices.deviceCount());
    CHECK_EQ(0, plainDevices.interruptSourceCount());

    // Same device and source IDs but a different configured counter baseline
    // is rejected during device staging and leaves the destination untouched.
    Machine wrongBaseline(0x40000, ExceptionLevel::EL1, 9, 1'000'000);
    const auto baselineCpu = cpuBytes(wrongBaseline);
    const auto baselineRam = ramBytes(wrongBaseline);
    const auto baselineTimer = timerBytes(wrongBaseline);
    CHECK(!AArch64DeviceSnapshot::restore(wrongBaseline.cpu, wrongBaseline.ram,
                                          wrongBaseline.devices, snapshot, &error));
    CHECK(baselineCpu == cpuBytes(wrongBaseline));
    CHECK(baselineRam == ramBytes(wrongBaseline));
    CHECK(baselineTimer == timerBytes(wrongBaseline));

    // Owner mismatch is exercised in an otherwise matching PL011 + timer
    // topology so the coordinator reaches source ownership validation.
    Machine combinedSource;
    CHECK(combinedSource.addUart());
    std::vector<uint8_t> combinedSnapshot;
    CHECK(AArch64DeviceSnapshot::capture(combinedSource.cpu, combinedSource.ram,
                                         combinedSource.devices, combinedSnapshot,
                                         &error));
    Machine wrongOwner;
    CHECK(wrongOwner.addUart());
    const auto ownerCpu = cpuBytes(wrongOwner);
    const auto ownerRam = ramBytes(wrongOwner);
    const auto ownerTimer = timerBytes(wrongOwner);
    CHECK(wrongOwner.devices.removeInterruptSource(
        wrongOwner.timer->physicalSourceId(), &error));
    CHECK(wrongOwner.devices.registerInterruptSource(
        wrongOwner.timer->physicalSourceId(), wrongOwner.uart->instanceId(),
        AArch64InterruptMode::Level, &error));
    const std::size_t ownerDeviceCount = wrongOwner.devices.deviceCount();
    const std::size_t ownerSourceCount = wrongOwner.devices.interruptSourceCount();
    CHECK(!AArch64DeviceSnapshot::restore(wrongOwner.cpu, wrongOwner.ram,
                                          wrongOwner.devices, combinedSnapshot,
                                          &error));
    CHECK(ownerCpu == cpuBytes(wrongOwner));
    CHECK(ownerRam == ramBytes(wrongOwner));
    CHECK(ownerTimer == timerBytes(wrongOwner));
    CHECK_EQ(ownerDeviceCount, wrongOwner.devices.deviceCount());
    CHECK_EQ(ownerSourceCount, wrongOwner.devices.interruptSourceCount());

    // Missing one of the mandatory source records is rejected before capture.
    CHECK(machine.devices.removeInterruptSource(machine.timer->virtualSourceId(), &error));
    CHECK(!AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                                          machine.devices, rejected, &error));
}

void testIndependentMachinesAndLifecycleStress() {
    Machine left(0x10000, ExceptionLevel::EL1, 7, 1'000'000, 31, 401, 402);
    Machine right(0x10000, ExceptionLevel::EL1, 100, 2'000'000, 32, 403, 404);
    writeWords(left.ram, kGuestStart, std::vector<uint32_t>(3, kNop));
    writeWords(right.ram, kGuestStart, std::vector<uint32_t>(5, kNop));
    CHECK(runSteps(left, 3));
    CHECK(runSteps(right, 5));
    CHECK_EQ(10, left.timer->counter());
    CHECK_EQ(105, right.timer->counter());
    CHECK_EQ(2'000'000, right.timer->frequencyHz());
    std::string error;
    CHECK(left.devices.resetAll(&error));
    CHECK_EQ(7, left.timer->counter());
    CHECK_EQ(105, right.timer->counter());

    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        const uint64_t owner = 1000 + cycle;
        const uint64_t psrc = 3000 + cycle * 2;
        const uint64_t vsrc = psrc + 1;
        Machine machine(0x10000, ExceptionLevel::EL1, cycle, 1'000'000,
                        owner, psrc, vsrc);
        const std::vector<uint32_t> program = {
            movzX(0, 12), msr(timer_sysreg::kCntvTvalEl0, 0),
            movzX(0, 1), msr(timer_sysreg::kCntvCtlEl0, 0),
            kNop, kNop, kNop,
        };
        writeWords(machine.ram, kGuestStart, program);
        CHECK(runSteps(machine, 4));
        const uint64_t savedCounter = machine.timer->counter();
        const uint64_t savedComparator = machine.timer->virtualComparator();
        std::vector<uint8_t> snapshot;
        CHECK(AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                                             machine.devices, snapshot, &error));
        CHECK(runSteps(machine, 3));
        CHECK(AArch64DeviceSnapshot::restore(machine.cpu, machine.ram,
                                             machine.devices, snapshot, &error));
        CHECK_EQ(savedCounter, machine.timer->counter());
        CHECK_EQ(savedComparator, machine.timer->virtualComparator());
        CHECK(machine.devices.resetAll(&error));
        CHECK_EQ(cycle, machine.timer->counter());
        CHECK_EQ(0, readSource(machine.devices, psrc));
        CHECK_EQ(0, readSource(machine.devices, vsrc));
        CHECK(machine.devices.unregisterDevice(owner, &error));
        CHECK_EQ(0, machine.devices.deviceCount());
        CHECK_EQ(0, machine.devices.interruptSourceCount());
    }
}

void testFailedQuiescenceRestoreAtomic() {
    std::string error;
    Machine source;
    auto sourceProbe = std::make_unique<QuiescenceProbe>(30, true);
    QuiescenceProbe* sourceProbeView = sourceProbe.get();
    CHECK(source.devices.registerDevice(std::move(sourceProbe), &error));
    sourceProbeView->setValue(0xA5);
    writeWords(source.ram, kGuestStart, {kNop});
    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram,
                                         source.devices, snapshot, &error));

    Machine destination;
    auto destinationProbe = std::make_unique<QuiescenceProbe>(30, false);
    QuiescenceProbe* destinationProbeView = destinationProbe.get();
    CHECK(destination.devices.registerDevice(std::move(destinationProbe), &error));
    destinationProbeView->setValue(0x5A);
    writeWords(destination.ram, kGuestStart, {kNop, kNop});
    CHECK(guestStep(destination));

    const auto cpuBefore = destination.cpu.serialize_state();
    const auto ramBefore = std::vector<uint8_t>(
        destination.ram.GetRawData(),
        destination.ram.GetRawData() + destination.ram.GetTotalSize());
    std::vector<uint8_t> timerBefore;
    CHECK(destination.timer->captureState(timerBefore, &error));
    const uint64_t counterBefore = destination.timer->counter();
    const std::size_t deviceCountBefore = destination.devices.deviceCount();
    const std::size_t sourceCountBefore = destination.devices.interruptSourceCount();

    CHECK(!AArch64DeviceSnapshot::restore(destination.cpu, destination.ram,
                                          destination.devices, snapshot, &error));
    CHECK(cpuBefore == destination.cpu.serialize_state());
    CHECK(ramBefore == std::vector<uint8_t>(
        destination.ram.GetRawData(),
        destination.ram.GetRawData() + destination.ram.GetTotalSize()));
    std::vector<uint8_t> timerAfter;
    CHECK(destination.timer->captureState(timerAfter, &error));
    CHECK(timerBefore == timerAfter);
    CHECK_EQ(0x5A, destinationProbeView->value());
    CHECK_EQ(deviceCountBefore, destination.devices.deviceCount());
    CHECK_EQ(sourceCountBefore, destination.devices.interruptSourceCount());
    CHECK_EQ(0, readSource(destination.devices,
                           destination.timer->physicalSourceId()));
    CHECK_EQ(0, readSource(destination.devices,
                           destination.timer->virtualSourceId()));

    // The failed boundary resumed devices that had already quiesced.
    CHECK(guestStep(destination));
    CHECK_EQ(counterBefore + 1, destination.timer->counter());
}

void testRegisterEncodingAndUnsupportedEventStream() {
    CHECK_EQ(timer_sysreg::kCntfrqEl0, (3u << 14) | (3u << 11) | (14u << 7));
    CHECK_EQ(timer_sysreg::kCntpctEl0, (3u << 14) | (3u << 11) | (14u << 7) | 1u);
    CHECK_EQ(timer_sysreg::kCntvctEl0, (3u << 14) | (3u << 11) | (14u << 7) | 2u);
    CHECK_EQ(timer_sysreg::kCntkctlEl1, (3u << 14) | (14u << 7) | (1u << 3));
    CHECK_EQ(timer_sysreg::kCntpCtlEl0, (3u << 14) | (3u << 11) | (14u << 7) | (2u << 3) | 1u);
    CHECK_EQ(timer_sysreg::kCntvCvalEl0, (3u << 14) | (3u << 11) | (14u << 7) | (3u << 3) | 2u);

    Machine machine;
    const std::vector<uint32_t> words = {
        movzX(0, 1u << 2), msr(timer_sysreg::kCntkctlEl1, 0), kNop,
    };
    writeWords(machine.ram, kGuestStart, words);
    CHECK(runSteps(machine, 1));
    CHECK(!guestStep(machine)); // unsupported EVNTEN write has no partial effect
    CHECK_EQ(1, machine.timer->counter());
    CHECK_EQ(0, machine.timer->cntkctl());
    CHECK_EQ(kGuestStart + 4, machine.cpu.cpu().pc());
    machine.cpu.cpu().setPstate(2u << 2); // EL2 is explicitly outside the profile.
    CHECK(!guestStep(machine));
    CHECK_EQ(1, machine.timer->counter());
}

} // namespace

int main() {
    try {
        testRegisterEncodingAndUnsupportedEventStream();
        testProgramA_CounterVerification();
        testProgramB_VirtualTimer();
        testProgramC_PhysicalTimerAndSimultaneousExpiry();
        testProgramD_El0Controls();
        testProgramE_TimerSnapshotReplay();
        testProgramF_Pl011TimerCoexistence();
        testCounterWrapTvalCvalAndControls();
        testFrequencyTvalBoundariesAndIndependentComparators();
        testQuiescenceAndTimerResetIsolation();
        testSystemRegisterNegativeAndSnapshots();
        testIndependentMachinesAndLifecycleStress();
        testFailedQuiescenceRestoreAtomic();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "AARCH64-11 UNCAUGHT: " << error.what() << '\n';
    }
    std::cout << "AARCH64-11 Generic Timer: " << checks << " assertions, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
