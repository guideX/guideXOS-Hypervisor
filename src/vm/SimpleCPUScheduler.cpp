#include "SimpleCPUScheduler.h"
#include "SimpleCPUScheduler.h"
#include "CPUContext.h"
#include "cpu.h"
#include <algorithm>

namespace ia64 {

SimpleCPUScheduler::SimpleCPUScheduler()
    : lastCPUIndex_(-1),
      numCPUs_(0),
      quantumSize_(10) {  // Default: 10 bundles (30 instructions) per quantum
}

SimpleCPUScheduler::~SimpleCPUScheduler() {
    // Nothing to clean up
}

void SimpleCPUScheduler::initialize(std::vector<CPUContext*>& cpus) {
    numCPUs_ = static_cast<int>(cpus.size());
    lastCPUIndex_ = -1;  // Start fresh
    
    // Initialize bundle counters for each CPU
    cpuBundleCounters_.resize(numCPUs_, 0);
}

int SimpleCPUScheduler::selectNextCPU(const std::vector<CPUContext*>& cpus) {
    if (cpus.empty()) {
        return -1;  // No CPUs available
    }
    
    // Special case: only one CPU
    if (cpus.size() == 1) {
        CPUContext* ctx = cpus[0];
        if (ctx && ctx->enabled && ctx->isRunning()) {
            return 0;
        }
        return -1;
    }
    
    // Round-robin: try to find next CPU after last one
    int startIndex = (lastCPUIndex_ + 1) % static_cast<int>(cpus.size());
    int nextCPU = findNextRunnableCPU(cpus, startIndex);
    
    if (nextCPU != -1) {
        return nextCPU;
    }
    
    // If no CPU found after lastCPUIndex_, try from beginning
    // (handles case where all CPUs after current are not runnable)
    if (startIndex != 0) {
        nextCPU = findNextRunnableCPU(cpus, 0);
    }
    
    return nextCPU;
}

void SimpleCPUScheduler::onCPUExecuted(int cpuIndex, uint64_t cyclesTaken) {
    // Update last CPU index for round-robin
    lastCPUIndex_ = cpuIndex;
    
    // Simple scheduler doesn't use cycle count
    // (Future: could implement time quantum based on cycles)
}

void SimpleCPUScheduler::onCPUStateChanged(int cpuIndex) {
    // Simple scheduler doesn't need to track state changes
    // It will naturally skip non-running CPUs in selectNextCPU()
}

void SimpleCPUScheduler::reset() {
    lastCPUIndex_ = -1;
    
    // Reset bundle counters
    std::fill(cpuBundleCounters_.begin(), cpuBundleCounters_.end(), 0);
}

const char* SimpleCPUScheduler::getName() const {
    return "SimpleCPUScheduler (Round-Robin)";
}

int SimpleCPUScheduler::findNextRunnableCPU(const std::vector<CPUContext*>& cpus, int startIndex) {
    int numCPUs = static_cast<int>(cpus.size());
    
    // Search from startIndex to end
    for (int i = startIndex; i < numCPUs; i++) {
        CPUContext* ctx = cpus[i];
        if (ctx && ctx->enabled && ctx->isRunning()) {
            return i;
        }
    }
    
    // Not found
    return -1;
}

// ============================================================================
// Extended Stepping Methods
// ============================================================================

bool SimpleCPUScheduler::stepCPU(std::vector<CPUContext*>& cpus, int cpuIndex) {
    // Validate CPU index
    if (cpuIndex < 0 || cpuIndex >= static_cast<int>(cpus.size())) {
        return false;
    }
    
    CPUContext* ctx = cpus[cpuIndex];
    if (!ctx || !ctx->enabled || !ctx->isRunning()) {
        return false;
    }
    
    // Execute single instruction
    return executeSingleInstruction(ctx, cpuIndex);
}

int SimpleCPUScheduler::stepAllCPUs(std::vector<CPUContext*>& cpus) {
    int successCount = 0;
    
    // Execute one instruction on each enabled/running CPU
    for (int i = 0; i < static_cast<int>(cpus.size()); i++) {
        CPUContext* ctx = cpus[i];
        if (ctx && ctx->enabled && ctx->isRunning()) {
            if (executeSingleInstruction(ctx, i)) {
                successCount++;
            }
        }
    }
    
    return successCount;
}

uint64_t SimpleCPUScheduler::stepQuantum(std::vector<CPUContext*>& cpus, int cpuIndex, uint64_t bundleCount) {
    // Validate CPU index
    if (cpuIndex < 0 || cpuIndex >= static_cast<int>(cpus.size())) {
        return 0;
    }
    
    CPUContext* ctx = cpus[cpuIndex];
    if (!ctx || !ctx->enabled || !ctx->isRunning()) {
        return 0;
    }
    
    uint64_t bundlesExecuted = 0;
    
    // Execute bundleCount bundles (3 instructions per bundle)
    for (uint64_t bundle = 0; bundle < bundleCount; bundle++) {
        // Execute 3 instructions (1 bundle)
        for (int instrInBundle = 0; instrInBundle < 3; instrInBundle++) {
            // Invoke before-schedule callbacks
            if (!invokeBeforeScheduleCallbacks(cpuIndex, cpus)) {
                // Preemption requested
                invokeQuantumExpiredCallbacks(cpuIndex, bundlesExecuted);
                return bundlesExecuted;
            }
            
            // Execute instruction
            if (!executeSingleInstruction(ctx, cpuIndex)) {
                // CPU halted or error
                invokeQuantumExpiredCallbacks(cpuIndex, bundlesExecuted);
                return bundlesExecuted;
            }
            
            // Invoke after-instruction callbacks
            uint64_t instructionsInQuantum = (bundlesExecuted * 3) + instrInBundle + 1;
            if (!invokeAfterInstructionCallbacks(cpuIndex, instructionsInQuantum)) {
                // Preemption requested
                invokeQuantumExpiredCallbacks(cpuIndex, bundlesExecuted);
                return bundlesExecuted;
            }
            
            // Check if CPU is still running
            if (!ctx->isRunning()) {
                invokeQuantumExpiredCallbacks(cpuIndex, bundlesExecuted);
                return bundlesExecuted;
            }
        }
        
        // Bundle completed
        bundlesExecuted++;
        
        // Update bundle counter for this CPU
        if (cpuIndex < static_cast<int>(cpuBundleCounters_.size())) {
            cpuBundleCounters_[cpuIndex]++;
        }
    }
    
    // Quantum completed successfully
    invokeQuantumExpiredCallbacks(cpuIndex, bundlesExecuted);
    return bundlesExecuted;
}

// ============================================================================
// Preemption Support
// ============================================================================

void SimpleCPUScheduler::registerPreemptionCallback(IPreemptionCallback* callback) {
    if (callback) {
        preemptionCallbacks_.push_back(callback);
    }
}

void SimpleCPUScheduler::unregisterPreemptionCallback(IPreemptionCallback* callback) {
    if (callback) {
        auto it = std::find(preemptionCallbacks_.begin(), preemptionCallbacks_.end(), callback);
        if (it != preemptionCallbacks_.end()) {
            preemptionCallbacks_.erase(it);
        }
    }
}

uint64_t SimpleCPUScheduler::getQuantumSize() const {
    return quantumSize_;
}

void SimpleCPUScheduler::setQuantumSize(uint64_t bundleCount) {
    quantumSize_ = bundleCount;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

bool SimpleCPUScheduler::executeSingleInstruction(CPUContext* ctx, int cpuIndex) {
    if (!ctx || !ctx->cpu) {
        return false;
    }
    
    // Execute one instruction
    bool shouldContinue = ctx->cpu->step();
    
    // Update CPU context statistics
    ctx->cyclesExecuted++;
    ctx->instructionsExecuted++;
    
    // Notify scheduler (for base class tracking)
    onCPUExecuted(cpuIndex, 1);
    
    return shouldContinue;
}

bool SimpleCPUScheduler::invokeBeforeScheduleCallbacks(int currentCPU, const std::vector<CPUContext*>& cpus) {
    for (auto* callback : preemptionCallbacks_) {
        if (callback && !callback->onBeforeSchedule(currentCPU, cpus)) {
            return false;  // Preemption requested
        }
    }
    return true;  // Continue
}

bool SimpleCPUScheduler::invokeAfterInstructionCallbacks(int cpuIndex, uint64_t instructionsInQuantum) {
    for (auto* callback : preemptionCallbacks_) {
        if (callback && !callback->onAfterInstruction(cpuIndex, instructionsInQuantum)) {
            return false;  // Preemption requested
        }
    }
    return true;  // Continue
}

void SimpleCPUScheduler::invokeQuantumExpiredCallbacks(int cpuIndex, uint64_t bundlesExecuted) {
    for (auto* callback : preemptionCallbacks_) {
        if (callback) {
            callback->onQuantumExpired(cpuIndex, bundlesExecuted);
        }
    }
}

} // namespace ia64

