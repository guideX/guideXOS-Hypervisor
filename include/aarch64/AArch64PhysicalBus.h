#pragma once

#include "IMemory.h"
#include "aarch64/AArch64DeviceState.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ia64 { class Memory; }

namespace aarch64 {

// RTTI marker consumed by Cpu without tying existing AArch64-only targets to
// the bus implementation object file.
class AArch64PhysicalMemory {
public:
    virtual ~AArch64PhysicalMemory() = default;
    virtual bool containsMmioRange(uint64_t address, size_t size) const = 0;
};

enum class AArch64PhysicalFaultKind : uint8_t {
    ExternalAbort,
    UnsupportedOperation
};

class AArch64PhysicalAccessFault final : public std::runtime_error {
public:
    AArch64PhysicalAccessFault(AArch64PhysicalFaultKind kind,
                               const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    AArch64PhysicalFaultKind kind() const noexcept { return kind_; }
private:
    AArch64PhysicalFaultKind kind_;
};

// A narrow physical dispatcher for one AArch64 machine. Address arguments to
// this class are already guest physical addresses; it never translates them.
// RAM remains owned by ia64::Memory and stateful devices remain owned by the
// paired AArch64DeviceRegistry.
class AArch64PhysicalBus final : public ia64::IMemory,
                                public AArch64PhysicalMemory {
public:
    AArch64PhysicalBus(ia64::Memory& ram, AArch64DeviceRegistry& devices);
    AArch64PhysicalBus(const AArch64PhysicalBus&) = delete;
    AArch64PhysicalBus& operator=(const AArch64PhysicalBus&) = delete;

    void Read(uint64_t address, uint8_t* dest, size_t size) const override;
    void Write(uint64_t address, const uint8_t* src, size_t size) override;
    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override;
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override;
    size_t GetTotalSize() const override;
    void Clear() override;
    const uint8_t* GetRawData() const override;

    ia64::Memory& ram() const noexcept { return *ram_; }
    bool containsMmioRange(uint64_t address, size_t size) const override;

private:
    ia64::Memory* ram_;
    AArch64DeviceRegistry* devices_;
};

} // namespace aarch64
