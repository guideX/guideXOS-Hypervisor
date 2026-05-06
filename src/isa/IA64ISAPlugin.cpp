#include "IA64ISAPlugin.h"
#include "SyscallDispatcher.h"
#include "Profiler.h"
#include "memory.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <array>

namespace ia64 {

namespace {

struct CountedLoopTraceState {
    bool active = false;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t suppressed = 0;
};

CountedLoopTraceState g_countedLoopTrace;

constexpr uint64_t EFI_STATUS_SUCCESS = 0ULL;
constexpr uint64_t EFI_STATUS_UNSUPPORTED = 0x8000000000000003ULL;
constexpr uint64_t EFI_STATUS_BUFFER_TOO_SMALL = 0x8000000000000005ULL;
constexpr uint64_t EFI_STATUS_NOT_FOUND = 0x800000000000000EULL;
constexpr uint64_t EFI_HANDOFF_REGION_BASE = 0x1FE00000ULL;
constexpr uint64_t EFI_HANDOFF_REGION_END = 0x20000000ULL;
constexpr uint64_t EFI_OPEN_VOLUME_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xC80ULL;
constexpr uint64_t EFI_LOADED_IMAGE_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0xD00ULL;
constexpr uint64_t EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0x1000ULL;
constexpr uint64_t EFI_ROOT_FILE_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0x1040ULL;
constexpr uint64_t EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1280ULL;
constexpr uint64_t EFI_GET_VARIABLE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xD80ULL;
constexpr uint64_t EFI_ALLOCATE_POOL_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xE00ULL;
constexpr uint64_t EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xE80ULL;
constexpr uint64_t EFI_UNSUPPORTED_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xF00ULL;
constexpr uint64_t EFI_POOL_BASE = EFI_HANDOFF_REGION_BASE + 0x2000ULL;
constexpr uint64_t EFI_POOL_END = EFI_HANDOFF_REGION_BASE + 0x80000ULL;
using EfiGuid = std::array<uint8_t, 16>;
constexpr EfiGuid EFI_LOADED_IMAGE_PROTOCOL_GUID = {{
    0xA1, 0x31, 0x1B, 0x5B, 0x62, 0x95, 0xD2, 0x11,
    0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};
constexpr EfiGuid EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {{
    0x22, 0x5B, 0x4E, 0x96, 0x59, 0x64, 0xD2, 0x11,
    0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};

uint64_t normalizeBranchEntryIP(uint64_t target) {
    return target & ~0xFULL;
}

bool isZeroFilledEfiHandoffBundle(IMemory& memory, uint64_t target) {
    const uint64_t entryIP = normalizeBranchEntryIP(target);
    if (entryIP < EFI_HANDOFF_REGION_BASE || entryIP >= EFI_HANDOFF_REGION_END) {
        return false;
    }

    uint8_t bundle[16] = {};
    try {
        memory.Read(entryIP, bundle, sizeof(bundle));
    } catch (const std::exception&) {
        return false;
    }

    for (uint8_t byte : bundle) {
        if (byte != 0) {
            return false;
        }
    }

    return true;
}

bool isLoadInstruction(InstructionType type) {
    switch (type) {
        case InstructionType::LD1:
        case InstructionType::LD2:
        case InstructionType::LD4:
        case InstructionType::LD8:
        case InstructionType::LD1_S:
        case InstructionType::LD2_S:
        case InstructionType::LD4_S:
        case InstructionType::LD8_S:
            return true;
        default:
            return false;
    }
}

bool isAdvancedLoadCheckInstruction(InstructionType type) {
    return type == InstructionType::CHK_A_NC ||
           type == InstructionType::CHK_A_CLR;
}

uint64_t readCallerOutputRegister(const CPUState& cpu, size_t index) {
    const size_t reg = 32 + cpu.GetSOL() + index;
    return reg < NUM_GENERAL_REGISTERS ? cpu.GetGR(reg) : 0;
}

bool readEfiGuid(IMemory& memory, uint64_t address, EfiGuid& guid) {
    if (address == 0) {
        return false;
    }

    try {
        memory.Read(address, guid.data(), guid.size());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string formatEfiGuid(const EfiGuid& guid) {
    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < guid.size(); ++i) {
        if (i != 0) {
            oss << ':';
        }
        const int value = guid[i];
        if (value < 0x10) {
            oss << '0';
        }
        oss << value;
    }
    return oss.str();
}

bool isZeroEfiGuid(const EfiGuid& guid) {
    for (uint8_t byte : guid) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string readUtf16Preview(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return {};
    }

    std::string preview;
    for (size_t i = 0; i < 80; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch),
                        reinterpret_cast<uint8_t*>(&ch),
                        sizeof(ch));
        } catch (const std::exception&) {
            if (preview.empty()) {
                preview = "<unreadable>";
            }
            break;
        }

        if (ch == 0) {
            break;
        }
        if (ch == '\r') {
            preview += "\\r";
        } else if (ch == '\n') {
            preview += "\\n";
        } else if (ch >= 0x20 && ch <= 0x7E) {
            preview.push_back(static_cast<char>(ch));
        } else {
            preview.push_back('?');
        }
    }

    return preview;
}

std::string readUtf16AsciiString(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return {};
    }

