#include "Ns16550Uart.h"

#include <iostream>
#include <memory>
#include <vector>

using namespace ia64;

namespace {
unsigned gPass = 0;
unsigned gFail = 0;
#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

struct Fixture {
    Ns16550Mapping mapping = Ns16550Mapping::MaltaOriented();
    PhysicalAddressMap bus;
    std::shared_ptr<GuestConsoleSink> sink = std::make_shared<GuestConsoleSink>(64);
    std::shared_ptr<Ns16550Uart> uart = std::make_shared<Ns16550Uart>(sink, 3);

    Fixture() { bus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(), uart, {1}); }
    void write(unsigned reg, uint8_t value) {
        bus.Write(mapping.physicalBase + reg * mapping.RegisterSpacing(), &value, 1);
    }
    uint8_t read(unsigned reg) {
        uint8_t value = 0;
        bus.Read(mapping.physicalBase + reg * mapping.RegisterSpacing(), &value, 1);
        return value;
    }
};

void testHeadErrorsSummaryAndLsrAcknowledgment() {
    Fixture f;
    f.write(2, 0x01); // FIFO, trigger 1.
    f.write(1, 0x05); // RDI + receiver line status.
    const std::vector<Ns16550RxInput> input{
        {'A', Ns16550Uart::kLsrParityError},
        {'B', 0},
        {'C', Ns16550Uart::kLsrFramingError},
        {'X', Ns16550Uart::kLsrBreakIndication},
        {'D', static_cast<uint8_t>(Ns16550Uart::kLsrParityError |
                                   Ns16550Uart::kLsrFramingError)}};
    CHECK(f.uart->EnqueueHostInput(input));
    CHECK(f.uart->AdvanceHostInput() == input.size());
    CHECK(f.uart->ReceiveFifoSize() == input.size());
    CHECK(f.read(2) == 0xC6); // Receiver line status outranks RDA.
    CHECK(f.read(5) == 0xE5); // PE at head, and errors remain later in FIFO.
    CHECK(f.read(2) == 0xC4); // LSR acknowledges only the current line status.
    CHECK(f.read(5) == 0xE1); // Head PE is acknowledged; FIFO summary remains.
    CHECK(f.read(0) == 'A');
    CHECK(f.read(5) == 0xE1); // Clean head does not hide later errors.
    CHECK(f.read(0) == 'B');
    CHECK(f.read(2) == 0xC6); // FE is exposed when its character reaches head.
    CHECK(f.read(5) == 0xE9);
    CHECK(f.read(0) == 'C'); // LSR still does not consume FE character.
    CHECK(f.read(2) == 0xC6); // BI follows FE at the FIFO head.
    CHECK(f.read(5) == 0xF1);
    CHECK(f.read(0) == 0);   // Break is associated with a NUL, not input 'X'.
    CHECK(f.read(2) == 0xC6); // Combined PE+FE appears on the last character.
    CHECK(f.read(5) == 0xED);
    CHECK(f.read(0) == 'D');
    CHECK(f.read(5) == 0x60);
    CHECK(f.read(2) == 0xC1);
    CHECK(!*f.uart->InterruptSignal());
}

void testInterruptMaskingAndTimeoutPriority() {
    Fixture f;
    f.write(2, 0x41); // FIFO, trigger 4.
    f.write(1, 0x01); // RDI only: line status remains masked.
    const std::vector<Ns16550RxInput> input{{'P', Ns16550Uart::kLsrParityError}};
    CHECK(f.uart->EnqueueHostInput(input));
    CHECK(f.uart->AdvanceHostInput() == 1);
    const uint64_t deadline = f.uart->ReceiveTimeoutDeadline();
    CHECK(f.read(2) == 0xC1);
    CHECK(!*f.uart->InterruptSignal());

    f.write(1, 0x05); // Enabling IER[2] reveals the retained PE.
    CHECK(f.read(2) == 0xC6);
    CHECK(*f.uart->InterruptSignal());
    f.uart->AdvanceVirtualTime(deadline);
    CHECK(f.uart->ReceiveTimeoutPending());
    CHECK(f.read(2) == 0xC6); // Line status has priority over timeout.
    CHECK(f.read(5) == 0xE5);
    CHECK(f.uart->ReceiveTimeoutDeadline() == deadline); // LSR does not retime RX.
    CHECK(f.read(2) == 0xCC); // After LSR acknowledgment, timeout is visible.
    CHECK(f.read(0) == 'P'); // RBR acknowledges timeout and consumes the byte.
    CHECK(f.read(2) == 0xC1);
    CHECK(!*f.uart->InterruptSignal());

    CHECK(f.uart->InjectReceivedCharacter({'Q', Ns16550Uart::kLsrFramingError}));
    f.write(1, 0x01);
    CHECK(f.read(2) == 0xC1); // Disabling IER[2] masks without erasing FE.
    f.write(1, 0x04);
    CHECK(f.read(2) == 0xC6);
}

