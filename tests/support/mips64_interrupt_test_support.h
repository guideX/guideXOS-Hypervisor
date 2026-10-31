// Test helpers for deterministic MIPS64r2 interrupt and VM qualification.
// Programs are genuine encoded guest instructions; fixture writes are to
// physical memory and CKSEG0 guest addresses use the production direct map.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "IMemory.h"
#include "mips64/Mips64Cpu.h"

namespace mips64_interrupt_test {

constexpr uint64_t kKernelBase = 0xFFFFFFFF80000000ULL;
constexpr uint64_t kProgramBase = kKernelBase + 0x1000;
constexpr uint64_t kGeneralVectorBev0 = kKernelBase + 0x180;
constexpr uint64_t kInterruptVectorBev0 = kKernelBase + 0x200;
constexpr uint64_t kGeneralVectorBev1 = 0xFFFFFFFFBFC00380ULL;
constexpr uint64_t kInterruptVectorBev1 = 0xFFFFFFFFBFC00400ULL;

class SparsePhysicalMemory final : public ia64::IMemory {
public:
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(address + i);
            if (it == bytes_.end()) {
                throw std::out_of_range("unmapped physical memory read");
            }
            dest[i] = it->second;
        }
    }

    void Write(uint64_t address, const uint8_t* source, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            bytes_[address + i] = source[i];
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

    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { bytes_.clear(); }
    const uint8_t* GetRawData() const override { return nullptr; }

    uint32_t read32(uint64_t address) const {
        uint8_t b[4]{};
        Read(address, b, sizeof(b));
        return static_cast<uint32_t>(b[0]) |
               (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) |
               (static_cast<uint32_t>(b[3]) << 24);
    }

    uint64_t read64(uint64_t address) const {
        uint8_t b[8]{};
        Read(address, b, sizeof(b));
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(b[i]) << (8 * i);
        }
        return value;
    }

private:
    std::unordered_map<uint64_t, uint8_t> bytes_;
};

inline uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | funct;
}

inline uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | imm;
}

inline uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel = 0) {
    return (0x10U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | (static_cast<uint32_t>(rd) << 11) | sel;
}

inline uint64_t physicalForDirectSegment(uint64_t address) {
    if (address <= mips64::kKusegEnd) {
        return address;  // Only used by tests that installed the matching TLB map.
    }
    if (address >= mips64::kCKSeg0Base && address <= mips64::kCKSeg1End) {
        return address & mips64::kDirectMapMask;
    }
    throw std::invalid_argument("test address has no direct physical mapping");
}

inline void storeWord(ia64::IMemory& memory, uint64_t address, uint32_t word) {
    const uint64_t physical = physicalForDirectSegment(address);
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    memory.Write(physical, bytes, sizeof(bytes));
}

class GuestProgram {
public:
    GuestProgram(ia64::IMemory& memory, uint64_t guestAddress)
        : memory_(memory), pc_(guestAddress) {}

    void emit(uint32_t word) {
        storeWord(memory_, pc_, word);
        pc_ += 4;
        ++wordCount_;
    }

    void load32(unsigned reg, uint32_t value) {
        emit(encI(0x0F, 0, static_cast<uint8_t>(reg), static_cast<uint16_t>(value >> 16)));
        emit(encI(0x0D, static_cast<uint8_t>(reg), static_cast<uint8_t>(reg),
                  static_cast<uint16_t>(value)));
    }

    uint64_t pc() const { return pc_; }
    size_t wordCount() const { return wordCount_; }

private:
    ia64::IMemory& memory_;
    uint64_t pc_;
    size_t wordCount_ = 0;
};

// Initializes CP0 only by executing encoded guest instructions. Three
// steps after the Count write have occurred by the time Compare is written.
inline bool installTimerSetup(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                              uint64_t programBase, uint32_t status,
                              uint32_t count, uint32_t compare,
                              uint64_t& nextPc) {
    GuestProgram program(memory, programBase);
    program.load32(8, status);
    program.emit(encCp0(4, 8, 12));  // mtc0 $t0, Status
    program.load32(9, count);
    program.emit(encCp0(5, 9, 9));   // dmtc0 $t1, Count
    program.load32(10, compare);
    program.emit(encCp0(5, 10, 11)); // dmtc0 $t2, Compare

    cpu.setPc(programBase);
    for (size_t i = 0; i < program.wordCount(); ++i) {
        if (cpu.step(memory) != mips64::Mips64StepResult::Continue) {
            return false;
        }
    }
    nextPc = program.pc();
    return cpu.pc() == nextPc;
}

inline bool appendRun(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                      uint64_t startPc, size_t steps) {
    cpu.setPc(startPc);
    for (size_t i = 0; i < steps; ++i) {
        if (cpu.step(memory) != mips64::Mips64StepResult::Continue) {
            return false;
        }
    }
    return true;
}

}  // namespace mips64_interrupt_test
