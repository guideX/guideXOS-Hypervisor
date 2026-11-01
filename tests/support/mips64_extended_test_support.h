// Shared sparse-memory and encoded-guest helpers for MIPS64-5 tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "IMemory.h"
#include "mips64/Mips64Cpu.h"

namespace mips64_extended_test {

class SparsePhysicalMemory final : public ia64::IMemory {
public:
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        for (size_t i = 0; i < size; ++i) {
            const auto it = storage_.find(address + i);
            if (it == storage_.end()) throw std::out_of_range("unmapped sparse read");
            dest[i] = it->second;
        }
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        for (size_t i = 0; i < size; ++i) storage_[address + i] = src[i];
    }
    void loadBuffer(uint64_t address, const uint8_t* src, size_t size) override {
        Write(address, src, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& src) override {
        if (!src.empty()) Write(address, src.data(), src.size());
    }
    size_t GetTotalSize() const override { return storage_.size(); }
    void Clear() override { storage_.clear(); }
    const uint8_t* GetRawData() const override { return nullptr; }

private:
    std::unordered_map<uint64_t, uint8_t> storage_;
};

inline uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t fn) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) | (uint32_t(rd) << 11) |
           (uint32_t(sa) << 6) | fn;
}

inline uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) | imm;
}

inline uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel = 0) {
    return (0x10U << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | sel;
}

inline void storeWord(ia64::IMemory& memory, uint64_t address, uint32_t word,
                      bool littleEndian) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i) {
        bytes[littleEndian ? i : 3 - i] = static_cast<uint8_t>(word >> (8 * i));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

inline void store32(ia64::IMemory& memory, uint64_t address, uint32_t value,
                    bool littleEndian) {
    storeWord(memory, address, value, littleEndian);
}

inline uint32_t load32(const ia64::IMemory& memory, uint64_t address,
                       bool littleEndian) {
    uint8_t bytes[4] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[littleEndian ? i : 3 - i]) << (8 * i);
    }
    return value;
}

inline void store64(ia64::IMemory& memory, uint64_t address, uint64_t value,
                    bool littleEndian) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i) {
        bytes[littleEndian ? i : 7 - i] = static_cast<uint8_t>(value >> (8 * i));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

inline uint64_t load64(const ia64::IMemory& memory, uint64_t address,
                       bool littleEndian) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= uint64_t(bytes[littleEndian ? i : 7 - i]) << (8 * i);
    }
    return value;
}

inline uint32_t encDsll(uint8_t reg, uint8_t amount) {
    return encR(0, reg, reg, amount, 0x38);
}

inline void emitLoad64(std::vector<uint32_t>& words, uint8_t reg, uint64_t value) {
    words.push_back(encI(0x0D, 0, reg, static_cast<uint16_t>(value >> 48)));
    words.push_back(encDsll(reg, 16));
    words.push_back(encI(0x0D, reg, reg, static_cast<uint16_t>(value >> 32)));
    words.push_back(encDsll(reg, 16));
    words.push_back(encI(0x0D, reg, reg, static_cast<uint16_t>(value >> 16)));
    words.push_back(encDsll(reg, 16));
    words.push_back(encI(0x0D, reg, reg, static_cast<uint16_t>(value)));
}

inline bool runKernel(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                       uint64_t codePhys, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        storeWord(memory, codePhys + i * 4, words[i], cpu.isLittleEndian());
    }
    const uint64_t savedPc = cpu.pc();
    cpu.setPc(mips64::kCKSeg0Base | (codePhys & mips64::kDirectMapMask));
    bool ok = true;
    for (size_t i = 0; i < words.size(); ++i) {
        if (cpu.step(memory) != mips64::Mips64StepResult::Continue) {
            ok = false;
            break;
        }
    }
    cpu.setPc(savedPc);
    return ok;
}

inline bool writeStatus(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                        uint64_t value, uint64_t codePhys = 0x6800ULL) {
    cpu.writeGpr(8, value);
    return runKernel(cpu, memory, codePhys, {encCp0(5, 8, 12)});
}

inline bool installTlbPair(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                           uint64_t va, uint32_t lo0, uint32_t lo1,
                           uint8_t asid, unsigned index,
                           uint64_t codePhys = 0x6900ULL) {
    const uint64_t entryHi = (va & (mips64::kEntryHiRegionMask |
                                    mips64::kEntryHiVpn2Mask)) | asid;
    cpu.writeGpr(8, entryHi);
    cpu.writeGpr(9, lo0);
    cpu.writeGpr(10, lo1);
    cpu.writeGpr(11, 0);  // 4 KiB pages
    cpu.writeGpr(12, index);
    return runKernel(cpu, memory, codePhys,
                     {encCp0(5, 8, 10), encCp0(5, 11, 5), encCp0(5, 9, 2),
                      encCp0(5, 10, 3), encCp0(5, 12, 0), 0x42000002U});
}

inline bool installTlbPairRandom(mips64::Mips64Cpu& cpu, ia64::IMemory& memory,
                                 uint64_t va, uint32_t lo0, uint32_t lo1,
                                 uint8_t asid,
                                 uint64_t codePhys = 0x6A00ULL) {
    const uint64_t entryHi = (va & (mips64::kEntryHiRegionMask |
                                    mips64::kEntryHiVpn2Mask)) | asid;
    cpu.writeGpr(8, entryHi);
    cpu.writeGpr(9, lo0);
    cpu.writeGpr(10, lo1);
    cpu.writeGpr(11, 0);
    return runKernel(cpu, memory, codePhys,
                     {encCp0(5, 8, 10), encCp0(5, 11, 5), encCp0(5, 9, 2),
                      encCp0(5, 10, 3), 0x42000006U});
}

inline uint32_t entryLo(uint32_t physicalPageNumber, bool global = true,
                        bool valid = true, bool dirty = true, uint8_t cca = 2) {
    return ((physicalPageNumber & mips64::kEntryLoPfnMask) << mips64::kEntryLoPfnShift) |
           (uint32_t(cca & 7U) << mips64::kEntryLoCShift) |
           (dirty ? mips64::kEntryLoDBit : 0U) |
           (valid ? mips64::kEntryLoVBit : 0U) |
           (global ? mips64::kEntryLoGBit : 0U);
}

}  // namespace mips64_extended_test
