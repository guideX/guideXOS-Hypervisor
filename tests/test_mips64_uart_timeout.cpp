#include "Ns16550Uart.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using namespace ia64;

namespace {
unsigned passed = 0;
unsigned failed = 0;

#define CHECK(expr) do { \
    if (expr) ++passed; \
    else { ++failed; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

struct Fixture {
    Ns16550Mapping mapping = Ns16550Mapping::MaltaOriented();
    PhysicalAddressMap bus;
    std::shared_ptr<GuestConsoleSink> sink = std::make_shared<GuestConsoleSink>(128);
    std::shared_ptr<Ns16550Uart> uart = std::make_shared<Ns16550Uart>(sink, 3);

    Fixture() { bus.RegisterMmio(mapping.physicalBase, mapping.RegionSize(), uart, {1}); }
    void write(unsigned reg, uint8_t value) {
        bus.Write(mapping.physicalBase + reg * mapping.RegisterSpacing(), &value, 1);
    }
    uint8_t read(unsigned reg) const {
        uint8_t value = 0xFF;
        bus.Read(mapping.physicalBase + reg * mapping.RegisterSpacing(), &value, 1);
        return value;
    }
    void configure(uint8_t triggerCode = 2, bool enableIer = true,
                   uint16_t divisor = 1, uint8_t lcr = 0x03) {
        write(3, static_cast<uint8_t>(lcr | 0x80));
        write(0, static_cast<uint8_t>(divisor));
        write(1, static_cast<uint8_t>(divisor >> 8));
        write(3, lcr);
        write(2, static_cast<uint8_t>(0x01 | (triggerCode << 6)));
        write(1, enableIer ? 0x01 : 0x00);
    }
    void input(const std::vector<uint8_t>& bytes) {
        CHECK(uart->EnqueueHostInput(bytes));
        CHECK(uart->AdvanceHostInput() == bytes.size());
    }
};

void testCharacterTimingArithmetic() {
    // Malta QEMU serial_mm_init uses baudbase=230400, i.e. 3.6864 MHz / 16.
    CHECK(Ns16550Uart::kMaltaUartReferenceClockHz == 3'686'400);
    CHECK(Ns16550Uart::kVirtualTimeUnitsPerSecond == 1'000'000);
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x03, 1) == 174); // 8N1
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x03, 2) == 348);
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x03, 3) == 521);
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x03, 256) == 44'445);
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x0B, 1) == 191); // 8 data + parity
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x0F, 1) == 209); // 8E2
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x04, 1) == 131); // 5N1.5
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x00, 1) == 122); // 5N1
    CHECK(Ns16550Uart::CharacterTimeoutVirtualUnits(0x03, 0) == 11'429); // 3500-baud fallback
}

void testExactBoundaryAndRbrLifecycle() {
    Fixture f;
    f.configure(); // Trigger 8, divisor 1, 8N1.
    f.input({'g', 'u', 'i', 'd', 'e', 'X', 'O'});
    const uint64_t deadline = f.uart->ReceiveTimeoutDeadline();
    CHECK(deadline == 174);
    CHECK(f.uart->ReceiveTimeoutArmed() && !f.uart->ReceiveTimeoutPending());
    CHECK(f.read(2) == 0xC1 && !*f.uart->InterruptSignal());

    f.uart->AdvanceVirtualTime(deadline - 1);
    CHECK(f.read(2) == 0xC1 && !f.uart->ReceiveTimeoutPending());
    f.uart->AdvanceVirtualTime(deadline);
    CHECK(f.uart->ReceiveTimeoutPending());
    CHECK(f.read(2) == 0xCC); // FIFO-identification bits plus ID 0x0C.
    CHECK(*f.uart->InterruptSignal());
    CHECK(f.read(2) == 0xCC); // IIR does not acknowledge timeout.
    f.uart->AdvanceVirtualTime(deadline + 1);
    CHECK(f.read(2) == 0xCC); // A long run cannot create a second timeout source.

    CHECK(f.read(0) == 'g'); // RBR acknowledges and restarts with unread bytes.
    CHECK(!f.uart->ReceiveTimeoutPending() && f.uart->ReceiveTimeoutArmed());
    CHECK(f.uart->ReceiveTimeoutDeadline() == deadline + 1 + 174);
    CHECK(f.read(2) == 0xC1 && !*f.uart->InterruptSignal());
    f.uart->AdvanceVirtualTime(f.uart->ReceiveTimeoutDeadline() - 1);
    CHECK(f.read(2) == 0xC1);
    f.uart->AdvanceVirtualTime(f.uart->ReceiveTimeoutDeadline());
    CHECK(f.read(2) == 0xCC);
    while (f.uart->ReceiveFifoSize() != 0) (void)f.read(0);
    CHECK(!f.uart->ReceiveTimeoutArmed() && !f.uart->ReceiveTimeoutPending());
    CHECK(f.uart->ReceiveTimeoutDeadline() == 0 && f.read(2) == 0xC1);
}

