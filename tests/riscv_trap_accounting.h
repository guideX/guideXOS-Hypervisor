#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "riscv_cpu.h"

namespace riscv_test {

constexpr size_t kTrapAccountingEventLimit = 16384;
constexpr size_t kTrapAccountingNestingLimit = 64;
constexpr size_t kTrapAccountingHandlerPathLimit = 32;

struct TrapAccountingEvent {
    uint64_t id = 0;
    uint64_t pc = 0;
    uint64_t vector = 0;
    uint64_t machineEntryPc = 0;
    uint64_t cause = 0;
    uint64_t parentId = UINT64_MAX;
    uint64_t totalRetiredAtTrap = 0;
    uint64_t totalRetiredAtReturn = 0;
    uint64_t totalRetiredAtResume = 0;
    riscv::PrivilegeMode origin = riscv::PrivilegeMode::Supervisor;
    riscv::PrivilegeMode target = riscv::PrivilegeMode::Machine;
    bool interrupt = false;
    bool sbiCall = false;
    bool handlerInvocation = true;
    uint64_t sbiEid = 0;
    uint64_t sbiFid = 0;
    std::array<uint64_t, 6> sbiArgs{};
    uint64_t handlerInstructions = 0;
    std::array<uint64_t, kTrapAccountingHandlerPathLimit> handlerPcPrefix{};
    size_t handlerPcPrefixSize = 0;
    bool returned = false;
    bool resumed = false;
    uint64_t returnInstructionPc = 0;
    uint64_t returnTargetPc = 0;
    riscv::PrivilegeMode returnTargetPrivilege = riscv::PrivilegeMode::Supervisor;
    uint64_t resumedPc = 0;
    riscv::PrivilegeMode resumedPrivilege = riscv::PrivilegeMode::Supervisor;
    uint64_t sbiError = 0;
    uint64_t sbiValue = 0;
    bool sbiResultCaptured = false;
};

struct TrapAccountingDelta {
    bool trapEntered = false;
    size_t enteredEventIndex = SIZE_MAX;
    bool trapReturned = false;
    size_t returnedEventIndex = SIZE_MAX;
};

struct TrapAccountingCounters {
    uint64_t stepCalls = 0;
    uint64_t nonRetiringTrapTransitions = 0;
    uint64_t totalRetired = 0;
    uint64_t interruptTransitions = 0;
    uint64_t userRetired = 0;
    uint64_t supervisorRetired = 0;
    uint64_t machineRetired = 0;
    uint64_t supervisorLinuxInstructions = 0;
    uint64_t supervisorHandlerInstructions = 0;
    uint64_t machineHandlerInstructions = 0;
    uint64_t unattributedHandlerInstructions = 0;
    uint64_t supervisorEcalls = 0;
    uint64_t sbiCalls = 0;
    uint64_t machineEcalls = 0;
    uint64_t machineTimerInterrupts = 0;
    uint64_t supervisorTimerInterrupts = 0;
    uint64_t externalInterrupts = 0;
    uint64_t otherInterrupts = 0;
    uint64_t supervisorSynchronousExceptions = 0;
    uint64_t machineSynchronousExceptions = 0;
    uint64_t mretInstructions = 0;
    uint64_t sretInstructions = 0;
    uint64_t handlerEntries = 0;
    uint64_t handlerReturns = 0;
    uint64_t maxNestingDepth = 0;
    uint64_t largestHandlerInvocation = 0;
    uint64_t unmatchedReturns = 0;
    uint64_t privilegeMismatches = 0;
    uint64_t droppedEvents = 0;
};

class TrapAccounting {
public:
    TrapAccounting() = default;

    void reset() {
        counters_ = {};
        events_.clear();
        active_.clear();
        pendingResume_.clear();
        nextId_ = 0;
        uncertain_ = false;
    }

    void seed(uint64_t stepCalls, uint64_t totalRetired,
              uint64_t supervisorRetired, uint64_t machineRetired = 0) {
        counters_.stepCalls = stepCalls;
        counters_.totalRetired = totalRetired;
        counters_.supervisorRetired = supervisorRetired;
        counters_.machineRetired = machineRetired;
        counters_.supervisorLinuxInstructions = supervisorRetired;
    }

