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
#include "Mips64ISAPlugin.h"
#include "mips64/Mips64ElfLoader.h"
#include "mips64/Mips64MaltaProm.h"
#include "VMSnapshot.h"
#include "VMSnapshotManager.h"
#include "VMCheckpoint.h"
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <array>
#include <exception>
#include <limits>

namespace ia64 {

namespace {

// A CPU context whose architectural state is the IA-64 register model: the
// legacy IA-64 CPU, or a CPU driven by the IA-64 plugin. Any other ISA owns
// its state; the VM must not read or present IA-64 state for it (MIPS64-4R1).
bool isIA64Context(const CPUContext& ctx) {
    return !ctx.isaPlugin || dynamic_cast<const IA64ISAPlugin*>(ctx.isaPlugin.get()) != nullptr;
}

bool isIA64Cpu(const CPU& cpu) {
    return !cpu.isUsingISAPlugin() ||
           dynamic_cast<const IA64ISAPlugin*>(cpu.getISAPlugin()) != nullptr;
}

bool allContextsIA64(const std::vector<CPUContext>& contexts) {
    for (const CPUContext& ctx : contexts) {
        if (!isIA64Context(ctx)) {
            return false;
        }
    }
    return true;
}

uint64_t hashMips64ElfImage(const uint8_t* bytes, size_t size) noexcept {
    constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t hash = kFnvOffset;
    for (unsigned index = 0; index < 8; ++index) {
        hash ^= static_cast<uint8_t>(static_cast<uint64_t>(size) >> (index * 8));
        hash *= kFnvPrime;
    }
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

std::string maltaBoardProfileIdentity(const MaltaBootstrapConfig& config) {
    if (config.bootMode == MaltaBootMode::SyntheticDemo) {
        return "MaltaBootstrapSubset";
    }
    std::vector<uint8_t> identity;
    const auto append64 = [&identity](uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            identity.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    };
    const auto appendString = [&identity, &append64](const std::string& value) {
        append64(value.size());
        identity.insert(identity.end(), value.begin(), value.end());
    };
    append64(static_cast<uint8_t>(config.bootMode));
    append64(static_cast<uint8_t>(config.endianness));
    append64(config.switches);
    append64(config.jumpers);
    append64(config.mapFlashMirror);
    append64(config.guestRamSize);
    append64(config.kernelEntryPoint);
    append64(config.kernelRequiresKx);
    append64(config.kernelImageIdentity);
    append64(config.prom.physicalAddress);
    appendString(config.prom.kernelImageName);
    appendString(config.prom.kernelCommandLine);
    append64(config.prom.includeMemsize);
    append64(config.prom.includeEmemsize);
    append64(config.prom.includeModetty0);
    appendString(config.prom.serialConfiguration);

    std::ostringstream result;
    result << "MaltaBootstrapSubset:ElfHandoff:" << std::hex << std::setfill('0')
           << std::setw(16) << Mips64FullSnapshotHashBytes(identity);
    return result.str();
}

// Builds the panic record for a failed CPU step. IA-64 CPUs keep the IA-64
// register and bundle capture; other ISAs record their own state text and no
// IA-64 bundle or register dump.
KernelPanic captureCpuFailure(KernelPanicDetector& detector, const CPU& cpu,
                              const std::string& message, uint64_t cycles) {
    if (isIA64Cpu(cpu)) {
        return detector.captureExceptionPanic(message, cpu.getState(), Bundle(), 0, cycles, cycles);
    }
    const IISA* isa = cpu.getISAPlugin();
    const std::string isaName = isa != nullptr ? isa->getName() : std::string("unknown");
    const std::string isaDump = isa != nullptr ? isa->dumpState() : std::string();
    return detector.captureIsaPanic(KernelPanicReason::UNHANDLED_EXCEPTION,
                                    "Unhandled exception: " + message, isaName, isaDump,
                                    cycles, cycles);
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualMachine::VirtualMachine(size_t memorySize, const std::string& isaName)
    : VirtualMachine(memorySize, 1, isaName) {}

VirtualMachine::VirtualMachine(size_t memorySize, size_t numCPUs, const std::string& isaName)
: memory_(nullptr),
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
        
    // An unregistered ISA is refused. It must never silently become an IA-64
    // CPU; "IA-64" keeps its legacy construction when no plugin is registered.
    if (isaName != "IA-64" && !ISAPluginRegistry::instance().isRegistered(isaName)) {
        throw std::invalid_argument("VirtualMachine: ISA '" + isaName + "' is not registered");
    }

    // Create CPU contexts with specified ISA
    if (!createCPUs(numCPUs, isaName)) {
        throw std::runtime_error("Failed to create CPU contexts with ISA: " + isaName);
    }

        interruptController_->RegisterDeliveryCallback([this](uint8_t vector) {
            const int targetCPU = activeCPUIndex_ >= 0 ? activeCPUIndex_ : 0;
            // External (IA-64 interrupt-controller) vectors are delivered to
            // IA-64 CPUs only. Other ISAs have no delivery path for them yet.
            if (isValidCPUIndex(targetCPU) && cpus_[targetCPU].cpu &&
                isIA64Context(cpus_[targetCPU])) {
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    // Halt execution if running
    if (state_ == VMState::RUNNING) {
        halt();
    }
    for (CPUContext& context : cpus_) {
        if (context.isaPlugin) {
            if (auto* plugin = dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get())) {
                plugin->core().setHardwareInterruptLine(2, false);
            }
        }
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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

        // A configured Malta profile starts from its architectural reset
        // vector. Platform-independent MIPS64 VMs retain their existing PC=0
        // reset behavior.
        if (maltaBootstrapBoard_) {
            for (CPUContext& context : cpus_) {
                auto* plugin = dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get());
                if (!plugin) {
                    throw std::logic_error("Malta bootstrap VM lost its MIPS64 CPU plugin");
                }
                plugin->core().setPc(kMaltaResetVector);
            }
            synchronizeMips64UartInterrupt();
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
        if (memory_ && memory_->GetPhysicalAddressMap()) {
            memory_->GetPhysicalAddressMap()->ResetDevices();
        }
        synchronizeMips64UartInterrupt();
        
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    // Keep the CPU pin derived from the live UART level even when execution
    // stops; pending device state is preserved, while stale route state is not.
    synchronizeMips64UartInterrupt();
}

// ============================================================================
// Execution Control
// ============================================================================

bool VirtualMachine::step() {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (state_ == VMState::CREATED) {
        LOG_ERROR("Cannot step - VM not initialized");
        return false;
    }

    if (state_ == VMState::ERROR) {
        LOG_ERROR("Cannot step - VM in error state");
        return false;
    }

    // A restored SOFTRES mailbox is handled at the next VM boundary before
    // the scheduler lets another guest instruction run.
    if (maltaBootstrapBoard_ && maltaBootstrapBoard_->ConsumeSoftwareResetRequest()) {
        if (!applyMips64MaltaColdReset(true)) {
            LOG_ERROR("Failed to apply pending Malta software reset");
            state_ = VMState::ERROR;
            return false;
        }
        return true;
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

        const bool stepSucceeded = stepCPU(cpuIndex);
        if (stepSucceeded && maltaBootstrapBoard_ &&
            maltaBootstrapBoard_->ConsumeSoftwareResetRequest()) {
            if (!applyMips64MaltaColdReset(true)) {
                LOG_ERROR("Failed to apply guest-requested Malta software reset");
                state_ = VMState::ERROR;
                return false;
            }
        }
        return stepSucceeded;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during step: " + std::string(e.what()));
        state_ = VMState::ERROR;
        return false;
    }
}

uint64_t VirtualMachine::run(uint64_t maxCycles) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
        // A strict physical map validates guest transaction widths. Host-side
        // image loading is initialization, so write its bounded RAM backing
        // directly instead of pretending an image-sized transfer is a guest
        // bus transaction.
        if (memory_->GetPhysicalAddressMap()) {
            memory_->loadBuffer(loadAddress, data, size);
        } else {
            memory_->Write(loadAddress, data, size);
        }
        
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
            if (!isIA64Cpu(*cpus_[activeCPUIndex_].cpu)) {
                throw std::runtime_error("CPUState access is not supported for ISA '" +
                                         cpus_[activeCPUIndex_].isaPlugin->getName() + "'");
            }
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    if (!isIA64Cpu(*cpus_[cpuIndex].cpu)) {
        throw std::runtime_error("CPUState access is not supported for ISA '" +
                                 cpus_[cpuIndex].isaPlugin->getName() + "'");
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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

PhysicalAddressMap& VirtualMachine::enableMips64PhysicalAddressMap() {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (state_ == VMState::RUNNING || state_ == VMState::PAUSED) {
        throw std::logic_error("MIPS64 physical map must be configured while the VM is stopped");
    }
    if (cpus_.empty()) {
        throw std::logic_error("MIPS64 physical map requires a constructed MIPS64 VM");
    }
    for (const CPUContext& context : cpus_) {
        if (!context.isaPlugin || context.isaPlugin->getName() != "MIPS64") {
            throw std::logic_error("MIPS64 physical map is only available to MIPS64 VMs");
        }
    }
    return memory_->EnablePhysicalAddressMap();
}

void VirtualMachine::configureMips64MaltaBootstrap() {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    configureMips64MaltaBootstrap(MaltaBootstrapConfig{});
}

void VirtualMachine::configureMips64MaltaBootstrap(
    const MaltaBootstrapConfig& config) {
    configureMips64MaltaBootstrapInternal(config, false);
}

void VirtualMachine::configureMips64MaltaBootstrapInternal(
    const MaltaBootstrapConfig& config, bool allowElfHandoff) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (config.bootMode == MaltaBootMode::ElfKernelHandoff && !allowElfHandoff) {
        throw std::logic_error(
            "ELF Malta mode requires configureMips64MaltaElfHandoff with a staged executable");
    }
    if (state_ == VMState::RUNNING || state_ == VMState::PAUSED) {
        throw std::logic_error("Malta bootstrap profile must be configured while the VM is stopped");
    }
    if (state_ == VMState::ERROR) {
        throw std::logic_error("Malta bootstrap profile cannot be configured on an errored VM");
    }
    if (maltaBootstrapBoard_) {
        throw std::logic_error("Malta bootstrap profile is already configured for this VM");
    }
    if (cpus_.empty()) {
        throw std::logic_error("Malta bootstrap profile requires a constructed MIPS64 VM");
    }
    for (const CPUContext& context : cpus_) {
        if (!dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get())) {
            throw std::logic_error("Malta bootstrap profile is only available to MIPS64 VMs");
        }
    }

    if (memory_->GetTotalSize() == 0 || memory_->GetTotalSize() >= kMaltaFlashBase) {
        throw std::invalid_argument("Malta bootstrap RAM must fit below the 0x1E000000 flash window");
    }

    const bool mapAlreadyEnabled = memory_->GetPhysicalAddressMap() != nullptr;
    PhysicalAddressMap& bus = enableMips64PhysicalAddressMap();
    MaltaBootstrapConfig boardConfig = config;
    if (boardConfig.guestRamSize == 0) {
        boardConfig.guestRamSize = memory_->GetTotalSize();
    }
    std::unique_ptr<MaltaBootstrapBoard> board;
    try {
        board = std::make_unique<MaltaBootstrapBoard>(bus, boardConfig);
    } catch (...) {
        if (!mapAlreadyEnabled) memory_->DisableEmptyPhysicalAddressMap();
        throw;
    }
    for (CPUContext& context : cpus_) {
        auto* plugin = dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get());
        plugin->configureEndianness(boardConfig.endianness == mips64::Mips64Endianness::Little);
    }
    maltaBootstrapBoard_ = std::move(board);

    // Configuration after VM initialization is supported while stopped and
    // immediately establishes a real board reset state.
    if (state_ != VMState::CREATED && !coldResetMips64Malta()) {
        maltaBootstrapBoard_.reset();
        throw std::runtime_error("failed to cold-reset configured Malta profile");
    }
}

bool VirtualMachine::configureMips64MaltaElfHandoff(
    const uint8_t* file, size_t fileSize, MaltaBootstrapConfig config,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (error) error->clear();
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (state_ == VMState::RUNNING || state_ == VMState::PAUSED ||
        state_ == VMState::ERROR || maltaBootstrapBoard_ || cpus_.empty()) {
        return fail("MIPS64 Malta ELF handoff requires a stopped MIPS64 VM without an existing board");
    }
    for (const CPUContext& context : cpus_) {
        if (!dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get())) {
            return fail("MIPS64 Malta ELF handoff is only available to MIPS64 VMs");
        }
    }
    const uint64_t ramSize = memory_->GetTotalSize();
    if (ramSize == 0 || ramSize >= kMaltaFlashBase) {
        return fail("Malta bootstrap RAM must fit below the 0x1E000000 flash window");
    }

    config.bootMode = MaltaBootMode::ElfKernelHandoff;
    config.useBuiltInDemoRom = false;
    config.bootRom.clear();
    config.guestRamSize = ramSize;
    mips64::Mips64ElfLoadOptions options;
    options.guestEndianness = config.endianness;
    options.guestRamSize = ramSize;
    options.reservedPhysicalRanges = {
        {config.prom.physicalAddress, mips64::kMaltaPromImageBytes},
        {mips64::kMaltaElfCompletionPhysicalAddress, sizeof(uint32_t)}
    };

    mips64::Mips64ElfLoadPlan plan;
    if (!mips64::Mips64ElfLoader::BuildPlan(file, fileSize, options, plan, error)) {
        return false;
    }
    mips64::MaltaPromImage prom;
    if (!mips64::Mips64MaltaProm::BuildImage(
            config.prom, config.endianness, ramSize, prom, error)) {
        return false;
    }
    const uint64_t promEnd = uint64_t(prom.physicalAddress) + prom.bytes.size();
    const uint64_t completionEnd =
        mips64::kMaltaElfCompletionPhysicalAddress + sizeof(uint32_t);
    if (prom.physicalAddress < completionEnd &&
        mips64::kMaltaElfCompletionPhysicalAddress < promEnd) {
        return fail("Malta PROM table overlaps the reserved ELF guest completion word");
    }
    config.kernelEntryPoint = plan.entryPoint;
    config.kernelRequiresKx = plan.requiresKernelKx;
    config.kernelImageIdentity = hashMips64ElfImage(file, fileSize);

    const bool mapAlreadyEnabled = memory_->GetPhysicalAddressMap() != nullptr;
    PhysicalAddressMap& bus = enableMips64PhysicalAddressMap();
    const std::vector<ia64::PhysicalRamWrite> extraWrites{{
        prom.physicalAddress, prom.bytes.data(), prom.bytes.size()
    }};
    ia64::PreparedPhysicalRamWriteBatch prepared;
    if (!mips64::Mips64ElfLoader::PreparePlan(
            plan, bus, extraWrites, prepared, error)) {
        if (!mapAlreadyEnabled) memory_->DisableEmptyPhysicalAddressMap();
        return false;
    }

    try {
        configureMips64MaltaBootstrapInternal(config, true);
    } catch (const std::exception& ex) {
        if (!mapAlreadyEnabled) memory_->DisableEmptyPhysicalAddressMap();
        return fail(ex.what());
    } catch (...) {
        if (!mapAlreadyEnabled) memory_->DisableEmptyPhysicalAddressMap();
        return fail("unknown failure while configuring the Malta ELF handoff");
    }

    // This commit is allocation-free and noexcept. All parsing, board checks,
    // PROM image construction, and RAM replacement allocation happened above.
    bus.CommitRamWriteBatch(std::move(prepared));
    return true;
}

bool VirtualMachine::coldResetMips64Malta() {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!maltaBootstrapBoard_) {
        LOG_ERROR("Malta cold reset requested without a configured board profile");
        return false;
    }
    if (state_ == VMState::RUNNING || state_ == VMState::PAUSED ||
        state_ == VMState::CREATED || state_ == VMState::ERROR) {
        LOG_ERROR("Malta cold reset requires an initialized, stopped VM");
        return false;
    }
    return applyMips64MaltaColdReset(false);
}

bool VirtualMachine::enqueueMips64UartInput(const uint8_t* bytes, size_t size) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!maltaBootstrapBoard_ ||
        (state_ != VMState::CREATED && state_ != VMState::STOPPED && state_ != VMState::PAUSED)) {
        return false;
    }
    const auto& uart = maltaBootstrapBoard_->Uart();
    if (!uart) return false;
    uart->AdvanceVirtualTime(cyclesExecuted_);
    if (!uart->EnqueueHostInput(bytes, size)) {
        synchronizeMips64UartInterrupt();
        return false;
    }
    uart->AdvanceHostInput();
    synchronizeMips64UartInterrupt();
    return true;
}

