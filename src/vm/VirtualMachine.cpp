#include "VirtualMachine.h"
#include "cpu.h"
#include "memory.h"
#include "decoder.h"
#include "cpu_state.h"
#include "logger.h"
#include <iostream>
#include <stdexcept>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualMachine::VirtualMachine(size_t memorySize)
    : memory_(nullptr),
      decoder_(nullptr),
      cpu_(nullptr),
      state_(VMState::UNINITIALIZED),
      debuggerAttached_(false),
      cyclesExecuted_(0) {
    
    try {
        // Create subsystems
        memory_ = std::make_unique<Memory>(memorySize);
        decoder_ = std::make_unique<InstructionDecoder>();
        cpu_ = std::make_unique<CPU>(*memory_, *decoder_);
        
        LOG_INFO("VirtualMachine created with " + std::to_string(memorySize) + " bytes of memory");
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create VirtualMachine: " + std::string(e.what()));
        throw;
    }
}

VirtualMachine::~VirtualMachine() {
    // Halt execution if running
    if (state_ == VMState::RUNNING) {
        halt();
    }
    
    // Subsystems are automatically destroyed in reverse order:
    // 1. cpu_ (depends on memory_ and decoder_)
    // 2. decoder_
    // 3. memory_
    
    LOG_INFO("VirtualMachine destroyed");
}

// ============================================================================
// Lifecycle Management
// ============================================================================

