#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "PPC64VirtualTerminal.h"

namespace ia64::ppc64 {

enum class PPC64PlatformProfile : uint32_t {
    NONE = 0,
    POWER9_PSERIES_FOUNDATION = 1,
};

enum class PPC64GuestEntryMode : uint32_t {
    FIRMWARE_ASSISTED = 1,
    DIRECT_FDT = 2,
};

struct PPC64MemoryRange {
    uint64_t base = 0;
    uint64_t size = 0;
    std::string purpose;
    bool guestAccessible = true;
};

struct PPC64PlatformConfiguration {
    PPC64PlatformProfile profile =
        PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    PPC64GuestEntryMode entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    uint32_t cpuCount = 1;
    uint64_t timebaseFrequency = 512'000'000ULL;
    uint64_t ticksPerInstruction = 1;
    uint64_t initialTimebase = 0;
    uint64_t requestedFdtAddress = 0; // zero selects deterministic top-of-RAM placement
    uint64_t maximumFdtSize = 64 * 1024;
    // Expose the VM-owned PAPR hvterm1 through the guest FDT when requested.
    bool virtualTerminalEnabled = false;
    uint32_t virtualTerminalId = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID;
    std::string bootArguments;
    // Host-supplied reservations for page tables and other guest RAM objects
    // whose ownership is not inferable from the PPC64 MMU configuration.
    std::vector<PPC64MemoryRange> reservedRanges;
};

struct PPC64GuestMemoryLayout {
    uint64_t ramBase = 0;
    uint64_t ramSize = 0;
    uint64_t imageBase = 0;
    uint64_t imageSize = 0;
    uint64_t fdtBase = 0;
    uint64_t fdtSize = 0;
    uint64_t entryAddress = 0;
    uint64_t initrdBase = 0;
    uint64_t initrdSize = 0;
    std::vector<PPC64MemoryRange> reservations;
    std::vector<PPC64MemoryRange> guestMemoryRanges;
};

struct PPC64PlatformSnapshot {
    bool enabled = false;
    PPC64PlatformConfiguration configuration;
    bool prepared = false;
    PPC64GuestMemoryLayout memoryLayout;
    uint64_t currentTimebase = 0;
};

// Host-owned state for the deliberately small PPC64 interrupt input profile.
// DEC pending follows the POWER9 level condition (DEC[0] == 1 with LPCR[LD]=0)
// and the external input remains asserted until the host deasserts it.
inline constexpr uint32_t PPC64_INTERRUPT_SNAPSHOT_VERSION = 1;
inline constexpr uint8_t PPC64_EXTERNAL_ROUTE_GUEST_SRR = 1;
inline constexpr uint8_t PPC64_INTERRUPT_SOURCE_NONE = 0;
inline constexpr uint8_t PPC64_INTERRUPT_SOURCE_EXTERNAL = 1;
inline constexpr uint8_t PPC64_INTERRUPT_SOURCE_DECREMENTER = 2;

struct PPC64VirtualInterruptSnapshot {
    uint32_t version = PPC64_INTERRUPT_SNAPSHOT_VERSION;
    bool decrementerPending = false;
    bool externalPending = false;
    bool externalAsserted = false;
    uint16_t externalLPID = 0;
    uint8_t externalRoutingProfile = PPC64_EXTERNAL_ROUTE_GUEST_SRR;
    uint8_t lastDeliverySource = PPC64_INTERRUPT_SOURCE_NONE;
    uint64_t lastDeliveryVector = 0;
    uint64_t lastDeliverySRR0 = 0;
    uint16_t lastDeliveryLPID = 0;
    uint64_t deliveryCount = 0;
};

struct PPC64PlatformCapabilities {
    bool power9FoundationProfile = false;
    bool directFdtEntry = false;
    bool validFdt = false;
    bool deterministicTimebase = false;
    bool paprHypercallSubset = false;
    bool firmwareClientInterface = false;
    bool virtualTerminal = false;
    bool interruptController = false;
    bool rtas = false;
    bool pciHostBridge = false;
    bool virtualStorage = false;
    bool smp = false;
};

class PPC64VirtualTimeBase {
public:
    PPC64VirtualTimeBase() = default;
    explicit PPC64VirtualTimeBase(const PPC64PlatformConfiguration& configuration);

    bool configure(const PPC64PlatformConfiguration& configuration,
                   std::string& reason);
    void reset();
    void advanceInstruction();
    uint64_t read() const { return value_; }
    uint64_t frequency() const { return frequency_; }
    uint64_t ticksPerInstruction() const { return ticksPerInstruction_; }
    uint64_t initialValue() const { return initialValue_; }
    bool restore(uint64_t value);

private:
    uint64_t value_ = 0;
    uint64_t initialValue_ = 0;
    uint64_t frequency_ = 0;
    uint64_t ticksPerInstruction_ = 0;
};

class PPC64FDTBuilder {
public:
    static bool build(const PPC64PlatformConfiguration& configuration,
                      const PPC64GuestMemoryLayout& layout,
                      std::vector<uint8_t>& output, std::string& reason);
    static bool validate(const uint8_t* data, size_t size,
                         std::string& reason);
};

bool validatePPC64PlatformConfiguration(
    const PPC64PlatformConfiguration& configuration, uint64_t ramSize,
    std::string& reason);
bool samePPC64PlatformConfiguration(
    const PPC64PlatformConfiguration& left,
    const PPC64PlatformConfiguration& right);
bool validatePPC64GuestMemoryLayout(const PPC64GuestMemoryLayout& layout,
                                    std::string& reason);
PPC64PlatformCapabilities getPPC64FoundationCapabilities();

} // namespace ia64::ppc64
