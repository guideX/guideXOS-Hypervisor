#include "VirtualMachine.h"
#include "SimpleCPUScheduler.h"
#include "cpu.h"
#include "memory.h"
#include "decoder.h"
#include "cpu_state.h"
#include "Console.h"
#include "InterruptController.h"
#include "Timer.h"
#include "FramebufferDevice.h"
#include "logger.h"
#include "ISAPluginRegistry.h"
#include "IA64ISAPlugin.h"
#include "VMSnapshot.h"
#include "VMSnapshotManager.h"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualMachine::VirtualMachine(size_t memorySize, size_t numCPUs, const std::string& isaName)
: memory_(nullptr),
  decoder_(nullptr),
  scheduler_(nullptr),
  interruptController_(nullptr),
  consoleDevice_(nullptr),
  timerDevice_(nullptr),
  state_(VMState::CREATED),
  bootStateMachine_(),
  bootTraceSystem_(10000),  // 10K event circular buffer
  panicDetector_(),
  lastPanic_(),
  activeCPUIndex_(-1),
  debuggerAttached_(false),
  nextMemoryBreakpointId_(1),
  maxSnapshotHistory_(256),
  cyclesExecuted_(0),
  snapshotManager_(std::make_unique<VMSnapshotManager>()) {
    
if (numCPUs == 0) {
    throw std::invalid_argument("VM must have at least 1 CPU");
}
    
try {
    // Initialize panic detector with boot trace system
    panicDetector_.setBootTraceSystem(&bootTraceSystem_);
    
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
    
    // Create framebuffer device (default 640x480 at standard VGA region)
    framebufferDevice_ = std::make_unique<FramebufferDevice>();

    memory_->RegisterDevice(consoleDevice_.get());
    memory_->RegisterDevice(timerDevice_.get());
    memory_->RegisterDevice(framebufferDevice_.get());
        
    // Create CPU contexts with specified ISA
    if (!createCPUs(numCPUs, isaName)) {
        throw std::runtime_error("Failed to create CPU contexts with ISA: " + isaName);
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
        oss << "VirtualMachine created with " << memorySize << " bytes of memory, " 
            << numCPUs << " CPU(s), ISA: " << isaName;
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
    
// Record memory allocation in boot trace
bootTraceSystem_.recordMemoryAllocation(memory_->GetTotalSize(), cyclesExecuted_);
    
// Transition to POWER_ON state
if (!bootStateMachine_.powerOn()) {
    LOG_ERROR("Failed to power on VM");
    return false;
}
    
    // Record power on event
    bootTraceSystem_.recordEvent(BootTraceEventType::POWER_ON, cyclesExecuted_, 0,
                                 VMBootState::POWER_ON, "Virtual machine powered on");
    
    try {
        // Transition to firmware init
        bootStateMachine_.transition(VMBootState::FIRMWARE_INIT, "Starting firmware initialization");
        bootTraceSystem_.recordBootStateChange(VMBootState::POWER_ON, VMBootState::FIRMWARE_INIT,
                                              cyclesExecuted_, 0, "Starting firmware initialization");
        
        // Initialize all subsystems
        if (!initializeSubsystems()) {
            LOG_ERROR("Failed to initialize subsystems");
            state_ = VMState::ERROR;
            bootStateMachine_.transition(VMBootState::BOOT_FAILED, "Subsystem initialization failed");
            bootTraceSystem_.recordEvent(BootTraceEventType::BOOT_FAILURE, cyclesExecuted_, 0,
                                        VMBootState::BOOT_FAILED, "Subsystem initialization failed");
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
        instructionBreakpoints_.clear();
        memoryBreakpoints_.clear();
        snapshotHistory_.clear();
        nextMemoryBreakpointId_ = 1;
        lastBreakReason_.clear();
        debuggerAttached_ = false;
        
        state_ = VMState::STOPPED;
        
        // Boot state machine now ready for kernel load
        // This will be advanced when loadProgram is called
        LOG_INFO("VirtualMachine initialized successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during initialization: " + std::string(e.what()));
        state_ = VMState::ERROR;
        bootStateMachine_.transition(VMBootState::BOOT_FAILED, "Exception: " + std::string(e.what()));
        bootTraceSystem_.recordEvent(BootTraceEventType::BOOT_FAILURE, cyclesExecuted_, 0,
                                    VMBootState::BOOT_FAILED, "Exception: " + std::string(e.what()));
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
        
        // Reset all IO devices
        if (timerDevice_) {
            timerDevice_->Reset();
        }
        
        if (consoleDevice_) {
            consoleDevice_->Reset();
        }
        
        if (interruptController_) {
            interruptController_->Reset();
        }
        
        // Clear all memory watchpoints (from debugger)
        for (const auto& entry : memoryBreakpoints_) {
            memory_->GetMMU().UnregisterWatchpoint(entry.first);
        }
        memoryBreakpoints_.clear();
        nextMemoryBreakpointId_ = 1;
        
        // Note: We don't clear memory - this allows program to persist
        // If you want to clear memory, call memory_->Clear() here
        
        // Reset execution state
        cyclesExecuted_ = 0;
        snapshotHistory_.clear();
        lastBreakReason_.clear();
        
        // Keep instruction breakpoints and debugger attachment
        // as they represent debugging configuration, not runtime state
        
        // Reset boot state machine and transition to POWER_ON
        bootStateMachine_.reset();
        if (!bootStateMachine_.powerOn()) {
            LOG_ERROR("Failed to transition to POWER_ON state during reset");
            state_ = VMState::ERROR;
            return false;
        }
        
        state_ = VMState::STOPPED;
        LOG_INFO("VirtualMachine reset successfully to POWER_ON state");
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
    state_ = VMState::STOPPED;
        
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
    if (state_ == VMState::CREATED) {
        LOG_ERROR("Cannot step - VM not initialized");
        return false;
    }

    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step - VM in error state");
        return false;
    }

    if (state_ != VMState::RUNNING && state_ != VMState::PAUSED) {
        state_ = VMState::RUNNING;
    }

    try {
        std::vector<CPUContext*> cpuPtrs;
        for (auto& ctx : cpus_) {
            cpuPtrs.push_back(&ctx);
        }

        const int cpuIndex = scheduler_->selectNextCPU(cpuPtrs);
        if (!isValidCPUIndex(cpuIndex)) {
            LOG_DEBUG("No runnable CPU found");
            state_ = VMState::STOPPED;
            return false;
        }

        return stepCPU(cpuIndex);
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
    if (state_ == VMState::CREATED) {
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
            state_ = VMState::STOPPED;
            break;
        }
        
        // Execute one step
        const uint64_t cyclesBeforeStep = cyclesExecuted_;
        bool shouldContinue = step();
        cyclesThisRun += (cyclesExecuted_ - cyclesBeforeStep);
        
        if (!shouldContinue) {
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
    
    // Transition to kernel load state if coming from firmware
    VMBootState currentBootState = bootStateMachine_.getCurrentState();
    if (currentBootState == VMBootState::FIRMWARE_INIT) {
        bootStateMachine_.transition(VMBootState::BOOTLOADER_EXEC, "Bootloader starting");
        bootTraceSystem_.recordBootStateChange(currentBootState, VMBootState::BOOTLOADER_EXEC,
                                              cyclesExecuted_, 0, "Bootloader starting");
        
        bootStateMachine_.transition(VMBootState::KERNEL_LOAD, "Loading kernel into memory");
        bootTraceSystem_.recordBootStateChange(VMBootState::BOOTLOADER_EXEC, VMBootState::KERNEL_LOAD,
                                              cyclesExecuted_, 0, "Loading kernel into memory");
    }
    
    try {
        // Write program to memory
        memory_->Write(loadAddress, data, size);
        
        // Record kernel load in boot trace
        bootTraceSystem_.recordKernelLoad(loadAddress, size, cyclesExecuted_);
        
        LOG_INFO("Program loaded successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load program: " + std::string(e.what()));
        bootStateMachine_.transition(VMBootState::BOOT_FAILED, "Kernel load failed");
        bootTraceSystem_.recordEvent(BootTraceEventType::BOOT_FAILURE, cyclesExecuted_, 0,
                                    VMBootState::BOOT_FAILED, "Kernel load failed: " + std::string(e.what()));
        return false;
    }
}

void VirtualMachine::setEntryPoint(uint64_t address) {
std::ostringstream oss;
oss << "Setting entry point to 0x" << std::hex << address << std::dec;
LOG_INFO(oss.str());
    
// Set entry point for active CPU (or CPU 0 if no active CPU)
int cpuIndex = (activeCPUIndex_ >= 0) ? activeCPUIndex_ : 0;
if (cpuIndex < static_cast<int>(cpus_.size()) && cpus_[cpuIndex].cpu) {
    uint64_t oldIP = cpus_[cpuIndex].cpu->getIP();
    cpus_[cpuIndex].cpu->setIP(address);
    uint64_t newIP = cpus_[cpuIndex].cpu->getIP();
        
    std::ostringstream debugOss;
    debugOss << "IP changed: 0x" << std::hex << oldIP << " -> 0x" << newIP << std::dec;
    LOG_DEBUG(debugOss.str());
        
        // Transition to kernel entry if we're in KERNEL_LOAD state
        VMBootState currentBootState = bootStateMachine_.getCurrentState();
        if (currentBootState == VMBootState::KERNEL_LOAD) {
            bootStateMachine_.transition(VMBootState::KERNEL_ENTRY, "Jumping to kernel entry point");
            bootTraceSystem_.recordBootStateChange(currentBootState, VMBootState::KERNEL_ENTRY,
                                                  cyclesExecuted_, address, "Jumping to kernel entry point");
            bootTraceSystem_.recordKernelEntry(address, cyclesExecuted_);
        }
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
    instructionBreakpoints_.clear();
    for (const auto& entry : memoryBreakpoints_) {
        memory_->GetMMU().UnregisterWatchpoint(entry.first);
    }
    memoryBreakpoints_.clear();
    lastBreakReason_.clear();
}

bool VirtualMachine::setBreakpoint(uint64_t address) {
    if (!debuggerAttached_) {
        LOG_ERROR("Cannot set breakpoint - no debugger attached");
        return false;
    }
    
    if (instructionBreakpoints_.find(address) != instructionBreakpoints_.end()) {
        LOG_WARN("Breakpoint already exists at 0x" + std::to_string(address));
        return false;
    }

    instructionBreakpoints_[address].address = address;
    LOG_INFO("Breakpoint set at 0x" + std::to_string(address));
    return true;
}

bool VirtualMachine::clearBreakpoint(uint64_t address) {
    if (!debuggerAttached_) {
        LOG_ERROR("Cannot clear breakpoint - no debugger attached");
        return false;
    }
    
    size_t removed = instructionBreakpoints_.erase(address);
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
        case VMState::CREATED:   std::cout << "CREATED\n"; break;
        case VMState::STOPPED:   std::cout << "STOPPED\n"; break;
        case VMState::RUNNING:   std::cout << "RUNNING\n"; break;
        case VMState::PAUSED:    std::cout << "PAUSED\n"; break;
        case VMState::ERROR:     std::cout << "ERROR\n"; break;
        default:                 std::cout << "OTHER (" << static_cast<int>(state_) << ")\n"; break;
    }
    
    std::cout << "CPUs: " << cpus_.size() << "\n";
    std::cout << "Active CPU: " << activeCPUIndex_ << "\n";
    std::cout << "Total Cycles Executed: " << cyclesExecuted_ << "\n";
    std::cout << "Debugger Attached: " << (debuggerAttached_ ? "Yes" : "No") << "\n";
    std::cout << "Instruction Breakpoints: " << instructionBreakpoints_.size() << "\n";
    std::cout << "Memory Breakpoints: " << memoryBreakpoints_.size() << "\n";
    
    if (!instructionBreakpoints_.empty()) {
        std::cout << "  Breakpoint addresses:\n";
        for (const auto& entry : instructionBreakpoints_) {
            const uint64_t addr = entry.first;
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

CPU& VirtualMachine::getCPU() {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        if (!cpus_.empty()) {
            return *dynamic_cast<CPU*>(cpus_[0].cpu.get());
        }
        throw std::runtime_error("No CPU available");
    }
    return *dynamic_cast<CPU*>(cpus_[activeCPUIndex_].cpu.get());
}

const CPU& VirtualMachine::getCPU() const {
    if (activeCPUIndex_ < 0 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        if (!cpus_.empty()) {
            return *dynamic_cast<const CPU*>(cpus_[0].cpu.get());
        }
        throw std::runtime_error("No CPU available");
    }
    return *dynamic_cast<const CPU*>(cpus_[activeCPUIndex_].cpu.get());
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
// Console Output Access
// ============================================================================

std::vector<std::string> VirtualMachine::getConsoleOutput() const {
    if (!consoleDevice_) {
        return std::vector<std::string>();
    }
    return consoleDevice_->getAllOutputLines();
}

std::vector<std::string> VirtualMachine::getConsoleOutput(size_t startLine, size_t count) const {
    if (!consoleDevice_) {
        return std::vector<std::string>();
    }
    return consoleDevice_->getOutputLines(startLine, count);
}

std::string VirtualMachine::getRecentConsoleOutput(size_t maxBytes) const {
    if (!consoleDevice_) {
        return std::string();
    }
    return consoleDevice_->getRecentOutput(maxBytes);
}

std::vector<std::string> VirtualMachine::getConsoleOutputSince(size_t lineNumber) const {
    if (!consoleDevice_) {
        return std::vector<std::string>();
    }
    return consoleDevice_->getOutputSince(lineNumber);
}

size_t VirtualMachine::getConsoleLineCount() const {
    if (!consoleDevice_) {
        return 0;
    }
    return consoleDevice_->getOutputLineCount();
}

uint64_t VirtualMachine::getConsoleTotalBytes() const {
    if (!consoleDevice_) {
        return 0;
    }
    return consoleDevice_->getTotalBytesWritten();
}

void VirtualMachine::clearConsoleOutput() {
    if (consoleDevice_) {
        consoleDevice_->clearOutput();
    }
}

// ============================================================================
// Framebuffer Access
// ============================================================================

uint16_t* VirtualMachine::getFramebuffer() {
    if (!framebufferDevice_) {
        return nullptr;
    }
    
    // The framebuffer device stores data as BGRA32, but we need to
    // return it as uint16_t* for VGA text mode (80x25 characters)
    // Each character is 2 bytes: character code + attribute
    // VGA text mode is at 0xB8000 in real mode, but we'll use our framebuffer
    
    // For now, we'll reinterpret the framebuffer as uint16_t*
    // This allows direct access to the first 80*25*2 = 4000 bytes
    const uint8_t* fb = framebufferDevice_->GetFramebuffer();
    return const_cast<uint16_t*>(reinterpret_cast<const uint16_t*>(fb));
}

const uint16_t* VirtualMachine::getFramebuffer() const {
    if (!framebufferDevice_) {
        return nullptr;
    }
    
    const uint8_t* fb = framebufferDevice_->GetFramebuffer();
    return reinterpret_cast<const uint16_t*>(fb);
}

bool VirtualMachine::setConditionalBreakpoint(uint64_t address, const DebugCondition& condition) {
    if (!setBreakpoint(address)) {
        return false;
    }

    instructionBreakpoints_[address].condition = condition;
    return true;
}

size_t VirtualMachine::setMemoryBreakpoint(uint64_t addressStart, uint64_t addressEnd, WatchpointType type, const DebugCondition& condition) {
    if (!debuggerAttached_) {
        LOG_ERROR("Cannot set memory breakpoint - no debugger attached");
        return 0;
    }

    if (addressEnd <= addressStart) {
        LOG_ERROR("Invalid memory breakpoint range");
        return 0;
    }

    MemoryBreakpoint breakpoint;
    breakpoint.id = nextMemoryBreakpointId_++;
    breakpoint.addressStart = addressStart;
    breakpoint.addressEnd = addressEnd;
    breakpoint.type = type;
    breakpoint.condition = condition;
    breakpoint.enabled = true;

    memoryBreakpoints_[breakpoint.id] = breakpoint;
    registerMemoryBreakpointHook(breakpoint);
    return breakpoint.id;
}

bool VirtualMachine::clearMemoryBreakpoint(size_t breakpointId) {
    const auto it = memoryBreakpoints_.find(breakpointId);
    if (it == memoryBreakpoints_.end()) {
        return false;
    }

    memory_->GetMMU().UnregisterWatchpoint(breakpointId);
    memoryBreakpoints_.erase(it);
    return true;
}

bool VirtualMachine::stepBundle() {
    if (!isValidCPUIndex(activeCPUIndex_)) {
        if (cpus_.empty()) {
            return false;
        }
        activeCPUIndex_ = 0;
    }

    CPU* cpu = getCPUForIndex(activeCPUIndex_);
    if (cpu == nullptr) {
        return false;
    }

    do {
        if (!stepCPU(activeCPUIndex_)) {
            return false;
        }
    } while (!cpu->isAtBundleBoundary());

    return true;
}

std::vector<uint8_t> VirtualMachine::inspectMemory(uint64_t address, size_t size) const {
    std::vector<uint8_t> buffer(size, 0);
    if (size == 0) {
        return buffer;
    }

    const uint8_t* raw = memory_->GetRawData();
    if (address + size > memory_->GetTotalSize()) {
        throw std::out_of_range("Memory inspection range out of bounds");
    }

    std::copy(raw + address, raw + address + size, buffer.begin());
    return buffer;
}

uint64_t VirtualMachine::inspectRegister(size_t index) const {
    return readGR(index);
}

bool VirtualMachine::inspectPredicate(size_t index) const {
    const CPU* cpu = getCPUForIndex(activeCPUIndex_);
    if (cpu == nullptr) {
        throw std::runtime_error("No active CPU");
    }

    return cpu->readPR(index);
}

DebuggerControlFlowState VirtualMachine::inspectControlFlow() const {
    const CPU* cpu = getCPUForIndex(activeCPUIndex_);
    if (cpu == nullptr) {
        throw std::runtime_error("No active CPU");
    }

    DebuggerControlFlowState controlFlow;
    controlFlow.instructionPointer = cpu->getIP();
    controlFlow.currentFrameMarker = cpu->getCFM();
    controlFlow.processorStatus = cpu->getState().GetPSR();
    controlFlow.currentSlot = cpu->getCurrentSlot();
    controlFlow.atBundleBoundary = cpu->isAtBundleBoundary();
    return controlFlow;
}

DebuggerSnapshot VirtualMachine::createSnapshot() const {
    DebuggerSnapshot snapshot;
    snapshot.memory = memory_->CreateSnapshot();
    snapshot.vmState = state_;
    snapshot.activeCPUIndex = activeCPUIndex_;
    snapshot.cyclesExecuted = cyclesExecuted_;
    snapshot.cpus.reserve(cpus_.size());

    for (const auto& ctx : cpus_) {
        DebuggerSnapshot::CPURecord record;
        if (ctx.cpu) {
            record.runtime = ctx.cpu->createSnapshot();
        }
        record.executionState = ctx.state;
        record.cyclesExecuted = ctx.cyclesExecuted;
        record.instructionsExecuted = ctx.instructionsExecuted;
        record.idleCycles = ctx.idleCycles;
        record.enabled = ctx.enabled;
        record.lastActivationTime = ctx.lastActivationTime;
        snapshot.cpus.push_back(record);
    }

    return snapshot;
}

bool VirtualMachine::restoreSnapshot(const DebuggerSnapshot& snapshot) {
    if (snapshot.cpus.size() != cpus_.size()) {
        return false;
    }

    memory_->RestoreSnapshot(snapshot.memory);
    for (size_t i = 0; i < cpus_.size(); ++i) {
        if (cpus_[i].cpu) {
            cpus_[i].cpu->restoreSnapshot(snapshot.cpus[i].runtime);
        }
        cpus_[i].state = snapshot.cpus[i].executionState;
        cpus_[i].cyclesExecuted = snapshot.cpus[i].cyclesExecuted;
        cpus_[i].instructionsExecuted = snapshot.cpus[i].instructionsExecuted;
        cpus_[i].idleCycles = snapshot.cpus[i].idleCycles;
        cpus_[i].enabled = snapshot.cpus[i].enabled;
        cpus_[i].lastActivationTime = snapshot.cpus[i].lastActivationTime;
    }

    state_ = VMState::PAUSED;
    activeCPUIndex_ = snapshot.activeCPUIndex;
    cyclesExecuted_ = snapshot.cyclesExecuted;
    lastBreakReason_ = "rewind";
    return true;
}

bool VirtualMachine::rewindToLastSnapshot() {
    if (snapshotHistory_.empty()) {
        return false;
    }

    const DebuggerSnapshot snapshot = snapshotHistory_.back();
    snapshotHistory_.pop_back();
    return restoreSnapshot(snapshot);
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool VirtualMachine::checkBreakpoint(uint64_t address) const {
    const auto it = instructionBreakpoints_.find(address);
    return it != instructionBreakpoints_.end() && it->second.enabled;
}

bool VirtualMachine::evaluateCondition(const DebugCondition& condition, const CPU& cpu) const {
    return evaluateCondition(condition, cpu.getState());
}

bool VirtualMachine::evaluateCondition(const DebugCondition& condition, const CPUState& cpuState) const {
    if (condition.target == DebugConditionTarget::NONE || condition.op == DebugConditionOperator::ANY) {
        return true;
    }

    uint64_t lhs = 0;
    switch (condition.target) {
        case DebugConditionTarget::GENERAL_REGISTER:
            lhs = cpuState.GetGR(condition.index);
            break;
        case DebugConditionTarget::PREDICATE_REGISTER:
            lhs = cpuState.GetPR(condition.index) ? 1ULL : 0ULL;
            break;
        case DebugConditionTarget::INSTRUCTION_POINTER:
            lhs = cpuState.GetIP();
            break;
        case DebugConditionTarget::NONE:
        default:
            return true;
    }

    switch (condition.op) {
        case DebugConditionOperator::ANY:
            return true;
        case DebugConditionOperator::EQUAL:
            return lhs == condition.value;
        case DebugConditionOperator::NOT_EQUAL:
            return lhs != condition.value;
        case DebugConditionOperator::GREATER:
            return lhs > condition.value;
        case DebugConditionOperator::GREATER_OR_EQUAL:
            return lhs >= condition.value;
        case DebugConditionOperator::LESS:
            return lhs < condition.value;
        case DebugConditionOperator::LESS_OR_EQUAL:
            return lhs <= condition.value;
        case DebugConditionOperator::IS_TRUE:
            return lhs != 0;
        case DebugConditionOperator::IS_FALSE:
            return lhs == 0;
        default:
            return false;
    }
}

size_t VirtualMachine::registerMemoryBreakpointHook(const MemoryBreakpoint& breakpoint) {
    Watchpoint watchpoint(breakpoint.addressStart, breakpoint.addressEnd, breakpoint.type,
        [this, breakpoint](WatchpointContext& context) {
            const CPU* cpu = getCPUForIndex(activeCPUIndex_);
            if (cpu == nullptr || !evaluateCondition(breakpoint.condition, cpu->getState())) {
                return;
            }

            state_ = VMState::PAUSED;
            lastBreakReason_ = "memory breakpoint";
        });

    watchpoint.enabled = breakpoint.enabled;
    watchpoint.description = "vm memory breakpoint";
    return memory_->GetMMU().RegisterWatchpoint(watchpoint);
}

void VirtualMachine::captureSnapshotIfBoundary(int cpuIndex) {
    if (!debuggerAttached_ || !isValidCPUIndex(cpuIndex)) {
        return;
    }

    CPU* cpu = getCPUForIndex(cpuIndex);
    if (cpu == nullptr) {
        return;
    }

    if (!snapshotHistory_.empty() && snapshotHistory_.back().cyclesExecuted == cyclesExecuted_) {
        return;
    }

    snapshotHistory_.push_back(createSnapshot());
    trimSnapshotHistory();
}

void VirtualMachine::trimSnapshotHistory() {
    while (snapshotHistory_.size() > maxSnapshotHistory_) {
        snapshotHistory_.pop_front();
    }
}

CPU* VirtualMachine::getCPUForIndex(int cpuIndex) {
    if (!isValidCPUIndex(cpuIndex) || !cpus_[cpuIndex].cpu) {
        return nullptr;
    }

    return cpus_[cpuIndex].cpu.get();
}

const CPU* VirtualMachine::getCPUForIndex(int cpuIndex) const {
    if (!isValidCPUIndex(cpuIndex) || !cpus_[cpuIndex].cpu) {
        return nullptr;
    }

    return cpus_[cpuIndex].cpu.get();
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
    return createCPUs(numCPUs, "IA-64");  // Default to IA-64 for backward compatibility
}

bool VirtualMachine::createCPUs(size_t numCPUs, const std::string& isaName) {
    try {
        cpus_.reserve(numCPUs);
        
        // Check if we should use ISA plugin architecture
        bool usePlugin = ISAPluginRegistry::instance().isRegistered(isaName);
        
        for (size_t i = 0; i < numCPUs; i++) {
            CPUContext ctx(static_cast<uint32_t>(i));
            
            if (usePlugin) {
                // Create ISA plugin instance
                IA64FactoryData factoryData(decoder_.get());
                auto isaPlugin = ISAPluginRegistry::instance().createISA(isaName, &factoryData);
                
                if (!isaPlugin) {
                    LOG_ERROR("Failed to create ISA plugin: " + isaName);
                    return false;
                }
                
                ctx.isaPlugin = std::move(isaPlugin);
                ctx.cpu = std::make_unique<CPU>(*memory_, *ctx.isaPlugin);
                
                std::ostringstream oss;
                oss << "Created CPU " << i << " with ISA: " << isaName << " (using plugin)";
                LOG_INFO(oss.str());
            } else {
                // Create CPU instance with shared memory and decoder (legacy mode)
                ctx.cpu = std::make_unique<CPU>(*memory_, *decoder_);
                
                std::ostringstream oss;
                oss << "Created CPU " << i << " (legacy mode)";
                LOG_INFO(oss.str());
            }
            
            ctx.enabled = false;  // Will be enabled in init()
            ctx.state = CPUExecutionState::IDLE;
            
            // Move into vector
            cpus_.push_back(std::move(ctx));
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
if (state_ == VMState::CREATED) {
    LOG_ERROR("Cannot step CPU - VM not initialized");
    return false;
}

if (state_ == VMState::ERROR) {
    LOG_ERROR("Cannot step CPU - VM in error state");
    return false;
}

if (!isValidCPUIndex(cpuIndex)) {
    LOG_ERROR("Invalid CPU index: " + std::to_string(cpuIndex));
    return false;
}

    try {
        if (state_ != VMState::RUNNING && state_ != VMState::PAUSED) {
            state_ = VMState::RUNNING;
        }

        CPUContext& ctx = cpus_[cpuIndex];
        CPU* cpu = getCPUForIndex(cpuIndex);
        if (cpu == nullptr) {
            throw std::runtime_error("CPU not initialized");
        }

        activeCPUIndex_ = cpuIndex;
        memory_->GetMMU().SetCPUStateReference(&cpu->getState());

        captureSnapshotIfBoundary(cpuIndex);

        const uint64_t currentIP = cpu->getIP();
        const bool bypassInstructionBreakpoint =
            state_ == VMState::PAUSED &&
            lastBreakReason_ == "instruction breakpoint" &&
            cpu->isAtBundleBoundary();

        if (debuggerAttached_ && !bypassInstructionBreakpoint && cpu->isAtBundleBoundary() &&
            checkBreakpoint(currentIP) && evaluateCondition(instructionBreakpoints_.find(currentIP)->second.condition, *cpu)) {
            state_ = VMState::PAUSED;
            lastBreakReason_ = "instruction breakpoint";
            return false;
        }

        if (bypassInstructionBreakpoint) {
            lastBreakReason_.clear();
            state_ = VMState::RUNNING;
        }

        const bool success = cpu->step();
        if (!success) {
            // CPU step failed - this could be a kernel panic condition
            // Capture panic state
            KernelPanic panic = panicDetector_.captureExceptionPanic(
                "CPU execution failed", cpu->getState(), Bundle(), 0,
                cyclesExecuted_, cyclesExecuted_
            );
            lastPanic_ = std::make_unique<KernelPanic>(panic);
            
            state_ = VMState::ERROR;
            bootStateMachine_.transition(VMBootState::KERNEL_PANIC, "CPU execution failed");
            return false;
        }

        ctx.cyclesExecuted++;
        ctx.instructionsExecuted++;
        cyclesExecuted_++;
        scheduler_->onCPUExecuted(cpuIndex, 1);
        
        // Record instruction milestone every 10000 instructions
        if (bootTraceSystem_.isEnabled() && ctx.instructionsExecuted % 10000 == 0) {
            bootTraceSystem_.recordInstructionMilestone(ctx.instructionsExecuted, cyclesExecuted_, currentIP);
        }

        if (timerDevice_) {
            timerDevice_->Tick(1);
        }

        if (state_ == VMState::PAUSED) {
            return false;
        }

        state_ = VMState::RUNNING;
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepCPU: " + std::string(e.what()));
        
        // Capture exception as kernel panic
        CPU* cpu = getCPUForIndex(cpuIndex);
        if (cpu) {
            KernelPanic panic = panicDetector_.captureExceptionPanic(
                e.what(), cpu->getState(), Bundle(), 0,
                cyclesExecuted_, cyclesExecuted_
            );
            lastPanic_ = std::make_unique<KernelPanic>(panic);
        }
        
        state_ = VMState::ERROR;
        bootStateMachine_.transition(VMBootState::KERNEL_PANIC, std::string("Exception: ") + e.what());
        return false;
    }
}

int VirtualMachine::stepAllCPUs() {
    if (state_ == VMState::CREATED) {
        LOG_ERROR("Cannot step all CPUs - VM not initialized");
        return 0;
    }

    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step all CPUs - VM in error state");
        return 0;
    }

    try {
        if (state_ != VMState::RUNNING && state_ != VMState::PAUSED) {
            state_ = VMState::RUNNING;
        }



        int successCount = 0;
        for (size_t i = 0; i < cpus_.size(); ++i) {
            if (!cpus_[i].enabled) {
                continue;
            }

            if (!stepCPU(static_cast<int>(i))) {
                if (state_ == VMState::PAUSED) {
                    break;
                }
                continue;
            }

            successCount++;
        }

        LOG_DEBUG("Stepped " + std::to_string(successCount) + " CPUs");
        return successCount;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepAllCPUs: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return 0;
    }
}

uint64_t VirtualMachine::stepQuantum(int cpuIndex, uint64_t bundleCount) {
    if (state_ == VMState::CREATED) {
        LOG_ERROR("Cannot step quantum - VM not initialized");
        return 0;
    }

    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step quantum - VM in error state");
        return 0;
    }

    if (!isValidCPUIndex(cpuIndex)) {

        LOG_ERROR("Invalid CPU index: " + std::to_string(cpuIndex));
        return 0;
    }

    try {
        if (state_ != VMState::RUNNING && state_ != VMState::PAUSED) {
            state_ = VMState::RUNNING;
        }

        activeCPUIndex_ = cpuIndex;

        uint64_t bundlesExecuted = 0;
        for (; bundlesExecuted < bundleCount; ++bundlesExecuted) {
            if (!stepBundle()) {
                break;
            }
        }

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

// ============================================================================
// VM Snapshotting - Full State Capture
// ============================================================================

std::string VirtualMachine::createFullSnapshot(const std::string& name, const std::string& description) {
    if (!snapshotManager_) {
        LOG_ERROR("Snapshot manager not initialized");
        return "";
    }
    
    std::string snapshotId = snapshotManager_->createFullSnapshot(*this, name, description);
    
    if (!snapshotId.empty()) {
        std::ostringstream oss;
        oss << "Created full snapshot: " << (name.empty() ? snapshotId : name);
        LOG_INFO(oss.str());
    }
    
    return snapshotId;
}

std::string VirtualMachine::createDeltaSnapshot(const std::string& parentSnapshotId, 
                                                 const std::string& name, 
                                                 const std::string& description) {
    if (!snapshotManager_) {
        LOG_ERROR("Snapshot manager not initialized");
        return "";
    }
    
    std::string snapshotId = snapshotManager_->createDeltaSnapshot(*this, parentSnapshotId, name, description);
    
    if (!snapshotId.empty()) {
        std::ostringstream oss;
        oss << "Created delta snapshot: " << (name.empty() ? snapshotId : name);
        LOG_INFO(oss.str());
        
        // Log compression stats
        auto stats = snapshotManager_->getCompressionStats(snapshotId);
        if (stats.fullSnapshotSize > 0) {
            std::ostringstream statsOss;
            statsOss << "Delta snapshot compression: " 
                     << stats.deltaSnapshotSize << " bytes (delta) vs " 
                     << stats.fullSnapshotSize << " bytes (full) - "
                     << (stats.compressionRatio * 100.0) << "% of full size";
            LOG_INFO(statsOss.str());
        }
    }
    
    return snapshotId;
}

bool VirtualMachine::restoreFromSnapshot(const std::string& snapshotId) {
if (!snapshotManager_) {
    LOG_ERROR("Snapshot manager not initialized");
    return false;
}
    
// Get the snapshot
VMStateSnapshot snapshot = snapshotManager_->resolveSnapshot(snapshotId);
    
    if (snapshot.metadata.snapshotId.empty()) {
        LOG_ERROR("Snapshot not found: " + snapshotId);
        return false;
    }
    
    // Restore the snapshot
    bool success = restoreVMSnapshot(snapshot);
    
    if (success) {
        std::ostringstream oss;
        oss << "Restored snapshot: " << snapshot.metadata.snapshotName;
        LOG_INFO(oss.str());
    } else {
        LOG_ERROR("Failed to restore snapshot: " + snapshotId);
    }
    
    return success;
}

VMStateSnapshot VirtualMachine::captureVMSnapshot() const {
    VMStateSnapshot snapshot;
    
    // Capture CPU states
    for (size_t i = 0; i < cpus_.size(); ++i) {
        CPUSnapshotRecord cpuRecord;
        cpuRecord.cpuId = static_cast<uint32_t>(i);
        
        const CPUContext& ctx = cpus_[i];
        if (ctx.cpu) {
            cpuRecord.architecturalState = ctx.cpu->getState();
            cpuRecord.executionState = ctx.state;
            cpuRecord.cyclesExecuted = ctx.cyclesExecuted;
            cpuRecord.instructionsExecuted = ctx.instructionsExecuted;
            cpuRecord.idleCycles = ctx.idleCycles;
            cpuRecord.enabled = ctx.enabled;
            cpuRecord.lastActivationTime = ctx.lastActivationTime;
            
            // Capture runtime state
            auto runtimeSnapshot = ctx.cpu->createSnapshot();
            cpuRecord.currentSlot = runtimeSnapshot.currentSlot;
            cpuRecord.bundleValid = runtimeSnapshot.bundleValid;
            cpuRecord.pendingInterrupts = runtimeSnapshot.pendingInterrupts;
            cpuRecord.interruptVectorBase = runtimeSnapshot.interruptVectorBase;
        }
        
        snapshot.cpus.push_back(cpuRecord);
    }
    
    snapshot.activeCPUIndex = activeCPUIndex_;
    
    // Capture memory state
    if (memory_) {
        snapshot.memoryState = memory_->CreateSnapshot();
    }
    
    // Capture console device state
    if (consoleDevice_) {
        snapshot.consoleState = consoleDevice_->createSnapshot();
    }
    
    // Capture timer device state
    if (timerDevice_) {
        snapshot.timerState = timerDevice_->createSnapshot();
    }
    
    // Capture interrupt controller state
    if (interruptController_) {
        snapshot.interruptControllerState = interruptController_->createSnapshot();
    }
    
    // Capture VM state
    snapshot.vmStateValue = static_cast<uint32_t>(state_);
    snapshot.totalCyclesExecuted = cyclesExecuted_;
    snapshot.quantumSize = getQuantumSize();
    
    return snapshot;
}

bool VirtualMachine::restoreVMSnapshot(const VMStateSnapshot& snapshot) {
    // Validate snapshot
    if (snapshot.cpus.size() != cpus_.size()) {
        LOG_ERROR("Snapshot CPU count mismatch");
        return false;
    }
    
    // Restore memory state
    if (memory_) {
        memory_->RestoreSnapshot(snapshot.memoryState);
    }
    
    // Restore CPU states
    for (size_t i = 0; i < cpus_.size() && i < snapshot.cpus.size(); ++i) {
        CPUContext& ctx = cpus_[i];
        const CPUSnapshotRecord& cpuRecord = snapshot.cpus[i];
        
        if (ctx.cpu) {
            // Restore architectural state
            CPURuntimeStateSnapshot runtimeState;
            runtimeState.architecturalState = cpuRecord.architecturalState;
            runtimeState.currentSlot = cpuRecord.currentSlot;
            runtimeState.bundleValid = cpuRecord.bundleValid;
            runtimeState.pendingInterrupts = cpuRecord.pendingInterrupts;
            runtimeState.interruptVectorBase = cpuRecord.interruptVectorBase;
            
            ctx.cpu->restoreSnapshot(runtimeState);
        }
        
        // Restore execution context
        ctx.state = cpuRecord.executionState;
        ctx.cyclesExecuted = cpuRecord.cyclesExecuted;
        ctx.instructionsExecuted = cpuRecord.instructionsExecuted;
        ctx.idleCycles = cpuRecord.idleCycles;
        ctx.enabled = cpuRecord.enabled;
        ctx.lastActivationTime = cpuRecord.lastActivationTime;
    }
    
    // Restore active CPU
    activeCPUIndex_ = snapshot.activeCPUIndex;
    
    // Restore console device state
    if (consoleDevice_) {
        consoleDevice_->restoreSnapshot(snapshot.consoleState);
    }
    
    // Restore timer device state
    if (timerDevice_) {
        timerDevice_->restoreSnapshot(snapshot.timerState);
    }
    
    // Restore interrupt controller state
    if (interruptController_) {
        interruptController_->restoreSnapshot(snapshot.interruptControllerState);
    }
    
    // Restore VM state
    state_ = static_cast<VMState>(snapshot.vmStateValue);
    cyclesExecuted_ = snapshot.totalCyclesExecuted;
    
    if (scheduler_ && snapshot.quantumSize > 0) {
        scheduler_->setQuantumSize(snapshot.quantumSize);
    }
    
    return true;
}

// ============================================================================
// Kernel Panic Detection
// ============================================================================

KernelPanic VirtualMachine::triggerKernelPanic(KernelPanicReason reason, const std::string& description) {
    LOG_ERROR("KERNEL PANIC: " + description);
    
    // Get active CPU state
    const CPU* cpu = getCPUForIndex(activeCPUIndex_);
    if (!cpu) {
        // Create empty panic if no active CPU
        KernelPanic panic;
        panic.reason = reason;
        panic.description = description;
        panic.timestamp = cyclesExecuted_;
        panic.cyclesExecuted = cyclesExecuted_;
        panic.bundleValid = false;
        lastPanic_ = std::make_unique<KernelPanic>(panic);
        
        // Halt the VM
        state_ = VMState::ERROR;
        bootStateMachine_.transition(VMBootState::KERNEL_PANIC, description);
        
        return panic;
    }
    
    // Capture last executed bundle (if available)
    Bundle lastBundle;
    size_t lastSlot = 0;
    bool bundleValid = false;
    
    try {
        // Try to fetch current bundle (16 bytes for IA-64 bundle)
        uint64_t bundleAddr = cpu->getIP();
        uint8_t bundleData[16];
        memory_->Read(bundleAddr & ~0xFULL, bundleData, 16);
        lastBundle = decoder_->DecodeBundle(bundleData);
        bundleValid = true;
    } catch (...) {
        // Bundle fetch failed, continue with invalid bundle
    }
    
    // Trigger panic with CPU state
    KernelPanic panic = panicDetector_.triggerPanic(
        reason, description, cpu->getState(), lastBundle, lastSlot,
        cyclesExecuted_, cyclesExecuted_
    );
    
    lastPanic_ = std::make_unique<KernelPanic>(panic);
    
    // Halt the VM
    state_ = VMState::ERROR;
    bootStateMachine_.transition(VMBootState::KERNEL_PANIC, description);
    
    return panic;
}

} // namespace ia64


