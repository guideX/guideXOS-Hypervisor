#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aarch64 {

inline constexpr std::size_t kMaxRegisteredDevices = 64;
inline constexpr std::size_t kMaxDeviceStateBytes = 64 * 1024;
inline constexpr std::size_t kMaxTotalDeviceStateBytes = 1024 * 1024;
inline constexpr std::size_t kMaxInterruptSources = 256;
inline constexpr std::size_t kMaxDeviceTypeIdBytes = 63;

enum class AArch64InterruptMode : uint8_t { Level = 1, Edge = 2 };

enum class AArch64MmioAccessResult : uint8_t {
    Success = 0,
    ExternalAbort = 1,
    Unsupported = 2,
    Unmapped = 3
};

inline constexpr std::size_t kMaxAArch64MmioRegions = 64;
inline constexpr uint64_t kMaxAArch64MmioRegionBytes = 16ULL * 1024ULL * 1024ULL;

struct AArch64InterruptSourceState {
    uint64_t id = 0;
    uint64_t ownerDeviceId = 0;
    AArch64InterruptMode mode = AArch64InterruptMode::Level;
    bool asserted = false;
    bool pending = false;
};

// A device restore token owns fully validated replacement state. A device's
// commitRestore must only swap/apply preallocated state and must not fail.
class AArch64DeviceRestoreToken {
public:
    virtual ~AArch64DeviceRestoreToken() = default;
};

class AArch64StatefulDevice {
public:
    virtual ~AArch64StatefulDevice() = default;
    virtual uint64_t instanceId() const noexcept = 0;
    virtual std::string typeId() const = 0;
    virtual uint32_t stateSchemaVersion() const noexcept = 0;
    virtual std::size_t maxStateBytes() const noexcept = 0;
    virtual bool supportsSnapshot() const noexcept = 0;

    virtual bool reset(std::string* error) = 0;
    virtual bool quiesce(std::string* error) = 0;
    virtual void resume() noexcept = 0;
    virtual bool captureState(std::vector<uint8_t>& state, std::string* error) const = 0;
    // Must validate and stage without changing live device state.
    virtual bool stageRestore(const uint8_t* state, std::size_t size,
                              std::unique_ptr<AArch64DeviceRestoreToken>& token,
                              std::string* error) const = 0;
    virtual void commitRestore(
        std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept = 0;

    // Devices with derived or multiple interrupt outputs may validate their
    // serialized state against the snapshot's separately owned source records.
    // The default keeps existing single-device contracts source-agnostic.
    virtual bool validateInterruptSourceState(
        const uint8_t* state, std::size_t size,
        const std::vector<AArch64InterruptSourceState>& sources,
        std::string* error) const {
        (void)state; (void)size; (void)sources; (void)error;
        return true;
    }
};

// Optional interface for a stateful device that owns one physical MMIO
// aperture. The registry publishes this range together with device
// registration and removes it before destroying the owner.
class AArch64MMIODevice {
public:
    virtual ~AArch64MMIODevice() = default;
    virtual uint64_t mmioBase() const noexcept = 0;
    virtual uint64_t mmioApertureBytes() const noexcept = 0;
    virtual uint64_t mmioAlignment() const noexcept = 0;
    virtual uint32_t supportedMmioWidths() const noexcept = 0; // bit N means N-byte access
    virtual AArch64MmioAccessResult readMmio(uint64_t offset, uint8_t* bytes,
                                             std::size_t size) = 0;
    virtual AArch64MmioAccessResult writeMmio(uint64_t offset,
                                              const uint8_t* bytes,
                                              std::size_t size) = 0;
    virtual uint64_t interruptSourceId() const noexcept { return 0; }
};

// Per-machine owner of snapshot-capable devices and their interrupt outputs.
// The registry owns devices through unique_ptr; IDs are logical guest/platform
// identities and never derive from host addresses or registration order.
class AArch64DeviceRegistry final {
public:
    AArch64DeviceRegistry() = default;
    AArch64DeviceRegistry(const AArch64DeviceRegistry&) = delete;
    AArch64DeviceRegistry& operator=(const AArch64DeviceRegistry&) = delete;

    bool registerDevice(std::unique_ptr<AArch64StatefulDevice> device,
                        std::string* error = nullptr);
    bool unregisterDevice(uint64_t instanceId, std::string* error = nullptr);
    // Borrowed view, valid until the next successful registry mutation.
    AArch64StatefulDevice* findDevice(uint64_t instanceId) const;
    std::size_t deviceCount() const;

    bool registerInterruptSource(uint64_t sourceId, uint64_t ownerDeviceId,
                                 AArch64InterruptMode mode,
                                 std::string* error = nullptr);
    bool removeInterruptSource(uint64_t sourceId, std::string* error = nullptr);
    bool setLevel(uint64_t sourceId, bool asserted, std::string* error = nullptr);
    bool signalEdge(uint64_t sourceId, std::string* error = nullptr);
    bool consumeEdge(uint64_t sourceId, bool& consumed,
                     std::string* error = nullptr);
    bool queryInterruptSource(uint64_t sourceId, AArch64InterruptSourceState& state,
                              std::string* error = nullptr) const;
    bool resetInterruptSource(uint64_t sourceId, std::string* error = nullptr);
    std::size_t interruptSourceCount() const;

    bool resetAll(std::string* error = nullptr);

private:
    friend class AArch64DeviceSnapshot;
    friend class AArch64PhysicalBus;
    friend class AArch64PL011;
    friend class AArch64GenericTimer;
    struct RegisteredDevice {
        std::unique_ptr<AArch64StatefulDevice> device;
        AArch64MMIODevice* mmio = nullptr;
        std::string type;
        uint32_t schema = 0;
        std::size_t maxStateBytes = 0;
    };
    struct MmioRegion {
        uint64_t base = 0;
        uint64_t size = 0;
        uint64_t end = 0;
        uint64_t owner = 0;
        uint32_t supportedWidths = 0;
        AArch64MMIODevice* device = nullptr;
    };
    bool bindPhysicalRam(uint64_t ramBytes, const void* ramIdentity,
                         std::string* error);
    bool containsMmioRange(uint64_t address, std::size_t size) const;
    AArch64MmioAccessResult accessMmio(uint64_t address, uint8_t* bytes,
                                       std::size_t size, bool write);
    bool beginDeviceOperation(uint64_t ownerId, AArch64StatefulDevice*& device,
                              std::string* error);
    void endDeviceOperation() noexcept;
    bool beginSnapshot(std::vector<AArch64StatefulDevice*>& devices,
                      std::vector<AArch64InterruptSourceState>& sources,
                      std::string* error);
    void endSnapshot() noexcept;

    mutable std::mutex mutex_;
    bool snapshotActive_ = false;
    std::size_t activeDeviceOperations_ = 0;
    bool physicalRamBound_ = false;
    uint64_t physicalRamBytes_ = 0;
    const void* physicalRamIdentity_ = nullptr;
    std::map<uint64_t, RegisteredDevice> devices_;
    std::map<uint64_t, AArch64InterruptSourceState> sources_;
    std::map<uint64_t, MmioRegion> mmioRegions_;
};

} // namespace aarch64
