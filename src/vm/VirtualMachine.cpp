#include "VirtualMachine.h"
#include "SimpleCPUScheduler.h"
#include "cpu.h"
#include "memory.h"
#include "decoder.h"
#include "cpu_state.h"
#include "Console.h"
#include "InterruptController.h"
#include "Timer.h"
#include "logger.h"
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualMachine::VirtualMachine(size_t memorySize, size_t numCPUs)
    : memory_(nullptr),
      decoder_(nullptr),
      scheduler_(nullptr),
      interruptController_(nullptr),
      consoleDevice_(nullptr),
      timerDevice_(nullptr),
      state_(VMState::UNINITIALIZED),
      activeCPUIndex_(-1),
      debuggerAttached_(false),
      cyclesExecuted_(0) {
    
    if (numCPUs == 0) {
        throw std::invalid_argument("VM must have at least 1 CPU");
    }
    
    try {
        // Create shared subsystems
        memory_ = std::make_unique<Memory>(memorySize);
        decoder_ = std::make_unique<InstructionDecoder>();
        
        // Create CPU scheduler
        scheduler_ = std::make_unique<SimpleCPUScheduler>();

        // Create interrupt controller and default memory-mapped devices
        interruptController_ = std::make_unique<BasicInterruptController>();

        const uint64_t ioBase = memorySize >= 0x200 ? static_cast<uint64_t>(memorySize - 0x200) : 0;
        consoleDevice_ = std::make_unique<VirtualConsole>(ioBase);
        timerDevice_ = std::make_unique<VirtualTimer>(ioBase + 0x100);
        timerDevice_->AttachInterruptController(interruptController_.get());

        memory_->RegisterDevice(consoleDevice_.get());
        memory_->RegisterDevice(timerDevice_.get());
        
        // Create CPU contexts
        if (!createCPUs(numCPUs)) {
            throw std::runtime_error("Failed to create CPU contexts");
        }

        interruptController_->RegisterDeliveryCallback([this](uint8_t vector) {
            const int targetCPU = activeCPUIndex_ >= 0 ? activeCPUIndex_ : 0;
            if (isValidCPUIndex(targetCPU) && cpus_[targetCPU].cpu) {
                cpus_[targetCPU].cpu->queueInterrupt(vector);
                if (cpus_[targetCPU].state == CPUExecutionState::WAITING) {
                    cpus_[targetCPU].state = CPUExecutionState::RUNNING;
                }
            }
        });
        
        std::ostringstream oss;
        oss << "VirtualMachine created with " << memorySize << " bytes of memory and " 
            << numCPUs << " CPU(s)";
        LOG_INFO(oss.str());
        
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
    // 1. cpus_ (vector of CPUContext, each owns a CPU)
    // 2. scheduler_
    // 3. decoder_
    // 4. memory_
    
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
        
        // Initialize scheduler with CPU contexts
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }
        scheduler_->initialize(cpuPtrs);
        
        // Set first CPU as active and running
        if (!cpus_.empty()) {
            activeCPUIndex_ = 0;
            cpus_[0].state = CPUExecutionState::RUNNING;
            cpus_[0].enabled = true;
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
        // Reset all CPU states
        for (auto& ctx : cpus_) {
            if (ctx.cpu) {
                ctx.cpu->reset();
                ctx.state = CPUExecutionState::IDLE;
                ctx.cyclesExecuted = 0;
                ctx.instructionsExecuted = 0;
                ctx.idleCycles = 0;
            }
        }
        
        // Re-enable first CPU
        if (!cpus_.empty()) {
            activeCPUIndex_ = 0;
            cpus_[0].state = CPUExecutionState::RUNNING;
            cpus_[0].enabled = true;
        }
        
        // Reset scheduler
        scheduler_->reset();
        
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
        
        // Halt all running CPUs
        for (auto& ctx : cpus_) {
            if (ctx.state == CPUExecutionState::RUNNING) {
                ctx.state = CPUExecutionState::HALTED;
            }
        }
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
        // Use scheduler to select next CPU
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }
        
        int cpuIndex = scheduler_->selectNextCPU(cpuPtrs);
        
        if (cpuIndex < 0 || cpuIndex >= static_cast<int>(cpus_.size())) {
            // No runnable CPU
            LOG_DEBUG("No runnable CPU found");
            state_ = VMState::HALTED;
            return false;
        }
        
        // Update active CPU
        activeCPUIndex_ = cpuIndex;
        CPUContext& ctx = cpus_[cpuIndex];
        
        // Check for breakpoint before execution
        uint64_t currentIP = ctx.cpu->getIP();
        if (debuggerAttached_ && checkBreakpoint(currentIP)) {
            std::ostringstream oss;
            oss << "Breakpoint hit at IP: 0x" << std::hex << currentIP << std::dec
                << " on CPU " << cpuIndex;
            LOG_DEBUG(oss.str());
            state_ = VMState::DEBUG_BREAK;
            return false;  // Stop execution
        }
        
        // Execute one instruction on selected CPU
        bool shouldContinue = ctx.cpu->step();

        if (timerDevice_) {
            timerDevice_->Tick(1);
        }
        
        // Update CPU context statistics
        ctx.cyclesExecuted++;
        ctx.instructionsExecuted++;
        
        // Increment global cycle count
        cyclesExecuted_++;
        
        // Notify scheduler
        scheduler_->onCPUExecuted(cpuIndex, 1);
        
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
    
    // Set entry point for active CPU (or CPU 0 if no active CPU)
    int cpuIndex = (activeCPUIndex_ >= 0) ? activeCPUIndex_ : 0;
    if (cpuIndex < static_cast<int>(cpus_.size()) && cpus_[cpuIndex].cpu) {
        cpus_[cpuIndex].cpu->setIP(address);
    }
}