void testOverrunCapacityAndIndependentAcknowledgment() {
    Fixture f;
    f.write(2, 0x01);
    f.write(1, 0x04); // RLS only isolates OE acknowledgment from RDA.
    for (uint8_t value = 0; value < Ns16550Uart::kReceiveFifoCapacity; ++value) {
        CHECK(f.uart->InjectReceivedCharacter({value, 0}));
    }
    CHECK(!f.uart->InjectReceivedCharacter({0xEE, Ns16550Uart::kLsrParityError}));
    CHECK(f.uart->ReceiveFifoSize() == Ns16550Uart::kReceiveFifoCapacity);
    CHECK(f.read(2) == 0xC6);
    CHECK(f.read(5) == 0x63); // OE is distinct from FIFO character errors.
    CHECK(f.read(2) == 0xC1);
    CHECK(!f.uart->InjectReceivedCharacter({0xEF, 0}));
    CHECK(f.read(5) == 0x63); // A later overrun is independently observable.
    CHECK(f.read(0) == 0); // Oldest data survived both rejected characters.
    CHECK(f.uart->InjectReceivedCharacter({0xF0, 0})); // Space can be reused.
    CHECK(f.uart->ReceiveFifoSize() == Ns16550Uart::kReceiveFifoCapacity);

    const std::vector<Ns16550RxInput> tooMany(Ns16550Uart::kHostInputCapacity + 1,
                                              Ns16550RxInput{0x55, 0});
    CHECK(!f.uart->EnqueueHostInput(tooMany)); // Separate atomic staging overflow policy.
    CHECK(f.read(5) & 0x02);
    CHECK(f.read(5) & 0x01); // LSR acknowledgment leaves FIFO data ready.
    f.write(2, 0x03); // Receiver FIFO clear removes data and error metadata.
    CHECK(f.uart->ReceiveFifoSize() == 0);
    CHECK(f.read(5) == 0x60);
    CHECK(f.read(2) == 0xC1);
    f.uart->Reset();
    CHECK(f.read(5) == 0x60 && f.read(2) == 0x01);
}

void testNonFifoErrorsAndBreakVersusNul() {
    Fixture f;
    f.write(1, 0x04);
    CHECK(f.uart->EnqueueHostInput(std::vector<Ns16550RxInput>{
        {'Q', Ns16550Uart::kLsrFramingError}}));
    CHECK(f.uart->AdvanceHostInput() == 1);
    CHECK(f.read(2) == 0x06); // Non-FIFO IIR has no FIFO identification bits.
    CHECK(f.read(5) == 0x69); // FE and DR, with LSR[7] always clear.
    CHECK(f.read(2) == 0x01); // LSR acknowledged FE but did not consume RBR.
    CHECK(f.read(5) == 0x61);
    CHECK(f.read(0) == 'Q');

    CHECK(f.uart->EnqueueHostInput(std::vector<Ns16550RxInput>{{0, 0}}));
    CHECK(f.uart->AdvanceHostInput() == 1);
    CHECK(f.read(2) == 0x01); // Ordinary NUL is not a break.
    CHECK(f.read(5) == 0x61);
    CHECK(f.read(0) == 0);
    CHECK(f.uart->EnqueueHostInput(std::vector<Ns16550RxInput>{
        {'X', Ns16550Uart::kLsrBreakIndication}}));
    CHECK(f.uart->AdvanceHostInput() == 1);
    CHECK(f.read(2) == 0x06);
    CHECK(f.read(5) == 0x71); // BI is distinct and break data is normalized to NUL.
    CHECK(f.read(0) == 0);
}

void testSnapshotPreservesAcknowledgedHeadAndLaterErrors() {
    Fixture source;
    source.write(2, 0x01);
    source.write(1, 0x04);
    CHECK(source.uart->EnqueueHostInput(std::vector<Ns16550RxInput>{
        {'A', Ns16550Uart::kLsrParityError},
        {'B', Ns16550Uart::kLsrFramingError}}));
    CHECK(source.uart->AdvanceHostInput() == 2);
    CHECK(source.read(5) == 0xE5);
    const auto snapshot = source.uart->CaptureSnapshot();
    CHECK(snapshot.version == 4 && Ns16550Uart::ValidateSnapshot(snapshot));
    CHECK(snapshot.receiveFifo[0].errorStatusAcknowledged);
    CHECK(!snapshot.receiveFifo[1].errorStatusAcknowledged);

    Fixture destination;
    Ns16550Uart::PreparedRestore prepared;
    CHECK(destination.uart->PrepareRestore(snapshot, prepared));
    destination.uart->CommitRestore(snapshot, std::move(prepared));
    CHECK(destination.read(2) == 0xC1); // Acked head PE is not replayed.
    CHECK(destination.read(5) == 0xE1); // FE deeper in FIFO remains summarized.
    CHECK(destination.read(0) == 'A');
    CHECK(destination.read(2) == 0xC6); // Later FE appears at the new FIFO head.
    CHECK(destination.read(5) == 0xE9);

    auto invalid = snapshot;
    invalid.receiveFifo[1].errorFlags = 0x80;
    CHECK(!Ns16550Uart::ValidateSnapshot(invalid));
}

} // namespace

int main() {
    testHeadErrorsSummaryAndLsrAcknowledgment();
    testInterruptMaskingAndTimeoutPriority();
    testOverrunCapacityAndIndependentAcknowledgment();
    testNonFifoErrorsAndBreakVersusNul();
    testSnapshotPreservesAcknowledgedHeadAndLaterErrors();
    std::cout << "MIPS64 UART line status: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
