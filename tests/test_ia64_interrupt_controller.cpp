#include "IA64ISAPlugin.h"
#include "decoder.h"
#include "memory.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    ia64::InstructionDecoder decoder;
    ia64::IA64ISAPlugin plugin(decoder);
    ia64::Memory memory(1024 * 1024);

    require(plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                ia64::IA64_SPURIOUS_INT_VECTOR,
            "idle IVR returns the IA-64 spurious vector");

    plugin.setInterruptsEnabled(true);
    plugin.queueInterrupt(0x20);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x20,
            "single pending vector is visible through IVR");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                ia64::IA64_SPURIOUS_INT_VECTOR,
            "EOI retires the vector and leaves IVR spurious");

    plugin.queueInterrupt(0x20);
    plugin.queueInterrupt(0x80);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x80,
            "highest-priority pending vector is selected first");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x20,
            "next pending vector is selected after EOI");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                ia64::IA64_SPURIOUS_INT_VECTOR,
            "all pending vectors retire cleanly");

    plugin.writeControlRegister(ia64::IA64_CR_TPR, 0x20);
    plugin.queueInterrupt(0x20);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                ia64::IA64_SPURIOUS_INT_VECTOR,
            "TPR masks vectors in the same priority class");
    require(plugin.hasPendingInterrupt(), "TPR masking retains the event");
    plugin.writeControlRegister(ia64::IA64_CR_TPR, 0);
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x20,
            "restoring TPR exposes the retained event");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);

    plugin.reset();
    plugin.getCPUState().SetIP(0x2000);
    plugin.getCPUState().SetCR(ia64::IA64_CR_IVA, 0x1000);
    plugin.setInterruptsEnabled(false);
    plugin.queueInterrupt(0x40);
    require(plugin.hasPendingInterrupt() && !plugin.hasInServiceInterrupt(),
            "disabled PSR.i retains a pending interrupt without entry");
    require(!plugin.tryDeliverPendingInterrupt(),
            "disabled PSR.i blocks interrupt entry");
    plugin.setInterruptsEnabled(true);
    const uint64_t savedPsr = plugin.getCPUState().GetCR(ia64::IA64_CR_IPSR);
    require(plugin.tryDeliverPendingInterrupt(),
            "re-enabling PSR.i permits entry at an instruction boundary");
    require(plugin.getCPUState().GetIP() == 0x1400,
            "interrupt entry uses CR.IVA plus vector stride");
    require(plugin.getCPUState().GetCR(ia64::IA64_CR_IIP) == 0x2000,
            "interrupt entry saves IIP");
    require((plugin.getCPUState().GetPSR() & ia64::IA64_PSR_I) == 0,
            "interrupt entry clears PSR.i");
    require(savedPsr == 0, "initial IPSR storage is not fabricated");
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x40,
            "IVR reports the vector that caused entry");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);

    plugin.getCPUState().SetAR(44, 0);
    plugin.setInterruptsEnabled(true);
    plugin.writeControlRegister(ia64::IA64_CR_ITV, 0xEF);
    plugin.writeControlRegister(ia64::IA64_CR_ITM, 100);
    plugin.advanceITC(99);
    require(!plugin.hasPendingInterrupt(), "ITM does not fire early");
    plugin.advanceITC(1);
    require(plugin.hasPendingInterrupt(), "ITC reaching ITM latches ITV");
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0xEF,
            "timer vector is exposed through IVR");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    plugin.writeControlRegister(ia64::IA64_CR_ITM, 200);
    plugin.advanceITC(99);
    require(!plugin.hasPendingInterrupt(),
            "reprogramming ITM clears the retired compare latch");
    plugin.advanceITC(1);
    require(plugin.hasPendingInterrupt(), "re-armed timer fires once at new ITM");

    const auto& telemetry = plugin.getInterruptTelemetry();
    require(telemetry.ivrReads >= 2, "IVR telemetry counts architectural reads");
    require(telemetry.eoiWrites >= 2, "EOI telemetry counts architectural writes");
    require(telemetry.timerCompareEvents == 2,
            "timer telemetry counts initial and re-armed compare events");

    plugin.reset();
    plugin.getCPUState().SetAR(44, 0);
    plugin.writeControlRegister(ia64::IA64_CR_ITV, 0xEF | ia64::IA64_ITV_MASK);
    plugin.writeControlRegister(ia64::IA64_CR_ITM, 1);
    plugin.advanceITC(1);
    require(!plugin.hasPendingInterrupt(), "masked ITV suppresses timer injection");
    plugin.writeControlRegister(ia64::IA64_CR_ITV, 0xEF);
    require(plugin.hasPendingInterrupt(), "unmasking a passed compare exposes one event");

    std::cout << "IA-64 interrupt-controller tests passed" << std::endl;
    return 0;
}