bool VirtualMachine::enqueueMips64UartReceiverInput(
    const std::vector<Ns16550RxInput>& characters) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!maltaBootstrapBoard_ ||
        (state_ != VMState::CREATED && state_ != VMState::STOPPED && state_ != VMState::PAUSED)) {
        return false;
    }
    const auto& uart = maltaBootstrapBoard_->Uart();
    if (!uart) return false;
    uart->AdvanceVirtualTime(cyclesExecuted_);
    if (!uart->EnqueueHostInput(characters)) {
        synchronizeMips64UartInterrupt();
        return false;
    }
    uart->AdvanceHostInput();
    synchronizeMips64UartInterrupt();
    return true;
}

bool VirtualMachine::receiveMips64UartCharacter(Ns16550RxInput character) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!maltaBootstrapBoard_ ||
        (state_ != VMState::CREATED && state_ != VMState::STOPPED && state_ != VMState::PAUSED)) {
        return false;
    }
    const auto& uart = maltaBootstrapBoard_->Uart();
    if (!uart) return false;
    uart->AdvanceVirtualTime(cyclesExecuted_);
    const bool accepted = uart->InjectReceivedCharacter(character);
    synchronizeMips64UartInterrupt();
    return accepted;
}

void VirtualMachine::synchronizeMips64UartInterrupt() noexcept {
    const bool asserted = maltaBootstrapBoard_ && maltaBootstrapBoard_->Uart() &&
        maltaBootstrapBoard_->Uart()->InterruptSignal().value_or(false);
    for (size_t index = 0; index < cpus_.size(); ++index) {
        auto* plugin = dynamic_cast<mips64::Mips64ISAPlugin*>(cpus_[index].isaPlugin.get());
        if (plugin) {
            // Malta CBUS UART: hardware INT2 -> Cause.IP4, CPU0 only.
            plugin->core().setHardwareInterruptLine(2, index == 0 && asserted);
        }
    }
}