// ============================================================================
// State Query
// ============================================================================

uint64_t VirtualMachine::getIP() const {
    // Return IP of active CPU
    if (activeCPUIndex_ >= 0 && activeCPUIndex_ < static_cast<int>(cpus_.size())) {
        if (cpus_[activeCPUIndex_].cpu) {
            return cpus_[activeCPUIndex_].cpu->getIP();
        }
    }
    return 0;
}

const CPUState& VirtualMachine::getCPUState() const {
    // Return state of active CPU
    if (activeCPUIndex_ >= 0 && activeCPUIndex_ < static_cast<int>(cpus_.size())) {
        if (cpus_[activeCPUIndex_].cpu) {
            return cpus_[activeCPUIndex_].cpu->getState();
        }
    }
    throw std::runtime_error("No active CPU");
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
// Register Access (active CPU)
// ============================================================================

uint64_t VirtualMachine::readGR(size_t index) const {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        throw std::runtime_error("No active CPU");
    }
    return cpus_[activeCPUIndex_].cpu->readGR(index);
}

void VirtualMachine::writeGR(size_t index, uint64_t value) {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        throw std::runtime_error("No active CPU");
    }
    cpus_[activeCPUIndex_].cpu->writeGR(index, value);
}

// ============================================================================
// Multi-CPU Management
// ============================================================================

size_t VirtualMachine::getCPUCount() const {
    return cpus_.size();
}

int VirtualMachine::getActiveCPUIndex() const {
    return activeCPUIndex_;
}

bool VirtualMachine::setActiveCPU(int cpuIndex) {
    if (!isValidCPUIndex(cpuIndex)) {
        LOG_ERROR("Invalid CPU index: " + std::to_string(cpuIndex));
        return false;
    }
    
    CPUContext& ctx = cpus_[cpuIndex];
    if (!ctx.enabled) {
        LOG_ERROR("CPU " + std::to_string(cpuIndex) + " is not enabled");
        return false;
    }
    
    if (ctx.state != CPUExecutionState::RUNNING && ctx.state != CPUExecutionState::IDLE) {
        LOG_ERROR("CPU " + std::to_string(cpuIndex) + " is not in runnable state");
        return false;
    }
    
    activeCPUIndex_ = cpuIndex;
    LOG_INFO("Active CPU set to " + std::to_string(cpuIndex));
    return true;
}

const CPUState& VirtualMachine::getCPUState(int cpuIndex) const {
    if (!isValidCPUIndex(cpuIndex)) {
        throw std::out_of_range("Invalid CPU index");
    }
    if (!cpus_[cpuIndex].cpu) {
        throw std::runtime_error("CPU not initialized");
    }
    return cpus_[cpuIndex].cpu->getState();
}

uint64_t VirtualMachine::readGR(int cpuIndex, size_t regIndex) const {
    if (!isValidCPUIndex(cpuIndex)) {
        throw std::out_of_range("Invalid CPU index");
    }
    if (!cpus_[cpuIndex].cpu) {
        throw std::runtime_error("CPU not initialized");
    }
    return cpus_[cpuIndex].cpu->readGR(regIndex);
}

void VirtualMachine::writeGR(int cpuIndex, size_t regIndex, uint64_t value) {
    if (!isValidCPUIndex(cpuIndex)) {
        throw std::out_of_range("Invalid CPU index");
    }
    if (!cpus_[cpuIndex].cpu) {
        throw std::runtime_error("CPU not initialized");
    }
    cpus_[cpuIndex].cpu->writeGR(regIndex, value);
}

