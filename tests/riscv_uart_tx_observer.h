#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

#include "riscv_cpu.h"

namespace riscv_test {

constexpr size_t kUartTxObservationLimit = 16384;

enum class UartTxOrigin : uint8_t {
    OpenSbiBeforeLinuxHandoff,
    LinuxSupervisorAfterHandoff,
    OpenSbiAfterLinuxHandoff,
    Other
};

inline const char* UartTxOriginName(UartTxOrigin origin) {
    switch (origin) {
        case UartTxOrigin::OpenSbiBeforeLinuxHandoff: return "opensbi_before_linux_handoff";
        case UartTxOrigin::LinuxSupervisorAfterHandoff: return "linux_s_mode_after_handoff";
        case UartTxOrigin::OpenSbiAfterLinuxHandoff: return "opensbi_after_linux_handoff";
        case UartTxOrigin::Other: return "other";
    }
    return "other";
}

struct UartTxObservation {
    uint64_t outputSequence = 0;
    uint64_t mmioSequence = 0;
    uint8_t uartIndex = 0;
    uint8_t byte = 0;
    uint64_t uartOutputOffset = 0;
    uint64_t guestPc = 0;
    riscv::PrivilegeMode privilege = riscv::PrivilegeMode::Machine;
    uint64_t postHandoffRetirements = 0;
    uint64_t mmioAddress = 0;
    uint8_t mmioRegisterOffset = 0;
    uint8_t mmioSize = 0;
    UartTxOrigin origin = UartTxOrigin::Other;
    bool afterLinuxHandoff = false;
    bool matchedMmioEvent = false;
    bool matchedStep = false;
    bool sbiCallAssociated = false;
    uint64_t sbiEventId = UINT64_MAX;
    uint64_t sbiEid = 0;
    uint64_t sbiFid = 0;
};

// Observes bytes that the existing UART model actually accepted into its TX
// buffer and binds each byte to the matching guest MMIO write event. This
// class never calls a device write or changes CPU, UART, RX, PLIC, or timer
// state; consumers sample it immediately after CPU::step().
class UartTxObserver {
public:
    explicit UartTxObserver(size_t capacity = kUartTxObservationLimit)
        : capacity_(capacity) {}

    void reset(const riscv::RV64CPU& cpu) {
        observations_.clear();
        outputBytes_ = {};
        outputBytesByOrigin_ = {};
        droppedEvents_ = 0;
        attributionFailures_ = 0;
        nextOutputSequence_ = 0;
        afterLinuxHandoff_ = false;
        handoffRetiredCounter_ = 0;
        lastMmioSequence_ = cpu.machineMmioEventSequence();
        lastOutputSizes_[0] = cpu.machineUartOutput().size();
        lastOutputSizes_[1] = cpu.machineUart1Output().size();
    }

    void beginLinuxHandoff(const riscv::RV64CPU& cpu) {
        afterLinuxHandoff_ = true;
        handoffRetiredCounter_ = cpu.instructionsRetired();
        lastMmioSequence_ = cpu.machineMmioEventSequence();
        lastOutputSizes_[0] = cpu.machineUartOutput().size();
        lastOutputSizes_[1] = cpu.machineUart1Output().size();
    }

    size_t observeAfterStep(const riscv::RV64CPU& cpu,
                            const riscv::StepResult& step,
                            riscv::PrivilegeMode privilegeBefore,
                            uint64_t postHandoffRetirements) {
        const std::array<const std::string*, 2> output{{
            &cpu.machineUartOutput(), &cpu.machineUart1Output()}};
        const uint64_t sequenceAfter = cpu.machineMmioEventSequence();
        size_t newlyObserved = 0;

        for (size_t uart = 0; uart < output.size(); ++uart) {
            const std::string& bytes = *output[uart];
            if (bytes.size() < lastOutputSizes_[uart]) {
                // A device reset starts a new observation epoch. Call reset()
                // explicitly so earlier output cannot be mistaken for it.
                ++attributionFailures_;
                lastOutputSizes_[uart] = bytes.size();
                continue;
            }
            for (size_t offset = lastOutputSizes_[uart]; offset < bytes.size(); ++offset) {
                const uint8_t value = static_cast<uint8_t>(bytes[offset]);
                const riscv::MachineMmioEvent* match = FindTransmitEvent(
                    cpu, uart, value, step.pc, lastMmioSequence_);

                UartTxObservation observation;
                observation.outputSequence = ++nextOutputSequence_;
                observation.uartIndex = static_cast<uint8_t>(uart);
                observation.byte = value;
                observation.uartOutputOffset = offset;
                observation.guestPc = match != nullptr ? match->pc : step.pc;
                observation.privilege = match != nullptr ? match->privilege : privilegeBefore;
                observation.afterLinuxHandoff = afterLinuxHandoff_;
                observation.postHandoffRetirements = afterLinuxHandoff_
                    ? postHandoffRetirements : 0;
                observation.matchedMmioEvent = match != nullptr;
                observation.matchedStep = match != nullptr &&
                    match->pc == step.pc && match->privilege == privilegeBefore &&
                    step.outcome == riscv::StepOutcome::Ok;
                if (match != nullptr) {
                    observation.mmioSequence = match->sequence;
                    observation.mmioAddress = match->address;
                    observation.mmioRegisterOffset = static_cast<uint8_t>(
                        match->address - UartBase(uart));
                    observation.mmioSize = match->size;
                } else {
                    ++attributionFailures_;
                }
                observation.origin = Classify(observation.afterLinuxHandoff,
                                              observation.privilege);
                const size_t originIndex = static_cast<size_t>(observation.origin);
                if (originIndex < outputBytesByOrigin_.size())
                    ++outputBytesByOrigin_[originIndex][uart];
                Retain(std::move(observation));
                ++outputBytes_[uart];
                ++newlyObserved;
            }
            lastOutputSizes_[uart] = bytes.size();
        }

        lastMmioSequence_ = sequenceAfter;
        return newlyObserved;
    }