Mips64FullVmSnapshot VirtualMachine::captureMips64FullSnapshot() const {
    std::unique_lock<std::recursive_mutex> boundaryLock(
        mips64SnapshotBoundaryMutex_, std::try_to_lock);
    if (!boundaryLock.owns_lock()) {
        throw std::logic_error("MIPS64 full snapshot requires an idle VM mutation boundary");
    }
    if (state_ != VMState::STOPPED && state_ != VMState::PAUSED) {
        throw std::logic_error("MIPS64 full snapshot requires a stopped or paused instruction boundary");
    }
    if (!maltaBootstrapBoard_ || !memory_ || !memory_->GetPhysicalAddressMap()) {
        throw std::logic_error("MIPS64 full snapshots require the configured Malta physical map");
    }
    const auto* scheduler = dynamic_cast<const SimpleCPUScheduler*>(scheduler_.get());
    if (!scheduler || cpus_.empty() || cpus_.size() > 64 ||
        activeCPUIndex_ < -1 || activeCPUIndex_ >= static_cast<int>(cpus_.size())) {
        throw std::logic_error("MIPS64 full snapshot encountered an inconsistent VM scheduler");
    }

    Mips64FullVmSnapshot snapshot;
    const MaltaBootstrapConfig& config = maltaBootstrapBoard_->Config();
    snapshot.cpuCount = static_cast<uint32_t>(cpus_.size());
    snapshot.boardProfileIdentity = maltaBoardProfileIdentity(config);
    snapshot.endianness = config.endianness;
    snapshot.ramSize = static_cast<uint64_t>(memory_->GetTotalSize());
    snapshot.romSize = static_cast<uint64_t>(maltaBootstrapBoard_->BootRomImage().size());
    snapshot.romContentHash = Mips64FullSnapshotHashBytes(maltaBootstrapBoard_->BootRomImage());
    snapshot.flashMirrorMapped = config.mapFlashMirror;
    snapshot.configuredSwitches = config.switches;
    snapshot.configuredJumpers = config.jumpers;
    snapshot.builtInDemoRom = config.useBuiltInDemoRom;
    snapshot.mmuEnabled = memory_->IsMMUEnabled();
    snapshot.physicalMemory = memory_->GetPhysicalAddressMap()->CaptureSnapshot();
    snapshot.fpga = maltaBootstrapBoard_->Fpga()->CaptureSnapshot();
    snapshot.uart = maltaBootstrapBoard_->Uart()->CaptureSnapshot();
    if (snapshot.uart.virtualTimeCycle != cyclesExecuted_) {
        throw std::logic_error("MIPS64 UART virtual clock is not synchronized to the VM boundary");
    }
    snapshot.uartInterruptTargetCpu = 0;
    snapshot.uartHardwareInterruptLine = 2;
    snapshot.uartCauseIp = 4;
    snapshot.routedUartInterruptLevel =
        Ns16550Uart::SnapshotInterruptSignal(snapshot.uart);
    snapshot.console = maltaBootstrapBoard_->ConsoleSink()->CaptureSnapshot();
    snapshot.pendingDeferredBoardReset = maltaBootstrapBoard_->HasSoftwareResetRequest();
    snapshot.vmStateValue = static_cast<uint32_t>(state_);
    snapshot.activeCpuIndex = activeCPUIndex_;
    snapshot.totalVirtualCycles = cyclesExecuted_;
    snapshot.scheduler = scheduler->createSnapshot();

    snapshot.cpus.reserve(cpus_.size());
    for (size_t index = 0; index < cpus_.size(); ++index) {
        const CPUContext& context = cpus_[index];
        const auto* plugin = dynamic_cast<const mips64::Mips64ISAPlugin*>(context.isaPlugin.get());
        if (!context.cpu || !plugin || context.cpuId != index) {
            throw std::logic_error("MIPS64 full snapshot requires one valid MIPS64 core per CPU");
        }
        Mips64FullSnapshotCpu cpu;
        cpu.cpuId = context.cpuId;
        cpu.architecturalState = plugin->core().serialize();
        cpu.executionState = context.state;
        cpu.cyclesExecuted = context.cyclesExecuted;
        cpu.instructionsExecuted = context.instructionsExecuted;
        cpu.idleCycles = context.idleCycles;
        cpu.enabled = context.enabled;
        cpu.lastActivationTime = context.lastActivationTime;
        snapshot.cpus.push_back(std::move(cpu));
    }
    if (!snapshot.RefreshIntegrity()) {
        throw std::length_error("MIPS64 full snapshot exceeds its bounded format limits");
    }
    return snapshot;
}

