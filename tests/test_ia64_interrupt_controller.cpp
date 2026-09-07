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
    static_assert(ia64::IA64_CR_LID == 64 && ia64::IA64_CR_IVR == 65 &&
                      ia64::IA64_CR_TPR == 66 && ia64::IA64_CR_EOI == 67 &&
                      ia64::IA64_CR_IRR0 == 68 && ia64::IA64_CR_IRR1 == 69 &&
                      ia64::IA64_CR_IRR2 == 70 && ia64::IA64_CR_IRR3 == 71 &&
                      ia64::IA64_CR_ITV == 72 && ia64::IA64_CR_PMV == 73 &&
                      ia64::IA64_CR_CMCV == 74 && ia64::IA64_CR_LRR0 == 80 &&
                      ia64::IA64_CR_LRR1 == 81,
                  "external interrupt control registers use the architectural cr3 selectors");

    ia64::InstructionDecoder decoder;
    ia64::IA64ISAPlugin plugin(decoder);
    ia64::Memory memory(1024 * 1024);

    const ia64::InstructionEx authenticIrr3Read =
        decoder.DecodeSlot(0x2124700480ULL, ia64::UnitType::M_UNIT, 0x4A07100);
    require(authenticIrr3Read.GetType() == ia64::InstructionType::MOV_FROM_CR &&
                authenticIrr3Read.GetSrc1() == ia64::IA64_CR_IRR3 &&
                authenticIrr3Read.GetDst() == 18,
            "authentic Linux IRR3 read decodes as mov r18=cr71");

    auto irrBit = [](uint8_t vector) -> uint64_t {
        return 1ULL << (vector & 63U);
    };

    require(plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                ia64::IA64_SPURIOUS_INT_VECTOR,
            "idle IVR returns the IA-64 spurious vector");

    plugin.queueInterrupt(0x20);
    plugin.queueInterrupt(0x40);
    plugin.queueInterrupt(0x80);
    plugin.queueInterrupt(0xC0);
    plugin.queueInterrupt(0xEF);
    plugin.queueInterrupt(0xFF);
    require(plugin.readControlRegister(ia64::IA64_CR_IRR0) == irrBit(0x20),
            "IRR0 maps vector 0x20 to bit 32");
    require(plugin.readControlRegister(ia64::IA64_CR_IRR1) == irrBit(0x40),
            "IRR1 maps vector 0x40 to bit 0");
    require(plugin.readControlRegister(ia64::IA64_CR_IRR2) == irrBit(0x80),
            "IRR2 maps vector 0x80 to bit 0");
    require(plugin.readControlRegister(ia64::IA64_CR_IRR3) ==
                (irrBit(0xC0) | irrBit(0xEF) | irrBit(0xFF)),
            "IRR3 maps vectors 0xC0, 0xEF, and 0xFF");
    require(plugin.readControlRegister(ia64::IA64_CR_IRR3) ==
                (irrBit(0xC0) | irrBit(0xEF) | irrBit(0xFF)),
            "IRR reads are non-destructive");
    bool irrWriteThrew = false;
    try {
        plugin.writeControlRegister(ia64::IA64_CR_IRR3, 0);
    } catch (const std::runtime_error&) {
        irrWriteThrew = true;
    }
    require(irrWriteThrew &&
                plugin.readControlRegister(ia64::IA64_CR_IRR3) ==
                    (irrBit(0xC0) | irrBit(0xEF) | irrBit(0xFF)),
            "writes to read-only IRR registers raise an illegal-operation error");

    plugin.reset();
    plugin.setInterruptsEnabled(false);
    plugin.queueInterrupt(0xEF);
    require(plugin.readControlRegister(ia64::IA64_CR_IRR3) == irrBit(0xEF),
            "PSR.i disabled does not hide a pending IRR3 bit");
    require(!plugin.tryDeliverPendingInterrupt() &&
                plugin.readControlRegister(ia64::IA64_CR_IRR3) == irrBit(0xEF) &&
                !plugin.hasInServiceInterrupt(),
            "PSR.i blocks entry without changing pending IRR state");

    plugin.writeControlRegister(ia64::IA64_CR_TPR, 0xE0);
    require(plugin.readControlRegister(ia64::IA64_CR_IRR3) == irrBit(0xEF) &&
                plugin.readControlRegister(ia64::IA64_CR_IVR) ==
                    ia64::IA64_SPURIOUS_INT_VECTOR,
            "TPR masks delivery while the pending IRR bit remains visible");
    plugin.writeControlRegister(ia64::IA64_CR_TPR, 0);
    plugin.setInterruptsEnabled(true);
    require(plugin.tryDeliverPendingInterrupt() &&
                plugin.readControlRegister(ia64::IA64_CR_IRR3) == irrBit(0xEF),
            "eligible entry does not consume IRR before IVR");
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0xEF &&
                plugin.readControlRegister(ia64::IA64_CR_IRR3) == 0 &&
                plugin.hasInServiceInterrupt() && plugin.getInServiceVector() == 0xEF,
            "IVR returns the vector and clears only its IRR bit");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    require(!plugin.hasInServiceInterrupt() &&
                plugin.readControlRegister(ia64::IA64_CR_IRR3) == 0,
            "EOI clears in-service state after IVR consumption");

    plugin.reset();
    plugin.setInterruptsEnabled(true);
    plugin.queueInterrupt(0x40);
    plugin.queueInterrupt(0x90);
    require(plugin.readControlRegister(ia64::IA64_CR_IRR1) == irrBit(0x40) &&
                plugin.readControlRegister(ia64::IA64_CR_IRR2) == irrBit(0x90),
            "multiple pending vectors coexist in distinct IRR banks");
    require(plugin.tryDeliverPendingInterrupt() && plugin.getInServiceVector() == 0x90,
            "highest-priority pending vector enters service first");
    require(plugin.readControlRegister(ia64::IA64_CR_IRR2) == irrBit(0x90) &&
                plugin.readControlRegister(ia64::IA64_CR_IRR1) == irrBit(0x40),
            "lower-priority pending IRR remains visible under in-service priority");
    require(plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x90 &&
                plugin.readControlRegister(ia64::IA64_CR_IRR2) == 0,
            "IVR consumes only the in-service vector request");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);
    require(plugin.readControlRegister(ia64::IA64_CR_IRR1) == irrBit(0x40),
            "EOI does not clear an unrelated pending IRR bit");
    plugin.setInterruptsEnabled(true);
    require(plugin.tryDeliverPendingInterrupt() &&
                plugin.readControlRegister(ia64::IA64_CR_IVR) == 0x40,
            "next eligible vector is delivered after EOI");
    plugin.writeControlRegister(ia64::IA64_CR_EOI, 0);

    plugin.reset();
    plugin.queueInterrupt(0x60);
    const std::vector<uint8_t> irrSnapshot = plugin.serialize_state();
    ia64::IA64ISAPlugin restored(decoder);
    require(restored.deserialize_state(irrSnapshot) &&
                restored.readControlRegister(ia64::IA64_CR_IRR1) == irrBit(0x60),
            "IRR is reconstructed from serialized pending vectors");

    plugin.reset();
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