uint64_t VirtualMachine::getIP(int cpuIndex) const {
    if (!isValidCPUIndex(cpuIndex)) {
        throw std::out_of_range("Invalid CPU index");
    }
    if (!cpus_[cpuIndex].cpu) {
        throw std::runtime_error("CPU not initialized");
    }
    return cpus_[cpuIndex].cpu->getIP();
}

void VirtualMachine::setIP(int cpuIndex, uint64_t address) {
    if (!isValidCPUIndex(cpuIndex)) {
        throw std::out_of_range("Invalid CPU index");
    }
    if (!cpus_[cpuIndex].cpu) {
        throw std::runtime_error("CPU not initialized");
    }
    cpus_[cpuIndex].cpu->setIP(address);
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
    
    std::cout << "CPUs: " << cpus_.size() << "\n";
    std::cout << "Active CPU: " << activeCPUIndex_ << "\n";
    std::cout << "Total Cycles Executed: " << cyclesExecuted_ << "\n";
    std::cout << "Debugger Attached: " << (debuggerAttached_ ? "Yes" : "No") << "\n";
    std::cout << "Breakpoints: " << breakpoints_.size() << "\n";
    
    if (!breakpoints_.empty()) {
        std::cout << "  Breakpoint addresses:\n";
        for (uint64_t addr : breakpoints_) {
            std::cout << "    0x" << std::hex << addr << std::dec << "\n";
        }
    }
    
    std::cout << "\n";
    
    // CPU states
    for (size_t i = 0; i < cpus_.size(); i++) {
        const CPUContext& ctx = cpus_[i];
        std::cout << "--- CPU " << i << " ";
        if (static_cast<int>(i) == activeCPUIndex_) {
            std::cout << "(ACTIVE) ";
        }
        std::cout << "---\n";
        std::cout << "  State: " << ctx.getStateName() << "\n";
        std::cout << "  Enabled: " << (ctx.enabled ? "Yes" : "No") << "\n";
        std::cout << "  Cycles: " << ctx.cyclesExecuted << "\n";
        std::cout << "  Instructions: " << ctx.instructionsExecuted << "\n";
        
        if (ctx.cpu) {
            std::cout << "  IP: 0x" << std::hex << ctx.cpu->getIP() << std::dec << "\n";
            // Optionally dump full CPU state for active CPU
            if (static_cast<int>(i) == activeCPUIndex_) {
                ctx.cpu->dump();
            }
        } else {
            std::cout << "  CPU: Not initialized\n";
        }
        std::cout << "\n";
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

CPUContext* VirtualMachine::getCPUContext(int cpuIndex) {
    if (!isValidCPUIndex(cpuIndex)) {
        return nullptr;
    }
    return &cpus_[cpuIndex];
}

const CPUContext* VirtualMachine::getCPUContext(int cpuIndex) const {
    if (!isValidCPUIndex(cpuIndex)) {
        return nullptr;
    }
    return &cpus_[cpuIndex];
}

ICPU* VirtualMachine::getActiveCPU() {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        return nullptr;
    }
    return cpus_[activeCPUIndex_].cpu.get();
}

const ICPU* VirtualMachine::getActiveCPU() const {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        return nullptr;
    }
    return cpus_[activeCPUIndex_].cpu.get();
}

uint64_t VirtualMachine::getConsoleBaseAddress() const {
    return consoleDevice_ ? consoleDevice_->GetBaseAddress() : 0;
}

uint64_t VirtualMachine::getTimerBaseAddress() const {
    return timerDevice_ ? timerDevice_->GetBaseAddress() : 0;
}

BasicInterruptController* VirtualMachine::getInterruptController() {
    return interruptController_.get();
}

