#include "IA64ISAPlugin.h"
#include "SyscallDispatcher.h"
#include "Profiler.h"
#include "memory.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace ia64 {

// ============================================================================
// IA64ISAState Implementation
// ============================================================================

IA64ISAState::IA64ISAState()
    : cpuState_()
    , currentBundle_()
    , currentSlot_(0)
    , bundleValid_(false)
    , predicateGroupSnapshot_()
    , pendingInterrupts_()
    , interruptVectorBase_(0) {
    predicateGroupSnapshot_.fill(false);
    predicateGroupSnapshot_[0] = true;
}

IA64ISAState::IA64ISAState(const CPUState& cpuState)
    : cpuState_(cpuState)
    , currentBundle_()
    , currentSlot_(0)
    , bundleValid_(false)
    , predicateGroupSnapshot_()
    , pendingInterrupts_()
    , interruptVectorBase_(0) {
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        predicateGroupSnapshot_[i] = cpuState_.GetPR(i);
    }
}

std::unique_ptr<ISAState> IA64ISAState::clone() const {
    auto cloned = std::make_unique<IA64ISAState>(cpuState_);
    cloned->currentBundle_ = currentBundle_;
    cloned->currentSlot_ = currentSlot_;
    cloned->bundleValid_ = bundleValid_;
    cloned->predicateGroupSnapshot_ = predicateGroupSnapshot_;
    cloned->pendingInterrupts_ = pendingInterrupts_;
    cloned->interruptVectorBase_ = interruptVectorBase_;
    return cloned;
}

void IA64ISAState::serialize(uint8_t* buffer) const {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in serialize");
    }
    
    size_t offset = 0;
    
    // Serialize CPUState registers
    // General registers (128 * 8 bytes)
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        uint64_t val = cpuState_.GetGR(i);
        std::memcpy(buffer + offset, &val, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }
    
    // Predicate registers (64 * 1 byte, padded)
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        uint8_t val = cpuState_.GetPR(i) ? 1 : 0;
        buffer[offset++] = val;
    }
    
    // Branch registers (8 * 8 bytes)
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        uint64_t val = cpuState_.GetBR(i);
        std::memcpy(buffer + offset, &val, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }
    
    // Special registers
    uint64_t ip = cpuState_.GetIP();
    std::memcpy(buffer + offset, &ip, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    uint64_t cfm = cpuState_.GetCFM();
    std::memcpy(buffer + offset, &cfm, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    uint64_t psr = cpuState_.GetPSR();
    std::memcpy(buffer + offset, &psr, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Runtime state
    std::memcpy(buffer + offset, &currentSlot_, sizeof(size_t));
    offset += sizeof(size_t);
    
    uint8_t valid = bundleValid_ ? 1 : 0;
    buffer[offset++] = valid;

    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        buffer[offset++] = predicateGroupSnapshot_[i] ? 1 : 0;
    }
    
    std::memcpy(buffer + offset, &interruptVectorBase_, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Pending interrupts
    size_t numInterrupts = pendingInterrupts_.size();
    std::memcpy(buffer + offset, &numInterrupts, sizeof(size_t));
    offset += sizeof(size_t);
    
    if (numInterrupts > 0) {
        std::memcpy(buffer + offset, pendingInterrupts_.data(), numInterrupts);
        offset += numInterrupts;
    }
}

void IA64ISAState::deserialize(const uint8_t* buffer) {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in deserialize");
    }
    
    size_t offset = 0;
    
    // Deserialize CPUState registers
    // General registers
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        uint64_t val;
        std::memcpy(&val, buffer + offset, sizeof(uint64_t));
        cpuState_.SetGR(i, val);
        offset += sizeof(uint64_t);
    }
    
    // Predicate registers
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        uint8_t val = buffer[offset++];
        cpuState_.SetPR(i, val != 0);
    }
    
    // Branch registers
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        uint64_t val;
        std::memcpy(&val, buffer + offset, sizeof(uint64_t));
        cpuState_.SetBR(i, val);
        offset += sizeof(uint64_t);
    }
    
    // Special registers
    uint64_t ip;
    std::memcpy(&ip, buffer + offset, sizeof(uint64_t));
    cpuState_.SetIP(ip);
    offset += sizeof(uint64_t);
    
    uint64_t cfm;
    std::memcpy(&cfm, buffer + offset, sizeof(uint64_t));
    cpuState_.SetCFM(cfm);
    offset += sizeof(uint64_t);
    
    uint64_t psr;
    std::memcpy(&psr, buffer + offset, sizeof(uint64_t));
    cpuState_.SetPSR(psr);
    offset += sizeof(uint64_t);
    
    // Runtime state
    std::memcpy(&currentSlot_, buffer + offset, sizeof(size_t));
    offset += sizeof(size_t);
    
    uint8_t valid = buffer[offset++];
    bundleValid_ = (valid != 0);

    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        predicateGroupSnapshot_[i] = buffer[offset++] != 0;
    }
    
    std::memcpy(&interruptVectorBase_, buffer + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Pending interrupts
    size_t numInterrupts;
    std::memcpy(&numInterrupts, buffer + offset, sizeof(size_t));
    offset += sizeof(size_t);
    
    pendingInterrupts_.clear();
    if (numInterrupts > 0) {
        pendingInterrupts_.resize(numInterrupts);
        std::memcpy(pendingInterrupts_.data(), buffer + offset, numInterrupts);
        offset += numInterrupts;
    }
}

