#pragma once

#include "IISA.h"
#include "cpu_state.h"
#include "decoder.h"
#include "IMemory.h"
#include "IDecoder.h"
#include "FATParser.h"
#include <memory>
#include <cstring>
#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace ia64 {

// Forward declarations
class SyscallDispatcher;
class Profiler;

/**
 * IA64ISAState - IA-64 specific architectural state
 * 
 * Wraps CPUState and additional IA-64 runtime state into the ISAState interface.
 */
class IA64ISAState : public ISAState {
public:
    IA64ISAState();
    explicit IA64ISAState(const CPUState& cpuState);
    ~IA64ISAState() override = default;
    
    std::unique_ptr<ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override;
    std::string toString() const override;
    void reset() override;
    
    // Access to underlying CPUState
    CPUState& getCPUState() { return cpuState_; }
    const CPUState& getCPUState() const { return cpuState_; }
    
    // Bundle execution state
    Bundle currentBundle_;
    size_t currentSlot_;
    bool bundleValid_;
    std::array<bool, NUM_PREDICATE_REGISTERS> predicateGroupSnapshot_;
    
    // Interrupt state
    std::vector<uint8_t> pendingInterrupts_;
    uint64_t interruptVectorBase_;
    
private:
    CPUState cpuState_;
};

/**
 * IA64ISAPlugin - IA-64 ISA implementation as a plugin
 * 
 * This class wraps the existing IA-64 CPU implementation behind the IISA interface,
 * enabling it to work within the generic ISA plugin architecture.
 * 
 * Design:
 * - Delegates to existing IDecoder for instruction decoding
 * - Implements instruction execution using existing InstructionEx framework
 * - Maintains IA-64 specific state (registers, bundles, interrupts)
 * - Provides serialization for VM checkpointing
 * 
 * IA-64 Specific Features:
 * - Bundle-based execution (3 instructions per 128-bit bundle)
 * - Predicated execution support
 * - Register rotation and register stack engine
 * - EPIC parallel instruction semantics
 */
class IA64ISAPlugin : public IISA {
public:
    /**
     * Constructor
     * @param decoder IA-64 instruction decoder
     */
    explicit IA64ISAPlugin(IDecoder& decoder);
    
    /**
     * Constructor with optional components
     * @param decoder IA-64 instruction decoder
     * @param syscallDispatcher Optional syscall dispatcher
     * @param profiler Optional profiler
     */
    IA64ISAPlugin(IDecoder& decoder, SyscallDispatcher* syscallDispatcher, Profiler* profiler);
    
    ~IA64ISAPlugin() override = default;
    
    // ========================================================================
    // IISA Interface Implementation
    // ========================================================================
    
    std::string getName() const override { return "IA-64"; }
    std::string getVersion() const override { return "1.0"; }
    
    void reset() override;
    
