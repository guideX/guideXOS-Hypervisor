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
    struct RegisteredDevice {
        std::unique_ptr<AArch64StatefulDevice> device;
        std::string type;
        uint32_t schema = 0;
        std::size_t maxStateBytes = 0;
    };
    bool beginSnapshot(std::vector<AArch64StatefulDevice*>& devices,
                      std::vector<AArch64InterruptSourceState>& sources,
                      std::string* error);
    void endSnapshot() noexcept;

    mutable std::mutex mutex_;
    bool snapshotActive_ = false;
    std::map<uint64_t, RegisteredDevice> devices_;
    std::map<uint64_t, AArch64InterruptSourceState> sources_;
};

} // namespace aarch64
