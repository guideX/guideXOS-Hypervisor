#include "KernelPanic.h"
#include <sstream>
#include <iomanip>

namespace ia64 {

KernelPanicDetector::KernelPanicDetector()
    : bootTrace_(nullptr)
    , stackBase_(0)
    , stackLimit_(0)
    , recentEventsCount_(20) {
}

KernelPanic KernelPanicDetector::buildPanic(KernelPanicReason reason, const std::string& description,
                                           const CPUState& cpuState, const Bundle& lastBundle,
                                           size_t lastSlot, uint64_t timestamp, uint64_t cycles) {
    KernelPanic panic;
    
    panic.reason = reason;
    panic.description = description;
    panic.timestamp = timestamp;
    panic.cyclesExecuted = cycles;
    
    // Capture register state
    panic.registers = captureRegisterDump(cpuState);
    
    // Capture last bundle
    panic.lastBundle = lastBundle;
    panic.lastSlot = lastSlot;
    panic.lastBundleAddress = cpuState.GetIP();
    panic.bundleValid = true;
    
    // Capture stack trace
    panic.stackTrace = captureStackTrace(cpuState);
    panic.stackPointer = cpuState.GetGR(12);  // r12 is stack pointer in IA-64
    panic.stackBase = stackBase_;
    panic.stackLimit = stackLimit_;
    
    // Capture recent boot events if boot trace is available
    if (bootTrace_) {
        panic.recentEvents = bootTrace_->getLastEvents(recentEventsCount_);
        
        // Record panic in boot trace
        bootTrace_->recordKernelPanic(description, timestamp, cpuState.GetIP());
    }
    
    return panic;
}

KernelPanic KernelPanicDetector::triggerPanic(KernelPanicReason reason, const std::string& description,
                                             const CPUState& cpuState, const Bundle& lastBundle,
                                             size_t lastSlot, uint64_t timestamp, uint64_t cycles) {
    return buildPanic(reason, description, cpuState, lastBundle, lastSlot, timestamp, cycles);
}

KernelPanic KernelPanicDetector::captureInvalidOpcodePanic(const CPUState& cpuState, const Bundle& bundle,
                                                          size_t slot, uint64_t timestamp, uint64_t cycles) {
    std::ostringstream desc;
    desc << "Invalid opcode encountered at slot " << slot 
         << " in bundle at 0x" << std::hex << std::setw(16) << std::setfill('0') << cpuState.GetIP();
    
    return buildPanic(KernelPanicReason::INVALID_OPCODE, desc.str(), 
                     cpuState, bundle, slot, timestamp, cycles);
}

KernelPanic KernelPanicDetector::captureMemoryAccessPanic(const CPUState& cpuState, uint64_t faultingAddress,
                                                         const Bundle& lastBundle, size_t lastSlot,
                                                         uint64_t timestamp, uint64_t cycles) {
    std::ostringstream desc;
    desc << "Invalid memory access at address 0x" << std::hex << std::setw(16) << std::setfill('0') 
         << faultingAddress << " from IP 0x" << cpuState.GetIP();
    
    KernelPanic panic = buildPanic(KernelPanicReason::INVALID_MEMORY_ACCESS, desc.str(),
                                  cpuState, lastBundle, lastSlot, timestamp, cycles);
    panic.faultingAddress = faultingAddress;
    return panic;
}

KernelPanic KernelPanicDetector::captureExceptionPanic(const std::string& exceptionMessage, 
                                                      const CPUState& cpuState,
                                                      const Bundle& lastBundle, size_t lastSlot,
                                                      uint64_t timestamp, uint64_t cycles) {
    std::ostringstream desc;
    desc << "Unhandled exception: " << exceptionMessage;
    
    return buildPanic(KernelPanicReason::UNHANDLED_EXCEPTION, desc.str(),
                     cpuState, lastBundle, lastSlot, timestamp, cycles);
}

RegisterDump KernelPanicDetector::captureRegisterDump(const CPUState& cpuState) const {
    RegisterDump dump;
    
    // Capture general registers
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; i++) {
        dump.generalRegisters[i] = cpuState.GetGR(i);
    }
    
    // Capture floating-point registers (simplified - just get raw bits)
    for (size_t i = 0; i < NUM_FLOAT_REGISTERS; i++) {
        uint8_t frData[16];
        cpuState.GetFR(i, frData);
        // Store first 8 bytes as uint64_t for dump
        std::memcpy(&dump.floatingPointRegisters[i], frData, sizeof(uint64_t));
    }
    