const BasicInterruptController* VirtualMachine::getInterruptController() const {
    return interruptController_.get();
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool VirtualMachine::checkBreakpoint(uint64_t address) const {
    return breakpoints_.find(address) != breakpoints_.end();
}

bool VirtualMachine::initializeSubsystems() {
    // Verify shared subsystems exist
    if (!memory_ || !decoder_ || !scheduler_) {
        LOG_ERROR("Shared subsystems not created");
        return false;
    }
    
    // Verify all CPUs are initialized
    for (auto& ctx : cpus_) {
        if (!ctx.cpu) {
            LOG_ERROR("CPU not initialized");
            return false;
        }
        // Reset CPU to initial state
        ctx.cpu->reset();
    }
    
    // Memory and decoder don't need explicit initialization
    // They're already in a valid state from construction
    
    return true;
}

bool VirtualMachine::createCPUs(size_t numCPUs) {
    try {
        cpus_.reserve(numCPUs);
        
        for (size_t i = 0; i < numCPUs; i++) {
            CPUContext ctx(static_cast<uint32_t>(i));
            
            // Create CPU instance with shared memory and decoder
            ctx.cpu = std::make_unique<CPU>(*memory_, *decoder_);
            ctx.enabled = false;  // Will be enabled in init()
            ctx.state = CPUExecutionState::IDLE;
            
            // Move into vector
            cpus_.push_back(std::move(ctx));
            
            std::ostringstream oss;
            oss << "Created CPU " << i;
            LOG_INFO(oss.str());
        }
        
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create CPUs: " + std::string(e.what()));
        return false;
    }
}

bool VirtualMachine::isValidCPUIndex(int cpuIndex) const {
    return cpuIndex >= 0 && cpuIndex < static_cast<int>(cpus_.size());
}

// ============================================================================
// Extended Execution Control - Scheduler Interface
// ============================================================================

bool VirtualMachine::stepCPU(int cpuIndex) {
    // Check VM state
    if (state_ == VMState::UNINITIALIZED) {
        LOG_ERROR("Cannot step CPU - VM not initialized");
        return false;
    }
    
    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step CPU - VM in error state");
        return false;
    }
    
    // Validate CPU index
    if (!isValidCPUIndex(cpuIndex)) {
        LOG_ERROR("Invalid CPU index: " + std::to_string(cpuIndex));
        return false;
    }
    
    try {
        // Set running state if needed
        if (state_ != VMState::RUNNING && state_ != VMState::DEBUG_BREAK) {
            state_ = VMState::RUNNING;
        }
        
        // Prepare CPU context pointers
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }
        
        // Use scheduler to step specific CPU
        bool success = scheduler_->stepCPU(cpuPtrs, cpuIndex);

        if (success && timerDevice_) {
            timerDevice_->Tick(1);
        }
        
        if (success) {
            cyclesExecuted_++;
        }
        
        return success;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepCPU: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return false;
    }
}

int VirtualMachine::stepAllCPUs() {
    // Check VM state
    if (state_ == VMState::UNINITIALIZED) {
        LOG_ERROR("Cannot step all CPUs - VM not initialized");
        return 0;
    }
    
    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step all CPUs - VM in error state");
        return 0;
    }
    
    try {
        // Set running state if needed
        if (state_ != VMState::RUNNING && state_ != VMState::DEBUG_BREAK) {
            state_ = VMState::RUNNING;
        }
        
        // Prepare CPU context pointers
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }
        
        // Use scheduler to step all CPUs
        int successCount = scheduler_->stepAllCPUs(cpuPtrs);
        
        // Update global cycle count (each successful CPU step = 1 cycle)
        cyclesExecuted_ += successCount;
        
        LOG_DEBUG("Stepped " + std::to_string(successCount) + " CPUs");
        return successCount;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepAllCPUs: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return 0;
    }
}

uint64_t VirtualMachine::stepQuantum(int cpuIndex, uint64_t bundleCount) {
    // Check VM state
    if (state_ == VMState::UNINITIALIZED) {
        LOG_ERROR("Cannot step quantum - VM not initialized");
        return 0;
    }
    
    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step quantum - VM in error state");
        return 0;
    }
    
    // Validate CPU index
    if (!isValidCPUIndex(cpuIndex)) {
        LOG_ERROR("Invalid CPU index: " + std::to_string(cpuIndex));
        return 0;
    }
    
    try {
        // Set running state if needed
        if (state_ != VMState::RUNNING && state_ != VMState::DEBUG_BREAK) {
            state_ = VMState::RUNNING;
        }
        
        // Prepare CPU context pointers
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }
        
        // Use scheduler to step quantum
        uint64_t bundlesExecuted = scheduler_->stepQuantum(cpuPtrs, cpuIndex, bundleCount);

        if (bundlesExecuted > 0 && timerDevice_) {
            timerDevice_->Tick(bundlesExecuted * 3);
        }
        
        // Update global cycle count (each bundle = 3 instructions)
        cyclesExecuted_ += (bundlesExecuted * 3);
        
        std::ostringstream oss;
        oss << "Executed " << bundlesExecuted << " bundles (" 
            << (bundlesExecuted * 3) << " instructions) on CPU " << cpuIndex;
        LOG_DEBUG(oss.str());
        
        return bundlesExecuted;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepQuantum: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return 0;
    }
}

uint64_t VirtualMachine::getQuantumSize() const {
    if (!scheduler_) {
        return 0;
    }
    return scheduler_->getQuantumSize();
}

void VirtualMachine::setQuantumSize(uint64_t bundleCount) {
    if (!scheduler_) {
        LOG_ERROR("Cannot set quantum size - scheduler not initialized");
        return;
    }
    
    scheduler_->setQuantumSize(bundleCount);
    
    std::ostringstream oss;
    oss << "Quantum size set to " << bundleCount << " bundles (" 
        << (bundleCount * 3) << " instructions)";
    LOG_INFO(oss.str());
}

} // namespace ia64