size_t IA64ISAState::getStateSize() const {
    size_t size = 0;
    
    // CPUState
    size += NUM_GENERAL_REGISTERS * sizeof(uint64_t);  // GRs
    size += NUM_PREDICATE_REGISTERS;                   // PRs (padded to bytes)
    size += NUM_BRANCH_REGISTERS * sizeof(uint64_t);   // BRs
    size += 3 * sizeof(uint64_t);                      // IP, CFM, PSR
    
    // Runtime state
    size += sizeof(size_t);      // currentSlot_
    size += 1;                   // bundleValid_
    size += NUM_PREDICATE_REGISTERS; // predicate group snapshot
    size += sizeof(uint64_t);    // interruptVectorBase_
    size += sizeof(size_t);      // pendingInterrupts_ count
    size += pendingInterrupts_.size();  // interrupt data
    
    return size;
}

std::string IA64ISAState::toString() const {
    std::stringstream ss;
    
    ss << "IA-64 CPU State:\n";
    ss << "  IP: 0x" << std::hex << cpuState_.GetIP() << std::dec << "\n";
    ss << "  CFM: 0x" << std::hex << cpuState_.GetCFM() << std::dec << "\n";
    ss << "  PSR: 0x" << std::hex << cpuState_.GetPSR() << std::dec << "\n";
    
    ss << "  General Registers:\n";
    for (size_t i = 0; i < 8; ++i) {
        ss << "    GR" << i << ": 0x" << std::hex << cpuState_.GetGR(i) << std::dec << "\n";
    }
    
    ss << "  Bundle State:\n";
    ss << "    Slot: " << currentSlot_ << "\n";
    ss << "    Valid: " << (bundleValid_ ? "yes" : "no") << "\n";
    
    ss << "  Interrupts:\n";
    ss << "    Pending: " << pendingInterrupts_.size() << "\n";
    ss << "    Vector Base: 0x" << std::hex << interruptVectorBase_ << std::dec << "\n";
    
    return ss.str();
}

void IA64ISAState::reset() {
    cpuState_.Reset();
    currentBundle_ = Bundle();
    currentSlot_ = 0;
    bundleValid_ = false;
    predicateGroupSnapshot_.fill(false);
    predicateGroupSnapshot_[0] = true;
    pendingInterrupts_.clear();
    interruptVectorBase_ = 0;
}

// ============================================================================
// IA64ISAPlugin Implementation
// ============================================================================

