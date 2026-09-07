#include "IA64ISAPlugin.h"
#include "ProcessorInterruptBlock.h"
#include "decoder.h"
#include "memory.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace ia64;

    InstructionDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    Memory memory(0x20000000ULL);
    size_t callbackCount = 0;
    uint8_t callbackId = 0;
    uint8_t callbackEid = 0;
    uint8_t callbackVector = 0;
    uint8_t callbackMode = 0;
    bool callbackRedirect = true;
    ProcessorInterruptBlock pib(
        [&](uint8_t id, uint8_t eid, uint8_t vector, uint8_t mode, bool redirect) {
            ++callbackCount;
            callbackId = id;
            callbackEid = eid;
            callbackVector = vector;
            callbackMode = mode;
            callbackRedirect = redirect;
            plugin.queueInterrupt(vector);
            return true;
        });
    memory.RegisterDevice(&pib);

    const uint64_t base = ProcessorInterruptBlock::kDefaultBaseAddress;
    const uint64_t targetAddress = base | (0x12ULL << 12) | (0x34ULL << 4);
    const auto decodedAddress = ProcessorInterruptBlock::DecodeAddress(targetAddress);
    require(decodedAddress.id == 0x12 && decodedAddress.eid == 0x34,
            "PIB address decodes ID and EID independently");
    require(!decodedAddress.redirect && decodedAddress.aligned,
            "direct PIB address has redirect clear and 8-byte alignment");

    const auto decodedRedirect = ProcessorInterruptBlock::DecodeAddress(targetAddress | 0x8ULL);
    require(decodedRedirect.redirect && decodedRedirect.id == 0x12 && decodedRedirect.eid == 0x34,
            "redirect is address bit 3, not part of ID or EID");

    const auto decodedMessage = ProcessorInterruptBlock::DecodeMessage(0x2EFULL);
    require(decodedMessage.vector == 0xEF && decodedMessage.deliveryMode == 2,
            "PIB message decodes vector and delivery mode fields");
    require(ProcessorInterruptBlock::IsIntVectorLegal(0) &&
                ProcessorInterruptBlock::IsIntVectorLegal(2) &&
                ProcessorInterruptBlock::IsIntVectorLegal(0xEF) &&
                !ProcessorInterruptBlock::IsIntVectorLegal(1) &&
                !ProcessorInterruptBlock::IsIntVectorLegal(15),
            "INT vector legality follows the architectural reserved-vector rules");

    auto writeMessage = [&](uint64_t address, uint64_t value) {
        memory.Write(address, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    };

    plugin.reset();
    plugin.setInterruptsEnabled(true);
    writeMessage(base, 0xEFULL);
    require(callbackCount == 1 && callbackId == 0 && callbackEid == 0 &&
                callbackVector == 0xEF && callbackMode == 0 && !callbackRedirect,
            "direct INT to CPU 0 reaches the delivery callback");
    require(plugin.hasPendingInterrupt() &&
                plugin.readControlRegister(IA64_CR_IRR3) == (1ULL << 47),
            "direct INT is pending in CR.IRR3 bit 47");

    plugin.reset();
    plugin.setInterruptsEnabled(false);
    const size_t beforeDisabled = callbackCount;
    writeMessage(base, 0xEFULL);
    require(callbackCount == beforeDisabled + 1 && plugin.hasPendingInterrupt() &&
                !plugin.hasInServiceInterrupt(),
            "PSR.i disabled leaves an accepted IPI pending without entry");
    require(!plugin.tryDeliverPendingInterrupt(), "disabled PSR.i blocks IPI entry");
    plugin.setInterruptsEnabled(true);
    require(plugin.tryDeliverPendingInterrupt(),
            "enabling PSR.i permits pending IPI delivery at an instruction boundary");
    require(plugin.hasInServiceInterrupt() && plugin.getInServiceVector() == 0xEF,
            "IPI delivery marks the vector in service");
    require(plugin.readControlRegister(IA64_CR_IRR3) == (1ULL << 47),
            "interrupt entry does not consume the pending IRR bit");
    require(plugin.readControlRegister(IA64_CR_IVR) == 0xEF &&
                plugin.readControlRegister(IA64_CR_IRR3) == 0,
            "IVR consumes the pending IPI request");
    plugin.writeControlRegister(IA64_CR_EOI, 0);
    require(!plugin.hasInServiceInterrupt() &&
                plugin.readControlRegister(IA64_CR_IVR) == IA64_SPURIOUS_INT_VECTOR,
            "EOI retires the delivered IPI and empty IVR is spurious");

    plugin.reset();
    plugin.setInterruptsEnabled(true);
    plugin.writeControlRegister(IA64_CR_TPR, 0x40);
    writeMessage(base, 0x40ULL);
    require(plugin.hasPendingInterrupt() && !plugin.tryDeliverPendingInterrupt(),
            "TPR masks an IPI while retaining it pending");
    plugin.writeControlRegister(IA64_CR_TPR, 0);
    require(plugin.tryDeliverPendingInterrupt(), "lowering TPR makes the IPI eligible");
    plugin.writeControlRegister(IA64_CR_EOI, 0);

    plugin.reset();
    plugin.setInterruptsEnabled(true);
    writeMessage(base, 0x40ULL);
    writeMessage(base, 0x90ULL);
    require(plugin.tryDeliverPendingInterrupt(), "highest-priority pending IPI is delivered");
    require(plugin.readControlRegister(IA64_CR_IVR) == 0x90,
            "priority selection chooses the highest pending vector");
    plugin.writeControlRegister(IA64_CR_EOI, 0);
    plugin.setInterruptsEnabled(true);
    require(plugin.tryDeliverPendingInterrupt(), "lower-priority pending IPI remains queued");
    require(plugin.readControlRegister(IA64_CR_IVR) == 0x40,
            "lower-priority vector follows after EOI");
    plugin.writeControlRegister(IA64_CR_EOI, 0);

    const size_t beforeInvalid = callbackCount;
    writeMessage(base, 1ULL);
    require(callbackCount == beforeInvalid && pib.getStatistics().ignoredInvalidVector == 1,
            "reserved INT vectors are ignored");
    writeMessage(base, 2ULL << 8);
    require(callbackCount == beforeInvalid && pib.getStatistics().ignoredUnsupportedMode == 1,
            "unsupported delivery modes are not faked as INT");
    writeMessage(base | 0x8ULL, 0xEFULL);
    require(callbackCount == beforeInvalid && pib.getStatistics().ignoredRedirect == 1,
            "redirected messages do not enter the direct CPU path");

    // Execute the actual store normalization path used by Linux's st8.rel.
    plugin.reset();
    plugin.getCPUState().SetGR(14, 0xC0000000FEE00000ULL);
    plugin.getCPUState().SetGR(34, 0xEFULL);
    InstructionEx store(InstructionType::ST8, UnitType::M_UNIT);
    store.SetOperands(14, 34);
    const size_t beforeAlias = callbackCount;
    store.Execute(plugin.getCPUState(), memory);
    require(callbackCount == beforeAlias + 1 && plugin.hasPendingInterrupt(),
            "region-6 uncached alias reaches the PIB through ST8 normalization");

    bool nearbyThrew = false;
    try {
        writeMessage(base + ProcessorInterruptBlock::kIpiRegionSize, 0xEFULL);
    } catch (const std::out_of_range&) {
        nearbyThrew = true;
    }
    require(nearbyThrew && callbackCount == beforeAlias + 1,
            "nearby non-IPI physical address does not route to the PIB");

    plugin.reset();
    plugin.queueInterrupt(0x60);
    const std::vector<uint8_t> snapshot = plugin.serialize_state();
    IA64ISAPlugin restored(decoder);
    require(restored.deserialize_state(snapshot) && restored.hasPendingInterrupt(),
            "pending IPI state survives ISA snapshot serialization");

    std::cout << "IA-64 Processor Interrupt Block tests passed" << std::endl;
    return 0;
}
