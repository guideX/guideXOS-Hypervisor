#include "Console.h"
#include "InterruptController.h"
#include "Timer.h"
#include "cpu.h"
#include "decoder.h"
#include "memory.h"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace ia64;

namespace {

void TestConsoleMMIO() {
    std::ostringstream output;
    Memory memory(0x4000);
    VirtualConsole console(0x1000, output);
    memory.RegisterDevice(&console);

    const char message[] = { 'H', 'i', '\n' };
    for (char value : message) {
        memory.write<uint8_t>(console.GetBaseAddress(), static_cast<uint8_t>(value));
    }

    assert(output.str() == "Hi\n");
    assert(memory.read<uint8_t>(console.GetBaseAddress() + VirtualConsole::kStatusRegisterOffset) == 0);
}

void TestInterruptControllerDelivery() {
    Memory memory(0x4000);
    InstructionDecoder decoder;
    CPU cpu(memory, decoder);
    BasicInterruptController controller;

    cpu.setInterruptsEnabled(true);
    cpu.setInterruptVectorBase(0x200);

    const size_t timerSource = controller.RegisterSource("timer", 0x31);
    controller.RegisterDeliveryCallback([&cpu](uint8_t vector) {
        cpu.queueInterrupt(vector);
    });

    assert(controller.RaiseInterrupt(timerSource));
    assert(cpu.hasPendingInterrupt());
}

void TestTimerMMIO() {
    Memory memory(0x4000);
    BasicInterruptController controller;
    VirtualTimer timer(0x1200);
    memory.RegisterDevice(&timer);

    int deliveredCount = 0;
    uint8_t deliveredVector = 0;
    controller.RegisterDeliveryCallback([&](uint8_t vector) {
        deliveredVector = vector;
        ++deliveredCount;
    });
    timer.AttachInterruptController(&controller);

    memory.write<uint64_t>(timer.GetBaseAddress() + VirtualTimer::kIntervalOffset, 3);
    memory.write<uint8_t>(timer.GetBaseAddress() + VirtualTimer::kVectorOffset, 0x40);
    memory.write<uint8_t>(timer.GetBaseAddress() + VirtualTimer::kControlOffset, 0x03);

    timer.Tick(2);
    assert(deliveredCount == 0);

    timer.Tick(1);
    assert(deliveredCount == 1);
    assert(deliveredVector == 0x40);
    assert(timer.HasPendingInterrupt());

    memory.write<uint8_t>(timer.GetBaseAddress() + VirtualTimer::kStatusOffset, 1);
    assert(!timer.HasPendingInterrupt());
}

int RunIoDeviceTests() {
    TestConsoleMMIO();
    TestInterruptControllerDelivery();
    TestTimerMMIO();
    std::cout << "All IO device tests passed!\n";
    return 0;
}

} // namespace

#if defined(IA64EMU_IO_TEST_STANDALONE)
int main() {
    return RunIoDeviceTests();
}
#endif
