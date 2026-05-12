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
#include <map>

using namespace ia64;

class SparseMemory : public IMemory {
public:
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(address + i);
            dest[i] = it == bytes_.end() ? 0 : it->second;
        }
    }

    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            bytes_[address + i] = src[i];
        }
    }

    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override {
        Write(address, buffer, size);
    }

    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override {
        Write(address, buffer.data(), buffer.size());
    }

    size_t GetTotalSize() const override { return 0; }
    void Clear() override { bytes_.clear(); }
    const uint8_t* GetRawData() const override { return nullptr; }

private:
    std::map<uint64_t, uint8_t> bytes_;
};

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

class FakeIndirectBranchDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx branch(InstructionType::BR_COND, UnitType::B_UNIT);
        branch.SetOperands(0, 6, 0);
        branch.SetRawBits(0x200000c000ULL);

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

class FakeIndirectCallDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx call(InstructionType::BR_CALL, UnitType::B_UNIT);
        call.SetOperands(0, 6, 0);
        call.SetRawBits(0x200000c000ULL);

        Bundle bundle;
        bundle.templateType = TemplateType::MIB;
        bundle.hasStop = false;
        bundle.instructions.push_back(call);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeIndirectSelfCallDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx call(InstructionType::BR_CALL, UnitType::B_UNIT);
        call.SetOperands(0, 0, 0);
        call.SetRawBits(0x2100001000ULL);

        Bundle bundle;
        bundle.templateType = TemplateType::MIB;
        bundle.hasStop = false;
        bundle.instructions.push_back(call);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeCountedLoopDecoder : public IDecoder {
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

        InstructionEx branch(InstructionType::BR_CLOOP, UnitType::B_UNIT);
        branch.SetBranchTarget(bundleIP);
        branch.SetRawBits(0xb1ffffc140ULL);

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

class FakeCallCountedLoopDecoder : public IDecoder {
public:
    explicit FakeCallCountedLoopDecoder(int64_t targetDelta = -0x20)
        : targetDelta_(targetDelta) {}

    InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const override {
        (void)bundleData;
        return InstructionBundle();
    }

    Bundle DecodeBundle(const uint8_t* bundleData) const override {
        return DecodeBundleAt(bundleData, 0);
    }

    Bundle DecodeBundleAt(const uint8_t* bundleData, uint64_t bundleIP) const override {
        (void)bundleData;

        InstructionEx branch(InstructionType::BR_CALL, UnitType::B_UNIT);
        branch.SetOperands(5, 0, 0);
        const uint64_t target = targetDelta_ < 0
            ? bundleIP - static_cast<uint64_t>(-targetDelta_)
            : bundleIP + static_cast<uint64_t>(targetDelta_);
        branch.SetBranchTarget(target);
        branch.SetRawBits(0x5a100ULL);

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

private:
    int64_t targetDelta_;
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

class FakeCompareStoreDecoder : public IDecoder {
public:
    explicit FakeCompareStoreDecoder(bool stopAfterCompare)
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
        compare.SetRawBits(0x40);

        InstructionEx store(InstructionType::ST8, UnitType::M_UNIT);
        store.SetPredicate(1);
        store.SetOperands(10, 11, 0);
        store.SetRawBits(0x41);

        Bundle bundle;
        bundle.templateType = stopAfterCompare_ ? TemplateType::MI_I : TemplateType::MII;
        bundle.hasStop = stopAfterCompare_;
        bundle.stopAfterSlot[0] = stopAfterCompare_;
        bundle.instructions.push_back(compare);
        bundle.instructions.push_back(store);
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

            InstructionEx clobberLocal(InstructionType::ADD, UnitType::I_UNIT);
            clobberLocal.SetOperands(36, 0, 0);
            clobberLocal.SetRawBits(0x21);
            bundle.instructions.push_back(clobberLocal);
        } else if (bundleIP == 0x2010) {
            InstructionEx ret(InstructionType::BR_RET, UnitType::B_UNIT);
            ret.SetOperands(0, 0, 0);
            ret.SetRawBits(0x30);
            bundle.instructions.push_back(ret);
        }

        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeStaleCallFrameDecoder : public IDecoder {
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
            alloc.SetOperands(50, 0, 0);
            alloc.SetImmediate(8 | (static_cast<uint64_t>(6) << 7));
            alloc.SetRawBits(0x20);
            bundle.instructions.push_back(alloc);

            InstructionEx nestedCall(InstructionType::BR_CALL, UnitType::B_UNIT);
            nestedCall.SetOperands(0, 0, 0);
            nestedCall.SetBranchTarget(0x3000);
            nestedCall.SetRawBits(0x21);
            bundle.instructions.push_back(nestedCall);
        } else if (bundleIP == 0x3000) {
            InstructionEx alloc(InstructionType::ALLOC, UnitType::M_UNIT);
            alloc.SetOperands(51, 0, 0);
            alloc.SetImmediate(5 | (static_cast<uint64_t>(3) << 7));
            alloc.SetRawBits(0x30);
            bundle.instructions.push_back(alloc);

            InstructionEx restoreOuterReturn(InstructionType::MOV_TO_BR, UnitType::I_UNIT);
            restoreOuterReturn.SetOperands(0, 40, 0);
            restoreOuterReturn.SetRawBits(0x31);
            bundle.instructions.push_back(restoreOuterReturn);

            InstructionEx ret(InstructionType::BR_RET, UnitType::B_UNIT);
            ret.SetOperands(0, 0, 0);
            ret.SetRawBits(0x32);
            bundle.instructions.push_back(ret);
        }

        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeEfiThunkReturnDecoder : public IDecoder {
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
            call.SetBranchTarget(0x1990);
            call.SetRawBits(0x10);
            bundle.instructions.push_back(call);
        } else if (bundleIP == 0x1990) {
            InstructionEx saveGp(InstructionType::MOV_GR, UnitType::I_UNIT);
            saveGp.SetOperands(61, 1, 0);
            saveGp.SetRawBits(0x20);
            bundle.instructions.push_back(saveGp);

            InstructionEx serviceThunk(InstructionType::BR_COND, UnitType::B_UNIT);
            serviceThunk.SetOperands(0, 6, 0);
            serviceThunk.SetRawBits(0x200000c000ULL);
            bundle.instructions.push_back(serviceThunk);
        } else if (bundleIP == 0x1fe00f80ULL) {
            InstructionEx ret(InstructionType::BR_RET, UnitType::B_UNIT);
            ret.SetOperands(0, 0, 0);
            ret.SetRawBits(0x30);
            bundle.instructions.push_back(ret);
        }

        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeTopLevelReturnDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx ret(InstructionType::BR_RET, UnitType::B_UNIT);
        ret.SetOperands(0, 0, 0);
        ret.SetRawBits(0x108000100ULL);

        Bundle bundle;
        bundle.templateType = TemplateType::MIB;
        bundle.hasStop = false;
        bundle.instructions.push_back(ret);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeOutOfBoundsLoadDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx load(InstructionType::LD8, UnitType::M_UNIT);
        load.SetOperands(14, 8, 0);
        load.SetImmediate(8);
        load.SetRawBits(0x0badf00dULL);

        Bundle bundle;
        bundle.templateType = TemplateType::MII;
        bundle.hasStop = false;
        bundle.instructions.push_back(load);
        return bundle;
    }

    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override {
        InstructionEx instr(InstructionType::UNKNOWN, unit);
        instr.SetRawBits(rawBits);
        return instr;
    }
};

class FakeOutOfBoundsProbeLoadDecoder : public IDecoder {
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
        (void)bundleIP;

        InstructionEx load(InstructionType::LD4, UnitType::M_UNIT);
        load.SetOperands(15, 14, 0);
        load.SetRawBits(0x8080e003c0ULL);

        Bundle bundle;
        bundle.templateType = TemplateType::MII;
        bundle.hasStop = false;
        bundle.instructions.push_back(load);
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

void testIA64CountedLoopBranchExecution() {
    std::cout << "Testing IA-64 counted loop branch execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeCountedLoopDecoder decoder;
    auto taken = createIA64ISA(decoder);
    auto& takenState = dynamic_cast<IA64ISAState&>(taken->getState());
    takenState.getCPUState().SetIP(0x1000);
    takenState.getCPUState().SetAR(65, 2);
    assert(taken->step(memory) == ISAExecutionResult::CONTINUE);
    assert(taken->getPC() == 0x1000);
    assert(takenState.getCPUState().GetAR(65) == 1);

    auto fallthrough = createIA64ISA(decoder);
    auto& fallthroughState = dynamic_cast<IA64ISAState&>(fallthrough->getState());
    fallthroughState.getCPUState().SetIP(0x1000);
    fallthroughState.getCPUState().SetAR(65, 0);
    assert(fallthrough->step(memory) == ISAExecutionResult::CONTINUE);
    assert(fallthrough->getPC() == 0x1010);
    assert(fallthroughState.getCPUState().GetAR(65) == 0);

    std::cout << "  ? br.cloop decrements ar.lc and falls through at zero\n";
}

void testIA64CallDecodedCountedLoopExecution() {
    std::cout << "Testing IA-64 call-decoded counted loop execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeCallCountedLoopDecoder decoder;
    auto taken = createIA64ISA(decoder);
    auto& takenState = dynamic_cast<IA64ISAState&>(taken->getState());
    takenState.getCPUState().SetIP(0x1000);
    takenState.getCPUState().SetAR(65, 2);
    takenState.getCPUState().SetBR(5, 0);
    assert(taken->step(memory) == ISAExecutionResult::CONTINUE);
    assert(taken->getPC() == 0x0fe0);
    assert(takenState.getCPUState().GetAR(65) == 1);
    assert(takenState.getCPUState().GetBR(5) == 0);

    auto fallthrough = createIA64ISA(decoder);
    auto& fallthroughState = dynamic_cast<IA64ISAState&>(fallthrough->getState());
    fallthroughState.getCPUState().SetIP(0x1000);
    fallthroughState.getCPUState().SetAR(65, 0);
    fallthroughState.getCPUState().SetBR(5, 0);
    assert(fallthrough->step(memory) == ISAExecutionResult::CONTINUE);
    assert(fallthrough->getPC() == 0x1010);
    assert(fallthroughState.getCPUState().GetAR(65) == 0);
    assert(fallthroughState.getCPUState().GetBR(5) == 0);

    FakeCallCountedLoopDecoder selfDecoder(0);
    auto self = createIA64ISA(selfDecoder);
    auto& selfState = dynamic_cast<IA64ISAState&>(self->getState());
    selfState.getCPUState().SetIP(0x1000);
    selfState.getCPUState().SetAR(65, 1);
    selfState.getCPUState().SetBR(5, 0);
    assert(self->step(memory) == ISAExecutionResult::CONTINUE);
    assert(self->getPC() == 0x1000);
    assert(selfState.getCPUState().GetAR(65) == 0);
    assert(selfState.getCPUState().GetBR(5) == 0);
    assert(self->step(memory) == ISAExecutionResult::CONTINUE);
    assert(self->getPC() == 0x1010);

    std::cout << "  ? backward/self br.call b5 uses ar.lc and does not link\n";
}

void testIA64BranchPredicateGroupSnapshot() {
    std::cout << "Testing IA-64 branch predicate visibility across instruction groups...\n";

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
    assert(noStop->getPC() == 0x2000);

    FakeCompareBranchDecoder stopDecoder(true);
    auto withStop = createIA64ISA(stopDecoder);
    auto& withStopState = dynamic_cast<IA64ISAState&>(withStop->getState());
    withStopState.getCPUState().SetIP(0x1000);
    withStopState.getCPUState().SetPR(1, false);
    assert(withStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(withStop->step(memory) == ISAExecutionResult::CONTINUE);
    assert(withStop->getPC() == 0x2000);

    std::cout << "  ? Same-group predicate write is visible to branches with or without a stop\n";
}

void testIA64NonBranchPredicateGroupSnapshot() {
    std::cout << "Testing IA-64 non-branch predicate visibility across instruction groups...\n";

    const uint64_t dataAddr = 0x3000;
    const uint64_t dataValue = 0x123456789abcdef0ULL;

    Memory noStopMemory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    noStopMemory.Write(0x1000, bundleBytes, sizeof(bundleBytes));
    noStopMemory.write<uint64_t>(dataAddr, 0);

    FakeCompareStoreDecoder noStopDecoder(false);
    auto noStop = createIA64ISA(noStopDecoder);
    auto& noStopState = dynamic_cast<IA64ISAState&>(noStop->getState());
    noStopState.getCPUState().SetIP(0x1000);
    noStopState.getCPUState().SetPR(1, false);
    noStopState.getCPUState().SetGR(10, dataAddr);
    noStopState.getCPUState().SetGR(11, dataValue);
    assert(noStop->step(noStopMemory) == ISAExecutionResult::CONTINUE);
    assert(noStop->step(noStopMemory) == ISAExecutionResult::CONTINUE);
    assert(noStopMemory.read<uint64_t>(dataAddr) == 0);

    Memory stopMemory(64 * 1024);
    stopMemory.Write(0x1000, bundleBytes, sizeof(bundleBytes));
    stopMemory.write<uint64_t>(dataAddr, 0);

    FakeCompareStoreDecoder stopDecoder(true);
    auto withStop = createIA64ISA(stopDecoder);
    auto& withStopState = dynamic_cast<IA64ISAState&>(withStop->getState());
    withStopState.getCPUState().SetIP(0x1000);
    withStopState.getCPUState().SetPR(1, false);
    withStopState.getCPUState().SetGR(10, dataAddr);
    withStopState.getCPUState().SetGR(11, dataValue);
    assert(withStop->step(stopMemory) == ISAExecutionResult::CONTINUE);
    assert(withStop->step(stopMemory) == ISAExecutionResult::CONTINUE);
    assert(stopMemory.read<uint64_t>(dataAddr) == dataValue);

    std::cout << "  ? Same-group predicate write is deferred for stores; stop makes it visible\n";
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

void testIA64IndirectCallDecode() {
    std::cout << "Testing IA-64 indirect call decode...\n";

    InstructionDecoder decoder;
    InstructionEx call = decoder.DecodeSlot(0x200000c000ULL, UnitType::B_UNIT, 0x1980);

    assert(call.GetType() == InstructionType::BR_CALL);
    assert(call.GetDst() == 0);
    assert(call.GetSrc1() == 6);
    assert(!call.HasBranchTarget());
    assert(call.GetDisassembly() == "br.call b0 = b6");

    std::cout << "  ? raw boot indirect service call decodes through b6 and links b0\n";
}

void testIA64IndirectSelfCallDecode() {
    std::cout << "Testing IA-64 hinted indirect self-call decode...\n";

    InstructionDecoder decoder;
    InstructionEx call = decoder.DecodeSlot(0x2100001000ULL, UnitType::B_UNIT, 0x1a50);

    assert(call.GetType() == InstructionType::BR_CALL);
    assert(call.GetDst() == 0);
    assert(call.GetSrc1() == 0);
    assert(!call.HasBranchTarget());
    assert(call.GetDisassembly() == "br.call b0 = b0");

    std::cout << "  ? raw boot hinted br.call b0 = b0 decodes as call, not cond\n";
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

void testIA64LegacyCallDecodedCountedLoop() {
    std::cout << "Testing IA-64 legacy call-decoded counted loop execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeCallCountedLoopDecoder decoder;
    CPU taken(memory, decoder);
    taken.getState().SetIP(0x1000);
    taken.getState().SetAR(65, 2);
    taken.getState().SetBR(5, 0);
    assert(taken.step());
    assert(taken.getState().GetIP() == 0x0fe0);
    assert(taken.getState().GetAR(65) == 1);
    assert(taken.getState().GetBR(5) == 0);

    CPU fallthrough(memory, decoder);
    fallthrough.getState().SetIP(0x1000);
    fallthrough.getState().SetAR(65, 0);
    fallthrough.getState().SetBR(5, 0);
    assert(fallthrough.step());
    assert(fallthrough.getState().GetIP() == 0x1010);
    assert(fallthrough.getState().GetAR(65) == 0);
    assert(fallthrough.getState().GetBR(5) == 0);

    FakeCallCountedLoopDecoder selfDecoder(0);
    CPU self(memory, selfDecoder);
    self.getState().SetIP(0x1000);
    self.getState().SetAR(65, 1);
    self.getState().SetBR(5, 0);
    assert(self.step());
    assert(self.getState().GetIP() == 0x1000);
    assert(self.getState().GetAR(65) == 0);
    assert(self.getState().GetBR(5) == 0);
    assert(self.step());
    assert(self.getState().GetIP() == 0x1010);

    std::cout << "  ? legacy backward/self br.call b5 uses ar.lc and does not link\n";
}

void testIA64LegacyIndirectBranchExecution() {
    std::cout << "Testing IA-64 legacy indirect branch execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeIndirectBranchDecoder decoder;
    CPU cpu(memory, decoder);
    cpu.getState().SetIP(0x1000);
    cpu.getState().SetBR(6, 0x2408);

    assert(cpu.step());
    assert(cpu.getState().GetIP() == 0x2400);

    std::cout << "  ? legacy br.cond b6 enters at the target bundle boundary\n";
}

void testIA64LegacyIndirectCallExecution() {
    std::cout << "Testing IA-64 legacy indirect call execution...\n";

    Memory memory(64 * 1024);
    InstructionDecoder decoder;
    CPU cpu(memory, decoder);
    InstructionEx call = decoder.DecodeSlot(0x200000c000ULL, UnitType::B_UNIT, 0x1990);

    cpu.getState().SetIP(0x1990);
    cpu.getState().SetBR(6, 0x2408);

    cpu.executeInstruction(call);
    assert(cpu.getState().GetIP() == 0x2400);
    assert(cpu.getState().GetBR(0) == 0x19a0);

    std::cout << "  ? legacy br.call b0 = b6 links b0 and enters at the target bundle boundary\n";
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

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetGR(36) == 0);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1010);
    assert(plugin.getCPUState().GetCFM() == (6 | (static_cast<uint64_t>(4) << 7)));
    assert(plugin.getCPUState().GetGR(36) == 0x12345678);
    assert(plugin.getCPUState().GetGR(37) == 0xabcdef00);

    std::cout << "  ? plugin br.call passes inputs and br.ret restores caller frame\n";
}

void testIA64PluginCallFrameRestoreSkipsStaleFrames() {
    std::cout << "Testing IA-64 plugin call-frame restore skips stale frames...\n";

    Memory memory(64 * 1024);
    FakeStaleCallFrameDecoder decoder;
    IA64ISAPlugin plugin(decoder);

    const uint64_t initialCfm = 6 | (static_cast<uint64_t>(4) << 7);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetCFM(initialCfm);
    plugin.getCPUState().SetGR(36, 0x12345678);
    plugin.getCPUState().SetGR(40, 0x1010);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x2000);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetCFM() == (8 | (static_cast<uint64_t>(6) << 7)));

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x3000);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetCFM() == (5 | (static_cast<uint64_t>(3) << 7)));

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetBR(0) == 0x1010);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1010);
    assert(plugin.getCPUState().GetCFM() == initialCfm);
    assert(plugin.getCPUState().GetGR(36) == 0x12345678);

    std::cout << "  ? returning to an older saved address restores the matching caller frame\n";
}