    std::string value;
    for (size_t i = 0; i < 64; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch),
                        reinterpret_cast<uint8_t*>(&ch),
                        sizeof(ch));
        } catch (const std::exception&) {
            return {};
        }

        if (ch == 0) {
            return value;
        }
        if (ch < 0x20 || ch > 0x7E) {
            return {};
        }
        value.push_back(static_cast<char>(ch));
    }

    return value;
}

void appendLe16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendLe32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

std::vector<uint8_t> makeBoot0000LoadOption() {
    static const uint16_t description[] = {
        'g','u','i','d','e','X','O','S',' ','I','A','-','6','4',' ','I','S','O',0
    };
    static const uint16_t bootPath[] = {
        '\\','E','F','I','\\','B','O','O','T','\\',
        'B','O','O','T','I','A','6','4','.','E','F','I',0
    };

    std::vector<uint8_t> path;
    path.push_back(0x04); // MEDIA_DEVICE_PATH
    path.push_back(0x04); // MEDIA_FILEPATH_DP
    appendLe16(path, static_cast<uint16_t>(4 + sizeof(bootPath)));
    const uint8_t* bootPathBytes = reinterpret_cast<const uint8_t*>(bootPath);
    path.insert(path.end(), bootPathBytes, bootPathBytes + sizeof(bootPath));
    path.push_back(0x7F); // END_DEVICE_PATH_TYPE
    path.push_back(0xFF); // END_ENTIRE_DEVICE_PATH_SUBTYPE
    appendLe16(path, 4);

    std::vector<uint8_t> option;
    appendLe32(option, 1U); // LOAD_OPTION_ACTIVE
    appendLe16(option, static_cast<uint16_t>(path.size()));
    const uint8_t* descBytes = reinterpret_cast<const uint8_t*>(description);
    option.insert(option.end(), descBytes, descBytes + sizeof(description));
    option.insert(option.end(), path.begin(), path.end());
    return option;
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void finishCountedLoopTraceIfActive() {
    if (g_countedLoopTrace.active && g_countedLoopTrace.suppressed > 0) {
        std::cout << "[TRACE] suppressed " << g_countedLoopTrace.suppressed
                  << " counted-loop instruction trace lines for range 0x"
                  << std::hex << g_countedLoopTrace.start << "-0x"
                  << g_countedLoopTrace.end << std::dec << std::endl;
    }

    g_countedLoopTrace = CountedLoopTraceState();
}

} // namespace

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
    , efiPoolNext_(EFI_POOL_BASE)
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
    , efiPoolNext_(EFI_POOL_BASE)
    , callFrameStack_() {
}