    TrapAccountingDelta observe(riscv::PrivilegeMode before,
                                const riscv::StepResult& step,
                                riscv::PrivilegeMode after,
                                const riscv::RV64CPU& cpu,
                                bool handlerInvocation = true) {
        TrapAccountingDelta delta;
        ++counters_.stepCalls;

        if (step.outcome == riscv::StepOutcome::Ok) {
            ++counters_.totalRetired;
            switch (before) {
                case riscv::PrivilegeMode::User: ++counters_.userRetired; break;
                case riscv::PrivilegeMode::Supervisor: ++counters_.supervisorRetired; break;
                case riscv::PrivilegeMode::Machine: ++counters_.machineRetired; break;
            }

            if (!active_.empty()) {
                ActiveFrame& frame = active_.back();
                if (frame.target != before) {
                    ++counters_.privilegeMismatches;
                    uncertain_ = true;
                    ++counters_.unattributedHandlerInstructions;
                } else {
                    ++frame.instructions;
                    counters_.largestHandlerInvocation =
                        std::max(counters_.largestHandlerInvocation, frame.instructions);
                    if (frame.eventIndex != SIZE_MAX && frame.eventIndex < events_.size()) {
                        TrapAccountingEvent& event = events_[frame.eventIndex];
                        ++event.handlerInstructions;
                        if (event.handlerPcPrefixSize < event.handlerPcPrefix.size())
                            event.handlerPcPrefix[event.handlerPcPrefixSize++] = step.pc;
                    }
                    if (before == riscv::PrivilegeMode::Machine)
                        ++counters_.machineHandlerInstructions;
                    else if (before == riscv::PrivilegeMode::Supervisor)
                        ++counters_.supervisorHandlerInstructions;
                    else
                        ++counters_.unattributedHandlerInstructions;
                }
            } else if (before == riscv::PrivilegeMode::Supervisor) {
                ++counters_.supervisorLinuxInstructions;
            }

            if (step.insn.mnemonic == riscv::Mnemonic::MRET) {
                ++counters_.mretInstructions;
                closeFrame(before, after, step, cpu, true, delta);
            } else if (step.insn.mnemonic == riscv::Mnemonic::SRET) {
                ++counters_.sretInstructions;
                closeFrame(before, after, step, cpu, false, delta);
            }

            for (auto it = pendingResume_.begin(); it != pendingResume_.end();) {
                if (*it < events_.size() && events_[*it].returnTargetPrivilege == before &&
                    events_[*it].returnTargetPc == step.pc) {
                    events_[*it].resumedPc = step.pc;
                    events_[*it].resumedPrivilege = before;
                    events_[*it].resumed = true;
                    events_[*it].totalRetiredAtResume = counters_.totalRetired;
                    it = pendingResume_.erase(it);
                } else {
                    ++it;
                }
            }
            return delta;
        }

        if (step.outcome != riscv::StepOutcome::Trap) return delta;

        ++counters_.nonRetiringTrapTransitions;
        if (step.interruptTaken) {
            ++counters_.interruptTransitions;
            const uint64_t cause = step.interruptCause;
            if (cause == riscv::kInterruptMachineTimer)
                ++counters_.machineTimerInterrupts;
            else if (cause == riscv::kInterruptSupervisorTimer)
                ++counters_.supervisorTimerInterrupts;
            else if (cause == 9 || cause == 11)
                ++counters_.externalInterrupts;
            else
                ++counters_.otherInterrupts;
        } else {
            const uint64_t cause = static_cast<uint64_t>(step.trapCause);
            if (before == riscv::PrivilegeMode::Supervisor) {
                ++counters_.supervisorSynchronousExceptions;
                if (cause == static_cast<uint64_t>(riscv::TrapCause::EcallFromS))
                    ++counters_.supervisorEcalls;
            } else if (before == riscv::PrivilegeMode::Machine) {
                ++counters_.machineSynchronousExceptions;
                if (cause == static_cast<uint64_t>(riscv::TrapCause::EcallFromM))
                    ++counters_.machineEcalls;
            }
        }

        if (active_.size() >= kTrapAccountingNestingLimit) {
            uncertain_ = true;
            ++counters_.droppedEvents;
            return delta;
        }

        TrapAccountingEvent event;
        event.id = nextId_++;
        event.pc = step.pc;
        event.vector = step.trapVector;
        event.machineEntryPc = step.trapTarget == riscv::PrivilegeMode::Machine
            ? step.nextPc : 0;
        event.cause = step.interruptTaken ? step.interruptCause
                                          : static_cast<uint64_t>(step.trapCause);
        event.totalRetiredAtTrap = counters_.totalRetired;
        event.origin = before;
        event.target = step.trapTarget;
        event.interrupt = step.interruptTaken;
        event.handlerInvocation = handlerInvocation;
        event.parentId = active_.empty() ? UINT64_MAX
                                         : active_.back().eventId;
        event.sbiCall = !step.interruptTaken &&
            step.trapCause == riscv::TrapCause::EcallFromS &&
            step.trapTarget == riscv::PrivilegeMode::Machine;
        if (event.sbiCall) {
            ++counters_.sbiCalls;
            event.sbiEid = cpu.readGR(17);
            event.sbiFid = cpu.readGR(16);
            for (size_t i = 0; i < event.sbiArgs.size(); ++i)
                event.sbiArgs[i] = cpu.readGR(10 + i);
        }

        size_t eventIndex = SIZE_MAX;
        if (events_.size() < kTrapAccountingEventLimit) {
            eventIndex = events_.size();
            events_.push_back(event);
        } else {
            ++counters_.droppedEvents;
        }
        if (!handlerInvocation) {
            delta.trapEntered = true;
            delta.enteredEventIndex = eventIndex;
            return delta;
        }
        ++counters_.handlerEntries;
        active_.push_back({step.trapTarget, event.id, eventIndex, 0});
        counters_.maxNestingDepth = std::max<uint64_t>(
            counters_.maxNestingDepth, active_.size());
        delta.trapEntered = true;
        delta.enteredEventIndex = eventIndex;
        return delta;
    }