    ISADecodeResult decode(IMemory& memory) override;
    size_t getInstructionLength(IMemory& memory, uint64_t address) override;
    
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& decodeResult) override;
    ISAExecutionResult step(IMemory& memory) override;
    
    ISAState& getState() override { return state_; }
    const ISAState& getState() const override { return state_; }
    void setState(const ISAState& state) override;
    
    uint64_t getPC() const override { return state_.getCPUState().GetIP(); }
    void setPC(uint64_t pc) override { state_.getCPUState().SetIP(pc); }
    
    std::vector<uint8_t> serialize_state() const override;
    bool deserialize_state(const std::vector<uint8_t>& data) override;
    
    std::string dumpState() const override;
    std::string disassemble(IMemory& memory, uint64_t address) override;
    
    size_t getWordSize() const override { return 64; }
    bool isLittleEndian() const override { return true; }
    bool hasFeature(const std::string& feature) const override;
    
    // ========================================================================
    // IA-64 Specific Extensions
    // ========================================================================
    
    /**
     * Get reference to underlying CPUState for IA-64 specific operations
     * This provides access to IA-64 specific registers and state.
     */
    CPUState& getCPUState() { return state_.getCPUState(); }
    const CPUState& getCPUState() const { return state_.getCPUState(); }
    
    /**
     * Register access (IA-64 specific)
     */
    uint64_t readGR(size_t index) const;
    void writeGR(size_t index, uint64_t value);
    
    bool readPR(size_t index) const;
    void writePR(size_t index, bool value);
    
    uint64_t readBR(size_t index) const;
    void writeBR(size_t index, uint64_t value);
    
    uint64_t getCFM() const;
    void setCFM(uint64_t value);
    
    /**
     * Interrupt management (IA-64 specific)
     */
    void queueInterrupt(uint8_t vector);
    bool hasPendingInterrupt() const;
    void setInterruptsEnabled(bool enabled);
    bool areInterruptsEnabled() const;
    void setInterruptVectorBase(uint64_t baseAddress);
    uint64_t getInterruptVectorBase() const;
    
    /**
     * Bundle execution state (IA-64 specific)
     */
    bool isAtBundleBoundary() const;
    size_t getCurrentSlot() const;
    
    /**
     * Set optional components
     */
    void setSyscallDispatcher(SyscallDispatcher* dispatcher) { 
        syscallDispatcher_ = dispatcher; 
    }
    
    void setProfiler(Profiler* profiler) { 
        profiler_ = profiler; 
    }
    
    /**
     * Get optional components
     */
    SyscallDispatcher* getSyscallDispatcher() const { 
        return syscallDispatcher_; 
    }
    
    Profiler* getProfiler() const { 
        return profiler_; 
    }
    
    bool isProfilingEnabled() const;
    