void testIA64PluginIndirectBranchExecution() {
    std::cout << "Testing IA-64 plugin indirect branch execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeIndirectBranchDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetBR(6, 0x2408);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x2400);
    assert(plugin.getCPUState().GetBR(0) == 0);

    std::cout << "  ? plugin br.cond b6 enters at the target bundle boundary\n";
}

void testIA64PluginEfiServiceThunkLinksReturn() {
    std::cout << "Testing IA-64 plugin EFI service thunk return link...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1990, bundleBytes, sizeof(bundleBytes));

    FakeIndirectBranchDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1990);
    plugin.getCPUState().SetBR(6, 0x1fe00f80ULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1fe00f80ULL);
    assert(plugin.getCPUState().GetBR(0) == 0x19a0);

    std::cout << "  ? plugin EFI br.cond b6 thunk links b0 for service return\n";
}

void testIA64PluginEfiThunkReturnDoesNotPopOuterCallFrame() {
    std::cout << "Testing IA-64 plugin EFI thunk return preserves outer call frame...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));
    memory.Write(0x1990, bundleBytes, sizeof(bundleBytes));
    memory.Write(0x1fe00f80ULL, bundleBytes, sizeof(bundleBytes));

    FakeEfiThunkReturnDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetGR(1, 0x238000);
    plugin.getCPUState().SetGR(61, 0);
    plugin.getCPUState().SetBR(6, 0x1fe00f80ULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1990);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetGR(61) == 0x238000);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1fe00f80ULL);
    assert(plugin.getCPUState().GetBR(0) == 0x19a0);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x19a0);
    assert(plugin.getCPUState().GetGR(61) == 0x238000);

    std::cout << "  ? EFI service br.ret does not restore an unrelated br.call frame\n";
}

