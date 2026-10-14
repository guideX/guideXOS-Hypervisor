// MIPS64-1 focused test: ISA plugin contract — registration, independent
// instances, real decode/execute stepping, PC/state management, clone and
// serialization round trips, and capability reporting.
//
// NOTE ON LINKING: this test links src/isa/ISAPluginRegistry.cpp to prove
// real "MIPS64" registration. That translation unit references the IA-64
// factory symbol, so this test-only TU provides throwing stubs for the two
// createIA64ISA overloads. They are never called (only "MIPS64" is created);
// they exist solely to keep this focused test independent of the full IA-64
// object chain. Production code is unaffected.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "IDecoder.h"
#include "IISA.h"
#include "IMemory.h"
#include "ISAPluginRegistry.h"
#include "Mips64ISAPlugin.h"
#include "Profiler.h"
#include "SyscallDispatcher.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

namespace ia64 {

// Test-link shims; see note above. Never invoked by this test.
std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder) {
    (void)decoder;
    throw std::runtime_error("IA-64 factory stub must not be called in MIPS64 tests");
}

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder,
                                    SyscallDispatcher* syscallDispatcher,
                                    Profiler* profiler) {
    (void)decoder;
    (void)syscallDispatcher;
    (void)profiler;
    throw std::runtime_error("IA-64 factory stub must not be called in MIPS64 tests");
}

}  // namespace ia64

using namespace mips64;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            ++g_pass;                                                                \
        } else {                                                                     \
            ++g_fail;                                                                \
            std::cout << "FAIL " << __func__ << " line " << __LINE__ << ": " #cond    \
                      << "\n";                                                       \
        }                                                                            \
    } while (0)

static uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | funct;
}

static uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | imm;
}

static void storeWordLE(ia64::IMemory& mem, uint64_t addr, uint32_t word) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>((word >> (8 * i)) & 0xFFU);
    }
    mem.Write(addr, bytes, 4);
}

// -- registration -----------------------------------------------------------------
static void testRegistration() {
    CHECK(ia64::ISAPluginRegistry::instance().isRegistered("MIPS64"));
    // Unknown ISAs still fail cleanly.
    CHECK(ia64::ISAPluginRegistry::instance().createISA("NO-SUCH-ISA") == nullptr);
}

// -- factory creation incl. endianness -------------------------------------------
static void testFactoryCreation() {
    Mips64FactoryData leData;
    leData.littleEndian = true;
    auto le = ia64::ISAPluginRegistry::instance().createISA("MIPS64", &leData);
    CHECK(le != nullptr);
    CHECK(le->getName() == "MIPS64");
    CHECK(le->isLittleEndian());

    Mips64FactoryData beData;
    beData.littleEndian = false;
    auto be = ia64::ISAPluginRegistry::instance().createISA("MIPS64", &beData);
    CHECK(be != nullptr);
    CHECK(!be->isLittleEndian());

    auto def = ia64::ISAPluginRegistry::instance().createISA("MIPS64", nullptr);
    CHECK(def != nullptr && def->isLittleEndian());

    auto direct = createMips64ISA(false);
    CHECK(direct != nullptr && !direct->isLittleEndian());
    CHECK(direct->getWordSize() == 64);
}

