#include "SyscallDispatcher.h"
#include "cpu_state.h"
#include "memory.h"
#include "abi.h"
#include "BootTraceSystem.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace ia64 {

// IA-64 Break instruction encoding details:
// Break is an X-unit instruction with format:
//   - Major opcode: 0 (bits 37-40)
//   - X-unit opcode extension in various bits
//   - 21-bit immediate value (imm21)
//   - Syscall convention uses break.i 0x100000
//
// Simplified detection: look for break pattern with 0x100000 immediate
// Full encoding would require detailed X-unit format parsing
// For now, we use a simplified approach that matches common patterns

static const uint64_t SYSCALL_BREAK_IMMEDIATE = 0x100000;

// Break instruction pattern masks (simplified)
// Real IA-64 would require full X-unit format decoding
static const uint64_t BREAK_OPCODE_MASK = 0x1E00000000ULL;  // Bits 37-40
static const uint64_t BREAK_OPCODE_VALUE = 0x0000000000ULL; // Major opcode 0

SyscallDispatcher::SyscallDispatcher(LinuxABI& abi)
    : abi_(abi)
    , tracingConfig_()
    , traceHistory_()
    , statistics_()
    , traceCounter_(0)
    , bootTrace_(nullptr)
    , firstSyscallRecorded_(false) {
}

SyscallDispatcher::~SyscallDispatcher() {
}

bool SyscallDispatcher::IsSyscallInstruction(uint64_t rawInstruction) const {
    // IA-64 Break instruction detection
    // This is a simplified check - real implementation would need full format decoding
    
    // Check for major opcode 0 (break instructions)
    uint64_t majorOpcode = (rawInstruction & BREAK_OPCODE_MASK) >> 37;
    if (majorOpcode != 0) {
        return false;
    }
    
    // Extract immediate field (bits 6-26 for break.i)
    // Break instruction format has imm21 field
    uint64_t immediate = (rawInstruction >> 6) & 0x1FFFFF;  // 21 bits
    
    // Check if this is syscall break (0x100000)
    return (immediate == SYSCALL_BREAK_IMMEDIATE);
}

bool SyscallDispatcher::DispatchSyscall(CPUState& cpu, IMemory& memory) {
    // Create trace entry
    SyscallTrace trace;
    
    // Capture instruction address
    trace.instructionAddress = cpu.GetIP();
    
    // Capture syscall arguments before execution
    CaptureSyscallArguments(cpu, trace);
    
    // Execute the syscall through the ABI layer
    SyscallResult result = abi_.ExecuteSyscall(cpu, memory);
    
    // Capture return values
    trace.returnValue = result.returnValue;
    trace.errorCode = result.errorCode;
    trace.success = result.success;
    trace.timestamp = traceCounter_++;
    
    // Record to boot trace system if available
    if (bootTrace_ && bootTrace_->isEnabled()) {
        if (!firstSyscallRecorded_) {
            bootTrace_->recordFirstSyscall(trace.syscallNumber, trace.syscallName, 
                                          trace.timestamp, trace.instructionAddress);
            firstSyscallRecorded_ = true;
        } else {
            bootTrace_->recordSyscall(trace.syscallNumber, trace.syscallName,
                                     trace.timestamp, trace.instructionAddress);
        }
    }
    
    // Log the trace if tracing is enabled
    if (tracingConfig_.enabled) {
        LogTrace(trace);
    }
    
    // Store in history
    if (tracingConfig_.enabled) {
        traceHistory_.push_back(trace);
    }
    
    // Update statistics
    if (tracingConfig_.collectStatistics) {
        UpdateStatistics(result.success);
    }
    
    return result.success;
}

void SyscallDispatcher::ConfigureTracing(const SyscallTracingConfig& config) {
    tracingConfig_ = config;
}

void SyscallDispatcher::ClearTraceHistory() {
    traceHistory_.clear();
}

void SyscallDispatcher::ResetStatistics() {
    statistics_ = SyscallStatistics();
}

std::string SyscallDispatcher::FormatTrace(const SyscallTrace& trace) {
    std::ostringstream oss;
    
    // Format: [SYSCALL #timestamp] name(args...) = ret @ IP
    oss << "[SYSCALL #" << trace.timestamp << "] ";
    oss << trace.syscallName << "(";
    
    // Format arguments (show all 6 for completeness)
    for (int i = 0; i < 6; i++) {
        if (i > 0) oss << ", ";
        oss << "0x" << std::hex << trace.arguments[i];
    }
    oss << std::dec << ")";
    
    // Return value
    oss << " = ";
    if (trace.success) {
        oss << trace.returnValue;
    } else {
        oss << "-1 (errno=" << trace.errorCode << ")";
    }
    
    // Instruction address
    oss << " @ IP=0x" << std::hex << trace.instructionAddress << std::dec;
    
    return oss.str();
}

void SyscallDispatcher::PrintTraceHistory() const {
    std::cout << "\n=== Syscall Trace History ===\n";
    std::cout << "Total syscalls: " << traceHistory_.size() << "\n";
    std::cout << "Statistics: " << statistics_.totalCalls << " total, "
              << statistics_.successfulCalls << " successful, "
              << statistics_.failedCalls << " failed\n\n";
    
    for (const auto& trace : traceHistory_) {
        std::cout << FormatTrace(trace) << "\n";
    }
    
    std::cout << "=== End Trace History ===\n\n";
}

void SyscallDispatcher::CaptureSyscallArguments(const CPUState& cpu, SyscallTrace& trace) {
    // IA-64 Linux ABI: syscall number in r15
    trace.syscallNumber = cpu.GetGR(15);
    
    // Get syscall name
    Syscall syscall = static_cast<Syscall>(trace.syscallNumber);
    trace.syscallName = LinuxABI::GetSyscallName(syscall);
    
    // IA-64 Linux ABI: arguments in r32-r37 (out0-out5)
    trace.arguments[0] = cpu.GetGR(32);  // out0
    trace.arguments[1] = cpu.GetGR(33);  // out1
    trace.arguments[2] = cpu.GetGR(34);  // out2
    trace.arguments[3] = cpu.GetGR(35);  // out3
    trace.arguments[4] = cpu.GetGR(36);  // out4
    trace.arguments[5] = cpu.GetGR(37);  // out5
}

void SyscallDispatcher::LogTrace(const SyscallTrace& trace) {
    if (!tracingConfig_.enabled) {
        return;
    }
    
    std::cout << "[SYSCALL TRACE] ";
    
    // Log instruction address if configured
    if (tracingConfig_.logInstructionAddress) {
        std::cout << "IP=0x" << std::hex << trace.instructionAddress << std::dec << " ";
    }
    
    // Log syscall name and number
    std::cout << trace.syscallName << "(#" << trace.syscallNumber << ")";
    
    // Log arguments if configured
    if (tracingConfig_.logArguments) {
        std::cout << " args=[";
        for (int i = 0; i < 6; i++) {
            if (i > 0) std::cout << ", ";
            std::cout << "0x" << std::hex << trace.arguments[i] << std::dec;
        }
        std::cout << "]";
    }
    
    // Log return value if configured
    if (tracingConfig_.logReturnValues) {
        std::cout << " -> ";
        if (trace.success) {
            std::cout << "ret=" << trace.returnValue;
        } else {
            std::cout << "ERROR: errno=" << trace.errorCode;
        }
    }
    
    std::cout << "\n";
}

void SyscallDispatcher::UpdateStatistics(bool success) {
    statistics_.totalCalls++;
    if (success) {
        statistics_.successfulCalls++;
    } else {
        statistics_.failedCalls++;
    }
}

} // namespace ia64