    void associateRecentSbiCall(uint64_t mmioSequence, uint64_t eventId,
                                uint64_t eid, uint64_t fid) {
        for (auto it = observations_.rbegin(); it != observations_.rend(); ++it) {
            if (it->mmioSequence != mmioSequence) continue;
            it->sbiCallAssociated = true;
            it->sbiEventId = eventId;
            it->sbiEid = eid;
            it->sbiFid = fid;
            return;
        }
    }

    const std::deque<UartTxObservation>& observations() const { return observations_; }
    const std::array<uint64_t, 2>& outputByteCounts() const { return outputBytes_; }
    const std::array<std::array<uint64_t, 2>, 4>& outputByteCountsByOrigin() const {
        return outputBytesByOrigin_;
    }
    size_t droppedEvents() const { return droppedEvents_; }
    size_t attributionFailures() const { return attributionFailures_; }
    bool afterLinuxHandoff() const { return afterLinuxHandoff_; }
    uint64_t handoffRetiredCounter() const { return handoffRetiredCounter_; }
    size_t capacity() const { return capacity_; }

private:
    static uint64_t UartBase(size_t uart) {
        return uart == 0 ? 0x10000000ULL : 0x1000A000ULL;
    }

    static UartTxOrigin Classify(bool afterHandoff, riscv::PrivilegeMode privilege) {
        if (privilege == riscv::PrivilegeMode::Machine) {
            return afterHandoff ? UartTxOrigin::OpenSbiAfterLinuxHandoff
                                : UartTxOrigin::OpenSbiBeforeLinuxHandoff;
        }
        if (afterHandoff && privilege == riscv::PrivilegeMode::Supervisor)
            return UartTxOrigin::LinuxSupervisorAfterHandoff;
        return UartTxOrigin::Other;
    }

    static const riscv::MachineMmioEvent* FindTransmitEvent(
        const riscv::RV64CPU& cpu, size_t uart, uint8_t value,
        uint64_t stepPc, uint64_t afterSequence) {
        const uint64_t base = UartBase(uart);
        for (auto it = cpu.machineMmioTrace().rbegin();
             it != cpu.machineMmioTrace().rend(); ++it) {
            if (it->sequence <= afterSequence) break;
            if (it->device == riscv::MachineMmioDevice::Uart &&
                it->operation == riscv::MachineMmioOperation::Write &&
                it->address == base && it->size == 1 &&
                static_cast<uint8_t>(it->value) == value && it->pc == stepPc) {
                return &*it;
            }
        }
        return nullptr;
    }

    void Retain(UartTxObservation observation) {
        if (capacity_ == 0) {
            ++droppedEvents_;
            return;
        }
        if (observations_.size() == capacity_) {
            observations_.pop_front();
            ++droppedEvents_;
        }
        observations_.push_back(std::move(observation));
    }

    size_t capacity_ = kUartTxObservationLimit;
    std::deque<UartTxObservation> observations_;
    std::array<size_t, 2> lastOutputSizes_{};
    std::array<uint64_t, 2> outputBytes_{};
    std::array<std::array<uint64_t, 2>, 4> outputBytesByOrigin_{};
    uint64_t lastMmioSequence_ = 0;
    uint64_t nextOutputSequence_ = 0;
    uint64_t handoffRetiredCounter_ = 0;
    size_t droppedEvents_ = 0;
    size_t attributionFailures_ = 0;
    bool afterLinuxHandoff_ = false;
};

}  // namespace riscv_test