IA64ISAPlugin::IA64ISAPlugin(IDecoder& decoder)
    : decoder_(decoder)
    , state_()
    , syscallDispatcher_(nullptr)
    , profiler_(nullptr)
    , cachedInstruction_()
    , hasCachedInstruction_(false)
    , pendingCallInputs_()
    , callFrameStack_() {
}

IA64ISAPlugin::IA64ISAPlugin(IDecoder& decoder, 
                             SyscallDispatcher* syscallDispatcher,
                             Profiler* profiler)
    : decoder_(decoder)
    , state_()
    , syscallDispatcher_(syscallDispatcher)
    , profiler_(profiler)
    , cachedInstruction_()
    , hasCachedInstruction_(false)
    , pendingCallInputs_()
    , callFrameStack_() {
}

void IA64ISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
    pendingCallInputs_.clear();
    callFrameStack_.clear();
}

ISADecodeResult IA64ISAPlugin::decode(IMemory& memory) {
    ISADecodeResult result;
    
    try {
        // Fetch bundle if needed
        if (!state_.bundleValid_ || state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
            fetchBundle(memory);
            state_.currentSlot_ = 0;
        }
        
        // Check if we have instructions
        if (state_.currentBundle_.instructions.empty()) {
            result.valid = false;
            result.instructionAddress = state_.getCPUState().GetIP();
            result.instructionLength = 0;
            result.disassembly = "<invalid bundle>";
            result.internalData = nullptr;
            return result;
        }
        
        // Get current instruction from bundle
        const InstructionEx& instr = state_.currentBundle_.instructions[state_.currentSlot_];
        
        // Cache the instruction for execute()
        cachedInstruction_ = instr;
        hasCachedInstruction_ = true;
        
        // Fill in result
        result.valid = true;
        result.instructionAddress = state_.getCPUState().GetIP();
        result.instructionLength = 16;  // IA-64 bundles are 16 bytes (we advance by bundle)
        result.disassembly = instr.GetDisassembly();
        result.internalData = &cachedInstruction_;
        
    } catch (const std::exception& e) {
        result.valid = false;
        result.instructionAddress = state_.getCPUState().GetIP();
        result.instructionLength = 0;
        result.disassembly = std::string("<decode error: ") + e.what() + ">";
        result.internalData = nullptr;
    }
    
    return result;
}

size_t IA64ISAPlugin::getInstructionLength(IMemory& memory, uint64_t address) {
    // IA-64 bundles are always 16 bytes (128 bits)
    // Individual instructions within bundles are 41 bits each
    // For this interface, we return the bundle size
    return 16;
}