void testRearmAndConfigurationSemantics() {
    Fixture f;
    f.configure();
    f.input({1, 2, 3});
    f.uart->AdvanceVirtualTime(80);
    CHECK(f.read(0) == 1);
    CHECK(f.uart->ReceiveTimeoutDeadline() == 80 + 174);
    f.uart->AdvanceVirtualTime(100);
    f.input({4});
    CHECK(f.uart->ReceiveTimeoutDeadline() == 100 + 174);

    // Divisor/LCR writes do not move an already armed deadline. The next RX
    // or RBR activity uses the newly programmed format.
    const uint64_t existingDeadline = f.uart->ReceiveTimeoutDeadline();
    f.write(3, 0x83);
    f.write(0, 2);
    f.write(3, 0x03);
    CHECK(f.uart->ReceiveTimeoutDeadline() == existingDeadline);
    f.uart->AdvanceVirtualTime(110);
    f.write(3, 0x0F); // 8 data, parity, two stop bits.
    CHECK(f.uart->ReceiveTimeoutDeadline() == existingDeadline);
    CHECK(f.read(0) == 2);
    CHECK(f.uart->ReceiveTimeoutDeadline() == 110 + 417);

    f.uart->AdvanceVirtualTime(f.uart->ReceiveTimeoutDeadline());
    CHECK(f.uart->ReceiveTimeoutPending());
    f.write(2, 0x87); // Receiver FIFO clear.
    CHECK(f.uart->ReceiveFifoSize() == 0);
    CHECK(!f.uart->ReceiveTimeoutArmed() && !f.uart->ReceiveTimeoutPending());
    CHECK(f.uart->ReceiveTimeoutDeadline() == 0 && !*f.uart->InterruptSignal());

    f.input({9});
    CHECK(f.uart->ReceiveTimeoutArmed());
    f.write(2, 0x00); // Disable FIFO: clear FIFO-specific timeout state.
    CHECK(f.uart->ReceiveFifoSize() == 0);
    CHECK(!f.uart->ReceiveTimeoutArmed() && !f.uart->ReceiveTimeoutPending());
    CHECK(f.read(2) == 0x01);
}