void testIA64PluginIndirectCallExecution() {
    std::cout << "Testing IA-64 plugin indirect call execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetBR(6, 0x2408);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x2400);
    assert(plugin.getCPUState().GetBR(0) == 0x1010);

    std::cout << "  ? plugin br.call b0 = b6 links b0 and enters at the target bundle boundary\n";
}

void testIA64PluginZeroFilledFirmwareCallReturnsSuccess() {
    std::cout << "Testing IA-64 plugin zero-filled firmware call handling...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetBR(6, 0x1ff92fa8ULL);
    plugin.getCPUState().SetGR(8, 0);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x1010);
    assert(plugin.getCPUState().GetBR(0) == 0x1010);
    assert(plugin.getCPUState().GetGR(8) == 0);

    std::cout << "  ? zero-filled high-memory br.call returns EFI_SUCCESS instead of exiting or walking NOPs\n";
}

void testIA64PluginNullFirmwareCallReturnsSuccess() {
    std::cout << "Testing IA-64 plugin null firmware call handling...\n";

    Memory memory(512 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x30e40, bundleBytes, sizeof(bundleBytes));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x30e40);
    plugin.getCPUState().SetBR(6, 0);
    plugin.getCPUState().SetGR(8, 0);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x30e50);
    assert(plugin.getCPUState().GetBR(0) == 0x30e50);
    assert(plugin.getCPUState().GetGR(8) == 0);

    std::cout << "  ? null indirect br.call returns EFI_SUCCESS instead of entering IP zero\n";
}