    const TrapAccountingCounters& counters() const { return counters_; }
    const std::vector<TrapAccountingEvent>& events() const { return events_; }
    size_t activeHandlerInvocations() const { return active_.size(); }
    uint64_t activeHandlerEventId() const {
        return active_.empty() ? UINT64_MAX : active_.back().eventId;
    }
    riscv::PrivilegeMode activeHandlerTarget() const {
        return active_.empty() ? riscv::PrivilegeMode::User : active_.back().target;
    }
    bool lifetimesUncertain() const { return uncertain_; }

private:
    struct ActiveFrame {
        riscv::PrivilegeMode target;
        uint64_t eventId;
        size_t eventIndex;
        uint64_t instructions;
    };

    void closeFrame(riscv::PrivilegeMode before, riscv::PrivilegeMode after,
                    const riscv::StepResult& step, const riscv::RV64CPU& cpu,
                    bool machineReturn, TrapAccountingDelta& delta) {
        const bool matchingMode = machineReturn
            ? before == riscv::PrivilegeMode::Machine
            : before == riscv::PrivilegeMode::Supervisor;
        if (active_.empty()) return;
        if (!matchingMode || active_.back().target != before) {
            ++counters_.unmatchedReturns;
            if (!active_.empty()) uncertain_ = true;
            return;
        }
        const ActiveFrame frame = active_.back();
        active_.pop_back();
        ++counters_.handlerReturns;
        if (frame.eventIndex != SIZE_MAX && frame.eventIndex < events_.size()) {
            TrapAccountingEvent& event = events_[frame.eventIndex];
            event.returned = true;
            event.returnInstructionPc = step.pc;
            event.returnTargetPc = step.nextPc;
            event.returnTargetPrivilege = after;
            event.totalRetiredAtReturn = counters_.totalRetired;
            if (event.sbiCall) {
                event.sbiError = cpu.readGR(10);
                event.sbiValue = cpu.readGR(11);
                event.sbiResultCaptured = true;
            }
            delta.trapReturned = true;
            delta.returnedEventIndex = frame.eventIndex;
            pendingResume_.push_back(frame.eventIndex);
        }
    }

    TrapAccountingCounters counters_{};
    std::vector<TrapAccountingEvent> events_;
    std::vector<ActiveFrame> active_;
    std::vector<size_t> pendingResume_;
    uint64_t nextId_ = 0;
    bool uncertain_ = false;
};

}  // namespace riscv_test