bool VirtualMachine::restoreMips64FullSnapshot(const Mips64FullVmSnapshot& snapshot,
                                               std::string* error) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    std::unique_lock<std::recursive_mutex> boundaryLock(
        mips64SnapshotBoundaryMutex_, std::try_to_lock);
    if (!boundaryLock.owns_lock()) {
        return fail("MIPS64 full restore requires an idle VM mutation boundary");
    }
    try {
        const Mips64SnapshotFailurePoint injectedFailure = mips64SnapshotFailurePointForTesting_;
        mips64SnapshotFailurePointForTesting_ = Mips64SnapshotFailurePoint::None;
        const auto injectFailure = [injectedFailure](Mips64SnapshotFailurePoint point) {
            return injectedFailure == point;
        };
        if (state_ != VMState::STOPPED && state_ != VMState::PAUSED) {
            return fail("MIPS64 full restore requires a stopped or paused instruction boundary");
        }
        if (!maltaBootstrapBoard_ || !memory_ || !memory_->GetPhysicalAddressMap()) {
            return fail("destination VM does not have the MIPS64 Malta physical map");
        }
        if (!snapshot.ValidateIntegrity()) return fail("snapshot component lengths or integrity checksum are invalid");
        constexpr std::array<uint8_t, 8> expectedMagic{{'G', 'X', 'M', '6', '4', 'S', 'N', 'P'}};
        if (snapshot.magic != expectedMagic || snapshot.formatVersion != Mips64FullVmSnapshot::kFormatVersion ||
            snapshot.isaIdentity != "MIPS64") {
            return fail("unsupported MIPS64 full snapshot format or identity");
        }
        if (snapshot.cpuComponentVersion != Mips64FullVmSnapshot::kCpuSerializationVersion ||
            snapshot.memoryComponentVersion != 1 || snapshot.fpgaComponentVersion != 1 ||
            snapshot.irqRoutingComponentVersion != 1 ||
            snapshot.consoleComponentVersion != 1 ||
            snapshot.schedulerComponentVersion != 1 || snapshot.vmComponentVersion != 1) {
            return fail("unsupported MIPS64 snapshot component version");
        }
        if (snapshot.uartComponentVersion != 4 || snapshot.uart.version != 4) {
            return fail("unsupported MIPS64 UART snapshot component version; expected v4");
        }
        const MaltaBootstrapConfig& config = maltaBootstrapBoard_->Config();
        if (snapshot.boardProfileIdentity != maltaBoardProfileIdentity(config)) {
            return fail("snapshot Malta boot profile or PROM identity does not match");
        }
        if (snapshot.byteOrderTag != 0x01020304 || snapshot.endianness != config.endianness ||
            snapshot.cpuCount != cpus_.size() || snapshot.cpus.size() != cpus_.size() ||
            snapshot.ramSize != memory_->GetTotalSize() ||
            snapshot.romBase != kMaltaResetRomBase ||
            snapshot.romSize != maltaBootstrapBoard_->BootRomImage().size() ||
            snapshot.romContentHash != Mips64FullSnapshotHashBytes(maltaBootstrapBoard_->BootRomImage()) ||
            snapshot.flashMirrorMapped != config.mapFlashMirror ||
            snapshot.configuredSwitches != config.switches ||
            snapshot.configuredJumpers != config.jumpers ||
            snapshot.builtInDemoRom != config.useBuiltInDemoRom ||
            snapshot.mmuEnabled != memory_->IsMMUEnabled()) {
            return fail("snapshot board, ROM, endianness, RAM, or VM configuration does not match");
        }
        if (snapshot.vmStateValue != static_cast<uint32_t>(VMState::STOPPED) &&
            snapshot.vmStateValue != static_cast<uint32_t>(VMState::PAUSED)) {
            return fail("snapshot contains an unsupported VM lifecycle state");
        }
        if (snapshot.activeCpuIndex < -1 ||
            snapshot.activeCpuIndex >= static_cast<int32_t>(cpus_.size())) {
            return fail("snapshot active CPU index is invalid");
        }
        if (snapshot.scheduler.numCPUs != static_cast<int>(cpus_.size()) ||
            snapshot.scheduler.quantumSize == 0 || snapshot.scheduler.lastCPUIndex < -1 ||
            snapshot.scheduler.lastCPUIndex >= static_cast<int>(cpus_.size()) ||
            snapshot.scheduler.cpuBundleCounters.size() != cpus_.size()) {
            return fail("snapshot round-robin scheduler state is invalid");
        }
        if (snapshot.pendingDeferredBoardReset != snapshot.fpga.resetPending ||
            !maltaBootstrapBoard_->Fpga()->ValidateSnapshot(snapshot.fpga) ||
            !Ns16550Uart::ValidateSnapshot(snapshot.uart) ||
            snapshot.uart.virtualTimeCycle != snapshot.totalVirtualCycles ||
            snapshot.uartInterruptTargetCpu != 0 ||
            snapshot.uartHardwareInterruptLine != 2 || snapshot.uartCauseIp != 4 ||
            snapshot.routedUartInterruptLevel !=
                Ns16550Uart::SnapshotInterruptSignal(snapshot.uart)) {
            return fail("snapshot Malta FPGA, UART, or deferred-reset state is invalid");
        }
        for (size_t index = 0; index < cpus_.size(); ++index) {
            if (!cpus_[index].cpu ||
                !dynamic_cast<mips64::Mips64ISAPlugin*>(cpus_[index].isaPlugin.get()) ||
                cpus_[index].cpuId != index) {
                return fail("destination VM has an unsupported CPU plugin or identity");
            }
        }

        const PhysicalAddressMapSnapshot& physical = snapshot.physicalMemory;
        if (injectFailure(Mips64SnapshotFailurePoint::RegionManifestValidation)) {
            return fail("injected region manifest validation failure");
        }
        if (physical.regions.empty() || physical.regions.size() > 64) {
            return fail("snapshot physical region manifest is invalid");
        }
        uint64_t previousEnd = 0;
        bool firstRegion = true;
        size_t resetRomCount = 0;
        size_t flashRomCount = 0;
        bool foundBaseRam = false;
        for (const PhysicalRegionSnapshot& region : physical.regions) {
            if (region.info.size == 0 || region.info.base >= kPhysicalAddressLimit ||
                region.info.size > kPhysicalAddressLimit - region.info.base ||
                (!firstRegion && region.info.base < previousEnd)) {
                return fail("snapshot physical region layout is overlapping or out of range");
            }
            firstRegion = false;
            previousEnd = region.info.base + region.info.size;
            if (region.info.type == PhysicalRegionType::Ram) {
                if (!region.info.readable || !region.info.writable ||
                    (region.backendKind != PhysicalBackendKind::DenseRam &&
                     region.backendKind != PhysicalBackendKind::SparseRam)) {
                    return fail("snapshot contains an unsupported RAM backend or permission set");
                }
                if (region.info.base == 0) {
                    if (foundBaseRam || region.info.size != snapshot.ramSize ||
                        region.backendKind != PhysicalBackendKind::DenseRam) {
                        return fail("snapshot base RAM geometry is invalid");
                    }
                    foundBaseRam = true;
                }
            } else if (region.info.type == PhysicalRegionType::Rom) {
                if (region.backendKind != PhysicalBackendKind::Rom || !region.info.readable ||
                    region.info.writable || region.info.size != kMaltaBootImageWindow) {
                    return fail("snapshot contains an unsupported ROM mapping");
                }
                if (region.info.base == kMaltaResetRomBase) ++resetRomCount;
                else if (region.info.base == kMaltaFlashBase) ++flashRomCount;
                else return fail("snapshot contains a ROM outside the Malta profile");
            } else if (region.info.type == PhysicalRegionType::Mmio) {
                if (region.backendKind != PhysicalBackendKind::Mmio || !region.info.readable ||
                    !region.info.writable) {
                    return fail("snapshot contains an unsupported MMIO region");
                }
                if (region.info.base == kMaltaFpgaBase) {
                    if (region.info.size != 0x900 ||
                        !memory_->GetPhysicalAddressMap()->IsMappedDeviceAt(
                            kMaltaFpgaBase, maltaBootstrapBoard_->Fpga().get())) {
                        return fail("snapshot Malta FPGA device registration is invalid");
                    }
                } else if (region.info.base == kMaltaUartBase) {
                    if (region.info.size != 0x40 ||
                        !memory_->GetPhysicalAddressMap()->IsMappedDeviceAt(
                            kMaltaUartBase, maltaBootstrapBoard_->Uart().get())) {
                        return fail("snapshot UART device registration is invalid");
                    }
                } else return fail("snapshot contains an unsupported MMIO device");
            } else return fail("snapshot contains an unsupported physical region type");
        }
        if (!foundBaseRam || resetRomCount != 1 ||
            flashRomCount != (config.mapFlashMirror ? 1U : 0U)) {
            return fail("snapshot Malta RAM or ROM map is incomplete");
        }

        std::vector<mips64::Mips64Cpu> stagedCpus;
        stagedCpus.reserve(snapshot.cpus.size());
        if (injectFailure(Mips64SnapshotFailurePoint::CpuStateValidation)) {
            return fail("injected MIPS64 CPU state validation failure");
        }
        for (size_t index = 0; index < snapshot.cpus.size(); ++index) {
            const Mips64FullSnapshotCpu& record = snapshot.cpus[index];
            if (record.cpuId != index || record.architecturalState.size() != mips64::Mips64Cpu::kSerializedSize ||
                record.architecturalState[4] != mips64::Mips64Cpu::kSerializationVersion ||
                record.architecturalState[5] != static_cast<uint8_t>(snapshot.endianness) ||
                std::any_of(record.architecturalState.begin() + 8,
                            record.architecturalState.begin() + 16,
                            [](uint8_t value) { return value != 0; })) {
                return fail("snapshot MIPS64 CPU record identity or v5 state is invalid");
            }
            switch (record.executionState) {
                case CPUExecutionState::IDLE:
                case CPUExecutionState::RUNNING:
                case CPUExecutionState::HALTED:
                case CPUExecutionState::WAITING:
                case CPUExecutionState::ERROR: break;
                default: return fail("snapshot CPU execution state is invalid");
            }
            stagedCpus.emplace_back(snapshot.endianness);
            mips64::Mips64Cpu& staged = stagedCpus.back();
            if (!staged.deserialize(record.architecturalState.data(), record.architecturalState.size()) ||
                staged.serialize() != record.architecturalState) {
                return fail("snapshot MIPS64 CPU v5 architectural state failed validation");
            }
        }

        PreparedPhysicalAddressMapRestore stagedMemory;
        const bool containsSparseRam = std::any_of(
            physical.regions.begin(), physical.regions.end(),
            [](const PhysicalRegionSnapshot& region) {
                return region.info.type == PhysicalRegionType::Ram &&
                       region.backendKind == PhysicalBackendKind::SparseRam;
            });
        if (containsSparseRam &&
            injectFailure(Mips64SnapshotFailurePoint::SparseMemoryPreparation)) {
            return fail("injected sparse-memory preparation failure");
        }
        if (!memory_->GetPhysicalAddressMap()->PrepareRestore(physical, stagedMemory, error)) return false;
        if (injectFailure(Mips64SnapshotFailurePoint::DeviceStatePreparation)) {
            return fail("injected device state preparation failure");
        }
        Ns16550Uart::PreparedRestore stagedUart;
        if (!maltaBootstrapBoard_->Uart()->PrepareRestore(snapshot.uart, stagedUart)) {
            return fail("snapshot UART receive state could not be staged");
        }
        GuestConsoleSink::PreparedRestore stagedConsole;
        if (!maltaBootstrapBoard_->ConsoleSink()->PrepareRestore(snapshot.console, stagedConsole)) {
            return fail("snapshot console capacity, occupancy, or output sequence is invalid");
        }
        SimpleCPUSchedulerState stagedScheduler = snapshot.scheduler;
        auto* liveScheduler = dynamic_cast<SimpleCPUScheduler*>(scheduler_.get());
        if (!liveScheduler) return fail("destination VM scheduler type is unsupported");
        if (injectFailure(Mips64SnapshotFailurePoint::RestoreCommitPreparation)) {
            return fail("injected restore commit preparation failure");
        }

        // Every validation and potentially allocating preparation is complete.
        memory_->GetPhysicalAddressMap()->CommitRestore(std::move(stagedMemory));
        for (size_t index = 0; index < cpus_.size(); ++index) {
            auto* plugin = static_cast<mips64::Mips64ISAPlugin*>(cpus_[index].isaPlugin.get());
            plugin->SwapCoreState(stagedCpus[index]);
            CPUContext& context = cpus_[index];
            const Mips64FullSnapshotCpu& record = snapshot.cpus[index];
            context.state = record.executionState;
            context.cyclesExecuted = record.cyclesExecuted;
            context.instructionsExecuted = record.instructionsExecuted;
            context.idleCycles = record.idleCycles;
            context.enabled = record.enabled;
            context.lastActivationTime = record.lastActivationTime;
        }
        maltaBootstrapBoard_->Fpga()->RestoreSnapshot(snapshot.fpga);
        maltaBootstrapBoard_->Uart()->CommitRestore(snapshot.uart, std::move(stagedUart));
        synchronizeMips64UartInterrupt();
        maltaBootstrapBoard_->ConsoleSink()->CommitRestore(std::move(stagedConsole));
        const bool schedulerCommitted = liveScheduler->restoreSnapshot(std::move(stagedScheduler));
        if (!schedulerCommitted) std::terminate();
        activeCPUIndex_ = snapshot.activeCpuIndex;
        cyclesExecuted_ = snapshot.totalVirtualCycles;
        state_ = VMState::PAUSED;
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("MIPS64 full snapshot restore preparation failed: ") + exception.what();
        return false;
    } catch (...) {
        if (error) *error = "MIPS64 full snapshot restore preparation failed";
        return false;
    }
}