ISAExecutionResult IA64ISAPlugin::execute(IMemory& memory, const ISADecodeResult& decodeResult) {
    if (!decodeResult.valid) {
        return ISAExecutionResult::EXCEPTION;
    }
    
    if (!hasCachedInstruction_) {
        std::cerr << "Warning: execute() called without prior decode()\n";
        return ISAExecutionResult::EXCEPTION;
    }
    
    try {
        // Profile instruction execution
        if (isProfilingEnabled()) {
            profiler_->recordInstructionExecution(
                decodeResult.instructionAddress, 
                decodeResult.disassembly
            );
        }
        
        // Log instruction execution
        std::cout << "[IP=0x" << std::hex << decodeResult.instructionAddress << std::dec 
                  << ", Slot=" << state_.currentSlot_ << "] "
                  << decodeResult.disassembly << std::endl;

        if (cachedInstruction_.GetType() == InstructionType::UNKNOWN) {
            const uint64_t rawBits = cachedInstruction_.GetRawBits();
            std::cerr << "Unsupported IA-64 instruction: "
                      << "bundle=0x" << std::hex << decodeResult.instructionAddress
                      << " slot=" << std::dec << state_.currentSlot_
                      << " template=0x" << std::hex
                      << static_cast<int>(state_.currentBundle_.templateType)
                      << " raw=0x" << rawBits
                      << " opcode=0x" << ((rawBits >> 37) & 0x0F)
                      << " qp=" << std::dec << (rawBits & 0x3F)
                      << " x3=" << ((rawBits >> 33) & 0x07)
                      << " x6=" << ((rawBits >> 27) & 0x3F)
                      << " disasm=\"" << decodeResult.disassembly << "\"\n";
            hasCachedInstruction_ = false;
            return ISAExecutionResult::EXCEPTION;
        }
        
        // Check for syscall
        if (cachedInstruction_.GetType() == InstructionType::BREAK &&
            cachedInstruction_.HasImmediate() &&
            cachedInstruction_.GetImmediate() == 0x100000) {
            
            if (syscallDispatcher_) {
                syscallDispatcher_->DispatchSyscall(state_.getCPUState(), memory);
            }
            
            // Advance to next instruction
            state_.currentSlot_++;
            if (state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
                state_.getCPUState().SetIP(state_.getCPUState().GetIP() + 16);
                state_.bundleValid_ = false;
            }
            
            hasCachedInstruction_ = false;
            return ISAExecutionResult::SYSCALL;
        }
        
        // Check for branch instructions
        bool isBranch = false;
        uint64_t branchTarget = 0;
        const uint8_t predicate = cachedInstruction_.GetPredicate();
        const bool snapshotPredicateTrue = (predicate == 0) || state_.predicateGroupSnapshot_[predicate];
        const bool livePredicateTrue = (predicate == 0) || state_.getCPUState().GetPR(predicate);
        const bool branchInstruction =
            cachedInstruction_.GetType() == InstructionType::BR_COND ||
            cachedInstruction_.GetType() == InstructionType::BR_CALL ||
            cachedInstruction_.GetType() == InstructionType::BR_RET;
        switch (cachedInstruction_.GetType()) {
            case InstructionType::BR_COND:
                if (livePredicateTrue && cachedInstruction_.HasBranchTarget()) {
                    branchTarget = cachedInstruction_.GetBranchTarget();
                    isBranch = true;
                }
                break;
                
            case InstructionType::BR_CALL:
                if (livePredicateTrue && cachedInstruction_.HasBranchTarget()) {
                    branchTarget = cachedInstruction_.GetBranchTarget();
                    isBranch = true;
                    saveCallFrame();
                    captureCallOutputRegisters();
                }
                break;
                
            case InstructionType::BR_RET:
                if (livePredicateTrue) {
                    branchTarget = state_.getCPUState().GetBR(cachedInstruction_.GetSrc1());
                    isBranch = true;
                }
                break;
                
            default:
                break;
        }
        
        // Keep non-branch predicate execution on the instruction-group snapshot,
        // while branches use the current predicate state for boot-critical flow.
        if ((branchInstruction && livePredicateTrue) ||
            (!branchInstruction && snapshotPredicateTrue)) {
            executeInstruction(memory, cachedInstruction_, true);
            if (cachedInstruction_.GetType() == InstructionType::ALLOC) {
                applyPendingCallInputRegisters();
            }
        }

        // Handle branch after execution
        if (isBranch) {
            if (cachedInstruction_.GetType() == InstructionType::BR_RET && livePredicateTrue) {
                restoreCallFrame();
            }
            state_.getCPUState().SetIP(branchTarget);
            state_.bundleValid_ = false;
            state_.currentSlot_ = 0;
            capturePredicateGroupSnapshot();
            hasCachedInstruction_ = false;
            return ISAExecutionResult::CONTINUE;
        }
        
        // Advance to next instruction (non-branch)
        const size_t executedSlot = state_.currentSlot_;
        state_.currentSlot_++;
        if (executedSlot < state_.currentBundle_.stopAfterSlot.size() &&
            state_.currentBundle_.stopAfterSlot[executedSlot]) {
            capturePredicateGroupSnapshot();
        }
        if (state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
            state_.getCPUState().SetIP(state_.getCPUState().GetIP() + 16);
            state_.bundleValid_ = false;
            capturePredicateGroupSnapshot();
        }
        
        hasCachedInstruction_ = false;
        return ISAExecutionResult::CONTINUE;
        
    } catch (const std::exception& e) {
        std::cerr << "Execution error: " << e.what() << "\n";
        hasCachedInstruction_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
}

ISAExecutionResult IA64ISAPlugin::step(IMemory& memory) {
    // Service interrupts first
    servicePendingInterrupt(memory);
    
    // Decode and execute
    auto decodeResult = decode(memory);
    if (!decodeResult.valid) {
        return ISAExecutionResult::EXCEPTION;
    }
    
    return execute(memory, decodeResult);
}

void IA64ISAPlugin::setState(const ISAState& state) {
    const IA64ISAState* ia64State = dynamic_cast<const IA64ISAState*>(&state);
    if (!ia64State) {
        throw std::invalid_argument("State is not IA64ISAState");
    }
    
    state_ = *ia64State;
    hasCachedInstruction_ = false;
    pendingCallInputs_.clear();
    callFrameStack_.clear();
}

std::vector<uint8_t> IA64ISAPlugin::serialize_state() const {
    size_t stateSize = state_.getStateSize();
    std::vector<uint8_t> data(stateSize);
    state_.serialize(data.data());
    return data;
}

bool IA64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    try {
        if (data.size() < state_.getStateSize()) {
            return false;
        }
        
        state_.deserialize(data.data());
        hasCachedInstruction_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

std::string IA64ISAPlugin::dumpState() const {
    return state_.toString();
}

std::string IA64ISAPlugin::disassemble(IMemory& memory, uint64_t address) {
    try {
        uint8_t bundleData[16];
        memory.Read(address, bundleData, 16);
        
        Bundle bundle = decoder_.DecodeBundleAt(bundleData, address);
        
        std::stringstream ss;
        ss << "Bundle at 0x" << std::hex << address << std::dec << ":\n";
        for (size_t i = 0; i < bundle.instructions.size(); ++i) {
            ss << "  [" << i << "] " << bundle.instructions[i].GetDisassembly() << "\n";
        }
        
        return ss.str();
    } catch (const std::exception& e) {
        return std::string("<disassembly error: ") + e.what() + ">";
    }
}

bool IA64ISAPlugin::hasFeature(const std::string& feature) const {
    // IA-64 features
    if (feature == "predication") return true;
    if (feature == "register_rotation") return true;
    if (feature == "register_stack") return true;
    if (feature == "bundles") return true;
    if (feature == "epic") return true;
    if (feature == "speculation") return false;  // Not yet implemented
    if (feature == "advanced_loads") return false;  // Not yet implemented
    
    return false;
}

// ============================================================================
// IA-64 Specific Methods
// ============================================================================

uint64_t IA64ISAPlugin::readGR(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    
    // Apply rotation for stacked registers (GR32-GR127)
    size_t physical = applyRegisterRotation(index, 'G');
    return state_.getCPUState().GetGR(physical);
}

void IA64ISAPlugin::writeGR(size_t index, uint64_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    
    // GR0 is hardwired to zero
    if (index == 0) {
        return;
    }
    
    size_t physical = applyRegisterRotation(index, 'G');
    state_.getCPUState().SetGR(physical, value);
}

bool IA64ISAPlugin::readPR(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    
    size_t physical = applyRegisterRotation(index, 'P');
    return state_.getCPUState().GetPR(physical);
}

void IA64ISAPlugin::writePR(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    
    // PR0 is hardwired to true
    if (index == 0) {
        return;
    }
    
    size_t physical = applyRegisterRotation(index, 'P');
    state_.getCPUState().SetPR(physical, value);
}

uint64_t IA64ISAPlugin::readBR(size_t index) const {
    return state_.getCPUState().GetBR(index);
}

void IA64ISAPlugin::writeBR(size_t index, uint64_t value) {
    state_.getCPUState().SetBR(index, value);
}

uint64_t IA64ISAPlugin::getCFM() const {
    return state_.getCPUState().GetCFM();
}

void IA64ISAPlugin::setCFM(uint64_t value) {
    state_.getCPUState().SetCFM(value);
}

void IA64ISAPlugin::queueInterrupt(uint8_t vector) {
    state_.pendingInterrupts_.push_back(vector);
}

bool IA64ISAPlugin::hasPendingInterrupt() const {
    return !state_.pendingInterrupts_.empty();
}

void IA64ISAPlugin::setInterruptsEnabled(bool enabled) {
    // Set/clear interrupt enable bit in PSR
    uint64_t psr = state_.getCPUState().GetPSR();
    if (enabled) {
        psr |= (1ULL << 14);  // PSR.i bit
    } else {
        psr &= ~(1ULL << 14);
    }
    state_.getCPUState().SetPSR(psr);
}

bool IA64ISAPlugin::areInterruptsEnabled() const {
    uint64_t psr = state_.getCPUState().GetPSR();
    return (psr & (1ULL << 14)) != 0;  // PSR.i bit
}

void IA64ISAPlugin::setInterruptVectorBase(uint64_t baseAddress) {
    state_.interruptVectorBase_ = baseAddress;
}

uint64_t IA64ISAPlugin::getInterruptVectorBase() const {
    return state_.interruptVectorBase_;
}

bool IA64ISAPlugin::isAtBundleBoundary() const {
    return state_.currentSlot_ == 0 && !state_.bundleValid_;
}

size_t IA64ISAPlugin::getCurrentSlot() const {
    return state_.currentSlot_;
}

bool IA64ISAPlugin::isProfilingEnabled() const {
    return profiler_ != nullptr && profiler_->isEnabled();
}

// ============================================================================
// Internal Helper Methods
// ============================================================================

void IA64ISAPlugin::fetchBundle(IMemory& memory) {
    uint64_t ip = state_.getCPUState().GetIP();
    uint8_t bundleData[16];
    
    try {
        memory.Read(ip, bundleData, 16);
        state_.currentBundle_ = decoder_.DecodeBundleAt(bundleData, ip);
        state_.bundleValid_ = true;
        capturePredicateGroupSnapshot();
    } catch (const std::exception& e) {
        std::cerr << "Bundle fetch error at IP 0x" << std::hex << ip << std::dec 
                  << ": " << e.what() << "\n";
        state_.bundleValid_ = false;
    }
}

void IA64ISAPlugin::capturePredicateGroupSnapshot() {
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        state_.predicateGroupSnapshot_[i] = state_.getCPUState().GetPR(i);
    }
}

void IA64ISAPlugin::captureCallOutputRegisters() {
    pendingCallInputs_.clear();

    const uint8_t sof = state_.getCPUState().GetSOF();
    const uint8_t sol = state_.getCPUState().GetSOL();
    if (sof <= sol) {
        return;
    }

    const size_t outputCount = static_cast<size_t>(sof - sol);
    const size_t firstOutput = 32 + sol;
    for (size_t i = 0; i < outputCount && firstOutput + i < NUM_GENERAL_REGISTERS; ++i) {
        const uint64_t value = state_.getCPUState().GetGR(firstOutput + i);
        pendingCallInputs_.push_back(value);
        state_.getCPUState().SetGR(32 + i, value);
    }
}

void IA64ISAPlugin::applyPendingCallInputRegisters() {
    if (pendingCallInputs_.empty()) {
        return;
    }

    for (size_t i = 0; i < pendingCallInputs_.size() && 32 + i < NUM_GENERAL_REGISTERS; ++i) {
        state_.getCPUState().SetGR(32 + i, pendingCallInputs_[i]);
    }

    pendingCallInputs_.clear();
}

void IA64ISAPlugin::saveCallFrame() {
    CallFrameSnapshot frame{};
    frame.cfm = state_.getCPUState().GetCFM();
    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        frame.stackedRegisters[i] = state_.getCPUState().GetGR(NUM_STATIC_GR + i);
    }
    callFrameStack_.push_back(frame);
}

