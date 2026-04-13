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
#include "ISAPluginRegistry.h"
#include "IA64ISAPlugin.h"
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
  activeCPUIndex_(-1),
  debuggerAttached_(false),
  nextMemoryBreakpointId_(1),
  maxSnapshotHistory_(256),
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
        instructionBreakpoints_.clear();
        memoryBreakpoints_.clear();
        snapshotHistory_.clear();
        nextMemoryBreakpointId_ = 1;
        lastBreakReason_.clear();
        debuggerAttached_ = false;
        
        state_ = VMState::STOPPED;
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
        snapshotHistory_.clear();
        lastBreakReason_.clear();
        
        // Keep breakpoints and debugger attachment
        
        state_ = VMState::STOPPED;
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
                
                // Store ISA plugin in context (we'll need to extend CPUContext for this)
                // For now, create CPU with legacy decoder interface
                ctx.cpu = std::make_unique<CPU>(*memory_, *decoder_);
                
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
            state_ = VMState::STOPPED;
            return false;
        }

        ctx.cyclesExecuted++;
        ctx.instructionsExecuted++;
        cyclesExecuted_++;
        scheduler_->onCPUExecuted(cpuIndex, 1);

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

} // namespace ia64