void testIerAndAllTriggerLevels() {
    constexpr uint8_t triggerLevels[] = {1, 4, 8, 14};
    for (uint8_t code = 0; code < 4; ++code) {
        Fixture f;
        f.configure(code, true);
        const uint8_t trigger = triggerLevels[code];
        CHECK(f.read(5) == 0x60); // Empty FIFO never times out.
        CHECK(f.read(2) == 0xC1);

        if (trigger > 1) {
            f.input(std::vector<uint8_t>(trigger - 1, 0x41));
            CHECK(f.read(2) == 0xC1);
            f.uart->AdvanceVirtualTime(f.uart->ReceiveTimeoutDeadline());
            CHECK(f.read(2) == 0xCC);
        }

        f.write(2, static_cast<uint8_t>(0x03 | (code << 6))); // clear, retain trigger.
        f.input(std::vector<uint8_t>(trigger, 0x42));
        CHECK(f.read(2) == 0xC4);
        CHECK(*f.uart->InterruptSignal());
        if (trigger < Ns16550Uart::kReceiveFifoCapacity) {
            f.input({0x43});
            CHECK(f.read(2) == 0xC4); // Above trigger remains ordinary RDA.
        }
        (void)f.read(0);
        if (trigger > 1) {
            CHECK(f.read(2) == 0xC4); // Exactly at the threshold still wins.
            (void)f.read(0);
            CHECK(f.read(2) == 0xC1); // Crossing below trigger drops RDA.
            f.uart->AdvanceVirtualTime(f.uart->ReceiveTimeoutDeadline());
            CHECK(f.read(2) == 0xCC); // Later reports the timeout source.
        }
    }

    Fixture masked;
    masked.configure(2, false);
    masked.input({1, 2, 3});
    masked.uart->AdvanceVirtualTime(masked.uart->ReceiveTimeoutDeadline());
    CHECK(masked.uart->ReceiveTimeoutPending());
    CHECK(masked.read(2) == 0xC1 && !*masked.uart->InterruptSignal());
    masked.write(1, 1);
    CHECK(masked.read(2) == 0xCC && *masked.uart->InterruptSignal());
    (void)masked.read(0);
    CHECK(masked.read(2) == 0xC1); // RBR clears pending; unread bytes rearm.

    Fixture compatibility16450;
    compatibility16450.write(3, 0x03);
    compatibility16450.write(1, 1);
    compatibility16450.input({0x5A});
    CHECK(compatibility16450.read(2) == 0x04); // No FIFO identification bits.
    CHECK(compatibility16450.uart->ReceiveTimeoutDeadline() == 0);
    compatibility16450.uart->AdvanceVirtualTime(1'000'000);
    CHECK(compatibility16450.read(2) == 0x04); // Non-FIFO mode never times out.
}

void testTimeoutSnapshotState() {
    Fixture source;
    source.configure();
    source.input({0, 0xFF, 3});
    source.uart->AdvanceVirtualTime(90);
    const Ns16550Uart::Snapshot armed = source.uart->CaptureSnapshot();
    CHECK(armed.version == 4 && Ns16550Uart::ValidateSnapshot(armed));
    CHECK(armed.receiveTimeoutArmed && !armed.receiveTimeoutPending);

    Fixture destination;
    Ns16550Uart::PreparedRestore prepared;
    CHECK(destination.uart->PrepareRestore(armed, prepared));
    destination.uart->CommitRestore(armed, std::move(prepared));
    CHECK(destination.uart->VirtualTimeCycle() == 90);
    CHECK(destination.uart->ReceiveTimeoutDeadline() == armed.receiveTimeoutDeadlineCycle);
    destination.uart->AdvanceVirtualTime(armed.receiveTimeoutDeadlineCycle);
    CHECK(destination.uart->ReceiveTimeoutPending());
    CHECK(destination.read(2) == 0xCC);
    CHECK(destination.read(0) == 0 && destination.read(0) == 0xFF);
    CHECK(destination.read(0) == 3 && destination.read(2) == 0xC1);

    Ns16550Uart::Snapshot malformed = armed;
    malformed.receiveTimeoutDeadlineCycle = malformed.virtualTimeCycle;
    CHECK(!Ns16550Uart::ValidateSnapshot(malformed));

    Fixture resetFixture;
    resetFixture.configure();
    resetFixture.input({1, 2, 3});
    resetFixture.uart->AdvanceVirtualTime(resetFixture.uart->ReceiveTimeoutDeadline());
    CHECK(resetFixture.uart->ReceiveTimeoutPending());
    resetFixture.uart->Reset();
    CHECK(resetFixture.uart->VirtualTimeCycle() == 0);
    CHECK(resetFixture.uart->ReceiveFifoSize() == 0);
    CHECK(!resetFixture.uart->ReceiveTimeoutArmed());
    CHECK(!resetFixture.uart->ReceiveTimeoutPending());
    CHECK(resetFixture.uart->ReceiveTimeoutDeadline() == 0);
}
}  // namespace

int main() {
    testCharacterTimingArithmetic();
    testExactBoundaryAndRbrLifecycle();
    testRearmAndConfigurationSemantics();
    testIerAndAllTriggerLevels();
    testTimeoutSnapshotState();
    std::cout << "test_mips64_uart_timeout: " << passed << " passed, "
              << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