void testIA64PluginHandleProtocolReturnsLoadedImage() {
    std::cout << "Testing IA-64 plugin HandleProtocol firmware stub...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1a50, bundleBytes, sizeof(bundleBytes));
    const uint8_t loadedImageGuid[16] = {
        0xA1, 0x31, 0x1B, 0x5B, 0x62, 0x95, 0xD2, 0x11,
        0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
    };
    memory.Write(0x3000, loadedImageGuid, sizeof(loadedImageGuid));

    FakeIndirectSelfCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1a50);
    plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(3) << 7));
    plugin.getCPUState().SetBR(0, 0x1fe00e80ULL);
    plugin.getCPUState().SetGR(36, 0x3000);
    plugin.getCPUState().SetGR(37, 0x4000);
    plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    uint64_t loadedImage = 0;
    memory.Read(0x4000, reinterpret_cast<uint8_t*>(&loadedImage), sizeof(loadedImage));
    assert(plugin.getCPUState().GetIP() == 0x1a60);
    assert(plugin.getCPUState().GetBR(0) == 0x1a60);
    assert(plugin.getCPUState().GetGR(8) == 0);
    assert(loadedImage == 0x1fe00d00ULL);

    std::cout << "  ? HandleProtocol writes a minimal LoadedImageProtocol pointer and returns EFI_SUCCESS\n";
}

void testIA64PluginHandleProtocolReturnsSimpleFileSystem() {
    std::cout << "Testing IA-64 plugin HandleProtocol SimpleFileSystem stub...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1cc0, bundleBytes, sizeof(bundleBytes));
    const uint8_t simpleFileSystemGuid[16] = {
        0x22, 0x5B, 0x4E, 0x96, 0x59, 0x64, 0xD2, 0x11,
        0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
    };
    memory.Write(0x3000, simpleFileSystemGuid, sizeof(simpleFileSystemGuid));

    FakeIndirectSelfCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1cc0);
    plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(3) << 7));
    plugin.getCPUState().SetBR(0, 0x1fe00e80ULL);
    plugin.getCPUState().SetGR(36, 0x3000);
    plugin.getCPUState().SetGR(37, 0x4000);
    plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    uint64_t simpleFileSystem = 0;
    memory.Read(0x4000, reinterpret_cast<uint8_t*>(&simpleFileSystem), sizeof(simpleFileSystem));
    assert(plugin.getCPUState().GetIP() == 0x1cd0);
    assert(plugin.getCPUState().GetBR(0) == 0x1cd0);
    assert(plugin.getCPUState().GetGR(8) == 0);
    assert(simpleFileSystem == 0x1fe01000ULL);

    std::cout << "  ? HandleProtocol writes a minimal SimpleFileSystem pointer and returns EFI_SUCCESS\n";
}

