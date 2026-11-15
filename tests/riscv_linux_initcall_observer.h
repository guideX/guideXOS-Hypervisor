#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace riscv_test {

struct LinuxInitcallCallbackCounts {
    uint64_t entries = 0;
    uint64_t completions = 0;
    uint64_t zeroReturns = 0;
    uint64_t nonzeroReturns = 0;
    std::map<int32_t, uint64_t> returnCodes;
};

struct LinuxInitcallInvocation {
    std::string callbackSymbol;
    uint64_t callbackAddress = 0;
    uint64_t entryRetired = 0;
    uint64_t returnAddress = 0;
    uint64_t taskPointer = 0;
    size_t detailEventIndex = static_cast<size_t>(-1);
};

struct LinuxInitcallCompletion {
    LinuxInitcallInvocation invocation;
    uint64_t returnRetired = 0;
    uint64_t returnValueA0 = 0;
    int32_t returnCode = 0;
};

class LinuxInitcallObserver {
public:
    static constexpr size_t kMaximumActiveInvocations = 64;
    static constexpr size_t kNoDetail = static_cast<size_t>(-1);

    explicit LinuxInitcallObserver(size_t detailLimit = 256,
                                   size_t activeLimit = kMaximumActiveInvocations)
        : detailLimit_(detailLimit), activeLimit_(activeLimit) {}

    bool detailAvailable() const { return retainedDetails_ < detailLimit_; }

    void enter(const std::string& callbackSymbol, uint64_t callbackAddress,
               uint64_t entryRetired, uint64_t returnAddress,
               uint64_t taskPointer, size_t detailEventIndex) {
        ++entries_;
        ++callbackCounts_[callbackSymbol].entries;
        if (detailEventIndex == kNoDetail)
            ++droppedDetails_;
        else
            ++retainedDetails_;

        if (active_.size() >= activeLimit_) {
            ++untrackedEntries_;
            return;
        }
        active_.push_back({callbackSymbol, callbackAddress, entryRetired,
                           returnAddress, taskPointer, detailEventIndex});
        if (active_.size() > maximumActiveDepth_)
            maximumActiveDepth_ = active_.size();
    }

    bool complete(uint64_t returnAddress, uint64_t taskPointer,
                  uint64_t returnValueA0, uint64_t returnRetired,
                  LinuxInitcallCompletion& completion) {
        for (size_t reverse = active_.size(); reverse > 0; --reverse) {
            const size_t index = reverse - 1;
            const LinuxInitcallInvocation& invocation = active_[index];
            if (invocation.returnAddress != returnAddress ||
                invocation.taskPointer != taskPointer) continue;

            completion.invocation = invocation;
            completion.returnRetired = returnRetired;
            completion.returnValueA0 = returnValueA0;
            completion.returnCode = static_cast<int32_t>(
                static_cast<uint32_t>(returnValueA0));

            LinuxInitcallCallbackCounts& counts = callbackCounts_[invocation.callbackSymbol];
            ++completions_;
            ++counts.completions;
            ++counts.returnCodes[completion.returnCode];
            if (completion.returnCode == 0) {
                ++zeroReturns_;
                ++counts.zeroReturns;
            } else {
                ++nonzeroReturns_;
                ++counts.nonzeroReturns;
            }
            active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        return false;
    }

    uint64_t entries() const { return entries_; }
    uint64_t completions() const { return completions_; }
    uint64_t zeroReturns() const { return zeroReturns_; }
    uint64_t nonzeroReturns() const { return nonzeroReturns_; }
    uint64_t droppedDetails() const { return droppedDetails_; }
    uint64_t untrackedEntries() const { return untrackedEntries_; }
    size_t retainedDetails() const { return retainedDetails_; }
    size_t activeInvocations() const { return active_.size(); }
    size_t maximumActiveDepth() const { return maximumActiveDepth_; }
    const std::vector<LinuxInitcallInvocation>& active() const { return active_; }
    const std::map<std::string, LinuxInitcallCallbackCounts>& callbackCounts() const {
        return callbackCounts_;
    }

private:
    size_t detailLimit_ = 0;
    size_t activeLimit_ = 0;
    size_t retainedDetails_ = 0;
    size_t maximumActiveDepth_ = 0;
    uint64_t entries_ = 0;
    uint64_t completions_ = 0;
    uint64_t zeroReturns_ = 0;
    uint64_t nonzeroReturns_ = 0;
    uint64_t droppedDetails_ = 0;
    uint64_t untrackedEntries_ = 0;
    std::vector<LinuxInitcallInvocation> active_;
    std::map<std::string, LinuxInitcallCallbackCounts> callbackCounts_;
};

} // namespace riscv_test
