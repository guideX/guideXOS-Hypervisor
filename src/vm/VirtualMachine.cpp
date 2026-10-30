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
#include "ProcessorInterruptBlock.h"
#include "logger.h"
#include "ISAPluginRegistry.h"
#include "IA64ISAPlugin.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64RadixMMU.h"
#include "PPC64VirtualTerminal.h"
#include "VMSnapshot.h"
#include "VMSnapshotManager.h"
#include "VMCheckpoint.h"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <limits>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualMachine::VirtualMachine(size_t memorySize, size_t numCPUs, const std::string& isaName)
: memory_(nullptr),
  ppc64PartitionManager_(nullptr),
  processorInterruptBlock_(nullptr),
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
  isaName_(isaName),
  snapshotManager_(std::make_unique<VMSnapshotManager>()) {
    
if (numCPUs == 0) {
    throw std::invalid_argument("VM must have at least 1 CPU");
}
if (isaName == "PPC64" && numCPUs != 1) {
    throw std::invalid_argument("PPC64 VM profile currently supports exactly one virtual CPU");
}
if (!ISAPluginRegistry::instance().isRegistered(isaName)) {
    throw std::invalid_argument("Unsupported or unregistered VM ISA: " + isaName);
}
    
try {
    // Initialize panic detector with boot trace system
    panicDetector_.setBootTraceSystem(&bootTraceSystem_);
    
    // Create shared subsystems
    memory_ = std::make_unique<Memory>(memorySize);
    decoder_ = std::make_unique<InstructionDecoder>();
    if (isaName == "PPC64") {
        ppc64PartitionManager_ = std::make_unique<ppc64::PPC64PartitionManager>();
        ppc64VirtualTerminal_ = std::make_unique<ppc64::PPC64VirtualTerminal>();
    }
        
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

    if (isaName != "PPC64") {
      processorInterruptBlock_ = std::make_unique<ProcessorInterruptBlock>(
        [this](uint8_t id, uint8_t eid, uint8_t vector, uint8_t deliveryMode,
               bool redirect) {
            if (redirect || deliveryMode != ProcessorInterruptBlock::kDeliveryModeInt) {
                return false;
            }

            const uint16_t targetLid =
                static_cast<uint16_t>((static_cast<uint16_t>(id) << 8) | eid);
            for (CPUContext& context : cpus_) {
                if (!context.isaPlugin || !context.cpu) {
                    continue;
                }
                auto* ia64 = dynamic_cast<IA64ISAPlugin*>(context.isaPlugin.get());
                if (ia64 == nullptr) {
                    continue;
                }
                const uint16_t localLid = static_cast<uint16_t>(
                    ia64->getCPUState().GetCR(IA64_CR_LID) & 0xffffULL);
                if (localLid != targetLid) {
                    continue;
                }
                ia64->queueInterrupt(vector);
                if (context.state == CPUExecutionState::WAITING) {
                    context.state = CPUExecutionState::RUNNING;
                }
                return true;
            }
            return false;
        });

      memory_->RegisterDevice(consoleDevice_.get());
      memory_->RegisterDevice(timerDevice_.get());
      memory_->RegisterDevice(framebufferDevice_.get());
      memory_->RegisterDevice(processorInterruptBlock_.get());
    }
        
    // Create CPU contexts with specified ISA
    if (!createCPUs(numCPUs, isaName)) {
        throw std::runtime_error("Failed to create CPU contexts with ISA: " + isaName);
    }
    if (isaName == "PPC64") {
        getPPC64ISAPlugin()->attachVirtualTerminal(ppc64VirtualTerminal_.get());
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

VirtualMachine::VirtualMachine(
    size_t memorySize, size_t numCPUs, const std::string& isaName,
    const ppc64::PPC64PlatformConfiguration& platformConfiguration)
    : VirtualMachine(memorySize, numCPUs, isaName) {
    std::string reason;
    if (!configurePPC64Platform(platformConfiguration, &reason)) {
        throw std::invalid_argument("Invalid PPC64 platform configuration: " + reason);
    }
}

VirtualMachine::~VirtualMachine() {
    // Halt execution if running
    if (state_ == VMState::RUNNING) {
        halt();
    }

    if (isaName_ == "PPC64") {
        getPPC64ISAPlugin()->deactivatePartitionContext();
    }
    if (ppc64PartitionManager_) {
        ppc64PartitionManager_->destroyAllPartitions();
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

        if (isaName_ == "PPC64") {
            auto* plugin = getPPC64ISAPlugin();
            plugin->deactivatePartitionContext();
            if (ppc64PartitionManager_) {
                ppc64PartitionManager_->destroyAllPartitions();
            }
            if (ppc64VirtualTimeBase_) {
                ppc64VirtualTimeBase_->reset();
            }
            if (ppc64VirtualTerminal_) {
                ppc64VirtualTerminal_->reset();
            }
            if (ppc64PlatformPrepared_) {
                GuestRealWriteSegment dtb;
                dtb.address = ppc64GuestMemoryLayout_.fdtBase;
                dtb.bytes = ppc64PlatformDTB_;
                memory_->WriteGuestRealTransaction({dtb});
                plugin->writeGPR(3, ppc64GuestMemoryLayout_.fdtBase);
                plugin->writeGPR(4, ppc64GuestMemoryLayout_.imageBase);
                plugin->writeGPR(5, 0);
                plugin->setMSR(ppc64::MSR_SF);
                plugin->setPC(ppc64PlatformEntryAddress_);
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
        lastExecutionDiagnostic_.clear();
        
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
if (state_ == VMState::RUNNING || state_ == VMState::PAUSED) {
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

bool VirtualMachine::pause() {
    if (state_ != VMState::RUNNING) {
        return false;
    }
    state_ = VMState::PAUSED;
    return true;
}

bool VirtualMachine::resume() {
    if (state_ != VMState::PAUSED && state_ != VMState::STOPPED) {
        return false;
    }
    for (CPUContext& context : cpus_) {
        if (context.enabled && context.state != CPUExecutionState::HALTED &&
            context.state != CPUExecutionState::ERROR) {
            context.state = CPUExecutionState::RUNNING;
        }
    }
    if (activeCPUIndex_ < 0 && !cpus_.empty()) {
        activeCPUIndex_ = 0;
    }
    state_ = VMState::RUNNING;
    if (lastBreakReason_ != "instruction breakpoint") {
        lastBreakReason_.clear();
    }
    return true;
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
if (isaName_ == "PPC64" && (address & 3ULL) != 0) {
    throw std::invalid_argument("PPC64 entry point must be 4-byte aligned");
}
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
    if (isaName_ == "PPC64" && (address & 3ULL) != 0) {
        LOG_ERROR("PPC64 instruction breakpoints must be 4-byte aligned");
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
    if (cpus_[cpuIndex].cpu->getArchitecture() == ISAArchitecture::PPC64 &&
        (address & 3ULL) != 0) {
        throw std::invalid_argument("PPC64 NIP must be 4-byte aligned");
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

ppc64::PPC64ISAPlugin* VirtualMachine::getPPC64ISAPlugin(int cpuIndex) {
    if (isaName_ != "PPC64") {
        throw std::logic_error("PPC64 CPU access requested from a non-PPC64 VM");
    }
    if (!isValidCPUIndex(cpuIndex) || !cpus_[cpuIndex].isaPlugin) {
        throw std::out_of_range("PPC64 CPU index is invalid or uninitialized");
    }
    auto* plugin = dynamic_cast<ppc64::PPC64ISAPlugin*>(cpus_[cpuIndex].isaPlugin.get());
    if (!plugin) {
        throw std::logic_error("VM architecture identity does not match its ISA plugin");
    }
    return plugin;
}

const ppc64::PPC64ISAPlugin* VirtualMachine::getPPC64ISAPlugin(int cpuIndex) const {
    return const_cast<VirtualMachine*>(this)->getPPC64ISAPlugin(cpuIndex);
}

ppc64::PPC64PartitionManager* VirtualMachine::getPPC64PartitionManager() {
    if (!ppc64PartitionManager_) {
        throw std::logic_error("PPC64 partition manager is unavailable for this VM");
    }
    return ppc64PartitionManager_.get();
}

const ppc64::PPC64PartitionManager* VirtualMachine::getPPC64PartitionManager() const {
    if (!ppc64PartitionManager_) {
        throw std::logic_error("PPC64 partition manager is unavailable for this VM");
    }
    return ppc64PartitionManager_.get();
}

PPC64RegisterView VirtualMachine::getPPC64RegisterView(int cpuIndex) const {
    const ppc64::PPC64ISAPlugin* plugin = getPPC64ISAPlugin(cpuIndex);
    PPC64RegisterView view;
    for (size_t i = 0; i < 32; ++i) {
        view.gpr[i] = plugin->readGPR(i);
    }
    view.nip = plugin->getNIP();
    view.lr = plugin->getLR();
    view.ctr = plugin->getCTR();
    view.cr = plugin->getCR();
    view.xer = plugin->getXER();
    view.msr = plugin->getMSR();
    view.pidr = plugin->getPIDR();
    view.srr0 = plugin->getSRR0();
    view.srr1 = plugin->getSRR1();
    view.dar = plugin->getDAR();
    view.dsisr = plugin->getDSISR();
    view.hsrr0 = plugin->getHSRR0();
    view.hsrr1 = plugin->getHSRR1();
    view.hdar = plugin->getHDAR();
    view.hdsisr = plugin->getHDSISR();
    return view;
}

bool VirtualMachine::configurePPC64Platform(
    const ppc64::PPC64PlatformConfiguration& configuration,
    std::string* error) {
    const auto reject = [error](const std::string& reason) {
        if (error != nullptr) *error = reason;
        return false;
    };
    if (isaName_ != "PPC64" || cpus_.size() != 1 ||
        (state_ != VMState::CREATED && state_ != VMState::STOPPED) ||
        ppc64PlatformPrepared_ || cyclesExecuted_ != 0) {
        return reject("platform configuration requires a new, stopped PPC64 VM");
    }
    std::string reason;
    if (!ppc64::validatePPC64PlatformConfiguration(
            configuration, memory_->GetTotalSize(), reason)) {
        return reject(reason);
    }
    std::unique_ptr<ppc64::PPC64VirtualTimeBase> clock;
    try {
        clock = std::make_unique<ppc64::PPC64VirtualTimeBase>(configuration);
    } catch (const std::exception& exception) {
        return reject(exception.what());
    }
    auto* plugin = getPPC64ISAPlugin();
    if (!ppc64VirtualTerminal_ ||
        !ppc64VirtualTerminal_->reconfigure(configuration.virtualTerminalId,
                                             configuration.virtualTerminalEnabled)) {
        return reject("virtual-terminal identity cannot change while channel state exists");
    }
    ppc64PlatformConfiguration_ = configuration;
    ppc64VirtualTimeBase_ = std::move(clock);
    ppc64PlatformEnabled_ = true;
    plugin->attachVirtualTimeBase(ppc64VirtualTimeBase_.get());
    if (error != nullptr) error->clear();
    return true;
}

bool VirtualMachine::preparePPC64Platform(
    const std::vector<uint8_t>& image, uint64_t imageAddress,
    uint64_t entryAddress, std::string* error) {
    const auto reject = [error](const std::string& reason) {
        if (error != nullptr) *error = reason;
        return false;
    };
    if (!ppc64PlatformEnabled_ || ppc64VirtualTimeBase_ == nullptr ||
        isaName_ != "PPC64" || cpus_.size() != 1 || state_ != VMState::STOPPED ||
        ppc64PlatformPrepared_) {
        return reject("platform preparation requires a configured, stopped PPC64 VM");
    }
    if (image.empty()) return reject("guest image is empty");
    if (ppc64PlatformConfiguration_.entryMode !=
        ppc64::PPC64GuestEntryMode::DIRECT_FDT) {
        return reject("firmware-assisted entry is not implemented");
    }
    const uint64_t ramSize = memory_->GetTotalSize();
    std::string reason;
    if (!ppc64::validatePPC64PlatformConfiguration(
            ppc64PlatformConfiguration_, ramSize, reason)) {
        return reject(reason);
    }
    ppc64::PPC64ISAPlugin* plugin = getPPC64ISAPlugin();
    if (plugin->hasActivePartitionContext()) {
        return reject("direct-FDT preparation requires the root partition context");
    }
    if ((imageAddress & 3ULL) != 0 || (entryAddress & 3ULL) != 0 ||
        image.size() > std::numeric_limits<uint64_t>::max() - imageAddress) {
        return reject("guest image or entry alignment/range is invalid");
    }

    ppc64::PPC64GuestMemoryLayout layout;
    layout.ramBase = 0;
    layout.ramSize = ramSize;
    layout.imageBase = imageAddress;
    layout.imageSize = static_cast<uint64_t>(image.size());
    layout.entryAddress = entryAddress;
    if (ppc64PlatformConfiguration_.requestedFdtAddress != 0) {
        layout.fdtBase = ppc64PlatformConfiguration_.requestedFdtAddress;
    } else {
        layout.fdtBase = (ramSize - ppc64PlatformConfiguration_.maximumFdtSize) & ~7ULL;
    }
    layout.fdtSize = ppc64PlatformConfiguration_.maximumFdtSize;
    if (layout.fdtBase > ramSize ||
        ppc64PlatformConfiguration_.maximumFdtSize > ramSize - layout.fdtBase) {
        return reject("FDT placement is outside configured guest RAM");
    }
    layout.reservations = ppc64PlatformConfiguration_.reservedRanges;

    const auto appendReservation = [&](uint64_t base, uint64_t size,
                                       const std::string& purpose,
                                       bool guestAccessible) -> bool {
        if (size == 0 || size > std::numeric_limits<uint64_t>::max() - base ||
            base > ramSize || size > ramSize - base) {
            return false;
        }
        for (ppc64::PPC64MemoryRange& existing : layout.reservations) {
            if (existing.base == base && existing.size == size) {
                existing.guestAccessible = existing.guestAccessible && guestAccessible;
                if (existing.purpose.empty()) existing.purpose = purpose;
                return true;
            }
            if (base < existing.base + existing.size &&
                existing.base < base + size) return false;
        }
        layout.reservations.push_back({base, size, purpose, guestAccessible});
        return true;
    };
    if (ppc64PartitionManager_ && ppc64PartitionManager_->isConfigured()) {
        const uint64_t tableSize = 1ULL << (
            static_cast<unsigned>(ppc64PartitionManager_->partitionTableSizeExponent()) + 12U);
        if (!appendReservation(ppc64PartitionManager_->partitionTableBase(),
                               tableSize, "PAPR partition table", true)) {
            return reject("partition-table reservation conflicts with another platform range");
        }
    }
    if (plugin->hasRadixTranslationConfiguration()) {
        const auto& radix = plugin->getRadixTranslationConfiguration();
        const uint64_t processTableSize = 1ULL << (
            static_cast<unsigned>(radix.processTableSizeExponent) + 12U);
        if (!appendReservation(radix.processTableGuestRealBase, processTableSize,
                               "PAPR process table", true)) {
            return reject("process-table reservation conflicts with another platform range");
        }
    }
    if (plugin->hasHypervisorMemoryConfiguration() &&
        !appendReservation(plugin->getHypervisorMemoryBase(),
                           plugin->getHypervisorMemorySize(),
                           "protected hypervisor handler backing", false)) {
        return reject("protected hypervisor handler conflicts with another platform range");
    }
    for (const ppc64::PPC64MemoryRange& reservation : layout.reservations) {
        if (imageAddress < reservation.base + reservation.size &&
            reservation.base < imageAddress + static_cast<uint64_t>(image.size())) {
            return reject("guest image overlaps a reserved or protected range");
        }
    }
    if (!appendReservation(imageAddress, static_cast<uint64_t>(image.size()),
                           "loaded guest image", true)) {
        return reject("guest image overlaps a reserved or protected range");
    }
    for (const ppc64::PPC64MemoryRange& reservation : layout.reservations) {
        if (reservation.base < layout.fdtBase + layout.fdtSize &&
            layout.fdtBase < reservation.base + reservation.size) {
            return reject("maximum FDT placement overlaps a reserved guest range");
        }
    }

    // Construct the FDT memory list by excluding backing that is explicitly
    // inaccessible to the guest (not merely reserved from its allocator).
    std::vector<ppc64::PPC64MemoryRange> inaccessible;
    for (const auto& reservation : layout.reservations) {
        if (!reservation.guestAccessible) inaccessible.push_back(reservation);
    }
    std::sort(inaccessible.begin(), inaccessible.end(), [](const auto& left, const auto& right) {
        return left.base < right.base;
    });
    uint64_t memoryCursor = layout.ramBase;
    for (const auto& hole : inaccessible) {
        if (memoryCursor < hole.base) {
            layout.guestMemoryRanges.push_back({memoryCursor, hole.base - memoryCursor,
                                                "guest accessible RAM", true});
        }
        memoryCursor = hole.base + hole.size;
    }
    const uint64_t ramEnd = layout.ramBase + layout.ramSize;
    if (memoryCursor < ramEnd) {
        layout.guestMemoryRanges.push_back({memoryCursor, ramEnd - memoryCursor,
                                            "guest accessible RAM", true});
    }
    if (layout.guestMemoryRanges.empty()) {
        return reject("protected reservations leave no guest-visible RAM");
    }
    if (!ppc64::validatePPC64GuestMemoryLayout(layout, reason)) {
        return reject(reason);
    }

    std::vector<uint8_t> dtb;
    if (!ppc64::PPC64FDTBuilder::build(
            ppc64PlatformConfiguration_, layout, dtb, reason)) {
        return reject(reason);
    }
    layout.fdtSize = static_cast<uint64_t>(dtb.size());
    if (!appendReservation(layout.fdtBase, layout.fdtSize, "flattened device tree", true)) {
        return reject("FDT reservation conflicts with a reserved guest range");
    }
    if (!ppc64::validatePPC64GuestMemoryLayout(layout, reason) ||
        !ppc64::PPC64FDTBuilder::build(
            ppc64PlatformConfiguration_, layout, dtb, reason)) {
        return reject(reason.empty() ? "final FDT validation failed" : reason);
    }
    if (layout.fdtSize != dtb.size()) {
        layout.fdtSize = static_cast<uint64_t>(dtb.size());
        for (auto& reservation : layout.reservations) {
            if (reservation.purpose == "flattened device tree") {
                reservation.size = layout.fdtSize;
            }
        }
        if (!ppc64::validatePPC64GuestMemoryLayout(layout, reason) ||
            !ppc64::PPC64FDTBuilder::build(
                ppc64PlatformConfiguration_, layout, dtb, reason)) {
            return reject(reason.empty() ? "FDT size did not stabilize" : reason);
        }
    }

    std::vector<GuestRealWriteSegment> writes(2);
    writes[0].address = imageAddress;
    writes[0].bytes = image;
    writes[1].address = layout.fdtBase;
    writes[1].bytes = dtb;
    try {
        memory_->WriteGuestRealTransaction(writes);
    } catch (const std::exception& exception) {
        return reject(std::string("guest image/FDT staging failed: ") + exception.what());
    }

    // Commit architectural state only after all input, memory and FDT checks
    // have succeeded. These direct plugin setters cannot fail.
    ppc64VirtualTimeBase_->reset();
    plugin->writeGPR(3, layout.fdtBase);
    plugin->writeGPR(4, layout.imageBase);
    plugin->writeGPR(5, 0);
    plugin->setMSR(ppc64::MSR_SF);
    plugin->setPC(entryAddress);
    ppc64GuestMemoryLayout_ = std::move(layout);
    ppc64PlatformDTB_ = std::move(dtb);
    ppc64PlatformEntryAddress_ = entryAddress;
    ppc64PlatformPrepared_ = true;
    if (error != nullptr) error->clear();
    return true;
}

ppc64::PPC64PlatformConfiguration
VirtualMachine::getPPC64PlatformConfiguration() const {
    if (!ppc64PlatformEnabled_) {
        throw std::logic_error("PPC64 platform is not configured for this VM");
    }
    return ppc64PlatformConfiguration_;
}

ppc64::PPC64PlatformSnapshot VirtualMachine::getPPC64PlatformSnapshot() const {
    ppc64::PPC64PlatformSnapshot snapshot;
    snapshot.enabled = ppc64PlatformEnabled_;
    if (!snapshot.enabled) return snapshot;
    snapshot.configuration = ppc64PlatformConfiguration_;
    snapshot.prepared = ppc64PlatformPrepared_;
    snapshot.memoryLayout = ppc64GuestMemoryLayout_;
    snapshot.currentTimebase = ppc64VirtualTimeBase_->read();
    return snapshot;
}

uint32_t VirtualMachine::getPPC64VirtualTerminalId() const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_) {
        throw std::logic_error("PAPRvty is only available on PPC64 VMs");
    }
    return ppc64VirtualTerminal_->terminalId();
}

ppc64::PPC64VirtualTerminalSnapshot
VirtualMachine::getPPC64TerminalSnapshot() const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_) {
        throw std::logic_error("PAPRvty is only available on PPC64 VMs");
    }
    return ppc64VirtualTerminal_->createSnapshot();
}

bool VirtualMachine::injectPPC64TerminalInput(
    const std::vector<uint8_t>& bytes, uint16_t lpid) {
    return injectPPC64TerminalInputWithStatus(bytes, lpid) ==
           ppc64::PPC64TerminalResult::SUCCESS;
}

ppc64::PPC64TerminalResult
VirtualMachine::injectPPC64TerminalInputWithStatus(
    const std::vector<uint8_t>& bytes, uint16_t lpid) {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_ ||
        (lpid != 0 && (!ppc64PartitionManager_ ||
                       !ppc64PartitionManager_->findPartition(lpid)))) {
        return ppc64::PPC64TerminalResult::PARAMETER;
    }
    return ppc64VirtualTerminal_->injectInputWithStatus(
        lpid, bytes.data(), bytes.size());
}

std::vector<uint8_t> VirtualMachine::readPPC64TerminalOutput(
    uint16_t lpid, size_t maximumCount) const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_ ||
        (lpid != 0 && (!ppc64PartitionManager_ ||
                       !ppc64PartitionManager_->findPartition(lpid)))) {
        return {};
    }
    return ppc64VirtualTerminal_->readOutput(lpid, maximumCount);
}

std::vector<uint8_t> VirtualMachine::drainPPC64TerminalOutput(
    uint16_t lpid, size_t maximumCount) {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_ ||
        (lpid != 0 && (!ppc64PartitionManager_ ||
                       !ppc64PartitionManager_->findPartition(lpid)))) {
        return {};
    }
    return ppc64VirtualTerminal_->drainOutput(lpid, maximumCount);
}

size_t VirtualMachine::getPPC64TerminalPendingInputBytes(uint16_t lpid) const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_ ||
        (lpid != 0 && (!ppc64PartitionManager_ ||
                       !ppc64PartitionManager_->findPartition(lpid)))) {
        return 0;
    }
    return ppc64VirtualTerminal_->pendingInputBytes(lpid);
}

size_t VirtualMachine::getPPC64TerminalPendingOutputBytes(uint16_t lpid) const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_ ||
        (lpid != 0 && (!ppc64PartitionManager_ ||
                       !ppc64PartitionManager_->findPartition(lpid)))) {
        return 0;
    }
    return ppc64VirtualTerminal_->pendingOutputBytes(lpid);
}

