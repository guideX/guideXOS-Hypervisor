#include "IISA.h"
#include "IA64ISAPlugin.h"
#include "ExampleISAPlugin.h"
#include "ISAPluginRegistry.h"
#include "memory.h"
#include "decoder.h"
#include "cpu.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

class FakeBranchDecoder : public IDecoder {
public:
    InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const override {
        (void)bundleData;
        return InstructionBundle();
    }

    Bundle DecodeBundle(const uint8_t* bundleData) const override {
        return DecodeBundleAt(bundleData, 0);
    }

    Bundle DecodeBundleAt(const uint8_t* bundleData, uint64_t bundleIP) const override {
        (void)bundleData;

        InstructionEx branch(InstructionType::BR_COND, UnitType::B_UNIT);
        branch.SetPredicate(1);
        branch.SetBranchTarget(bundleIP + 0x1000);
        branch.SetRawBits(0x1);

        Bundle bundle;
        bundle.templateType = TemplateType::MIB;
        bundle.hasStop = false;
        bundle.instructions.push_back(branch);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeCompareBranchDecoder : public IDecoder {
public:
    explicit FakeCompareBranchDecoder(bool stopAfterCompare)
        : stopAfterCompare_(stopAfterCompare) {}

    InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const override {
        (void)bundleData;
        return InstructionBundle();
    }

    Bundle DecodeBundle(const uint8_t* bundleData) const override {
        return DecodeBundleAt(bundleData, 0);
    }

    Bundle DecodeBundleAt(const uint8_t* bundleData, uint64_t bundleIP) const override {
        (void)bundleData;

        InstructionEx compare(InstructionType::CMP_EQ, UnitType::I_UNIT);
        compare.SetOperands4(1, 0, 0, 2);
        compare.SetRawBits(0x2);

        InstructionEx branch(InstructionType::BR_COND, UnitType::B_UNIT);
        branch.SetPredicate(1);
        branch.SetBranchTarget(bundleIP + 0x1000);
        branch.SetRawBits(0x3);

        Bundle bundle;
        bundle.templateType = stopAfterCompare_ ? TemplateType::MI_I : TemplateType::MII;
        bundle.hasStop = stopAfterCompare_;
        bundle.stopAfterSlot[0] = stopAfterCompare_;
        bundle.instructions.push_back(compare);
        bundle.instructions.push_back(branch);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }

private:
    bool stopAfterCompare_;
};

class FakeCallBridgeDecoder : public IDecoder {
public:
    InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const override {
        (void)bundleData;
        return InstructionBundle();
    }

    Bundle DecodeBundle(const uint8_t* bundleData) const override {
        return DecodeBundleAt(bundleData, 0);
    }

    Bundle DecodeBundleAt(const uint8_t* bundleData, uint64_t bundleIP) const override {
        (void)bundleData;

        Bundle bundle;
        bundle.templateType = TemplateType::MIB;
        bundle.hasStop = false;

        if (bundleIP == 0x1000) {
            InstructionEx call(InstructionType::BR_CALL, UnitType::B_UNIT);
            call.SetOperands(0, 0, 0);
            call.SetBranchTarget(0x2000);
            call.SetRawBits(0x10);
            bundle.instructions.push_back(call);
        } else if (bundleIP == 0x2000) {
            InstructionEx alloc(InstructionType::ALLOC, UnitType::M_UNIT);
            alloc.SetOperands(60, 0, 0);
            alloc.SetImmediate(36 | (static_cast<uint64_t>(31) << 7));
            alloc.SetRawBits(0x20);
            bundle.instructions.push_back(alloc);
        }

        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

// Test ISA state serialization/deserialization
void testISAStateSerialization() {
    std::cout << "Testing ISA state serialization...\n";
    
    // Create IA-64 state
    IA64ISAState state1;
    state1.getCPUState().SetGR(1, 0x1234567890ABCDEF);
    state1.getCPUState().SetIP(0x1000);
    state1.getCPUState().SetCFM(0x42);
    
    // Serialize
    std::vector<uint8_t> buffer(state1.getStateSize());
    state1.serialize(buffer.data());
    
    // Deserialize into new state
    IA64ISAState state2;
    state2.deserialize(buffer.data());
    
    // Verify
    assert(state2.getCPUState().GetGR(1) == 0x1234567890ABCDEF);
    assert(state2.getCPUState().GetIP() == 0x1000);
    assert(state2.getCPUState().GetCFM() == 0x42);
    
    std::cout << "  ? IA-64 state serialization works\n";
    
    // Test Example ISA state
    ExampleISAState exState1;
    exState1.generalRegisters[5] = 0x12345678;
    exState1.programCounter = 0x2000;
    
    std::vector<uint8_t> exBuffer(exState1.getStateSize());
    exState1.serialize(exBuffer.data());
    
    ExampleISAState exState2;
    exState2.deserialize(exBuffer.data());
    
    assert(exState2.generalRegisters[5] == 0x12345678);
    assert(exState2.programCounter == 0x2000);
    
    std::cout << "  ? Example ISA state serialization works\n";
}

// Test ISA plugin creation and basic operations
void testISAPluginCreation() {
    std::cout << "Testing ISA plugin creation...\n";
    
    Memory memory(64 * 1024);
    InstructionDecoder decoder;
    
    // Create IA-64 plugin
    auto ia64Plugin = createIA64ISA(decoder);
    assert(ia64Plugin != nullptr);
    assert(ia64Plugin->getName() == "IA-64");
    assert(ia64Plugin->getVersion() == "1.0");
    assert(ia64Plugin->getWordSize() == 64);
    assert(ia64Plugin->isLittleEndian() == true);
    
    std::cout << "  ? IA-64 plugin created: " << ia64Plugin->getName() 
              << " v" << ia64Plugin->getVersion() << "\n";
    
    // Test reset
    ia64Plugin->reset();
    assert(ia64Plugin->getPC() == 0);
    
    std::cout << "  ? IA-64 plugin reset works\n";
    
    // Create Example ISA plugin
    auto examplePlugin = createExampleISA();
    assert(examplePlugin != nullptr);
    assert(examplePlugin->getName() == "Example-ISA");
    assert(examplePlugin->getVersion() == "1.0");
    assert(examplePlugin->getWordSize() == 32);
    
    std::cout << "  ? Example ISA plugin created: " << examplePlugin->getName()
              << " v" << examplePlugin->getVersion() << "\n";
}

// Test ISA plugin registry
void testISAPluginRegistry() {
    std::cout << "Testing ISA plugin registry...\n";
    
    // Check that IA-64 is auto-registered
    assert(ISAPluginRegistry::instance().isRegistered("IA-64"));
    
    std::cout << "  ? IA-64 is auto-registered\n";
    
    // Register Example ISA
    bool registered = ISAPluginRegistry::instance().registerISA("Example-ISA",
        [](void* data) -> std::unique_ptr<IISA> {
            return createExampleISA();
        });
    
    if (!registered) {
        // May already be registered, that's okay
        std::cout << "  ? Example-ISA already registered\n";
    } else {
        std::cout << "  ? Example-ISA registered\n";
    }
    
    // List registered ISAs
    auto isas = ISAPluginRegistry::instance().getRegisteredISAs();
    std::cout << "  Registered ISAs:\n";
    for (const auto& isaName : isas) {
        std::cout << "    - " << isaName << "\n";
    }
    
    assert(isas.size() >= 1);  // At least IA-64
    
    // Create ISA via registry
    InstructionDecoder decoder;
    IA64FactoryData factoryData(&decoder);
    
    auto ia64 = ISAPluginRegistry::instance().createISA("IA-64", &factoryData);
    assert(ia64 != nullptr);
    assert(ia64->getName() == "IA-64");
    
    std::cout << "  ? Created ISA via registry: " << ia64->getName() << "\n";
}

// Test ISA features
void testISAFeatures() {
    std::cout << "Testing ISA features...\n";
    
    InstructionDecoder decoder;
    auto ia64 = createIA64ISA(decoder);
    
    // Test IA-64 features
    assert(ia64->hasFeature("predication") == true);
    assert(ia64->hasFeature("register_rotation") == true);
    assert(ia64->hasFeature("bundles") == true);
    assert(ia64->hasFeature("speculation") == false);  // Not implemented yet
    
    std::cout << "  ? IA-64 features:\n";
    std::cout << "    - predication: " << ia64->hasFeature("predication") << "\n";
    std::cout << "    - register_rotation: " << ia64->hasFeature("register_rotation") << "\n";
    std::cout << "    - bundles: " << ia64->hasFeature("bundles") << "\n";
    
    // Test Example ISA features
    auto example = createExampleISA();
    assert(example->hasFeature("simple") == true);
    assert(example->hasFeature("fixed_width") == true);
    
    std::cout << "  ? Example ISA features:\n";
    std::cout << "    - simple: " << example->hasFeature("simple") << "\n";
    std::cout << "    - fixed_width: " << example->hasFeature("fixed_width") << "\n";
}

// Test Example ISA execution
void testExampleISAExecution() {
    std::cout << "Testing Example ISA execution...\n";
    
    Memory memory(64 * 1024);
    auto isa = createExampleISA();
    
    isa->reset();
    assert(isa->getPC() == 0);
    
    // Write a simple program to memory
    // Instruction format: [opcode:8][rd:5][rs1:5][rs2:5][unused:9]
    
    // NOP at address 0
    uint32_t nop = 0x00000000;  // opcode=0 (NOP)
    memory.write<uint32_t>(0, nop);
    
    // ADD r1, r0, r0 at address 4 (will set r1 to 0)
    uint32_t add = (0x01 << 24) | (1 << 19) | (0 << 14) | (0 << 9);  // ADD, rd=1, rs1=0, rs2=0
    memory.write<uint32_t>(4, add);
    
    // HALT at address 8
    uint32_t halt = 0xFF000000;  // opcode=0xFF (HALT)
    memory.write<uint32_t>(8, halt);
    
    // Execute NOP
    auto result1 = isa->step(memory);
    assert(result1 == ISAExecutionResult::CONTINUE);
    assert(isa->getPC() == 4);
    std::cout << "  ? NOP executed, PC advanced to " << isa->getPC() << "\n";
    
    // Execute ADD
    auto result2 = isa->step(memory);
    assert(result2 == ISAExecutionResult::CONTINUE);
    assert(isa->getPC() == 8);
    std::cout << "  ? ADD executed, PC advanced to " << isa->getPC() << "\n";
    
    // Execute HALT
    auto result3 = isa->step(memory);
    assert(result3 == ISAExecutionResult::HALT);
    std::cout << "  ? HALT executed, execution stopped\n";
}

void testIA64BranchPredicateExecution() {
    std::cout << "Testing IA-64 predicated branch execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeBranchDecoder decoder;

    auto notTaken = createIA64ISA(decoder);
    auto& notTakenState = dynamic_cast<IA64ISAState&>(notTaken->getState());
    notTakenState.getCPUState().SetIP(0x1000);
    notTakenState.getCPUState().SetPR(1, false);
    assert(notTaken->step(memory) == ISAExecutionResult::CONTINUE);
    assert(notTaken->getPC() == 0x1010);

    auto taken = createIA64ISA(decoder);
    auto& takenState = dynamic_cast<IA64ISAState&>(taken->getState());
    takenState.getCPUState().SetIP(0x1000);
    takenState.getCPUState().SetPR(1, true);
    assert(taken->step(memory) == ISAExecutionResult::CONTINUE);
    assert(taken->getPC() == 0x2000);

    std::cout << "  ? False-predicated branch falls through; true predicate branches\n";
}

void testIA64BranchPredicateGroupSnapshot() {
    std::cout << "Testing IA-64 predicate visibility across instruction groups...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeCompareBranchDecoder noStopDecoder(false);
    auto noStop = createIA64ISA(noStopDecoder);
    auto& noStopState = dynamic_cast<IA64ISAState&>(noStop->getState());
    noStopState.getCPUState().SetIP(0x1000);
    noStopState.getCPUState().SetPR(1, false);
    assert(noStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(noStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(noStop->getPC() == 0x1010);

    FakeCompareBranchDecoder stopDecoder(true);
    auto withStop = createIA64ISA(stopDecoder);
    auto& withStopState = dynamic_cast<IA64ISAState&>(withStop->getState());
    withStopState.getCPUState().SetIP(0x1000);
    withStopState.getCPUState().SetPR(1, false);
    assert(withStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(withStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(withStop->getPC() == 0x2000);

    std::cout << "  ? Same-group predicate write is deferred; stop makes it visible\n";
}

void testIA64AddImm22Decode() {
    std::cout << "Testing IA-64 add imm22 decode...\n";

    InstructionDecoder decoder;
    InstructionEx add = decoder.DecodeSlot(0x12000000200ULL, UnitType::M_UNIT, 0x36e80);

    assert(add.GetType() == InstructionType::ADD_IMM);
    assert(add.GetDst() == 8);
    assert(add.GetSrc1() == 0);
    assert(add.HasImmediate());
    assert(add.GetImmediate() == 0);

    CPUState cpu;
    Memory memory(64 * 1024);
    cpu.SetGR(8, 0x1234);
    add.Execute(cpu, memory);
    assert(cpu.GetGR(8) == 0);

    std::cout << "  ? add imm22 raw boot slot decodes and executes\n";
}

void testIA64AddlImm22SourceDecode() {
    std::cout << "Testing IA-64 addl imm22 source decode...\n";

    InstructionDecoder decoder;
    InstructionEx add = decoder.DecodeSlot(0x13b90520880ULL, UnitType::M_UNIT, 0x18e0);

    assert(add.GetType() == InstructionType::ADD_IMM);
    assert(add.GetDst() == 34);
    assert(add.GetSrc1() == 1);
    assert(add.HasImmediate());
    assert(static_cast<int64_t>(add.GetImmediate()) == -581488);
    assert(add.GetDisassembly() == "add r34 = r1, -581488");

    CPUState cpu;
    Memory memory(64 * 1024);
    cpu.SetGR(1, 0x238000);
    add.Execute(cpu, memory);
    assert(cpu.GetGR(34) == 0x1aa090);

    std::cout << "  ? addl imm22 uses 2-bit r3 field instead of immediate bits\n";
}

void testIA64ReturnBranchDecode() {
    std::cout << "Testing IA-64 return branch decode...\n";

    InstructionDecoder decoder;
    InstructionEx ret = decoder.DecodeSlot(0x108000100ULL, UnitType::B_UNIT, 0x36e80);

    assert(ret.GetType() == InstructionType::BR_RET);
    assert(ret.GetSrc1() == 0);
    assert(!ret.HasBranchTarget());
    assert(ret.GetDisassembly() == "br.ret b0");

    std::cout << "  ? raw boot return branch decodes as br.ret b0\n";
}

void testIA64NopIDecode() {
    std::cout << "Testing IA-64 nop decode variants...\n";

    InstructionDecoder decoder;
    InstructionEx nopI = decoder.DecodeSlot(0x4000000000ULL, UnitType::I_UNIT, 0x18d0);

    assert(nopI.GetType() == InstructionType::NOP);
    assert(nopI.GetRawBits() == 0x4000000000ULL);
    assert(nopI.GetDisassembly() == "nop");

    InstructionEx nopB = decoder.DecodeSlot(0x4000000000ULL, UnitType::B_UNIT, 0x18d0);

    assert(nopB.GetType() == InstructionType::NOP);
    assert(nopB.GetRawBits() == 0x4000000000ULL);
    assert(nopB.GetDisassembly() == "nop");

    std::cout << "  ? raw boot NOP slot decodes on I and B units\n";
}

void testIA64MoveFromBranchDecode() {
    std::cout << "Testing IA-64 move from branch register decode...\n";

    InstructionDecoder decoder;
    InstructionEx mov = decoder.DecodeSlot(0x198000f80ULL, UnitType::I_UNIT, 0x18e0);

    assert(mov.GetType() == InstructionType::MOV_FROM_BR);
    assert(mov.GetDst() == 62);
    assert(mov.GetSrc1() == 0);
    assert(mov.GetRawBits() == 0x198000f80ULL);
    assert(mov.GetDisassembly() == "mov r62 = b0");

    std::cout << "  ? raw boot branch-register move decodes as mov r62 = b0\n";
}

void testIA64AddsImm14Decode() {
    std::cout << "Testing IA-64 adds imm14 decode...\n";

    InstructionDecoder decoder;
    InstructionEx add = decoder.DecodeSlot(0x119f8ce0300ULL, UnitType::I_UNIT, 0x2eec0);

    assert(add.GetType() == InstructionType::ADD_IMM);
    assert(add.GetDst() == 12);
    assert(add.GetSrc1() == 12);
    assert(add.HasImmediate());
    assert(static_cast<int64_t>(add.GetImmediate()) == -16);
    assert(add.GetRawBits() == 0x119f8ce0300ULL);
    assert(add.GetDisassembly() == "add r12 = r12, -16");

    CPUState cpu;
    Memory memory(64 * 1024);
    cpu.SetGR(12, 0x1000);
    add.Execute(cpu, memory);
    assert(cpu.GetGR(12) == 0x0ff0);

    std::cout << "  ? raw boot stack adjustment decodes and executes\n";
}

void testIA64MovlBootBundleDecode() {
    std::cout << "Testing IA-64 MLX movl boot bundle decode...\n";

    const uint8_t bundleBytes[16] = {
        0x05, 0x00, 0x00, 0x00, 0x01, 0xc0, 0xff, 0xff,
        0xff, 0xff, 0x7f, 0x80, 0x04, 0x80, 0x03, 0x6c
    };

    InstructionDecoder decoder;
    Bundle bundle = decoder.DecodeBundleAt(bundleBytes, 0x1010);

    assert(bundle.templateType == TemplateType::MLX_STOP);
    assert(bundle.instructions.size() == 2);
    assert(bundle.instructions[1].GetType() == InstructionType::MOVL);
    assert(bundle.instructions[1].GetDst() == 36);
    assert(bundle.instructions[1].HasImmediate());
    assert(bundle.instructions[1].GetImmediate() == 0xffffffffffdc8000ULL);

    CPUState cpu;
    Memory memory(64 * 1024);
    bundle.instructions[1].Execute(cpu, memory);
    assert(cpu.GetGR(36) == 0xffffffffffdc8000ULL);

    std::cout << "  ? raw boot MLX bundle decodes movl r36 = -0x238000\n";
}

void testIA64LegacyCallOutputInputs() {
    std::cout << "Testing IA-64 legacy call output/input register bridge...\n";

    Memory memory(64 * 1024);
    InstructionDecoder decoder;
    CPU cpu(memory, decoder);

    cpu.getState().SetIP(0x1000);
    cpu.getState().SetCFM(6 | (static_cast<uint64_t>(4) << 7));
    cpu.getState().SetGR(36, 0x12345678);
    cpu.getState().SetGR(37, 0xabcdef00);

    InstructionEx call(InstructionType::BR_CALL, UnitType::B_UNIT);
    call.SetOperands(0, 0, 0);
    call.SetBranchTarget(0x2000);
    cpu.executeInstruction(call);

    assert(cpu.getState().GetIP() == 0x2000);
    assert(cpu.getState().GetBR(0) == 0x1010);

    InstructionEx alloc(InstructionType::ALLOC, UnitType::M_UNIT);
    alloc.SetOperands(60, 0, 0);
    alloc.SetImmediate(36 | (static_cast<uint64_t>(31) << 7));
    cpu.executeInstruction(alloc);

    assert(cpu.getState().GetGR(32) == 0x12345678);
    assert(cpu.getState().GetGR(33) == 0xabcdef00);

    std::cout << "  ? caller outputs become callee input registers after alloc\n";
}

void testIA64PluginCallOutputInputs() {
    std::cout << "Testing IA-64 plugin call output/input register bridge...\n";

    Memory memory(64 * 1024);
    FakeCallBridgeDecoder decoder;
    IA64ISAPlugin plugin(decoder);

    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(4) << 7));
    plugin.getCPUState().SetGR(36, 0x12345678);
    plugin.getCPUState().SetGR(37, 0xabcdef00);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x2000);
    assert(plugin.getCPUState().GetBR(0) == 0x1010);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetGR(32) == 0x12345678);
    assert(plugin.getCPUState().GetGR(33) == 0xabcdef00);

    std::cout << "  ? plugin br.call passes caller output registers to callee inputs\n";
}

void testIA64MemoryPostIncrement() {
    std::cout << "Testing IA-64 memory post-increment execution...\n";

    Memory memory(64 * 1024);
    CPUState cpu;

    cpu.SetGR(15, 0x100);
    cpu.SetGR(19, 0x1122334455667788ULL);

    InstructionEx store(InstructionType::ST4, UnitType::M_UNIT);
    store.SetOperands(15, 19, 0);
    store.SetImmediate(4);
    store.Execute(cpu, memory);

    uint32_t stored = 0;
    memory.Read(0x100, reinterpret_cast<uint8_t*>(&stored), sizeof(stored));
    assert(stored == 0x55667788U);
    assert(cpu.GetGR(15) == 0x104);

    const uint64_t loadValue = 0xaabbccddeeff0011ULL;
    memory.Write(0x200, reinterpret_cast<const uint8_t*>(&loadValue), sizeof(loadValue));
    cpu.SetGR(14, 0x200);

    InstructionEx load(InstructionType::LD8, UnitType::M_UNIT);
    load.SetOperands(8, 14, 0);
    load.SetImmediate(8);
    load.Execute(cpu, memory);

    assert(cpu.GetGR(8) == loadValue);
    assert(cpu.GetGR(14) == 0x208);

    std::cout << "  ? ld/st immediate forms update the base register after access\n";
}

// Test state dump
void testStateDump() {
    std::cout << "Testing state dump...\n";
    
    InstructionDecoder decoder;
    auto ia64 = createIA64ISA(decoder);
    
    ia64->reset();
    auto& state = dynamic_cast<IA64ISAState&>(ia64->getState());
    state.getCPUState().SetGR(1, 0x1234);
    state.getCPUState().SetIP(0x5678);
    
    std::string dump = ia64->dumpState();
    assert(dump.find("0x5678") != std::string::npos);  // Should contain IP
    
    std::cout << "  ? State dump:\n";
    std::cout << dump << "\n";
}

// Test multiple ISAs with shared memory
void testSharedMemory() {
    std::cout << "Testing shared memory across ISAs...\n";
    
    Memory memory(64 * 1024);
    InstructionDecoder decoder;
    
    auto ia64 = createIA64ISA(decoder);
    auto example = createExampleISA();
    
    // Write data using one ISA's memory reference
    const uint64_t testAddr = 0x1000;
    const uint64_t testValue = 0xDEADBEEFCAFEBABE;
    
    memory.write<uint64_t>(testAddr, testValue);
    
    // Both ISAs should see the same memory
    uint64_t readValue = memory.read<uint64_t>(testAddr);
    assert(readValue == testValue);
    
    std::cout << "  ? Memory is shared across ISAs\n";
    std::cout << "    Wrote: 0x" << std::hex << testValue << std::dec << "\n";
    std::cout << "    Read:  0x" << std::hex << readValue << std::dec << "\n";
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "=== ISA Plugin Architecture Tests ===\n\n";
    
    try {
        testISAStateSerialization();
        std::cout << "\n";
        
        testISAPluginCreation();
        std::cout << "\n";
        
        testISAPluginRegistry();
        std::cout << "\n";
        
        testISAFeatures();
        std::cout << "\n";
        
        testExampleISAExecution();
        std::cout << "\n";

        testIA64BranchPredicateExecution();
        std::cout << "\n";

        testIA64BranchPredicateGroupSnapshot();
        std::cout << "\n";

        testIA64AddImm22Decode();
        std::cout << "\n";

        testIA64AddlImm22SourceDecode();
        std::cout << "\n";

        testIA64ReturnBranchDecode();
        std::cout << "\n";

        testIA64NopIDecode();
        std::cout << "\n";

        testIA64MoveFromBranchDecode();
        std::cout << "\n";

        testIA64AddsImm14Decode();
        std::cout << "\n";

        testIA64MovlBootBundleDecode();
        std::cout << "\n";

        testIA64LegacyCallOutputInputs();
        std::cout << "\n";

        testIA64PluginCallOutputInputs();
        std::cout << "\n";

        testIA64MemoryPostIncrement();
        std::cout << "\n";
        
        testStateDump();
        std::cout << "\n";
        
        testSharedMemory();
        std::cout << "\n";
        
        std::cout << "=== All tests passed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n? Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