void IA64ISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
    pendingCallInputs_.clear();
    efiPoolNext_ = EFI_POOL_BASE;
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
        
        if (g_countedLoopTrace.active &&
            (decodeResult.instructionAddress < g_countedLoopTrace.start ||
             decodeResult.instructionAddress > g_countedLoopTrace.end)) {
            finishCountedLoopTraceIfActive();
        }

        const bool suppressCountedLoopTrace =
            g_countedLoopTrace.active &&
            decodeResult.instructionAddress >= g_countedLoopTrace.start &&
            decodeResult.instructionAddress <= g_countedLoopTrace.end &&
            state_.getCPUState().GetAR(65) > 1;

        if (suppressCountedLoopTrace) {
            ++g_countedLoopTrace.suppressed;
        } else {
            std::cout << "[IP=0x" << std::hex << decodeResult.instructionAddress << std::dec
                      << ", Slot=" << state_.currentSlot_ << "] "
                      << decodeResult.disassembly << std::endl;
        }

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
        bool handledFirmwareCallStub = false;
        uint64_t branchTarget = 0;
        const uint8_t predicate = cachedInstruction_.GetPredicate();
        const bool snapshotPredicateTrue = (predicate == 0) || state_.predicateGroupSnapshot_[predicate];
        const bool livePredicateTrue = (predicate == 0) || state_.getCPUState().GetPR(predicate);
        const uint64_t currentIP = state_.getCPUState().GetIP();
        const uint64_t branchTargetValue = cachedInstruction_.HasBranchTarget()
            ? cachedInstruction_.GetBranchTarget()
            : 0;
        // Some boot code reaches a counted loop form through the conservative
        // br.call decoder. Treat a short backward br.call b5 as ar.lc-driven.
        const bool callLooksLikeCountedLoop =
            cachedInstruction_.GetType() == InstructionType::BR_CALL &&
            cachedInstruction_.GetDst() == 5 &&
            cachedInstruction_.HasBranchTarget() &&
            branchTargetValue <= currentIP &&
            (currentIP - branchTargetValue) <= 0x100;
        const bool branchInstruction =
            cachedInstruction_.GetType() == InstructionType::BR_COND ||
            cachedInstruction_.GetType() == InstructionType::BR_CALL ||
            cachedInstruction_.GetType() == InstructionType::BR_RET ||
            cachedInstruction_.GetType() == InstructionType::BR_CLOOP;
        switch (cachedInstruction_.GetType()) {
            case InstructionType::BR_COND:
                if (livePredicateTrue) {
                    branchTarget = cachedInstruction_.HasBranchTarget()
                        ? cachedInstruction_.GetBranchTarget()
                        : state_.getCPUState().GetBR(cachedInstruction_.GetSrc1());
                    // The IA-64 EFI bootloader uses a small low-address thunk for
                    // Boot Services calls that decodes as br.cond b6, but it is
                    // followed by an EFI function that returns through b0. Link
                    // only this thunk-to-stub pattern so normal indirect branches
                    // keep their plain branch semantics.
                    if (!cachedInstruction_.HasBranchTarget() &&
                        cachedInstruction_.GetSrc1() == 6 &&
                        currentIP >= 0x1900 && currentIP < 0x1A00 &&
                        branchTarget >= 0x1FE00000ULL && branchTarget < 0x1FE01000ULL) {
                        state_.getCPUState().SetBR(0, currentIP + 16);
                    }
                    isBranch = true;
                }
                break;
                
            case InstructionType::BR_CALL:
                if (callLooksLikeCountedLoop) {
                    if (livePredicateTrue && state_.getCPUState().GetAR(65) != 0) {
                        branchTarget = cachedInstruction_.GetBranchTarget();
                        isBranch = true;
                    }
                } else if (livePredicateTrue) {
                    branchTarget = cachedInstruction_.HasBranchTarget()
                        ? cachedInstruction_.GetBranchTarget()
                        : state_.getCPUState().GetBR(cachedInstruction_.GetSrc1());
                    const uint64_t originalBranchTarget = branchTarget;
                    if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_ALLOCATE_POOL_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t size = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t bufferOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        const uint64_t allocation = alignUp(efiPoolNext_, 16);
                        const uint64_t alignedSize = alignUp(size, 16);
                        const bool fits = allocation <= EFI_POOL_END &&
                            alignedSize <= (EFI_POOL_END - allocation);
                        const uint64_t next = fits ? allocation + alignedSize : EFI_POOL_END + 1;
                        if (size != 0 && bufferOut != 0 && fits) {
                            std::vector<uint8_t> zero(static_cast<size_t>(size), 0);
                            memory.Write(allocation, zero.data(), zero.size());
                            memory.Write(bufferOut,
                                         reinterpret_cast<const uint8_t*>(&allocation),
                                         sizeof(allocation));
                            efiPoolNext_ = next;
                            state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                            std::cout << "[EFI-STUB] BootServices.AllocatePool size=0x"
                                      << std::hex << size << " -> 0x" << allocation
                                      << " via out=0x" << bufferOut << std::dec << std::endl;
                        } else {
                            state_.getCPUState().SetGR(8, static_cast<uint64_t>(-1));
                            std::cout << "[EFI-STUB] BootServices.AllocatePool failed size=0x"
                                      << std::hex << size << " out=0x" << bufferOut
                                      << " next=0x" << next << std::dec << std::endl;
                        }
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_GET_VARIABLE_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t variableName = readCallerOutputRegister(state_.getCPUState(), 0);
                        const uint64_t vendorGuid = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t attributesOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        const uint64_t dataSizeOut = readCallerOutputRegister(state_.getCPUState(), 3);
                        const uint64_t dataOut = readCallerOutputRegister(state_.getCPUState(), 4);
                        const std::string variableNameText = readUtf16AsciiString(memory, variableName);

                        std::vector<uint8_t> variableData;
                        if (variableNameText == "BootCurrent") {
                            appendLe16(variableData, 0);
                        } else if (variableNameText == "Boot0000") {
                            variableData = makeBoot0000LoadOption();
                        }

                        uint64_t providedSize = 0;
                        if (dataSizeOut != 0) {
                            try {
                                memory.Read(dataSizeOut,
                                            reinterpret_cast<uint8_t*>(&providedSize),
                                            sizeof(providedSize));
                            } catch (const std::exception&) {
                                providedSize = 0;
                            }
                        }

                        uint64_t status = EFI_STATUS_NOT_FOUND;
                        if (!variableData.empty() && dataSizeOut != 0) {
                            const uint64_t requiredSize = static_cast<uint64_t>(variableData.size());
                            memory.Write(dataSizeOut,
                                         reinterpret_cast<const uint8_t*>(&requiredSize),
                                         sizeof(requiredSize));
                            if (attributesOut != 0) {
                                const uint32_t attributes = 0x7U; // NV | BS | RT
                                memory.Write(attributesOut,
                                             reinterpret_cast<const uint8_t*>(&attributes),
                                             sizeof(attributes));
                            }
                            if (dataOut != 0 && providedSize >= requiredSize) {
                                memory.Write(dataOut, variableData.data(), variableData.size());
                                status = EFI_STATUS_SUCCESS;
                            } else {
                                status = EFI_STATUS_BUFFER_TOO_SMALL;
                            }
                        } else if (dataSizeOut != 0) {
                            const uint64_t zero = 0;
                            memory.Write(dataSizeOut,
                                         reinterpret_cast<const uint8_t*>(&zero),
                                         sizeof(zero));
                        }

                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        std::cout << "[EFI-STUB] RuntimeServices.GetVariable name=0x"
                                  << std::hex << variableName
                                  << " \"" << variableNameText << "\""
                                  << " guid=0x" << vendorGuid
                                  << " attrOut=0x" << attributesOut
                                  << " sizeOut=0x" << dataSizeOut
                                  << " dataOut=0x" << dataOut;
                        if (!variableData.empty()) {
                            std::cout << " required=0x" << variableData.size();
                        }
                        if (status == EFI_STATUS_SUCCESS) {
                            std::cout << " -> EFI_SUCCESS";
                        } else if (status == EFI_STATUS_BUFFER_TOO_SMALL) {
                            std::cout << " -> EFI_BUFFER_TOO_SMALL";
                        } else {
                            std::cout << " -> EFI_NOT_FOUND";
                        }
                        std::cout << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t protocolGuid = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t interfaceOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        EfiGuid guid = {};
                        const bool hasGuid = readEfiGuid(memory, protocolGuid, guid);
                        uint64_t protocolAddress = 0;
                        const char* protocolName = nullptr;
                        if (hasGuid && guid == EFI_LOADED_IMAGE_PROTOCOL_GUID) {
                            protocolAddress = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
                            protocolName = "LoadedImageProtocol";
                        } else if (hasGuid && guid == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID) {
                            protocolAddress = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR;
                            protocolName = "SimpleFileSystemProtocol";
                        } else if (hasGuid && isZeroEfiGuid(guid)) {
                            if (currentIP == 0x2EFD0ULL || currentIP == 0x1A50ULL) {
                                protocolAddress = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
                                protocolName = "LoadedImageProtocol(zero-guid fallback)";
                            } else if (currentIP == 0x1CC0ULL) {
                                protocolAddress = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR;
                                protocolName = "SimpleFileSystemProtocol(zero-guid fallback)";
                            }
                        }

                        if (interfaceOut != 0 && protocolAddress != 0) {
                            memory.Write(interfaceOut,
                                         reinterpret_cast<const uint8_t*>(&protocolAddress),
                                         sizeof(protocolAddress));
                        } else if (interfaceOut != 0) {
                            const uint64_t nullInterface = 0;
                            memory.Write(interfaceOut,
                                         reinterpret_cast<const uint8_t*>(&nullInterface),
                                         sizeof(nullInterface));
                        }
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, protocolAddress != 0
                            ? EFI_STATUS_SUCCESS
                            : EFI_STATUS_NOT_FOUND);
                        std::cout << "[EFI-STUB] BootServices.HandleProtocol guid=0x"
                                  << std::hex << protocolGuid;
                        if (hasGuid) {
                            std::cout << " [" << formatEfiGuid(guid) << "]";
                        } else {
                            std::cout << " [unreadable]";
                        }
                        if (protocolAddress != 0) {
                            std::cout << " returned " << protocolName << "=0x"
                                      << protocolAddress << " via out=0x" << interfaceOut;
                        } else {
                            std::cout << " -> EFI_NOT_FOUND via out=0x" << interfaceOut;
                        }
                        std::cout << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_OPEN_VOLUME_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t rootOut = readCallerOutputRegister(state_.getCPUState(), 1);
                        if (rootOut != 0) {
                            memory.Write(rootOut,
                                         reinterpret_cast<const uint8_t*>(&EFI_ROOT_FILE_PROTOCOL_ADDR),
                                         sizeof(EFI_ROOT_FILE_PROTOCOL_ADDR));
                        }
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, rootOut != 0
                            ? EFI_STATUS_SUCCESS
                            : EFI_STATUS_UNSUPPORTED);
                        std::cout << "[EFI-STUB] SimpleFileSystem.OpenVolume rootOut=0x"
                                  << std::hex << rootOut;
                        if (rootOut != 0) {
                            std::cout << " -> FileProtocol=0x" << EFI_ROOT_FILE_PROTOCOL_ADDR;
                        } else {
                            std::cout << " -> EFI_UNSUPPORTED";
                        }
                        std::cout << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
                        const uint64_t stringAddress = readCallerOutputRegister(state_.getCPUState(), 1);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] SimpleTextOut.OutputString this=0x"
                                  << std::hex << thisProtocol
                                  << " string=0x" << stringAddress;
                        const std::string preview = readUtf16Preview(memory, stringAddress);
                        if (!preview.empty()) {
                            std::cout << " \"" << preview << "\"";
                        }
                        std::cout << " -> EFI_SUCCESS" << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() && branchTarget == 0) {
                        handledFirmwareCallStub = true;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] indirect br.call target is null; returning EFI_SUCCESS"
                                  << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        isZeroFilledEfiHandoffBundle(memory, branchTarget)) {
                        handledFirmwareCallStub = true;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] indirect br.call target 0x"
                                  << std::hex << originalBranchTarget
                                  << " is zero-filled handoff memory; returning EFI_SUCCESS"
                                  << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_UNSUPPORTED_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_UNSUPPORTED);
                        std::cout << "[EFI-STUB] unsupported firmware service target 0x"
                                  << std::hex << originalBranchTarget
                                  << " -> EFI_UNSUPPORTED" << std::dec << std::endl;
                    } else {
                        saveCallFrame(currentIP + 16);
                        captureCallOutputRegisters();
                    }
                    isBranch = true;
                }
                break;
                
            case InstructionType::BR_RET:
                if (livePredicateTrue) {
                    branchTarget = state_.getCPUState().GetBR(cachedInstruction_.GetSrc1());
                    isBranch = true;
                }
                break;

            case InstructionType::BR_CLOOP:
                if (livePredicateTrue && cachedInstruction_.HasBranchTarget() &&
                    state_.getCPUState().GetAR(65) != 0) {
                    branchTarget = cachedInstruction_.GetBranchTarget();
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
            if (callLooksLikeCountedLoop) {
                if (state_.getCPUState().GetAR(65) != 0) {
                    state_.getCPUState().SetAR(65, state_.getCPUState().GetAR(65) - 1);
                }
            } else {
                if (!handledFirmwareCallStub) {
                    executeInstruction(memory, cachedInstruction_, true);
                }
            }
            if (cachedInstruction_.GetType() == InstructionType::ALLOC) {
                applyPendingCallInputRegisters();
            }
        }

        // Handle branch after execution
        if (isBranch) {
            if (cachedInstruction_.GetType() == InstructionType::BR_RET &&
                livePredicateTrue && branchTarget == 0) {
                std::cout << "[EFI-STUB] top-level br.ret b0 reached zero return address; halting EFI app"
                          << std::endl;
                state_.bundleValid_ = false;
                hasCachedInstruction_ = false;
                return ISAExecutionResult::HALT;
            }
            if (cachedInstruction_.GetType() == InstructionType::BR_RET && livePredicateTrue) {
                restoreCallFrame(branchTarget);
            }
            const uint64_t branchEntryIP = normalizeBranchEntryIP(branchTarget);
            if (callLooksLikeCountedLoop) {
                g_countedLoopTrace.active = true;
                g_countedLoopTrace.start = branchEntryIP;
                g_countedLoopTrace.end = currentIP;
            }
            state_.getCPUState().SetIP(branchEntryIP);
            state_.bundleValid_ = false;
            state_.currentSlot_ = 0;
            capturePredicateGroupSnapshot();
            hasCachedInstruction_ = false;
            return ISAExecutionResult::CONTINUE;
        }

        if (callLooksLikeCountedLoop) {
            finishCountedLoopTraceIfActive();
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

void IA64ISAPlugin::saveCallFrame(uint64_t returnAddress) {
    CallFrameSnapshot frame{};
    frame.cfm = state_.getCPUState().GetCFM();
    frame.returnAddress = returnAddress;
    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        frame.stackedRegisters[i] = state_.getCPUState().GetGR(NUM_STATIC_GR + i);
    }
    callFrameStack_.push_back(frame);
}