void testIA64PluginHandleProtocolZeroGuidFallbacks() {
    std::cout << "Testing IA-64 plugin HandleProtocol zero-GUID boot fallbacks...\n";

    uint8_t bundleBytes[16] = {};
    uint8_t zeroGuid[16] = {};

    {
        SparseMemory memory;
        memory.Write(0x1a50, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x3000, zeroGuid, sizeof(zeroGuid));

        FakeIndirectSelfCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x1a50);
        plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(3) << 7));
        plugin.getCPUState().SetBR(0, 0x1fe00e80ULL);
        plugin.getCPUState().SetGR(36, 0x3000);
        plugin.getCPUState().SetGR(37, 0x4000);
        plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        uint64_t loadedImage = 0;
        memory.Read(0x4000, reinterpret_cast<uint8_t*>(&loadedImage), sizeof(loadedImage));
        assert(plugin.getCPUState().GetGR(8) == 0);
        assert(loadedImage == 0x1fe00d00ULL);
    }

    {
        SparseMemory memory;
        memory.Write(0x1cc0, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x3000, zeroGuid, sizeof(zeroGuid));

        FakeIndirectSelfCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x1cc0);
        plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(3) << 7));
        plugin.getCPUState().SetBR(0, 0x1fe00e80ULL);
        plugin.getCPUState().SetGR(36, 0x3000);
        plugin.getCPUState().SetGR(37, 0x4000);
        plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        uint64_t simpleFileSystem = 0;
        memory.Read(0x4000, reinterpret_cast<uint8_t*>(&simpleFileSystem), sizeof(simpleFileSystem));
        assert(plugin.getCPUState().GetGR(8) == 0);
        assert(simpleFileSystem == 0x1fe01000ULL);
    }

    std::cout << "  ? zeroed protocol GUIDs at known boot call sites map to the expected safe stubs\n";
}

void testIA64PluginOpenVolumeReturnsRootFileProtocol() {
    std::cout << "Testing IA-64 plugin SimpleFileSystem OpenVolume stub...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x30e40, bundleBytes, sizeof(bundleBytes));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x30e40);
    plugin.getCPUState().SetCFM(2);
    plugin.getCPUState().SetBR(6, 0x1fe00c80ULL);
    plugin.getCPUState().SetGR(33, 0x5000);
    plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    uint64_t rootFileProtocol = 0;
    memory.Read(0x5000, reinterpret_cast<uint8_t*>(&rootFileProtocol), sizeof(rootFileProtocol));
    assert(plugin.getCPUState().GetIP() == 0x30e50);
    assert(plugin.getCPUState().GetBR(0) == 0x30e50);
    assert(plugin.getCPUState().GetGR(8) == 0);
    assert(rootFileProtocol == 0x1fe01040ULL);

    std::cout << "  ? OpenVolume writes a root FileProtocol pointer and returns EFI_SUCCESS\n";
}

void testIA64PluginFileProtocolOpenNoMediaIsSafe() {
    std::cout << "Testing IA-64 plugin FileProtocol Open without backing media...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x30e40, bundleBytes, sizeof(bundleBytes));
    const uint16_t kernelPath[] = { '\\','E','F','I','\\','B','O','O','T','\\','B','O','O','T','I','A','6','4','.','E','F','I',0 };
    memory.Write(0x6000, reinterpret_cast<const uint8_t*>(kernelPath), sizeof(kernelPath));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x30e40);
    plugin.getCPUState().SetCFM(5);
    plugin.getCPUState().SetBR(6, 0x1fe01400ULL);
    plugin.getCPUState().SetGR(32, 0x1fe01040ULL);
    plugin.getCPUState().SetGR(33, 0x5000);
    plugin.getCPUState().SetGR(34, 0x6000);
    plugin.getCPUState().SetGR(35, 1);
    plugin.getCPUState().SetGR(36, 0);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    uint64_t newHandle = 0xffffffffffffffffULL;
    memory.Read(0x5000, reinterpret_cast<uint8_t*>(&newHandle), sizeof(newHandle));
    assert(plugin.getCPUState().GetIP() == 0x30e50);
    assert(plugin.getCPUState().GetBR(0) == 0x30e50);
    assert(plugin.getCPUState().GetGR(8) == 0x800000000000000cULL);
    assert(newHandle == 0);

    std::cout << "  ? FileProtocol.Open reports EFI_NO_MEDIA without destabilizing the boot stub path\n";
}

void testIA64PluginTextOutputStringReturnsSuccess() {
    std::cout << "Testing IA-64 plugin SimpleTextOut OutputString stub...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0x30e40, bundleBytes, sizeof(bundleBytes));
    const uint16_t message[] = { 'B', 'o', 'o', 't', 0 };
    memory.Write(0x6000, reinterpret_cast<const uint8_t*>(message), sizeof(message));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x30e40);
    plugin.getCPUState().SetCFM(2);
    plugin.getCPUState().SetBR(6, 0x1fe01280ULL);
    plugin.getCPUState().SetGR(32, 0x1fe01200ULL);
    plugin.getCPUState().SetGR(33, 0x6000);
    plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    assert(plugin.getCPUState().GetIP() == 0x30e50);
    assert(plugin.getCPUState().GetBR(0) == 0x30e50);
    assert(plugin.getCPUState().GetGR(8) == 0);

    std::cout << "  ? OutputString returns EFI_SUCCESS for safe console diagnostics\n";
}