size_t VirtualMachine::getPPC64TerminalQueueCapacity() const {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_) {
        throw std::logic_error("PAPRvty is only available on PPC64 VMs");
    }
    return ppc64::PPC64_TERMINAL_QUEUE_CAPACITY;
}

void VirtualMachine::clearPPC64TerminalState() {
    if (isaName_ != "PPC64" || !ppc64VirtualTerminal_) {
        throw std::logic_error("PAPRvty is only available on PPC64 VMs");
    }
    ppc64VirtualTerminal_->reset();
}

ppc64::PPC64PlatformCapabilities VirtualMachine::getPPC64PlatformCapabilities() const {
    if (!ppc64PlatformEnabled_) return {};
    auto capabilities = ppc64::getPPC64FoundationCapabilities();
    capabilities.virtualTerminal =
        ppc64PlatformConfiguration_.virtualTerminalEnabled;
    return capabilities;
}

uint64_t VirtualMachine::getPPC64Timebase() const {
    if (!ppc64VirtualTimeBase_) {
        throw std::logic_error("PPC64 time base is not configured for this VM");
    }
    return ppc64VirtualTimeBase_->read();
}

std::string VirtualMachine::getLastExecutionDiagnostic() const {
    if (isaName_ == "PPC64" && !cpus_.empty() && cpus_[0].isaPlugin) {
        const std::string pluginDiagnostic =
            cpus_[0].isaPlugin->getLastExecutionDiagnostic();
        if (!pluginDiagnostic.empty()) {
            return pluginDiagnostic;
        }
    }
    return lastExecutionDiagnostic_;
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
    if (isaName_ == "PPC64" &&
        (condition.target == DebugConditionTarget::PREDICATE_REGISTER ||
         condition.target == DebugConditionTarget::GENERAL_REGISTER && condition.index >= 32)) {
        return false;
    }
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
    if (isaName_ == "PPC64") {
        return false;
    }
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
    if (cpu->getArchitecture() == ISAArchitecture::PPC64) {
        throw std::logic_error("IA-64 CFM/bundle control-flow inspection is unavailable for PPC64; use getPPC64RegisterView()");
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
    if (cpu.getArchitecture() == ISAArchitecture::PPC64) {
        uint64_t lhs = 0;
        switch (condition.target) {
            case DebugConditionTarget::NONE:
                return true;
            case DebugConditionTarget::GENERAL_REGISTER:
                lhs = cpu.readGR(condition.index);
                break;
            case DebugConditionTarget::INSTRUCTION_POINTER:
                lhs = cpu.getIP();
                break;
            case DebugConditionTarget::PREDICATE_REGISTER:
                return false;
        }
        switch (condition.op) {
            case DebugConditionOperator::ANY: return true;
            case DebugConditionOperator::EQUAL: return lhs == condition.value;
            case DebugConditionOperator::NOT_EQUAL: return lhs != condition.value;
            case DebugConditionOperator::GREATER: return lhs > condition.value;
            case DebugConditionOperator::GREATER_OR_EQUAL: return lhs >= condition.value;
            case DebugConditionOperator::LESS: return lhs < condition.value;
            case DebugConditionOperator::LESS_OR_EQUAL: return lhs <= condition.value;
            case DebugConditionOperator::IS_TRUE: return lhs != 0;
            case DebugConditionOperator::IS_FALSE: return lhs == 0;
        }
        return false;
    }
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
        if (!ISAPluginRegistry::instance().isRegistered(isaName)) {
            LOG_ERROR("Requested ISA is not registered: " + isaName);
            return false;
        }
        if (isaName == "PPC64" && numCPUs != 1) {
            LOG_ERROR("PPC64 VM profile requires exactly one CPU");
            return false;
        }
        cpus_.reserve(numCPUs);
        
        for (size_t i = 0; i < numCPUs; i++) {
            CPUContext ctx(static_cast<uint32_t>(i));

            IA64FactoryData ia64FactoryData(decoder_.get());
            void* factoryData = isaName == "IA-64" ? &ia64FactoryData : nullptr;
            auto isaPlugin = ISAPluginRegistry::instance().createISA(isaName, factoryData);
            if (!isaPlugin) {
                LOG_ERROR("Failed to create ISA plugin: " + isaName);
                return false;
            }

            ctx.isaPlugin = std::move(isaPlugin);
            ctx.cpu = std::make_unique<CPU>(*memory_, *ctx.isaPlugin);

            std::ostringstream oss;
            oss << "Created CPU " << i << " with ISA: " << isaName << " (using plugin)";
            LOG_INFO(oss.str());
            
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
        if (cpu->getArchitecture() != ISAArchitecture::PPC64) {
            memory_->GetMMU().SetCPUStateReference(&cpu->getState());
        }

        captureSnapshotIfBoundary(cpuIndex);

        const uint64_t currentIP = cpu->getIP();
        const bool bypassInstructionBreakpoint =
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

        const ISAExecutionResult executionResult = cpu->stepDetailed();
        const bool attemptedInstruction = executionResult != ISAExecutionResult::BREAKPOINT;
        if (attemptedInstruction) {
            ++ctx.cyclesExecuted;
            ++ctx.instructionsExecuted;
            ++cyclesExecuted_;
            if (cpu->getArchitecture() == ISAArchitecture::PPC64 &&
                ppc64VirtualTimeBase_) {
                ppc64VirtualTimeBase_->advanceInstruction();
            }
            scheduler_->onCPUExecuted(cpuIndex, 1);
            if (timerDevice_ && cpu->getArchitecture() != ISAArchitecture::PPC64) {
                timerDevice_->Tick(1);
            }
        }

        if (const IISA* plugin = cpu->getISAPlugin()) {
            const std::string diagnostic = plugin->getLastExecutionDiagnostic();
            if (!diagnostic.empty()) {
                lastExecutionDiagnostic_ = diagnostic;
            }
        }

        if (executionResult == ISAExecutionResult::HALT) {
            ctx.state = CPUExecutionState::HALTED;
            state_ = VMState::STOPPED;
            LOG_INFO("CPU " + std::to_string(cpuIndex) + " halted cleanly");
            return false;
        }
        if (executionResult == ISAExecutionResult::EXCEPTION) {
            const std::string diagnostic = cpu->getISAPlugin()
                ? cpu->getISAPlugin()->getLastExecutionDiagnostic()
                : std::string("CPU execution failed");
            lastExecutionDiagnostic_ = diagnostic.empty() ? "CPU execution failed" : diagnostic;
            ctx.state = CPUExecutionState::ERROR;
            state_ = VMState::ERROR;
            if (cpu->getArchitecture() == ISAArchitecture::IA64) {
                KernelPanic panic = panicDetector_.captureExceptionPanic(
                    lastExecutionDiagnostic_, cpu->getState(), Bundle(), 0,
                    cyclesExecuted_, cyclesExecuted_);
                lastPanic_ = std::make_unique<KernelPanic>(panic);
                bootStateMachine_.transition(VMBootState::KERNEL_PANIC,
                                             "CPU execution failed");
            }
            return false;
        }
        if (executionResult == ISAExecutionResult::BREAKPOINT) {
            state_ = VMState::PAUSED;
            lastBreakReason_ = "ISA breakpoint";
            return false;
        }

        // Record instruction milestone every 10000 instructions
        if (bootTraceSystem_.isEnabled() && ctx.instructionsExecuted % 10000 == 0) {
            bootTraceSystem_.recordInstructionMilestone(ctx.instructionsExecuted, cyclesExecuted_, currentIP);
        }

        if (state_ == VMState::PAUSED) {
            return false;
        }

        state_ = VMState::RUNNING;
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception during stepCPU: " + std::string(e.what()));
        lastExecutionDiagnostic_ = e.what();
        CPU* cpu = getCPUForIndex(cpuIndex);
        if (cpu && cpu->getArchitecture() == ISAArchitecture::IA64) {
            KernelPanic panic = panicDetector_.captureExceptionPanic(
                e.what(), cpu->getState(), Bundle(), 0,
                cyclesExecuted_, cyclesExecuted_
            );
            lastPanic_ = std::make_unique<KernelPanic>(panic);
            bootStateMachine_.transition(VMBootState::KERNEL_PANIC,
                                         std::string("Exception: ") + e.what());
        }
        
        state_ = VMState::ERROR;
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

        if (isaName_ == "PPC64") {
            uint64_t instructionsExecuted = 0;
            while (instructionsExecuted < bundleCount) {
                const uint64_t cyclesBefore = cyclesExecuted_;
                const bool keepGoing = stepCPU(cpuIndex);
                const bool executed = cyclesExecuted_ != cyclesBefore;
                if (executed) {
                    ++instructionsExecuted;
                }
                if (!executed || !keepGoing) {
                    break;
                }
            }
            return instructionsExecuted;
        }

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
    if (isaName_ == "PPC64") {
        oss << "PPC64 instruction quantum set to " << bundleCount << " instructions";
    } else {
        oss << "Quantum size set to " << bundleCount << " bundles ("
            << (bundleCount * 3) << " instructions)";
    }
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
    snapshot.architecture = isaName_;
    
    // Capture CPU states
    for (size_t i = 0; i < cpus_.size(); ++i) {
        CPUSnapshotRecord cpuRecord;
        cpuRecord.cpuId = static_cast<uint32_t>(i);
        cpuRecord.architecture = cpus_[i].isaPlugin
            ? cpus_[i].isaPlugin->getName() : "legacy-IA-64";
        
        const CPUContext& ctx = cpus_[i];
        if (ctx.cpu) {
            if (ctx.cpu->getArchitecture() != ISAArchitecture::PPC64) {
                cpuRecord.architecturalState = ctx.cpu->getState();
            }
            if (ctx.isaPlugin) {
                cpuRecord.isaState = ctx.isaPlugin->serialize_state();
                cpuRecord.lastExecutionDiagnostic =
                    ctx.isaPlugin->getLastExecutionDiagnostic();
            }
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
            cpuRecord.halted = runtimeSnapshot.halted;
            cpuRecord.pendingCallInputs = runtimeSnapshot.pendingCallInputs;
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

    if (isaName_ == "PPC64" && ppc64PartitionManager_) {
        snapshot.ppc64PartitionTableConfigured = ppc64PartitionManager_->isConfigured();
        snapshot.ppc64PartitionTableBase = ppc64PartitionManager_->partitionTableBase();
        snapshot.ppc64PartitionTableSizeExponent =
            ppc64PartitionManager_->partitionTableSizeExponent();
        snapshot.ppc64PartitionSnapshots = ppc64PartitionManager_->serializePartitions();
        const ppc64::PPC64ISAPlugin* plugin = getPPC64ISAPlugin();
        snapshot.ppc64HasActiveLPID = plugin->hasActivePartitionContext();
        snapshot.ppc64ActiveLPID = plugin->getActiveLPID();
        snapshot.ppc64HypervisorMemoryConfigured =
            plugin->hasHypervisorMemoryConfiguration();
        snapshot.ppc64HypervisorMemoryBase = plugin->getHypervisorMemoryBase();
        snapshot.ppc64HypervisorMemorySize = plugin->getHypervisorMemorySize();
        snapshot.ppc64RadixConfigured = plugin->hasRadixTranslationConfiguration();
        if (snapshot.ppc64RadixConfigured) {
            snapshot.ppc64RadixConfiguration =
                plugin->getRadixTranslationConfiguration();
        }
        snapshot.ppc64Platform = getPPC64PlatformSnapshot();
        if (ppc64VirtualTerminal_) {
            snapshot.ppc64VirtualTerminal =
                ppc64VirtualTerminal_->createSnapshot();
        }
    }
    
    return snapshot;
}

bool VirtualMachine::restoreVMSnapshot(const VMStateSnapshot& snapshot) {
    return restoreVMSnapshot(snapshot, false);
}

bool VirtualMachine::restoreVMSnapshot(const VMStateSnapshot& snapshot,
                                       bool authorizeHypervisorState) {
    if (isaName_ == "PPC64") {
        const auto reject = [](const std::string& reason) {
            LOG_ERROR("Rejected PPC64 VM snapshot: " + reason);
            return false;
        };
        if (snapshot.architecture != "PPC64") {
            return reject("architecture identity mismatch");
        }
        if (cpus_.size() != 1 || snapshot.cpus.size() != 1 ||
            snapshot.activeCPUIndex < -1 || snapshot.activeCPUIndex >= 1) {
            return reject("unsupported CPU count or active CPU index");
        }
        if (snapshot.consoleState.baseAddress != consoleDevice_->GetBaseAddress() ||
            snapshot.timerState.baseAddress != timerDevice_->GetBaseAddress()) {
            return reject("VM device configuration mismatch");
        }
        const CPUSnapshotRecord& cpuRecord = snapshot.cpus[0];
        if (cpuRecord.cpuId != 0 || cpuRecord.architecture != "PPC64" ||
            cpuRecord.isaState.empty() || cpuRecord.currentSlot != 0 ||
            cpuRecord.bundleValid) {
            return reject("missing or mismatched PPC64 CPU state");
        }
        if (snapshot.memoryState.data.size() != memory_->GetTotalSize()) {
            return reject("RAM size mismatch");
        }
        if (snapshot.vmStateValue > static_cast<uint32_t>(VMState::RESTORING) ||
            static_cast<uint32_t>(cpuRecord.executionState) >
                static_cast<uint32_t>(CPUExecutionState::ERROR)) {
            return reject("invalid VM or CPU lifecycle state");
        }
        if (snapshot.quantumSize == 0) {
            return reject("instruction quantum must be nonzero");
        }
        if (snapshot.ppc64HasActiveLPID &&
            (!snapshot.ppc64PartitionTableConfigured ||
             snapshot.ppc64ActiveLPID > ppc64::PPC64_POWER9_MAX_LPID)) {
            return reject("active LPID has no valid partition table");
        }
        if (!snapshot.ppc64PartitionTableConfigured &&
            !snapshot.ppc64PartitionSnapshots.empty()) {
            return reject("partition metadata exists without a configured table");
        }
        if (!snapshot.ppc64PartitionTableConfigured &&
            (snapshot.ppc64PartitionTableBase != 0 ||
             snapshot.ppc64PartitionTableSizeExponent != 0)) {
            return reject("partition table fields exist without a configured table");
        }
        if (!snapshot.ppc64HasActiveLPID && snapshot.ppc64ActiveLPID != 0) {
            return reject("inactive partition state contains an active LPID");
        }
        if (!snapshot.ppc64HypervisorMemoryConfigured &&
            (snapshot.ppc64HypervisorMemoryBase != 0 ||
             snapshot.ppc64HypervisorMemorySize != 0)) {
            return reject("hypervisor memory range is inconsistent");
        }
        if (!snapshot.ppc64RadixConfigured &&
            (snapshot.ppc64RadixConfiguration.size != 0 ||
             snapshot.ppc64RadixConfiguration.processTableSizeExponent != 0 ||
             snapshot.ppc64RadixConfiguration.processTableGuestRealBase != 0 ||
             snapshot.ppc64RadixConfiguration.guestRealBase != 0 ||
             snapshot.ppc64RadixConfiguration.hostBackingBase != 0)) {
            return reject("radix configuration is inconsistent");
        }

        if (snapshot.ppc64Platform.enabled != ppc64PlatformEnabled_) {
            return reject("PPC64 platform profile does not match the destination VM");
        }
        if (!ppc64VirtualTerminal_) {
            return reject("PPC64 VM has no virtual-terminal backend");
        }
        const uint32_t expectedTerminalId = snapshot.ppc64Platform.enabled
            ? ppc64PlatformConfiguration_.virtualTerminalId
            : ppc64VirtualTerminal_->terminalId();
        const bool expectedTerminalEnabled = snapshot.ppc64Platform.enabled
            ? ppc64PlatformConfiguration_.virtualTerminalEnabled
            : ppc64VirtualTerminal_->isEnabled();
        if (snapshot.ppc64VirtualTerminal.terminalId != expectedTerminalId ||
            snapshot.ppc64VirtualTerminal.enabled != expectedTerminalEnabled) {
            return reject("virtual-terminal identity or exposure disagrees with the destination FDT profile");
        }
        std::vector<uint8_t> canonicalDTB;
        if (snapshot.ppc64Platform.enabled) {
            const auto& platform = snapshot.ppc64Platform;
            std::string platformReason;
            if (!ppc64::samePPC64PlatformConfiguration(
                    platform.configuration, ppc64PlatformConfiguration_) ||
                !ppc64::validatePPC64PlatformConfiguration(
                    platform.configuration, snapshot.memoryState.data.size(),
                    platformReason)) {
                return reject(platformReason.empty()
                    ? "PPC64 platform configuration mismatch" : platformReason);
            }
            if (platform.prepared) {
                const auto& layout = platform.memoryLayout;
                if (layout.ramBase != 0 ||
                    layout.ramSize != snapshot.memoryState.data.size() ||
                    layout.initrdBase != 0 || layout.initrdSize != 0 ||
                    layout.fdtSize > platform.configuration.maximumFdtSize ||
                    layout.fdtBase > snapshot.memoryState.data.size() ||
                    layout.fdtSize > snapshot.memoryState.data.size() - layout.fdtBase ||
                    !ppc64::validatePPC64GuestMemoryLayout(layout, platformReason)) {
                    return reject(platformReason.empty()
                        ? "PPC64 platform memory layout is inconsistent" : platformReason);
                }
                const auto hasRange = [&layout](uint64_t base, uint64_t size,
                                                bool guestAccessible) {
                    return std::any_of(layout.reservations.begin(),
                        layout.reservations.end(), [&](const auto& range) {
                            return range.base == base && range.size == size &&
                                   range.guestAccessible == guestAccessible;
                        });
                };
                if (!hasRange(layout.imageBase, layout.imageSize, true) ||
                    !hasRange(layout.fdtBase, layout.fdtSize, true)) {
                    return reject("platform checkpoint omits its image or FDT reservation");
                }
                for (const auto& configured : platform.configuration.reservedRanges) {
                    if (!hasRange(configured.base, configured.size,
                                  configured.guestAccessible)) {
                        return reject("platform checkpoint lost a configured memory reservation");
                    }
                }
                if (snapshot.ppc64HypervisorMemoryConfigured &&
                    !hasRange(snapshot.ppc64HypervisorMemoryBase,
                               snapshot.ppc64HypervisorMemorySize, false)) {
                    return reject("platform checkpoint omits protected handler backing");
                }
                if (snapshot.ppc64RadixConfigured) {
                    const auto& radix = snapshot.ppc64RadixConfiguration;
                    if (radix.processTableSizeExponent > 31 ||
                        !hasRange(radix.processTableGuestRealBase,
                            1ULL << (static_cast<unsigned>(radix.processTableSizeExponent) + 12U),
                            true)) {
                        return reject("platform checkpoint process-table reservation is inconsistent");
                    }
                }
                if (snapshot.ppc64PartitionTableConfigured) {
                    if (snapshot.ppc64PartitionTableSizeExponent !=
                        ppc64::PPC64_POWER9_PARTITION_TABLE_PATS) {
                        return reject("platform checkpoint partition-table geometry is unsupported");
                    }
                    const uint64_t tableSize = 1ULL << (
                        static_cast<unsigned>(snapshot.ppc64PartitionTableSizeExponent) + 12U);
                    if (!hasRange(snapshot.ppc64PartitionTableBase, tableSize, true)) {
                        return reject("platform checkpoint partition-table reservation is inconsistent");
                    }
                }
                if (!ppc64::PPC64FDTBuilder::validate(
                        snapshot.memoryState.data.data() + layout.fdtBase,
                        static_cast<size_t>(layout.fdtSize), platformReason) ||
                    !ppc64::PPC64FDTBuilder::build(
                        platform.configuration, layout, canonicalDTB, platformReason) ||
                    canonicalDTB.size() != layout.fdtSize ||
                    !std::equal(canonicalDTB.begin(), canonicalDTB.end(),
                        snapshot.memoryState.data.begin() +
                            static_cast<std::ptrdiff_t>(layout.fdtBase))) {
                    return reject(platformReason.empty()
                        ? "platform checkpoint DTB bytes or metadata are inconsistent"
                        : platformReason);
                }
            } else if (platform.memoryLayout.ramBase != 0 ||
                       platform.memoryLayout.ramSize != 0 ||
                       platform.memoryLayout.imageBase != 0 ||
                       platform.memoryLayout.imageSize != 0 ||
                       platform.memoryLayout.fdtBase != 0 ||
                       platform.memoryLayout.fdtSize != 0 ||
                       platform.memoryLayout.entryAddress != 0 ||
                       platform.memoryLayout.initrdBase != 0 ||
                       platform.memoryLayout.initrdSize != 0 ||
                       !platform.memoryLayout.reservations.empty() ||
                       !platform.memoryLayout.guestMemoryRanges.empty()) {
                return reject("unprepared platform checkpoint contains entry metadata");
            }
        }

        const size_t pageSize = memory_->GetMMU().GetPageSize();
        for (const auto& mapping : snapshot.memoryState.pageTable) {
            const uint64_t virtualAddress = mapping.first;
            const PageEntry& entry = mapping.second;
            if ((virtualAddress & (pageSize - 1U)) != 0 ||
                (entry.physicalAddress & (pageSize - 1U)) != 0 ||
                static_cast<uint64_t>(entry.permissions) & ~0x7ULL ||
                entry.physicalAddress > snapshot.memoryState.data.size() ||
                pageSize > snapshot.memoryState.data.size() - entry.physicalAddress) {
                return reject("memory page table contains an invalid mapping");
            }
        }

        // Restore into isolated objects first. The plugin, wrapper, memory and
        // partition manager are all disposable until every cross-reference and
        // serialized field has passed validation.
        auto stagedMemory = std::make_unique<Memory>(snapshot.memoryState.data.size());
        std::unique_ptr<ppc64::PPC64PartitionManager> stagedManager;
        std::unique_ptr<IISA> stagedPluginOwner;
        std::unique_ptr<CPU> stagedCPU;
        std::unique_ptr<VirtualConsole> stagedConsole;
        std::unique_ptr<VirtualTimer> stagedTimer;
        std::unique_ptr<BasicInterruptController> stagedInterruptController;
        std::unique_ptr<ppc64::PPC64VirtualTimeBase> stagedTimeBase;
        auto stagedTerminal = std::make_unique<ppc64::PPC64VirtualTerminal>(
            expectedTerminalId, expectedTerminalEnabled);
        std::vector<uint8_t> stagedDTB = std::move(canonicalDTB);
        try {
            stagedMemory->RestoreSnapshot(snapshot.memoryState);
            if (snapshot.ppc64Platform.enabled) {
                stagedTimeBase = std::make_unique<ppc64::PPC64VirtualTimeBase>(
                    snapshot.ppc64Platform.configuration);
                if (!stagedTimeBase->restore(
                        snapshot.ppc64Platform.currentTimebase)) {
                    return reject("platform time-base restore failed");
                }
            }
            stagedManager = std::make_unique<ppc64::PPC64PartitionManager>();

            stagedConsole = std::make_unique<VirtualConsole>(
                snapshot.consoleState.baseAddress);
            stagedConsole->restoreSnapshot(snapshot.consoleState);
            stagedInterruptController =
                std::make_unique<BasicInterruptController>();
            stagedInterruptController->restoreSnapshot(
                snapshot.interruptControllerState);
            stagedTimer = std::make_unique<VirtualTimer>(
                snapshot.timerState.baseAddress);
            stagedTimer->AttachInterruptController(
                stagedInterruptController.get());
            stagedTimer->restoreSnapshot(snapshot.timerState);
            stagedInterruptController->RegisterDeliveryCallback(
                [this](uint8_t vector) {
                    const int targetCPU = activeCPUIndex_ >= 0
                        ? activeCPUIndex_ : 0;
                    if (isValidCPUIndex(targetCPU) && cpus_[targetCPU].cpu) {
                        cpus_[targetCPU].cpu->queueInterrupt(vector);
                        if (cpus_[targetCPU].state ==
                            CPUExecutionState::WAITING) {
                            cpus_[targetCPU].state = CPUExecutionState::RUNNING;
                        }
                    }
                });

            if (snapshot.ppc64PartitionTableConfigured &&
                snapshot.ppc64PartitionSnapshots.empty()) {
                std::string reason;
                if (snapshot.ppc64PartitionTableSizeExponent !=
                        ppc64::PPC64_POWER9_PARTITION_TABLE_PATS ||
                    !stagedManager->configurePartitionTable(
                        *stagedMemory, snapshot.ppc64PartitionTableBase,
                        snapshot.ppc64PartitionTableSizeExponent, reason)) {
                    return reject("partition table restore failed: " + reason);
                }
            }

            for (const auto& partitionSnapshot : snapshot.ppc64PartitionSnapshots) {
                std::string reason;
                if (!stagedManager->restorePartition(*stagedMemory,
                                                     partitionSnapshot, reason)) {
                    return reject("partition metadata restore failed: " + reason);
                }
                if (stagedManager->partitionTableBase() !=
                        snapshot.ppc64PartitionTableBase ||
                    stagedManager->partitionTableSizeExponent() !=
                        snapshot.ppc64PartitionTableSizeExponent) {
                    return reject("partition metadata disagrees with VM table configuration");
                }
            }
            if (stagedManager->isConfigured() !=
                    snapshot.ppc64PartitionTableConfigured) {
                return reject("partition table configuration was not restored");
            }

            stagedPluginOwner = ISAPluginRegistry::instance().createISA("PPC64");
            auto* stagedPlugin = dynamic_cast<ppc64::PPC64ISAPlugin*>(
                stagedPluginOwner.get());
            if (stagedPlugin == nullptr) {
                return reject("PPC64 ISA factory returned an incompatible plugin");
            }
            stagedPlugin->attachVirtualTimeBase(stagedTimeBase.get());
            stagedPlugin->attachVirtualTerminal(stagedTerminal.get());
            stagedCPU = std::make_unique<CPU>(*stagedMemory, *stagedPlugin);

            std::string reason;
            if (snapshot.ppc64HypervisorMemoryConfigured &&
                !stagedPlugin->configureHypervisorMemory(
                    *stagedMemory, snapshot.ppc64HypervisorMemoryBase,
                    snapshot.ppc64HypervisorMemorySize, reason)) {
                return reject("hypervisor memory restore failed: " + reason);
            }
            if (snapshot.ppc64RadixConfigured &&
                !stagedPlugin->configureRadixTranslation(
                    snapshot.ppc64RadixConfiguration, *stagedMemory, reason)) {
                return reject("radix configuration restore failed: " + reason);
            }

            std::shared_ptr<ppc64::PPC64PartitionContext> activeContext;
            std::vector<uint16_t> authorizedTerminalLPIDs;
            for (const auto& partitionSnapshot : snapshot.ppc64PartitionSnapshots) {
                // The metadata has already been validated and installed above.
                // The LPID is encoded in bytes 8..9 in the versioned Phase 8
                // partition metadata format.
                if (partitionSnapshot.size() < 10) {
                    return reject("partition metadata is truncated");
                }
                const uint16_t lpid = static_cast<uint16_t>(partitionSnapshot[8]) |
                    (static_cast<uint16_t>(partitionSnapshot[9]) << 8);
                authorizedTerminalLPIDs.push_back(lpid);
                if (snapshot.ppc64HasActiveLPID &&
                    lpid == snapshot.ppc64ActiveLPID) {
                    activeContext = stagedManager->findPartition(lpid);
                }
            }
            if (!stagedTerminal->restoreSnapshot(snapshot.ppc64VirtualTerminal,
                                                 authorizedTerminalLPIDs)) {
                return reject("virtual-terminal checkpoint state is invalid or belongs to an unknown LPID");
            }
            if (snapshot.ppc64HasActiveLPID) {
                if (!activeContext ||
                    !stagedPlugin->activatePartitionContext(activeContext, reason)) {
                    return reject("active LPID restore failed: " + reason);
                }
            } else if (snapshot.ppc64ActiveLPID != 0) {
                return reject("snapshot active LPID relationship is invalid");
            }

            if (authorizeHypervisorState) {
                if (!snapshot.ppc64HypervisorMemoryConfigured) {
                    return reject("HV authorization requires configured handler memory");
                }
                stagedPlugin->setMSR(ppc64::MSR_HV | ppc64::MSR_SF);
            }
            if (!stagedPlugin->deserialize_state(cpuRecord.isaState)) {
                return reject("PPC64 architectural state failed restore validation or HV authorization");
            }
            const auto* restoredState = dynamic_cast<const ppc64::PPC64ISAState*>(
                &stagedPlugin->getState());
            if (restoredState == nullptr ||
                ((restoredState->msr & (ppc64::MSR_IR | ppc64::MSR_DR)) != 0 &&
                 !snapshot.ppc64RadixConfigured)) {
                return reject("restored MSR requires an unavailable radix configuration");
            }
            if (snapshot.ppc64HasActiveLPID !=
                stagedPlugin->hasActivePartitionContext()) {
                return reject("active partition binding does not match snapshot metadata");
            }
        } catch (const std::exception& exception) {
            return reject(std::string("staged restore failed: ") + exception.what());
        } catch (...) {
            return reject("staged restore failed with an unknown error");
        }

        // Commit. Drop the wrapper before its non-owning plugin, then release
        // the old partition manager while its old memory is still alive.
        cpus_[0].cpu.reset();
        cpus_[0].isaPlugin.reset();
        auto oldManager = std::move(ppc64PartitionManager_);
        auto oldMemory = std::move(memory_);
        if (oldManager) {
            oldManager->destroyAllPartitions();
        }

        memory_ = std::move(stagedMemory);
        ppc64PartitionManager_ = std::move(stagedManager);
        ppc64VirtualTerminal_ = std::move(stagedTerminal);
        ppc64VirtualTimeBase_ = std::move(stagedTimeBase);
        ppc64PlatformEnabled_ = snapshot.ppc64Platform.enabled;
        ppc64PlatformPrepared_ = snapshot.ppc64Platform.enabled &&
            snapshot.ppc64Platform.prepared;
        if (ppc64PlatformEnabled_) {
            ppc64PlatformConfiguration_ = snapshot.ppc64Platform.configuration;
            ppc64GuestMemoryLayout_ = snapshot.ppc64Platform.memoryLayout;
            ppc64PlatformEntryAddress_ = snapshot.ppc64Platform.prepared
                ? snapshot.ppc64Platform.memoryLayout.entryAddress : 0;
            ppc64PlatformDTB_ = std::move(stagedDTB);
        } else {
            ppc64PlatformConfiguration_ = {};
            ppc64GuestMemoryLayout_ = {};
            ppc64PlatformEntryAddress_ = 0;
            ppc64PlatformDTB_.clear();
        }
        cpus_[0].isaPlugin = std::move(stagedPluginOwner);
        cpus_[0].cpu = std::move(stagedCPU);
        consoleDevice_ = std::move(stagedConsole);
        interruptController_ = std::move(stagedInterruptController);
        timerDevice_ = std::move(stagedTimer);
        cpus_[0].state = cpuRecord.executionState;
        cpus_[0].cyclesExecuted = cpuRecord.cyclesExecuted;
        cpus_[0].instructionsExecuted = cpuRecord.instructionsExecuted;
        cpus_[0].idleCycles = cpuRecord.idleCycles;
        cpus_[0].enabled = cpuRecord.enabled;
        cpus_[0].lastActivationTime = cpuRecord.lastActivationTime;

        CPURuntimeStateSnapshot runtimeState;
        runtimeState.currentSlot = cpuRecord.currentSlot;
        runtimeState.bundleValid = cpuRecord.bundleValid;
        runtimeState.pendingInterrupts = cpuRecord.pendingInterrupts;
        runtimeState.interruptVectorBase = cpuRecord.interruptVectorBase;
        runtimeState.halted = cpuRecord.halted;
        runtimeState.pendingCallInputs = cpuRecord.pendingCallInputs;
        cpus_[0].cpu->restoreSnapshot(runtimeState);

        activeCPUIndex_ = snapshot.activeCPUIndex;
        state_ = static_cast<VMState>(snapshot.vmStateValue);
        cyclesExecuted_ = snapshot.totalCyclesExecuted;
        lastExecutionDiagnostic_ = cpuRecord.lastExecutionDiagnostic;
        lastBreakReason_.clear();
        if (scheduler_) {
            scheduler_->setQuantumSize(snapshot.quantumSize);
        }
        return true;
    }

    if (!snapshot.architecture.empty() && snapshot.architecture != "IA-64") {
        LOG_ERROR("Rejected IA-64 VM snapshot with a different architecture identity");
        return false;
    }
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

bool VirtualMachine::writeDiagnosticCheckpoint(const std::string& path,
                                               const std::string& guestIdentity,
                                               std::string* error) const {
    return VMCheckpointCodec::write(*this, path, guestIdentity, error);
}

bool VirtualMachine::readDiagnosticCheckpoint(const std::string& path,
                                              const std::string& expectedGuestIdentity,
                                               std::string* error,
                                               bool authorizeHypervisorState) {
    return VMCheckpointCodec::read(*this, path, expectedGuestIdentity, error,
                                   authorizeHypervisorState);
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