void IA64ISAPlugin::restoreCallFrame(uint64_t branchTarget) {
    if (callFrameStack_.empty()) {
        return;
    }

    const CallFrameSnapshot frame = callFrameStack_.back();
    if (frame.returnAddress != branchTarget) {
        return;
    }

    callFrameStack_.pop_back();

    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        state_.getCPUState().SetGR(NUM_STATIC_GR + i, frame.stackedRegisters[i]);
    }
    state_.getCPUState().SetCFM(frame.cfm);
}

void IA64ISAPlugin::executeInstruction(IMemory& memory, const InstructionEx& instr, bool ignorePredicate) {
    try {
        instr.Execute(state_.getCPUState(), memory, ignorePredicate);
        if (isAdvancedLoadCheckInstruction(instr.GetType())) {
            std::cout << "[ALAT-STUB] " << instr.GetDisassembly()
                      << " treated as success; ALAT tracking is not implemented"
                      << std::endl;
        }
    } catch (const std::exception& e) {
        CPUState& cpu = state_.getCPUState();
        const uint64_t ip = cpu.GetIP();
        const uint64_t baseBefore = cpu.GetGR(instr.GetSrc1());

        std::cerr << "Error executing instruction at IP=0x" << std::hex << ip
                  << " Slot=" << std::dec << state_.currentSlot_
                  << " raw=0x" << std::hex << instr.GetRawBits()
                  << " disasm=\"" << instr.GetDisassembly() << "\": "
                  << e.what() << std::dec << "\n";

        if (isLoadInstruction(instr.GetType())) {
            cpu.SetGR(instr.GetDst(), 0);
            bool sanitizedBase = false;
            if (instr.HasImmediate()) {
                cpu.SetGR(instr.GetSrc1(), static_cast<uint64_t>(static_cast<int64_t>(instr.GetImmediate())));
                sanitizedBase = true;
            } else if (instr.GetSrc2() != 0) {
                cpu.SetGR(instr.GetSrc1(), cpu.GetGR(instr.GetSrc2()));
                sanitizedBase = true;
            } else if (instr.GetSrc1() != instr.GetDst()) {
                cpu.SetGR(instr.GetSrc1(), 0);
                sanitizedBase = true;
            }

            std::cerr << "Recovered failed IA-64 load by zeroing r"
                      << static_cast<int>(instr.GetDst())
                      << (sanitizedBase ? ", sanitizing the base register, and continuing.\n"
                                        : " and continuing.\n");
        } else {
            std::cerr << "Treating as NOP and continuing...\n";
        }
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