bool VirtualMachine::init() {
    LOG_INFO("Initializing VirtualMachine...");
    
    try {
        // Initialize all subsystems
        if (!initializeSubsystems()) {
            LOG_ERROR("Failed to initialize subsystems");
            state_ = VMState::ERROR;
            return false;
        }
        
        // Reset execution state
        cyclesExecuted_ = 0;
        breakpoints_.clear();
        debuggerAttached_ = false;
        
        state_ = VMState::INITIALIZED;
        LOG_INFO("VirtualMachine initialized successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during initialization: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return false;
    }
}

bool VirtualMachine::reset() {
    LOG_INFO("Resetting VirtualMachine...");
    
    try {
        // Reset CPU state
        cpu_->reset();
        
        // Note: We don't clear memory - this allows program to persist
        // If you want to clear memory, call memory_->Clear() here
        
        // Reset execution state
        cyclesExecuted_ = 0;
        
        // Keep breakpoints and debugger attachment
        
        state_ = VMState::INITIALIZED;
        LOG_INFO("VirtualMachine reset successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during reset: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return false;
    }
}

void VirtualMachine::halt() {
    if (state_ == VMState::RUNNING) {
        LOG_INFO("Halting VirtualMachine execution");
        state_ = VMState::HALTED;
    }
}

// ============================================================================
// Execution Control
// ============================================================================

bool VirtualMachine::step() {
    // Check state
    if (state_ == VMState::UNINITIALIZED) {
        LOG_ERROR("Cannot step - VM not initialized");
        return false;
    }
    
    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step - VM in error state");
        return false;
    }
    
    // Set running state
    if (state_ != VMState::RUNNING && state_ != VMState::DEBUG_BREAK) {
        state_ = VMState::RUNNING;
    }
    
    try {
        // Check for breakpoint before execution
        uint64_t currentIP = cpu_->getIP();
        if (debuggerAttached_ && checkBreakpoint(currentIP)) {
            LOG_DEBUG("Breakpoint hit at IP: 0x" + std::to_string(currentIP));
            state_ = VMState::DEBUG_BREAK;
            return false;  // Stop execution
        }
        
        // Execute one instruction
        bool shouldContinue = cpu_->step();
        
        // Increment cycle count
        cyclesExecuted_++;
        
        // Check execution result
        if (!shouldContinue) {
            LOG_INFO("CPU signaled halt");
            state_ = VMState::HALTED;
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during step: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return false;
    }
}

uint64_t VirtualMachine::run(uint64_t maxCycles) {
    LOG_INFO("Starting VM execution" + 
             (maxCycles > 0 ? " (max cycles: " + std::to_string(maxCycles) + ")" : " (unlimited)"));
    
    // Check state
    if (state_ == VMState::UNINITIALIZED) {
        LOG_ERROR("Cannot run - VM not initialized");
        return 0;
    }
    
    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot run - VM in error state");
        return 0;
    }
    
    // Set running state
    state_ = VMState::RUNNING;
    
    uint64_t cyclesThisRun = 0;
    
    // Execute until halt or max cycles
    while (state_ == VMState::RUNNING) {
        // Check cycle limit
        if (maxCycles > 0 && cyclesThisRun >= maxCycles) {
            LOG_INFO("Maximum cycle count reached: " + std::to_string(maxCycles));
            state_ = VMState::HALTED;
            break;
        }
        
        // Execute one step
        bool shouldContinue = step();
        
        if (shouldContinue) {
            cyclesThisRun++;
        } else {
            // step() already set state appropriately
            break;
        }
    }
    
    LOG_INFO("VM execution stopped after " + std::to_string(cyclesThisRun) + " cycles");
    LOG_INFO("Total cycles executed: " + std::to_string(cyclesExecuted_));
    
    return cyclesThisRun;
}

// ============================================================================
// Program Loading
// ============================================================================

bool VirtualMachine::loadProgram(const uint8_t* data, size_t size, uint64_t loadAddress) {
    if (!data || size == 0) {
        LOG_ERROR("Invalid program data");
        return false;
    }
    
    LOG_INFO("Loading program: " + std::to_string(size) + " bytes at 0x" + 
             std::to_string(loadAddress));
    
    try {
        // Write program to memory
        memory_->Write(loadAddress, data, size);
        
        LOG_INFO("Program loaded successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load program: " + std::string(e.what()));
        return false;
    }
}

void VirtualMachine::setEntryPoint(uint64_t address) {
    LOG_INFO("Setting entry point to 0x" + std::to_string(address));
    cpu_->setIP(address);
}

// ============================================================================
// State Query
// ============================================================================

uint64_t VirtualMachine::getIP() const {
    if (!cpu_) {
        return 0;
    }
    return cpu_->getIP();
}

const CPUState& VirtualMachine::getCPUState() const {
    if (!cpu_) {
        throw std::runtime_error("CPU not initialized");
    }
    return cpu_->getState();
}

// ============================================================================
// Debugging Support
// ============================================================================

bool VirtualMachine::attach_debugger() {
    if (debuggerAttached_) {
        LOG_WARN("Debugger already attached");
        return false;
    }
    
    LOG_INFO("Attaching debugger to VM");
    debuggerAttached_ = true;
    return true;
}

void VirtualMachine::detach_debugger() {
    if (!debuggerAttached_) {
        LOG_WARN("No debugger attached");
        return;
    }
    
    LOG_INFO("Detaching debugger from VM");
    debuggerAttached_ = false;
    
    // Clear all breakpoints on detach
    breakpoints_.clear();
}

bool VirtualMachine::setBreakpoint(uint64_t address) {
    if (!debuggerAttached_) {
        LOG_ERROR("Cannot set breakpoint - no debugger attached");
        return false;
    }
    
    auto result = breakpoints_.insert(address);
    if (result.second) {
        LOG_INFO("Breakpoint set at 0x" + std::to_string(address));
        return true;
    } else {
        LOG_WARN("Breakpoint already exists at 0x" + std::to_string(address));
        return false;
    }
}

bool VirtualMachine::clearBreakpoint(uint64_t address) {
    if (!debuggerAttached_) {
        LOG_ERROR("Cannot clear breakpoint - no debugger attached");
        return false;
    }
    
    size_t removed = breakpoints_.erase(address);
    if (removed > 0) {
        LOG_INFO("Breakpoint cleared at 0x" + std::to_string(address));
        return true;
    } else {
        LOG_WARN("No breakpoint at 0x" + std::to_string(address));
        return false;
    }
}

// ============================================================================
// Register Access
// ============================================================================

uint64_t VirtualMachine::readGR(size_t index) const {
    if (!cpu_) {
        throw std::runtime_error("CPU not initialized");
    }
    return cpu_->readGR(index);
}

void VirtualMachine::writeGR(size_t index, uint64_t value) {
    if (!cpu_) {
        throw std::runtime_error("CPU not initialized");
    }
    cpu_->writeGR(index, value);
}

// ============================================================================
// Debug Support
// ============================================================================

void VirtualMachine::dump() const {
    std::cout << "\n========================================\n";
    std::cout << "Virtual Machine State\n";
    std::cout << "========================================\n";
    
    // VM state
    std::cout << "VM State: ";
    switch (state_) {
        case VMState::UNINITIALIZED: std::cout << "UNINITIALIZED\n"; break;
        case VMState::INITIALIZED:   std::cout << "INITIALIZED\n"; break;
        case VMState::RUNNING:       std::cout << "RUNNING\n"; break;
        case VMState::HALTED:        std::cout << "HALTED\n"; break;
        case VMState::ERROR:         std::cout << "ERROR\n"; break;
        case VMState::DEBUG_BREAK:   std::cout << "DEBUG_BREAK\n"; break;
    }
    
    std::cout << "Cycles Executed: " << cyclesExecuted_ << "\n";
    std::cout << "Debugger Attached: " << (debuggerAttached_ ? "Yes" : "No") << "\n";
    std::cout << "Breakpoints: " << breakpoints_.size() << "\n";
    
    if (!breakpoints_.empty()) {
        std::cout << "  Breakpoint addresses:\n";
        for (uint64_t addr : breakpoints_) {
            std::cout << "    0x" << std::hex << addr << std::dec << "\n";
        }
    }
    
    std::cout << "\n";
    
    // CPU state
    if (cpu_) {
        cpu_->dump();
    } else {
        std::cout << "CPU: Not initialized\n";
    }
    
    std::cout << "========================================\n\n";
}

// ============================================================================
// Extended API
// ============================================================================

IMemory& VirtualMachine::getMemory() {
    return *memory_;
}

const IMemory& VirtualMachine::getMemory() const {
    return *memory_;
}

ICPU& VirtualMachine::getCPU() {
    return *cpu_;
}

const ICPU& VirtualMachine::getCPU() const {
    return *cpu_;
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool VirtualMachine::checkBreakpoint(uint64_t address) const {
    return breakpoints_.find(address) != breakpoints_.end();
}

bool VirtualMachine::initializeSubsystems() {
    // Verify subsystems exist
    if (!memory_ || !decoder_ || !cpu_) {
        LOG_ERROR("Subsystems not created");
        return false;
    }
    
    // Reset CPU to initial state
    cpu_->reset();
    
    // Memory and decoder don't need explicit initialization
    // They're already in a valid state from construction
    
    return true;
}

} // namespace ia64
