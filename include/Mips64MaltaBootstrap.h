#pragma once

#include "Ns16550Uart.h"
#include "PhysicalAddressMap.h"
#include "mips64/Mips64Cpu.h"
#include "mips64/Mips64MaltaProm.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ia64 {

constexpr uint64_t kMaltaFlashBase = 0x1E000000ULL;
constexpr uint64_t kMaltaFpgaBase = 0x1F000000ULL;
constexpr uint64_t kMaltaUartBase = 0x1F000900ULL;
constexpr uint64_t kMaltaResetRomBase = 0x1FC00000ULL;
constexpr uint64_t kMaltaBootImageWindow = 4ULL * 1024 * 1024;
constexpr uint64_t kMaltaResetVector = 0xFFFFFFFFBFC00000ULL;
constexpr uint64_t kMaltaRamPayloadVirtualAddress = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kMaltaRamPayloadPhysicalAddress = 0x00001000ULL;

enum class MaltaBootMode : uint8_t {
    SyntheticDemo = 0,
    ElfKernelHandoff = 1
};

struct MaltaBootstrapConfig {
    mips64::Mips64Endianness endianness = mips64::Mips64Endianness::Little;
    uint8_t switches = 0;
    uint8_t jumpers = 0;
    bool mapFlashMirror = true;
    bool useBuiltInDemoRom = true;
    std::vector<uint8_t> bootRom;
    MaltaBootMode bootMode = MaltaBootMode::SyntheticDemo;
    uint64_t guestRamSize = 0;
    uint64_t kernelEntryPoint = 0;
    bool kernelRequiresKx = false;
    uint64_t kernelImageIdentity = 0;
    mips64::MaltaPromConfig prom;
};

struct MaltaResetMailbox {
    std::atomic_bool pending{false};
    std::atomic_uint64_t requests{0};
};

// Small, strict subset of the Malta FPGA registers. Registers are native to
// the selected guest byte order and accept aligned 32-bit transactions only.
class MaltaFpgaRegisters final : public IMmioDevice {
public:
    struct Snapshot {
        uint32_t version = 1;
        mips64::Mips64Endianness endianness = mips64::Mips64Endianness::Little;
        uint8_t switches = 0;
        uint8_t jumpers = 0;
        uint8_t ledBar = 0;
        std::array<char, 8> asciiDisplay{};
        uint8_t breakReset = 0x0A;
        bool resetPending = false;
        uint64_t resetRequestCount = 0;
    };

    MaltaFpgaRegisters(mips64::Mips64Endianness endianness,
                       uint8_t switches, uint8_t jumpers,
                       std::shared_ptr<MaltaResetMailbox> resetMailbox);

    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 4; }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override;
    void Write(uint64_t offset, const uint8_t* source, size_t size) override;
    void Reset() override;
    std::map<std::string, uint64_t> InspectState() const override;

    uint8_t LedBar() const noexcept { return ledBar_; }
    const std::array<char, 8>& AsciiDisplay() const noexcept { return asciiDisplay_; }
    uint8_t BreakReset() const noexcept { return breakReset_; }
    bool ResetPending() const noexcept;
    uint64_t ResetRequestCount() const noexcept;
    Snapshot CaptureSnapshot() const noexcept;
    bool ValidateSnapshot(const Snapshot& snapshot) const noexcept;
    void RestoreSnapshot(const Snapshot& snapshot) noexcept;

private:
    uint32_t ReadRegister(uint64_t offset) const;
    void WriteRegister(uint64_t offset, uint32_t value);
    uint32_t DecodeWord(const uint8_t* source) const noexcept;
    void EncodeWord(uint32_t value, uint8_t* destination) const noexcept;

    mips64::Mips64Endianness endianness_;
    uint8_t switches_;
    uint8_t jumpers_;
    uint8_t ledBar_ = 0;
    uint8_t breakReset_ = 0x0A;
    std::array<char, 8> asciiDisplay_{};
    std::shared_ptr<MaltaResetMailbox> resetMailbox_;
};

// VM-scoped board configuration and device owner. The physical map owns its
// mapped devices as well; the board exposes shared handles for inspection.
class MaltaBootstrapBoard {
public:
    MaltaBootstrapBoard(PhysicalAddressMap& bus,
                        const MaltaBootstrapConfig& config);
    MaltaBootstrapBoard(const MaltaBootstrapBoard&) = delete;
    MaltaBootstrapBoard& operator=(const MaltaBootstrapBoard&) = delete;

    static std::vector<uint8_t> BuildDemoBootRom(mips64::Mips64Endianness endianness);
    static std::vector<uint8_t> BuildDemoRamPayload(mips64::Mips64Endianness endianness);
    static std::vector<uint8_t> BuildElfHandoffBootRom(
        mips64::Mips64Endianness endianness, uint64_t kernelEntryPoint,
        uint64_t guestRamSize, bool kernelRequiresKx,
        uint32_t argumentCount = 2,
        uint32_t argumentTablePointer32 = 0x80002000,
        uint32_t environmentTablePointer32 = 0x80002008);

    const MaltaBootstrapConfig& Config() const noexcept { return config_; }
    const std::vector<uint8_t>& BootRomImage() const noexcept { return bootRom_; }
    const std::shared_ptr<GuestConsoleSink>& ConsoleSink() const noexcept { return consoleSink_; }
    const std::shared_ptr<Ns16550Uart>& Uart() const noexcept { return uart_; }
    const std::shared_ptr<MaltaFpgaRegisters>& Fpga() const noexcept { return fpga_; }

    bool ConsumeSoftwareResetRequest() noexcept;
    bool HasSoftwareResetRequest() const noexcept;
    uint64_t SoftwareResetRequestCount() const noexcept;

private:
    MaltaBootstrapConfig config_;
    std::vector<uint8_t> bootRom_;
    std::shared_ptr<GuestConsoleSink> consoleSink_;
    std::shared_ptr<Ns16550Uart> uart_;
    std::shared_ptr<MaltaResetMailbox> resetMailbox_;
    std::shared_ptr<MaltaFpgaRegisters> fpga_;
};

}  // namespace ia64