bool VirtualMachine::applyMips64MaltaColdReset(bool preserveVmCycleCount) {
    if (!maltaBootstrapBoard_) return false;
    const uint64_t completedCycles = cyclesExecuted_;
    if (!reset()) return false;
    if (preserveVmCycleCount) cyclesExecuted_ = completedCycles;
    maltaBootstrapBoard_->Uart()->AdvanceVirtualTime(cyclesExecuted_);

    for (CPUContext& context : cpus_) {
        auto* plugin = dynamic_cast<mips64::Mips64ISAPlugin*>(context.isaPlugin.get());
        if (!plugin) {
            state_ = VMState::ERROR;
            return false;
        }
        plugin->core().setPc(kMaltaResetVector);
    }
    state_ = VMState::STOPPED;
    return true;
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (snapshot.cpus.size() != cpus_.size()) {
        return false;
    }
    // Refuse before mutating anything: a debugger snapshot carries IA-64 CPU
    // state only.
    if (!allContextsIA64(cpus_)) {
        LOG_ERROR("Debugger snapshot restore is not supported for this ISA");
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    if (condition.target == DebugConditionTarget::NONE || condition.op == DebugConditionOperator::ANY) {
        return true;
    }
    if (!isIA64Cpu(cpu)) {
        // IA-64 register/IP conditions cannot be evaluated against another
        // ISA's state. The condition is reported and treated as not matched.
        LOG_WARN("Breakpoint condition not evaluated: CPU has no IA-64 register state");
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
            if (cpu == nullptr || !evaluateCondition(breakpoint.condition, *cpu)) {
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
    // Debugger rewind history is only captured for IA-64 CPUs; other ISAs have
    // no runtime snapshot path yet, so no history is kept for them.
    if (!isIA64Cpu(*cpu)) {
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
                // Create ISA plugin instance. Only the IA-64 factory consumes
                // IA64FactoryData; every other factory receives null (its own
                // defaults) instead of a reinterpreted IA-64 structure.
                IA64FactoryData factoryData(decoder_.get());
                void* factoryPointer = (isaName == "IA-64") ? static_cast<void*>(&factoryData)
                                                            : nullptr;
                auto isaPlugin = ISAPluginRegistry::instance().createISA(isaName, factoryPointer);
                
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
        memory_->GetMMU().SetCPUStateReference(isIA64Cpu(*cpu) ? &cpu->getState() : nullptr);

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

        if (maltaBootstrapBoard_) {
            maltaBootstrapBoard_->Uart()->AdvanceVirtualTime(cyclesExecuted_);
            maltaBootstrapBoard_->Uart()->AdvanceHostInput();
            synchronizeMips64UartInterrupt();
        }
        const bool success = cpu->step();
        if (maltaBootstrapBoard_) {
            // Publish MMIO acknowledgements immediately. Staged input moves
            // after the global virtual cycle is advanced below.
            synchronizeMips64UartInterrupt();
        }
        if (!success) {
            if (cpu->isHalted()) {
                ctx.state = CPUExecutionState::HALTED;
                state_ = VMState::STOPPED;
                LOG_INFO("CPU " + std::to_string(cpuIndex) + " halted cleanly");
                return false;
            }

            // CPU step failed without delivering an architectural exception:
            // an execution failure (unsupported operation or host fault). Guest
            // exceptions are never reported here. Capture panic state.
            KernelPanic panic = captureCpuFailure(panicDetector_, *cpu,
                                                  "CPU execution failed", cyclesExecuted_);
            lastPanic_ = std::make_unique<KernelPanic>(panic);
            ctx.state = CPUExecutionState::ERROR;
            state_ = VMState::ERROR;
            bootStateMachine_.transition(VMBootState::KERNEL_PANIC, "CPU execution failed");
            return false;
        }

        ctx.cyclesExecuted++;
        if (cpu->didLastStepRetireInstruction()) {
            ctx.instructionsExecuted++;
        }
        cyclesExecuted_++;
        scheduler_->onCPUExecuted(cpuIndex, 1);

        if (maltaBootstrapBoard_) {
            // One VM virtual-time unit elapses per successful scheduler CPU
            // step, regardless of the selected vCPU. Clocked devices observe
            // the new cycle before the next instruction can sample interrupts.
            maltaBootstrapBoard_->Uart()->AdvanceVirtualTime(cyclesExecuted_);
            maltaBootstrapBoard_->Uart()->AdvanceHostInput();
            maltaBootstrapBoard_->Uart()->AdvanceVirtualTime(cyclesExecuted_);
            synchronizeMips64UartInterrupt();
        }
        
        // Record instruction milestone every 10000 instructions
        if (bootTraceSystem_.isEnabled() && cpu->didLastStepRetireInstruction() &&
            ctx.instructionsExecuted % 10000 == 0) {
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
            KernelPanic panic = captureCpuFailure(panicDetector_, *cpu, e.what(), cyclesExecuted_);
            lastPanic_ = std::make_unique<KernelPanic>(panic);
        }
        if (isValidCPUIndex(cpuIndex)) {
            cpus_[cpuIndex].state = CPUExecutionState::ERROR;
        }
        
        state_ = VMState::ERROR;
        bootStateMachine_.transition(VMBootState::KERNEL_PANIC, std::string("Exception: ") + e.what());
        return false;
    }
}

int VirtualMachine::stepAllCPUs() {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!snapshotManager_) {
        LOG_ERROR("Snapshot manager not initialized");
        return "";
    }
    // Full-VM snapshots are IA-64 only. A snapshot of a non-IA-64 CPU would
    // omit its architectural state, so it is refused rather than recorded.
    if (!allContextsIA64(cpus_)) {
        LOG_ERROR("Full VM snapshots are not supported for this ISA (CPU state is not captured)");
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    if (!snapshotManager_) {
        LOG_ERROR("Snapshot manager not initialized");
        return "";
    }
    if (!allContextsIA64(cpus_)) {
        LOG_ERROR("Delta VM snapshots are not supported for this ISA (CPU state is not captured)");
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    VMStateSnapshot snapshot;
    
    // Capture CPU states
    for (size_t i = 0; i < cpus_.size(); ++i) {
        CPUSnapshotRecord cpuRecord;
        cpuRecord.cpuId = static_cast<uint32_t>(i);
        
        const CPUContext& ctx = cpus_[i];
        if (ctx.cpu) {
            // Non-IA-64 CPUs have no CPUState to record (see createFullSnapshot).
            if (isIA64Context(ctx)) {
                cpuRecord.architecturalState = ctx.cpu->getState();
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    // Validate snapshot
    if (snapshot.cpus.size() != cpus_.size()) {
        LOG_ERROR("Snapshot CPU count mismatch");
        return false;
    }
    // Refuse before mutating memory or CPU state (no partial restore).
    if (!allContextsIA64(cpus_)) {
        LOG_ERROR("VM snapshot restore is not supported for this ISA");
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
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    return VMCheckpointCodec::write(*this, path, guestIdentity, error);
}

bool VirtualMachine::readDiagnosticCheckpoint(const std::string& path,
                                              const std::string& expectedGuestIdentity,
                                              std::string* error) {
    std::lock_guard<std::recursive_mutex> boundaryLock(mips64SnapshotBoundaryMutex_);
    return VMCheckpointCodec::read(*this, path, expectedGuestIdentity, error);
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
    
    // A non-IA-64 CPU has no IA-64 bundle or register dump to capture. Its
    // panic records the ISA's own state text instead (no IA-64 decode).
    if (!isIA64Cpu(*cpu)) {
        const IISA* isa = cpu->getISAPlugin();
        const std::string isaName = isa != nullptr ? isa->getName() : std::string("unknown");
        const std::string isaDump = isa != nullptr ? isa->dumpState() : std::string();
        KernelPanic panic = panicDetector_.captureIsaPanic(reason, description, isaName, isaDump,
                                                           cyclesExecuted_, cyclesExecuted_);
        lastPanic_ = std::make_unique<KernelPanic>(panic);
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