void IA64ISAPlugin::restoreCallFrame() {
    if (callFrameStack_.empty()) {
        return;
    }

    const CallFrameSnapshot frame = callFrameStack_.back();
    callFrameStack_.pop_back();

    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        state_.getCPUState().SetGR(NUM_STATIC_GR + i, frame.stackedRegisters[i]);
    }
    state_.getCPUState().SetCFM(frame.cfm);
}

void IA64ISAPlugin::executeInstruction(IMemory& memory, const InstructionEx& instr, bool ignorePredicate) {
    try {
        instr.Execute(state_.getCPUState(), memory, ignorePredicate);
    } catch (const std::exception& e) {
        std::cerr << "Error executing instruction: " << e.what() << "\n";
        std::cerr << "Treating as NOP and continuing...\n";
    }
}

size_t IA64ISAPlugin::applyRegisterRotation(size_t logicalReg, char regType) const {
    switch (regType) {
        case 'G':  // General register
            if (logicalReg < 32) {
                return logicalReg;
            } else {
                uint8_t rrb = getGRRotationBase();
                size_t rotatingSize = 96;
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'F':  // Floating-point register
            if (logicalReg < 32) {
                return logicalReg;
            } else {
                uint8_t rrb = getFRRotationBase();
                size_t rotatingSize = 96;
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'P':  // Predicate register
            if (logicalReg < 16) {
                return logicalReg;
            } else {
                uint8_t rrb = getPRRotationBase();
                size_t rotatingSize = 48;
                size_t offset = (logicalReg - 16 + rrb) % rotatingSize;
                return 16 + offset;
            }
            
        default:
            return logicalReg;
    }
}

bool IA64ISAPlugin::checkPredicate(size_t predicateReg) const {
    if (predicateReg >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register out of range");
    }
    
    size_t physical = applyRegisterRotation(predicateReg, 'P');
    return state_.getCPUState().GetPR(physical);
}

void IA64ISAPlugin::servicePendingInterrupt(IMemory& memory) {
    if (!hasPendingInterrupt() || !areInterruptsEnabled()) {
        return;
    }
    
    // Get the first pending interrupt
    uint8_t vector = state_.pendingInterrupts_.front();
    state_.pendingInterrupts_.erase(state_.pendingInterrupts_.begin());
    
    // Calculate interrupt handler address
    uint64_t handlerAddress = state_.interruptVectorBase_ + (vector * 16);
    
    // In a real implementation, we would:
    // 1. Save current IP to IIP (Interruption Instruction Pointer)
    // 2. Save PSR to IPSR (Interruption PSR)
    // 3. Set IP to handler address
    // 4. Disable interrupts
    // 5. Switch to privileged mode
    
    std::cout << "Servicing interrupt vector " << static_cast<int>(vector)
              << " at handler 0x" << std::hex << handlerAddress << std::dec << "\n";
    
    // For now, just jump to the handler
    state_.getCPUState().SetIP(handlerAddress);
    state_.bundleValid_ = false;
}

uint8_t IA64ISAPlugin::getGRRotationBase() const {
    return state_.getCPUState().GetRRB_GR();
}

uint8_t IA64ISAPlugin::getFRRotationBase() const {
    return state_.getCPUState().GetRRB_FR();
}

uint8_t IA64ISAPlugin::getPRRotationBase() const {
    return state_.getCPUState().GetRRB_PR();
}

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder) {
    return std::make_unique<IA64ISAPlugin>(decoder);
}

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder,
                                     SyscallDispatcher* syscallDispatcher,
                                     Profiler* profiler) {
    return std::make_unique<IA64ISAPlugin>(decoder, syscallDispatcher, profiler);
}

} // namespace ia64