void testIA64PluginAllocatePoolReturnsScratchBuffer() {
    std::cout << "Testing IA-64 plugin AllocatePool firmware stub...\n";

    SparseMemory memory;
    uint8_t bundleBytes[16] = {};
    memory.Write(0xa210, bundleBytes, sizeof(bundleBytes));

    FakeIndirectCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0xa210);
    plugin.getCPUState().SetCFM(6 | (static_cast<uint64_t>(3) << 7));
    plugin.getCPUState().SetBR(6, 0x1fe00e00ULL);
    plugin.getCPUState().SetGR(36, 0x30);
    plugin.getCPUState().SetGR(37, 0x5000);
    plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

    uint64_t buffer = 0;
    uint64_t firstBytes = 0xffffffffffffffffULL;
    memory.Read(0x5000, reinterpret_cast<uint8_t*>(&buffer), sizeof(buffer));
    memory.Read(buffer, reinterpret_cast<uint8_t*>(&firstBytes), sizeof(firstBytes));
    assert(plugin.getCPUState().GetIP() == 0xa220);
    assert(plugin.getCPUState().GetBR(0) == 0xa220);
    assert(plugin.getCPUState().GetGR(8) == 0);
    assert(buffer == 0x1fe02000ULL);
    assert(firstBytes == 0);

    std::cout << "  ? AllocatePool writes a zeroed scratch buffer pointer and returns EFI_SUCCESS\n";
}

void testIA64PluginGetVariableBootVariables() {
    std::cout << "Testing IA-64 plugin GetVariable firmware stub...\n";

    uint8_t bundleBytes[16] = {};
    const uint8_t globalVariableGuid[16] = {
        0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
        0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C
    };
    const uint16_t unknownName[] = { 'N', 'o', 'S', 'u', 'c', 'h', 0 };
    const uint16_t bootCurrentName[] = { 'B', 'o', 'o', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't', 0 };
    const uint16_t boot0000Name[] = { 'B', 'o', 'o', 't', '0', '0', '0', '0', 0 };
    const uint16_t platformLangName[] = { 'P', 'l', 'a', 't', 'f', 'o', 'r', 'm', 'L', 'a', 'n', 'g', 0 };
    const uint16_t oneCharLanguageProbeName[] = { 'P', 0 };

    {
        SparseMemory memory;
        memory.Write(0x2ee50, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x6000, reinterpret_cast<const uint8_t*>(unknownName), sizeof(unknownName));
        memory.Write(0x7000, globalVariableGuid, sizeof(globalVariableGuid));

        FakeIndirectCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x2ee50);
        plugin.getCPUState().SetCFM(5);
        plugin.getCPUState().SetBR(6, 0x1fe00d80ULL);
        plugin.getCPUState().SetGR(32, 0x6000);
        plugin.getCPUState().SetGR(33, 0x7000);
        plugin.getCPUState().SetGR(34, 0x9000);
        plugin.getCPUState().SetGR(35, 0x8000);
        plugin.getCPUState().SetGR(36, 0xA000);
        plugin.getCPUState().SetGR(8, 0);

        uint64_t size = 0x1234;
        memory.Write(0x8000, reinterpret_cast<const uint8_t*>(&size), sizeof(size));

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        memory.Read(0x8000, reinterpret_cast<uint8_t*>(&size), sizeof(size));
        assert(plugin.getCPUState().GetIP() == 0x2ee60);
        assert(plugin.getCPUState().GetBR(0) == 0x2ee60);
        assert(plugin.getCPUState().GetGR(8) == 0x800000000000000eULL);
        assert(size == 0);
    }

    {
        SparseMemory memory;
        memory.Write(0x2ee50, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x6000, reinterpret_cast<const uint8_t*>(bootCurrentName), sizeof(bootCurrentName));
        memory.Write(0x7000, globalVariableGuid, sizeof(globalVariableGuid));

        FakeIndirectCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x2ee50);
        plugin.getCPUState().SetCFM(5);
        plugin.getCPUState().SetBR(6, 0x1fe00d80ULL);
        plugin.getCPUState().SetGR(32, 0x6000);
        plugin.getCPUState().SetGR(33, 0x7000);
        plugin.getCPUState().SetGR(34, 0x9000);
        plugin.getCPUState().SetGR(35, 0x8000);
        plugin.getCPUState().SetGR(36, 0xA000);
        plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

        uint64_t size = 2;
        memory.Write(0x8000, reinterpret_cast<const uint8_t*>(&size), sizeof(size));

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        uint16_t bootCurrent = 0xffff;
        uint32_t attributes = 0;
        memory.Read(0x8000, reinterpret_cast<uint8_t*>(&size), sizeof(size));
        memory.Read(0x9000, reinterpret_cast<uint8_t*>(&attributes), sizeof(attributes));
        memory.Read(0xA000, reinterpret_cast<uint8_t*>(&bootCurrent), sizeof(bootCurrent));
        assert(plugin.getCPUState().GetGR(8) == 0);
        assert(size == 2);
        assert(attributes == 0x7U);
        assert(bootCurrent == 0);
    }

    {
        SparseMemory memory;
        memory.Write(0x2ee50, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x6000, reinterpret_cast<const uint8_t*>(boot0000Name), sizeof(boot0000Name));
        memory.Write(0x7000, globalVariableGuid, sizeof(globalVariableGuid));

        FakeIndirectCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x2ee50);
        plugin.getCPUState().SetCFM(5);
        plugin.getCPUState().SetBR(6, 0x1fe00d80ULL);
        plugin.getCPUState().SetGR(32, 0x6000);
        plugin.getCPUState().SetGR(33, 0x7000);
        plugin.getCPUState().SetGR(34, 0);
        plugin.getCPUState().SetGR(35, 0x8000);
        plugin.getCPUState().SetGR(36, 0);
        plugin.getCPUState().SetGR(8, 0);

        uint64_t size = 0;
        memory.Write(0x8000, reinterpret_cast<const uint8_t*>(&size), sizeof(size));

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        memory.Read(0x8000, reinterpret_cast<uint8_t*>(&size), sizeof(size));
        assert(plugin.getCPUState().GetGR(8) == 0x8000000000000005ULL);
        assert(size > 6);
    }

    {
        SparseMemory memory;
        memory.Write(0x2ee50, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x6000, reinterpret_cast<const uint8_t*>(platformLangName), sizeof(platformLangName));
        memory.Write(0x7000, globalVariableGuid, sizeof(globalVariableGuid));

        FakeIndirectCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x2ee50);
        plugin.getCPUState().SetCFM(5);
        plugin.getCPUState().SetBR(6, 0x1fe00d80ULL);
        plugin.getCPUState().SetGR(32, 0x6000);
        plugin.getCPUState().SetGR(33, 0x7000);
        plugin.getCPUState().SetGR(34, 0x9000);
        plugin.getCPUState().SetGR(35, 0x8000);
        plugin.getCPUState().SetGR(36, 0xA000);
        plugin.getCPUState().SetGR(8, 0xffffffffffffffffULL);

        uint64_t size = 6;
        memory.Write(0x8000, reinterpret_cast<const uint8_t*>(&size), sizeof(size));

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        char lang[6] = {};
        uint32_t attributes = 0;
        memory.Read(0x8000, reinterpret_cast<uint8_t*>(&size), sizeof(size));
        memory.Read(0x9000, reinterpret_cast<uint8_t*>(&attributes), sizeof(attributes));
        memory.Read(0xA000, reinterpret_cast<uint8_t*>(lang), sizeof(lang));
        assert(plugin.getCPUState().GetGR(8) == 0);
        assert(size == 6);
        assert(attributes == 0x7U);
        assert(std::strcmp(lang, "en-US") == 0);
    }

    {
        SparseMemory memory;
        memory.Write(0x1b00, bundleBytes, sizeof(bundleBytes));
        memory.Write(0x6000, reinterpret_cast<const uint8_t*>(oneCharLanguageProbeName), sizeof(oneCharLanguageProbeName));
        memory.Write(0x7000, globalVariableGuid, sizeof(globalVariableGuid));

        FakeIndirectCallDecoder decoder;
        IA64ISAPlugin plugin(decoder);
        plugin.getCPUState().SetIP(0x1b00);
        plugin.getCPUState().SetCFM(5);
        plugin.getCPUState().SetBR(6, 0x1fe00d80ULL);
        plugin.getCPUState().SetGR(32, 0x6000);
        plugin.getCPUState().SetGR(33, 0x7000);
        plugin.getCPUState().SetGR(34, 0);
        plugin.getCPUState().SetGR(35, 0x8000);
        plugin.getCPUState().SetGR(36, 0xA000);
        plugin.getCPUState().SetGR(8, 0);

        uint64_t size = 1;
        memory.Write(0x8000, reinterpret_cast<const uint8_t*>(&size), sizeof(size));

        assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);

        memory.Read(0x8000, reinterpret_cast<uint8_t*>(&size), sizeof(size));
        assert(plugin.getCPUState().GetGR(8) == 0x8000000000000005ULL);
        assert(size == 6);
    }

    std::cout << "  ? GetVariable handles unknown variables, boot variables, and language probes\n";
}