    // Capture predicate registers (pack into uint64_t)
    uint64_t pr0 = 0, pr1 = 0;
    for (size_t i = 0; i < 64; i++) {
        if (cpuState.GetPR(i)) {
            if (i < 64) {
                pr0 |= (1ULL << i);
            }
        }
    }
    dump.predicateRegisters[0] = pr0;
    dump.predicateRegisters[1] = pr1;
    
    // Capture branch registers
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; i++) {
        dump.branchRegisters[i] = cpuState.GetBR(i);
    }
    
    // Capture special registers
    dump.instructionPointer = cpuState.GetIP();
    dump.currentFrameMarker = cpuState.GetCFM();
    dump.processorStatusRegister = cpuState.GetPSR();
    
    // Capture important application registers
    dump.registerStackEngine = cpuState.GetAR(16);    // AR.RSC
    dump.backingStorePointer = cpuState.GetAR(17);    // AR.BSP
    dump.previousFunctionState = cpuState.GetAR(64);  // AR.PFS
    
    return dump;
}

StackTrace KernelPanicDetector::captureStackTrace(const CPUState& cpuState, size_t maxDepth) const {
    StackTrace trace;
    trace.maxDepth = maxDepth;
    
    // Get current frame pointer (r5 commonly used as frame pointer in IA-64 conventions)
    // and stack pointer (r12)
    uint64_t fp = cpuState.GetGR(5);   // Frame pointer
    uint64_t sp = cpuState.GetGR(12);  // Stack pointer
    uint64_t ip = cpuState.GetIP();
    
    // Add current frame
    StackFrame currentFrame;
    currentFrame.framePointer = fp;
    currentFrame.instructionPointer = ip;
    currentFrame.returnAddress = cpuState.GetBR(0);  // Return address typically in b0
    currentFrame.functionName = "current";
    trace.frames.push_back(currentFrame);
    
    // Walk the stack (simplified - would need proper unwinding in real implementation)
    // This is a placeholder that shows the structure
    size_t depth = 1;
    uint64_t currentFP = fp;
    
    while (depth < maxDepth && currentFP != 0) {
        // Check if frame pointer is within valid stack range
        if (stackBase_ != 0 && stackLimit_ != 0) {
            if (currentFP < stackBase_ || currentFP > stackLimit_) {
                trace.truncated = true;
                break;
            }
        }
        
        StackFrame frame;
        frame.framePointer = currentFP;
        frame.instructionPointer = 0;  // Would read from stack in real implementation
        frame.returnAddress = 0;       // Would read from stack in real implementation
        frame.functionName = "frame_" + std::to_string(depth);
        trace.frames.push_back(frame);
        
        depth++;
        
        // In real implementation, would read previous frame pointer from stack
        // For now, just stop after current frame
        break;
    }
    
    if (depth >= maxDepth) {
        trace.truncated = true;
    }
    
    return trace;
}

std::string KernelPanicDetector::generatePanicReport(const KernelPanic& panic) const {
    std::ostringstream report;
    
    report << "\n";
    report << "================================================================================\n";
    report << "                           KERNEL PANIC DETECTED                                \n";
    report << "================================================================================\n\n";
    
    report << "Panic Reason: " << kernelPanicReasonToString(panic.reason) << "\n";
    report << "Description:  " << panic.description << "\n";
    report << "Timestamp:    " << std::dec << panic.timestamp << " cycles\n";
    report << "Total Cycles: " << panic.cyclesExecuted << "\n";
    
    if (panic.faultingAddress != 0) {
        report << "Faulting Addr: 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << panic.faultingAddress << "\n";
    }
    
    report << "\n";
    report << "--- CPU STATE AT PANIC ---\n";
    report << generateRegisterDump(panic.registers);
    
    report << "\n";
    report << "--- LAST EXECUTED BUNDLE ---\n";
    if (panic.bundleValid) {
        report << generateBundleDisassembly(panic.lastBundle, panic.lastBundleAddress);
        report << "Last Slot: " << panic.lastSlot << "\n";
    } else {
        report << "No valid bundle information available\n";
    }
    
    report << "\n";
    report << "--- STACK TRACE ---\n";
    report << generateStackTrace(panic.stackTrace);
    
    report << "\n";
    report << "--- RECENT BOOT EVENTS (Last " << panic.recentEvents.size() << " events) ---\n";
    if (panic.recentEvents.empty()) {
        report << "No boot trace events available\n";
    } else {
        report << std::left;
        report << std::setw(12) << "Timestamp" << " | "
               << std::setw(30) << "Event" << " | "
               << "Description\n";
        report << std::string(80, '-') << "\n";
        
        for (const auto& event : panic.recentEvents) {
            report << std::dec << std::setw(12) << event.timestamp << " | ";
            report << std::setw(30) << bootTraceEventTypeToString(event.type) << " | ";
            report << event.description << "\n";
        }
    }
    
    if (!panic.additionalInfo.empty()) {
        report << "\n";
        report << "--- ADDITIONAL INFORMATION ---\n";
        report << panic.additionalInfo << "\n";
    }
    
    report << "\n";
    report << "================================================================================\n";
    report << "                          END OF PANIC REPORT                                  \n";
    report << "================================================================================\n";
    
    return report.str();
}