private:
    // ========================================================================
    // Internal Helper Methods
    // ========================================================================
    
    /**
     * Fetch and decode current bundle from memory
     */
    void fetchBundle(IMemory& memory);
    void capturePredicateGroupSnapshot();
    void captureCallOutputRegisters();
    void applyPendingCallInputRegisters();
    void saveCallFrame(uint64_t returnAddress);
    void restoreCallFrame(uint64_t branchTarget);
    void rememberBranchTarget(uint64_t target);
    void logEfiServiceCall(IMemory& memory,
                           const char* serviceName,
                           uint64_t callerIP,
                           uint64_t descriptorAddress,
                           uint64_t codePointer,
                           uint64_t status);
    void recordRecentInstruction(uint64_t ip, size_t slot, const std::string& disasm);
    void recordTrackedRegisterWrite(size_t reg, uint64_t value, uint64_t ip, size_t slot, const std::string& disasm);
    void dumpRecentFaultContext(const CPUState& cpu, uint64_t ip, size_t slot, const InstructionEx& instr, uint64_t baseBefore) const;
    bool ensureEfiBootFat(IMemory& memory);
    void installFileProtocolTable(IMemory& memory, uint64_t protocolAddress);
    uint64_t allocateEfiPool(IMemory& memory, uint64_t size, uint64_t alignment = 16);
    uint64_t handleEfiFileOpen(IMemory& memory);
    uint64_t handleEfiFileRead(IMemory& memory);
    uint64_t handleEfiFileGetInfo(IMemory& memory);
    uint64_t handleEfiFileClose(IMemory& memory);
    uint64_t handleEfiFileGetPosition(IMemory& memory);
    uint64_t handleEfiFileSetPosition(IMemory& memory);
    uint64_t handleEfiLocateHandle(IMemory& memory);
    uint64_t handleEfiLocateProtocol(IMemory& memory);
    uint64_t handleEfiGetMemoryMap(IMemory& memory);
    
    /**
     * Execute a single IA-64 instruction
     */
    void executeInstruction(IMemory& memory, const InstructionEx& instr, bool ignorePredicate = false);
    
    /**
     * Apply register rotation for IA-64 stacked registers
     */
    size_t applyRegisterRotation(size_t logicalReg, char regType) const;
    
    /**
     * Check predicate register value (with rotation)
     */
    bool checkPredicate(size_t predicateReg) const;
    
    /**
     * Service pending interrupts
     */
    void servicePendingInterrupt(IMemory& memory);
    
    /**
     * Extract rotation bases from CFM
     */
    uint8_t getGRRotationBase() const;
    uint8_t getFRRotationBase() const;
    uint8_t getPRRotationBase() const;
    
    // ========================================================================
    // Member Variables
    // ========================================================================
    
    IDecoder& decoder_;
    IA64ISAState state_;
    
    // Optional components
    SyscallDispatcher* syscallDispatcher_;
    Profiler* profiler_;
    
    // Cached decoded instruction for execute()
    InstructionEx cachedInstruction_;
    bool hasCachedInstruction_;
    std::vector<uint64_t> pendingCallInputs_;
    uint64_t efiPoolNext_;
    size_t efiTextOutputCalls_;
    size_t efiTextOutputMirrored_;
    size_t efiTextOutputFramebuffer_;
    size_t efiOpenVolumeCalls_;
    size_t efiSimpleFsProtocolReturns_;
    size_t efiGenericSuccessCalls_;
    size_t efiGenericUnsupportedCalls_;
    size_t efiZeroGuidProtocolCalls_;
    size_t efiHandleProtocolCalls_;
    size_t efiLocateHandleCalls_;
    size_t efiLocateProtocolCalls_;
    size_t efiGetMemoryMapCalls_;
    size_t efiExitBootServicesCalls_;
    size_t efiAllocatePoolCalls_;
    size_t efiFreePoolCalls_;
    size_t efiLoadImageCalls_;
    size_t efiStartImageCalls_;
    size_t efiFileOpenCalls_;
    size_t efiFileReadCalls_;
    size_t efiFileGetInfoCalls_;
    size_t efiFileCloseCalls_;
    size_t efiFileSetPositionCalls_;
    size_t efiFileGetPositionCalls_;
    size_t efiFirstSuccessfulFileOpen_;
    uint64_t efiTotalFileBytesRead_;
    size_t descriptorCallCount_;
    size_t gpSwitchCount_;
    size_t suspiciousGpCount_;
    size_t unknownRegionCallCount_;
    size_t recoveredLoadStoreCount_;
    std::string lastEfiCallName_;
    uint64_t lastEfiCallIP_;
    uint64_t lastDescriptorAddress_;
    uint64_t lastDescriptorCode_;
    uint64_t lastDescriptorGp_;
    std::vector<uint64_t> lastBranchTargets_;
    struct RecentInstructionTrace {
        uint64_t ip = 0;
        size_t slot = 0;
        std::string disasm;
    };
    struct RecentRegisterWriteTrace {
        size_t reg = 0;
        uint64_t value = 0;
        uint64_t ip = 0;
        size_t slot = 0;
        std::string disasm;
    };
    std::deque<RecentInstructionTrace> recentInstructions_;
    std::deque<RecentRegisterWriteTrace> recentTrackedRegisterWrites_;

    struct EfiFileHandle {
        uint64_t protocolAddress = 0;
        std::string path;
        bool isDirectory = false;
        std::vector<uint8_t> data;
        std::vector<guideXOS::FATFileInfo> directoryEntries;
        size_t directoryIndex = 0;
        uint64_t position = 0;
    };
    std::map<uint64_t, EfiFileHandle> efiFileHandles_;
    std::vector<uint8_t> efiBootImage_;
    std::unique_ptr<guideXOS::FATParser> efiBootFat_;

    struct CallFrameSnapshot {
        std::array<uint64_t, NUM_GENERAL_REGISTERS - NUM_STATIC_GR> stackedRegisters;
        uint64_t cfm;
        uint64_t returnAddress;
    };
    std::vector<CallFrameSnapshot> callFrameStack_;
};

/**
 * Factory function for creating IA-64 ISA instances
 */
std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder);

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder, 
                                     SyscallDispatcher* syscallDispatcher,
                                     Profiler* profiler);

} // namespace ia64