void testIA64PluginTopLevelReturnHalts() {
    std::cout << "Testing IA-64 plugin top-level EFI return handling...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1070, bundleBytes, sizeof(bundleBytes));

    FakeTopLevelReturnDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1070);
    plugin.getCPUState().SetBR(0, 0);

    assert(plugin.step(memory) == ISAExecutionResult::HALT);

    std::cout << "  ? br.ret b0 with zero return address halts cleanly\n";
}

void testIA64PluginOutOfBoundsLoadRecovery() {
    std::cout << "Testing IA-64 plugin out-of-bounds load recovery...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1000, bundleBytes, sizeof(bundleBytes));

    FakeOutOfBoundsLoadDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1000);
    plugin.getCPUState().SetGR(8, 0xffff00000004ULL);
    plugin.getCPUState().SetGR(14, 0xdeadbeefcafebabeULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetGR(14) == 0);
    assert(plugin.getCPUState().GetGR(8) == 8);
    assert(plugin.getCPUState().GetIP() == 0x1010);

    std::cout << "  ? failed ld8 clears the stale destination and contains post-increment base state\n";
}

void testIA64PluginOutOfBoundsProbeClearsBase() {
    std::cout << "Testing IA-64 plugin out-of-bounds probe load base recovery...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x2000, bundleBytes, sizeof(bundleBytes));

    FakeOutOfBoundsProbeLoadDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x2000);
    plugin.getCPUState().SetGR(14, 0x685421cd4c01b829ULL);
    plugin.getCPUState().SetGR(15, 0xdeadbeefULL);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetGR(15) == 0);
    assert(plugin.getCPUState().GetGR(14) == 0);
    assert(plugin.getCPUState().GetIP() == 0x2010);

    std::cout << "  ? failed probe load clears both destination and invalid base pointer\n";
}

void testIA64PluginIndirectSelfCallExecution() {
    std::cout << "Testing IA-64 plugin indirect self-call execution...\n";

    Memory memory(64 * 1024);
    uint8_t bundleBytes[16] = {};
    memory.Write(0x1a50, bundleBytes, sizeof(bundleBytes));

    FakeIndirectSelfCallDecoder decoder;
    IA64ISAPlugin plugin(decoder);
    plugin.getCPUState().SetIP(0x1a50);
    plugin.getCPUState().SetBR(0, 0x2400);

    assert(plugin.step(memory) == ISAExecutionResult::CONTINUE);
    assert(plugin.getCPUState().GetIP() == 0x2400);
    assert(plugin.getCPUState().GetBR(0) == 0x1a60);

    std::cout << "  ? plugin br.call b0 = b0 branches through old b0 and links return\n";
}

