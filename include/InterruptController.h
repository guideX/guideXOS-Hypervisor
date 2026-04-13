#pragma once
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ia64 {

using InterruptDeliveryCallback = std::function<void(uint8_t vector)>;

struct InterruptSource {
    size_t id;
    std::string name;
    uint8_t vector;
    bool enabled;
};

class IInterruptController {
public:
    virtual ~IInterruptController() = default;

    virtual void RegisterDeliveryCallback(InterruptDeliveryCallback callback) = 0;
    virtual size_t RegisterSource(const std::string& name, uint8_t vector) = 0;
    virtual bool SetSourceEnabled(size_t sourceId, bool enabled) = 0;
    virtual bool RaiseInterrupt(uint8_t vector) = 0;
    virtual bool RaiseInterrupt(size_t sourceId) = 0;
    virtual bool HasPendingInterrupt() const = 0;
    virtual bool GetNextPendingInterrupt(uint8_t& vector) = 0;
    virtual void DeliverPendingInterrupts() = 0;
    virtual void Reset() = 0;
};

class BasicInterruptController : public IInterruptController {
public:
    BasicInterruptController();
    ~BasicInterruptController() override = default;

    void RegisterDeliveryCallback(InterruptDeliveryCallback callback) override;
    size_t RegisterSource(const std::string& name, uint8_t vector) override;
    bool SetSourceEnabled(size_t sourceId, bool enabled) override;
    bool RaiseInterrupt(uint8_t vector) override;
    bool RaiseInterrupt(size_t sourceId) override;
    bool HasPendingInterrupt() const override;
    bool GetNextPendingInterrupt(uint8_t& vector) override;
    void DeliverPendingInterrupts() override;
    void Reset() override;

private:
    InterruptDeliveryCallback deliveryCallback_;
    std::vector<InterruptSource> sources_;
    std::deque<uint8_t> pendingVectors_;
    size_t nextSourceId_;
};

} // namespace ia64