std::string KernelPanicDetector::generateRegisterDump(const RegisterDump& dump) const {
    std::ostringstream report;
    
    report << "Instruction Pointer: 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.instructionPointer << "\n";
    report << "Current Frame Marker: 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.currentFrameMarker << "\n";
    report << "Processor Status: 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.processorStatusRegister << "\n\n";
    
    // General registers (show important ones)
    report << "General Registers:\n";
    for (size_t i = 0; i < 32; i += 4) {
        report << "  r" << std::dec << std::setw(2) << i << "-r" << (i+3) << ": ";
        for (size_t j = 0; j < 4 && (i+j) < 32; j++) {
            report << "0x" << std::hex << std::setw(16) << std::setfill('0') 
                   << dump.generalRegisters[i+j];
            if (j < 3) report << " ";
        }
        report << "\n";
    }
    
    // Show stack pointer specifically
    report << "  r12 (sp):  0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.generalRegisters[12] << "\n";
    
    // Branch registers
    report << "\nBranch Registers:\n";
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; i++) {
        report << "  b" << std::dec << i << ": 0x" << std::hex << std::setw(16) << std::setfill('0') 
               << dump.branchRegisters[i] << "\n";
    }
    
    // Important application registers
    report << "\nApplication Registers:\n";
    report << "  AR.RSC (RSE Config): 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.registerStackEngine << "\n";
    report << "  AR.BSP (Backing Store): 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.backingStorePointer << "\n";
    report << "  AR.PFS (Previous Frame): 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.previousFunctionState << "\n";
    
    // Predicate registers
    report << "\nPredicate Registers: 0x" << std::hex << std::setw(16) << std::setfill('0') 
           << dump.predicateRegisters[0] << "\n";
    
    return report.str();
}

std::string KernelPanicDetector::generateStackTrace(const StackTrace& trace) const {
    std::ostringstream report;
    
    if (trace.frames.empty()) {
        report << "No stack frames available\n";
        return report.str();
    }
    
    report << "Stack Depth: " << trace.frames.size();
    if (trace.truncated) {
        report << " (truncated at " << trace.maxDepth << " frames)";
    }
    report << "\n\n";
    
    report << std::left;
    report << "#  " << std::setw(18) << "Frame Pointer" << " "
           << std::setw(18) << "Instruction Ptr" << " "
           << std::setw(18) << "Return Address" << " "
           << "Function\n";
    report << std::string(80, '-') << "\n";
    
    for (size_t i = 0; i < trace.frames.size(); i++) {
        const auto& frame = trace.frames[i];
        report << std::dec << std::setw(2) << i << " ";
        report << "0x" << std::hex << std::setw(16) << std::setfill('0') << frame.framePointer << " ";
        report << "0x" << std::hex << std::setw(16) << std::setfill('0') << frame.instructionPointer << " ";
        report << "0x" << std::hex << std::setw(16) << std::setfill('0') << frame.returnAddress << " ";
        report << frame.functionName << "\n";
    }
    
    return report.str();
}

std::string KernelPanicDetector::generateBundleDisassembly(const Bundle& bundle, uint64_t address) const {
    std::ostringstream report;
    
    report << "Address: 0x" << std::hex << std::setw(16) << std::setfill('0') << address << "\n";
    report << "Template: 0x" << std::hex << static_cast<int>(bundle.templateType) 
           << " (" << std::dec << static_cast<int>(bundle.templateType) << ")\n";
    report << "Stop bit: " << (bundle.hasStop ? "yes" : "no") << "\n";
    
    report << "Instructions:\n";
    for (size_t i = 0; i < bundle.instructions.size(); i++) {
        report << "  Slot " << i << ": " << bundle.instructions[i].GetDisassembly() << "\n";
    }
    
    return report.str();
}

} // namespace ia64