void testIA64CPUWrapperDelegatesToPluginState() {
    std::cout << "Testing IA-64 CPU wrapper delegates state to plugin...\n";

    Memory memory(64 * 1024);
    InstructionDecoder decoder;
    auto plugin = createIA64ISA(decoder);
    CPU cpu(memory, *plugin);

    cpu.setIP(0x1234);
    cpu.writeGR(1, 0x238000);
    cpu.writeGR(32, 0x1);
    cpu.writeGR(33, 0x200000);

    assert(cpu.isUsingISAPlugin());
    assert(plugin->getPC() == 0x1234);

    auto& ia64State = dynamic_cast<IA64ISAState&>(plugin->getState());
    assert(ia64State.getCPUState().GetIP() == 0x1234);
    assert(ia64State.getCPUState().GetGR(1) == 0x238000);
    assert(ia64State.getCPUState().GetGR(32) == 0x1);
    assert(ia64State.getCPUState().GetGR(33) == 0x200000);
    assert(cpu.getState().GetGR(33) == 0x200000);

    std::cout << "  ? CPU wrapper register/IP access reaches plugin state\n";
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

void testIA64MUnitBootLoadStoreDecode() {
    std::cout << "Testing IA-64 M-unit boot load/store decode...\n";

    InstructionDecoder decoder;

    const uint8_t bundleBytes[16] = {
        0x09, 0x98, 0x10, 0x1e, 0x10, 0x14, 0x20, 0x21,
        0x38, 0x20, 0x28, 0x00, 0x00, 0x00, 0x04, 0x00
    };

    Bundle bundle = decoder.DecodeBundleAt(bundleBytes, 0x34aa0);
    assert(bundle.instructions.size() >= 2);

    const InstructionEx& load0 = bundle.instructions[0];
    assert(load0.GetType() == InstructionType::LD4);
    assert(load0.GetDst() == 19);
    assert(load0.GetSrc1() == 15);
    assert(load0.HasImmediate());
    assert(static_cast<int64_t>(load0.GetImmediate()) == 4);
    assert(load0.GetDisassembly() == "ld4 r19 = [r15], 4");

    const InstructionEx& load1 = bundle.instructions[1];
    assert(load1.GetType() == InstructionType::LD4);
    assert(load1.GetDst() == 18);
    assert(load1.GetSrc1() == 14);
    assert(load1.HasImmediate());
    assert(static_cast<int64_t>(load1.GetImmediate()) == 4);
    assert(load1.GetDisassembly() == "ld4 r18 = [r14], 4");

    Memory memory(64 * 1024);
    CPUState cpu;
    const uint32_t value0 = 0x11223344U;
    const uint32_t value1 = 0xaabbccddU;
    memory.Write(0x100, reinterpret_cast<const uint8_t*>(&value0), sizeof(value0));
    memory.Write(0x200, reinterpret_cast<const uint8_t*>(&value1), sizeof(value1));
    cpu.SetGR(15, 0x100);
    cpu.SetGR(14, 0x200);

    load0.Execute(cpu, memory);
    load1.Execute(cpu, memory);
    assert(cpu.GetGR(19) == value0);
    assert(cpu.GetGR(18) == value1);
    assert(cpu.GetGR(15) == 0x104);
    assert(cpu.GetGR(14) == 0x204);

    InstructionEx store = decoder.DecodeSlot(0x8cc0942000ULL, UnitType::M_UNIT, 0x1930);
    assert(store.GetType() == InstructionType::ST8);
    assert(store.GetDst() == 9);
    assert(store.GetSrc1() == 33);
    assert(!store.HasImmediate());
    assert(store.GetDisassembly() == "st8 [r9] = r33");

    InstructionEx normalLoad = decoder.DecodeSlot(0x8080f00200ULL, UnitType::M_UNIT, 0x34ab0);
    assert(normalLoad.GetType() == InstructionType::LD4);
    assert(normalLoad.GetDst() == 8);
    assert(normalLoad.GetSrc1() == 15);
    assert(!normalLoad.HasImmediate());
    assert(normalLoad.GetDisassembly() == "ld4 r8 = [r15]");

    std::cout << "  ? boot M-unit rows distinguish normal stores, normal loads, and immediate-update loads\n";
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

        testIA64CountedLoopBranchExecution();
        std::cout << "\n";

        testIA64CallDecodedCountedLoopExecution();
        std::cout << "\n";

        testIA64BranchPredicateGroupSnapshot();
        std::cout << "\n";

        testIA64NonBranchPredicateGroupSnapshot();
        std::cout << "\n";

        testIA64AddImm22Decode();
        std::cout << "\n";

        testIA64AddlImm22SourceDecode();
        std::cout << "\n";

        testIA64ReturnBranchDecode();
        std::cout << "\n";

        testIA64IndirectCallDecode();
        std::cout << "\n";

        testIA64IndirectSelfCallDecode();
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

        testIA64LegacyCallDecodedCountedLoop();
        std::cout << "\n";

        testIA64LegacyIndirectBranchExecution();
        std::cout << "\n";

        testIA64LegacyIndirectCallExecution();
        std::cout << "\n";

        testIA64PluginCallOutputInputs();
        std::cout << "\n";

        testIA64PluginCallFrameRestoreSkipsStaleFrames();
        std::cout << "\n";

        testIA64PluginIndirectBranchExecution();
        std::cout << "\n";

        testIA64PluginEfiServiceThunkLinksReturn();
        std::cout << "\n";

        testIA64PluginEfiThunkReturnDoesNotPopOuterCallFrame();
        std::cout << "\n";

        testIA64PluginIndirectCallExecution();
        std::cout << "\n";

        testIA64PluginZeroFilledFirmwareCallReturnsSuccess();
        std::cout << "\n";

        testIA64PluginNullFirmwareCallReturnsSuccess();
        std::cout << "\n";

        testIA64PluginHandleProtocolReturnsLoadedImage();
        std::cout << "\n";

        testIA64PluginHandleProtocolReturnsSimpleFileSystem();
        std::cout << "\n";

        testIA64PluginHandleProtocolZeroGuidFallbacks();
        std::cout << "\n";

        testIA64PluginOpenVolumeReturnsRootFileProtocol();
        testIA64PluginFileProtocolOpenNoMediaIsSafe();
        std::cout << "\n";

        testIA64PluginTextOutputStringReturnsSuccess();
        std::cout << "\n";

        testIA64PluginAllocatePoolReturnsScratchBuffer();
        std::cout << "\n";

        testIA64PluginGetVariableBootVariables();
        std::cout << "\n";

        testIA64PluginTopLevelReturnHalts();
        std::cout << "\n";

        testIA64PluginOutOfBoundsLoadRecovery();
        std::cout << "\n";

        testIA64PluginOutOfBoundsProbeClearsBase();
        std::cout << "\n";

        testIA64PluginIndirectSelfCallExecution();
        std::cout << "\n";

        testIA64CPUWrapperDelegatesToPluginState();
        std::cout << "\n";

        testIA64MemoryPostIncrement();
        std::cout << "\n";

        testIA64MUnitBootLoadStoreDecode();
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
