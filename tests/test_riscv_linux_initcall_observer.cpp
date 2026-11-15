#include "riscv_linux_initcall_observer.h"

#include <cstdint>
#include <iostream>

namespace {

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (condition) {
        std::cout << "PASS: " << description << '\n';
        return;
    }
    std::cerr << "FAIL: " << description << '\n';
    ++g_failures;
}

void TestCappedInitcallDetailsAndAggregateResults() {
    using riscv_test::LinuxInitcallCompletion;
    using riscv_test::LinuxInitcallObserver;

    LinuxInitcallObserver observer(1, 2);
    Check(observer.detailAvailable(), "observer begins with one available detail slot");
    observer.enter("core_init", 0x1000, 10, 0x2000, 0xA000, 7);
    Check(!observer.detailAvailable(), "first callback consumes the bounded detail slot");
    observer.enter("optional_device_init", 0x1100, 20, 0x2100, 0xA000,
                   LinuxInitcallObserver::kNoDetail);
    observer.enter("active_limit_overflow", 0x1200, 30, 0x2200, 0xA000,
                   LinuxInitcallObserver::kNoDetail);

    Check(observer.entries() == 3 && observer.retainedDetails() == 1 &&
          observer.droppedDetails() == 2 && observer.activeInvocations() == 2 &&
          observer.untrackedEntries() == 1 && observer.maximumActiveDepth() == 2,
          "aggregate entries and bounded detail/active retention remain independent");

    LinuxInitcallCompletion completion;
    const uint64_t enodev = static_cast<uint64_t>(static_cast<int64_t>(-19));
    Check(observer.complete(0x2100, 0xA000, enodev, 40, completion) &&
          completion.invocation.callbackSymbol == "optional_device_init" &&
          completion.invocation.detailEventIndex == LinuxInitcallObserver::kNoDetail &&
          completion.returnCode == -19,
          "a dropped detail still identifies its callback and nonzero result");
    Check(observer.complete(0x2000, 0xA000, 0, 50, completion) &&
          completion.invocation.callbackSymbol == "core_init" &&
          completion.invocation.detailEventIndex == 7,
          "retained callback details stay paired with the matching return");
    Check(!observer.complete(0x2200, 0xA000, 0, 60, completion),
          "an entry beyond the active-invocation cap is not fabricated as complete");

    const auto& callbacks = observer.callbackCounts();
    const auto optional = callbacks.find("optional_device_init");
    const auto untracked = callbacks.find("active_limit_overflow");
    Check(observer.completions() == 2 && observer.zeroReturns() == 1 &&
          observer.nonzeroReturns() == 1 && observer.activeInvocations() == 0 &&
          optional != callbacks.end() && optional->second.completions == 1 &&
          optional->second.returnCodes.at(-19) == 1 &&
          untracked != callbacks.end() && untracked->second.entries == 1 &&
          untracked->second.completions == 0,
          "completion classes and per-callback result aggregates reconcile");
}

} // namespace

int main() {
    TestCappedInitcallDetailsAndAggregateResults();
    if (g_failures != 0) {
        std::cerr << "RISC-V initcall observer regression failures=" << g_failures << '\n';
        return 1;
    }
    std::cout << "RISC-V initcall observer regression passed\n";
    return 0;
}
