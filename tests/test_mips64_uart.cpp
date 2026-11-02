#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "Ns16550Uart.h"

using namespace ia64;

static unsigned g_pass = 0;
static unsigned g_fail = 0;
#define CHECK(expr) do { \
    if (expr) ++g_pass; \
    else { ++g_fail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

template <typename Fn>
static PhysicalBusErrorCode expectBusError(Fn&& operation) {
    try { operation(); }
    catch (const PhysicalBusError& error) { return error.code(); }
    return PhysicalBusErrorCode::DeviceFailure;
}

static void writeReg(PhysicalAddressMap& map, const Ns16550Mapping& config,
                     unsigned reg, uint8_t value) {
    map.Write(config.physicalBase + reg * config.RegisterSpacing(), &value, 1);
}

static uint8_t readReg(const PhysicalAddressMap& map, const Ns16550Mapping& config,
                       unsigned reg) {
    uint8_t value = 0xFF;
    map.Read(config.physicalBase + reg * config.RegisterSpacing(), &value, 1);
    return value;
}

static void testMaltaOrientedMappingAndRegisterSubset() {
    const Ns16550Mapping config = Ns16550Mapping::MaltaOriented();
    CHECK(config.physicalBase == 0x1F000900ULL);
    CHECK(config.registerShift == 3 && config.RegisterSpacing() == 8);
    CHECK(config.RegionSize() == 64);
    Ns16550Mapping invalidSpacing;
    invalidSpacing.registerShift = 9;
    bool invalidSpacingRejected = false;
    try { (void)invalidSpacing.RegionSize(); }
    catch (const std::invalid_argument&) { invalidSpacingRejected = true; }
    CHECK(invalidSpacingRejected);

    PhysicalAddressMap map;
    auto sink = std::make_shared<GuestConsoleSink>(32);
    auto uart = std::make_shared<Ns16550Uart>(sink, config.registerShift);
    map.RegisterMmio(config.physicalBase, config.RegionSize(), uart, {1});

    CHECK(readReg(map, config, 0) == 0);  // Empty RBR; no receive input exists.
    CHECK(readReg(map, config, 5) == 0x60);  // THRE | TEMT are always ready.
    CHECK(readReg(map, config, 2) == 0x01);  // No interrupt pending.
    CHECK(uart->InterruptSignal().has_value() && !*uart->InterruptSignal());

    writeReg(map, config, 3, 0x80);  // DLAB
    writeReg(map, config, 0, 0x34);  // DLL
    writeReg(map, config, 1, 0x12);  // DLM
    CHECK(readReg(map, config, 0) == 0x34 && readReg(map, config, 1) == 0x12);
    writeReg(map, config, 0, 'X');
    CHECK(sink->Bytes().empty());  // DLAB prevents THR transmit.
    CHECK(readReg(map, config, 0) == 'X');
    writeReg(map, config, 0, 0x34);

    writeReg(map, config, 3, 0x03);
    CHECK(readReg(map, config, 3) == 0x03);
    CHECK(readReg(map, config, 1) == 0);  // IER is independently selected.
    writeReg(map, config, 1, 0x0F);
    CHECK(readReg(map, config, 1) == 0x0F);  // Retained, but IRQ delivery is absent.
    CHECK(readReg(map, config, 2) == 0x01);
    writeReg(map, config, 2, 0x07);  // FCR write accepted; no FIFO behavior claimed.
    CHECK(readReg(map, config, 2) == 0x01);  // IIR is read-only/no interrupt.
    writeReg(map, config, 4, 0x03);
    CHECK(readReg(map, config, 4) == 0x03);  // MCR storage only.
    CHECK(readReg(map, config, 6) == 0);     // No modem inputs.
    writeReg(map, config, 7, 0xA5);
    CHECK(readReg(map, config, 7) == 0xA5);

    writeReg(map, config, 0, 'H');
    writeReg(map, config, 0, 'i');
    CHECK(sink->Bytes() == std::vector<uint8_t>({'H', 'i'}));
    const uint8_t multi[2] = {'!', '?'};
    CHECK(expectBusError([&] {
        map.Write(config.physicalBase, multi, 2);
    }) == PhysicalBusErrorCode::InvalidWidth);
    CHECK(expectBusError([&] {
        map.Write(config.physicalBase + config.RegionSize() - 1, multi, 2);
    }) == PhysicalBusErrorCode::BoundaryCrossing);
    CHECK(expectBusError([&] {
        map.Write(config.physicalBase + 1, multi, 1);
    }) == PhysicalBusErrorCode::Unmapped);
    CHECK(expectBusError([&] {
        writeReg(map, config, 5, 0xFF);
    }) == PhysicalBusErrorCode::PermissionDenied);
    CHECK(sink->Bytes() == std::vector<uint8_t>({'H', 'i'}));

    const auto inspection = uart->InspectState();
    CHECK(inspection.at("DLL") == 0x34 && inspection.at("DLM") == 0x12);
    CHECK(inspection.at("LCR") == 0x03 && inspection.at("SCR") == 0xA5);
    CHECK(inspection.at("IER") == 0x0F && inspection.at("FCR_last_write") == 0x07);

    map.ResetDevices();
    CHECK(sink->Bytes().empty());
    CHECK(readReg(map, config, 3) == 0 && readReg(map, config, 7) == 0);
    CHECK(readReg(map, config, 5) == 0x60);
}

static void testBinaryOutputAndCapacityAtomicity() {
    PhysicalAddressMap map;
    auto sink = std::make_shared<GuestConsoleSink>(3);
    auto uart = std::make_shared<Ns16550Uart>(sink);
    map.RegisterMmio(0x4000, uart->RegionSize(), uart, {1});
    const uint8_t bytes[] = {0x00, 0x80, 0xFF};
    for (unsigned i = 0; i < 3; ++i) map.Write(0x4000, &bytes[i], 1);
    CHECK(sink->Bytes() == std::vector<uint8_t>({0x00, 0x80, 0xFF}));
    const uint8_t extra = 0x41;
    CHECK(expectBusError([&] { map.Write(0x4000, &extra, 1); }) ==
          PhysicalBusErrorCode::DeviceFailure);
    CHECK(sink->Bytes() == std::vector<uint8_t>({0x00, 0x80, 0xFF}));
    CHECK(uart->InspectState().at("console_bytes") == 3);
    map.ResetDevices();
    CHECK(sink->Bytes().empty());
}

int main() {
    testMaltaOrientedMappingAndRegisterSubset();
    testBinaryOutputAndCapacityAtomicity();
    std::cout << "test_mips64_uart: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