// -- real stepping through the plugin --------------------------------------------
static void testRealStepping() {
    ia64::Memory mem(64 * 1024);
    auto isa = createMips64ISA(true);
    CHECK(isa->getPC() == 0);
    // addiu $8,$0,10 ; addu $10,$8,$8 ; sw $10,0x200($0) ; lw $11,0x200($0)
    storeWordLE(mem, 0, encodeI(9, 0, 8, 10));
    storeWordLE(mem, 4, encodeR(8, 8, 10, 0, 33));
    storeWordLE(mem, 8, encodeI(43, 0, 10, 0x200));
    storeWordLE(mem, 12, encodeI(35, 0, 11, 0x200));

    for (int i = 0; i < 4; ++i) {
        CHECK(isa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    }
    CHECK(isa->getPC() == 16);
    auto* plugin = dynamic_cast<Mips64ISAPlugin*>(isa.get());
    CHECK(plugin != nullptr);
    CHECK(plugin->core().readGpr(10) == 20);
    CHECK(plugin->core().readGpr(11) == 20);

    // Delay-slot branch stepping through the plugin interface.
    isa->reset();
    CHECK(isa->getPC() == 0);
    // beq $8,$8,+2 (taken, skips one) ; addiu $12,$0,7 (delay) ; addiu $13,$0,9 (skipped) ; addiu $14,$0,3
    storeWordLE(mem, 0, encodeI(4, 8, 8, 2));
    storeWordLE(mem, 4, encodeI(9, 0, 12, 7));
    storeWordLE(mem, 8, encodeI(9, 0, 13, 9));
    storeWordLE(mem, 12, encodeI(9, 0, 14, 3));
    CHECK(isa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(isa->getPC() == 4);
    CHECK(isa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(isa->getPC() == 12);
    CHECK(isa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(plugin->core().readGpr(12) == 7);
    CHECK(plugin->core().readGpr(13) == 0);
    CHECK(plugin->core().readGpr(14) == 3);

    // Unsupported encodings surface as EXCEPTION, never silent NOPs.
    storeWordLE(mem, 16, encodeR(8, 9, 10, 0, 0x18));  // mult
    isa->setPC(16);
    CHECK(isa->step(mem) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK(isa->getPC() == 16);
}

// -- decode/execute split ----------------------------------------------------------
static void testDecodeExecuteSplit() {
    ia64::Memory mem(64 * 1024);
    auto isa = createMips64ISA(true);
    storeWordLE(mem, 0, encodeI(9, 0, 8, 42));
    ia64::ISADecodeResult decoded = isa->decode(mem);
    CHECK(decoded.valid);
    CHECK(decoded.instructionAddress == 0);
    CHECK(decoded.instructionLength == 4);
    CHECK(decoded.disassembly.find("addiu") != std::string::npos);
    CHECK(isa->getPC() == 0);  // decode does not advance PC
    CHECK(isa->execute(mem, decoded) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(isa->getPC() == 4);
    auto* plugin = dynamic_cast<Mips64ISAPlugin*>(isa.get());
    CHECK(plugin->core().readGpr(8) == 42);

    // Stale decode results are rejected.
    CHECK(isa->execute(mem, decoded) == ia64::ISAExecutionResult::EXCEPTION);
    ia64::ISADecodeResult invalid;
    CHECK(isa->execute(mem, invalid) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK(isa->getInstructionLength(mem, 0x100) == 4);
}

// -- PC + state inspection ------------------------------------------------------------
static void testPcAndState() {
    auto isa = createMips64ISA(true);
    isa->setPC(0x40);
    CHECK(isa->getPC() == 0x40);
    isa->reset();
    CHECK(isa->getPC() == 0);

    ia64::ISAState& state = isa->getState();
    auto* mipsState = dynamic_cast<Mips64ISAState*>(&state);
    CHECK(mipsState != nullptr);
    CHECK(mipsState->getStateSize() == Mips64Cpu::kSerializedSize);
    mipsState->cpu().writeGpr(8, 1234);
    CHECK(mipsState->toString().find("MIPS64") != std::string::npos);
    CHECK(isa->dumpState().find("MIPS64") != std::string::npos);

    // setState round trip + type safety.
    auto clone = state.clone();
    auto isa2 = createMips64ISA(true);
    isa2->setState(*clone);
    auto* plugin2 = dynamic_cast<Mips64ISAPlugin*>(isa2.get());
    CHECK(plugin2->core().readGpr(8) == 1234);

    bool threw = false;
    try {
        // Deliberately wrong state type must be rejected, not bit-cast.
        class OtherState : public ia64::ISAState {
        public:
            std::unique_ptr<ia64::ISAState> clone() const override {
                return std::make_unique<OtherState>();
            }
            void serialize(uint8_t*) const override {}
            void deserialize(const uint8_t*) override {}
            size_t getStateSize() const override { return 0; }
            std::string toString() const override { return {}; }
            void reset() override {}
        };
        OtherState other;
        isa2->setState(other);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// -- serialization incl. delay slot + endianness ------------------------------------------
static void testSerialization() {
    ia64::Memory mem(64 * 1024);
    auto isa = createMips64ISA(false);  // big-endian instance
    // beq $0,$0,+1 taken at 0 ; delay slot at 4
    uint32_t beq = encodeI(4, 0, 0, 1);
    uint8_t beqBytes[4] = {static_cast<uint8_t>(beq >> 24), static_cast<uint8_t>(beq >> 16),
                           static_cast<uint8_t>(beq >> 8), static_cast<uint8_t>(beq)};
    mem.Write(0, beqBytes, 4);
    CHECK(isa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    auto* plugin = dynamic_cast<Mips64ISAPlugin*>(isa.get());
    CHECK(plugin->core().delayArmed());

    const std::vector<uint8_t> blob = isa->serialize_state();
    CHECK(blob.size() == Mips64Cpu::kSerializedSize);

    // Restore into a little-endian instance: snapshot preserves config.
    auto restored = createMips64ISA(true);
    CHECK(restored->isLittleEndian());
    CHECK(restored->deserialize_state(blob));
    CHECK(!restored->isLittleEndian());
    auto* restoredPlugin = dynamic_cast<Mips64ISAPlugin*>(restored.get());
    CHECK(restoredPlugin->core().delayArmed());
    CHECK(restoredPlugin->core().delayTarget() == plugin->core().delayTarget());

    // Truncated blobs are rejected.
    CHECK(!restored->deserialize_state(std::vector<uint8_t>(blob.begin(), blob.end() - 1)));
    CHECK(!restored->deserialize_state(std::vector<uint8_t>()));
}

// -- capabilities + disassembly + instance independence ----------------------------------------
static void testCapabilitiesAndIndependence() {
    auto le = createMips64ISA(true);
    auto be = createMips64ISA(false);
    CHECK(le->getVersion().find("MIPS64") != std::string::npos);
    for (const char* feature :
         {"mips64r2", "delay_slot", "fixed_width_32", "load_store", "sign_extended_words"}) {
        CHECK(le->hasFeature(feature));
        CHECK(be->hasFeature(feature));
    }
    CHECK(le->hasFeature("little_endian") && !le->hasFeature("big_endian"));
    CHECK(be->hasFeature("big_endian") && !be->hasFeature("little_endian"));
    CHECK(!le->hasFeature("jit"));

    ia64::Memory mem(64 * 1024);
    storeWordLE(mem, 0x80, encodeI(9, 0, 8, 5));
    CHECK(le->disassemble(mem, 0x80).find("addiu") != std::string::npos);
    CHECK(le->disassemble(mem, 0x81) == "<misaligned>");
    CHECK(!le->disassemble(mem, 0xFFFFFF000ULL).empty());

    // Independent CPU instances via the plugin.
    auto a = createMips64ISA(true);
    auto b = createMips64ISA(true);
    storeWordLE(mem, 0, encodeI(9, 0, 8, 11));
    CHECK(a->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(dynamic_cast<Mips64ISAPlugin*>(a.get())->core().readGpr(8) == 11);
    CHECK(dynamic_cast<Mips64ISAPlugin*>(b.get())->core().readGpr(8) == 0);
    CHECK(b->getPC() == 0);
}

int main() {
    testRegistration();
    testFactoryCreation();
    testRealStepping();
    testDecodeExecuteSplit();
    testPcAndState();
    testSerialization();
    testCapabilitiesAndIndependence();

    std::cout << "test_mips64_plugin: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
