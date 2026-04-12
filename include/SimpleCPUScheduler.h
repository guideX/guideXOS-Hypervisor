#pragma once

#include "ICPUScheduler.h"
#include <cstdint>

namespace ia64 {

/**
 * SimpleCPUScheduler - Basic round-robin CPU scheduler
 * 
 * This scheduler implements a simple round-robin policy:
 * - Cycles through CPUs in order (0, 1, 2, ..., N-1, 0, ...)
 * - Each CPU gets one instruction per turn
 * - Skips CPUs that are not enabled or not in RUNNING state
 * - Falls back to CPU 0 if only one CPU is active
 * 
 * This is the default scheduler and provides fair time-sharing
 * across all CPUs with minimal overhead.
 * 
 * Future Enhancements:
 * - Time quantum (execute N instructions before switching)
 * - Priority levels (some CPUs get more time)
 * - Weighted round-robin (based on CPU load)
 */
class SimpleCPUScheduler : public ICPUScheduler {
public:
    /**
     * Constructor
     */
    SimpleCPUScheduler();

    /**
     * Destructor
     */
    ~SimpleCPUScheduler() override;

    // ICPUScheduler implementation
    void initialize(std::vector<CPUContext*>& cpus) override;
    int selectNextCPU(const std::vector<CPUContext*>& cpus) override;
    void onCPUExecuted(int cpuIndex, uint64_t cyclesTaken) override;
    void onCPUStateChanged(int cpuIndex) override;
    void reset() override;
    const char* getName() const override;
    
    // Extended stepping methods
    bool stepCPU(std::vector<CPUContext*>& cpus, int cpuIndex) override;
    int stepAllCPUs(std::vector<CPUContext*>& cpus) override;
    uint64_t stepQuantum(std::vector<CPUContext*>& cpus, int cpuIndex, uint64_t bundleCount) override;
    
    // Preemption support
    void registerPreemptionCallback(IPreemptionCallback* callback) override;
    void unregisterPreemptionCallback(IPreemptionCallback* callback) override;
    uint64_t getQuantumSize() const override;
    void setQuantumSize(uint64_t bundleCount) override;

private:
    int lastCPUIndex_;      // Last CPU that was selected
    int numCPUs_;           // Total number of CPUs
    uint64_t quantumSize_;  // Quantum size in bundles (default: 10)
    std::vector<IPreemptionCallback*> preemptionCallbacks_;  // Registered callbacks
    std::vector<uint64_t> cpuBundleCounters_;  // Track bundles executed per CPU
    
    /**
     * Find next runnable CPU starting from given index
     * 
     * @param cpus All CPU contexts
     * @param startIndex Index to start searching from
     * @return Index of next runnable CPU, or -1 if none found
     */
    int findNextRunnableCPU(const std::vector<CPUContext*>& cpus, int startIndex);
    
    /**
     * Execute a single instruction on a CPU
     * 
     * @param ctx CPU context to execute on
     * @param cpuIndex Index of CPU
     * @return true if instruction executed successfully
     */
    bool executeSingleInstruction(CPUContext* ctx, int cpuIndex);
    
    /**
     * Invoke preemption callbacks before scheduling
     * 
     * @param currentCPU Currently running CPU index
     * @param cpus All CPU contexts
     * @return true if scheduling should continue, false to force preemption
     */
    bool invokeBeforeScheduleCallbacks(int currentCPU, const std::vector<CPUContext*>& cpus);
    
    /**
     * Invoke preemption callbacks after instruction
     * 
     * @param cpuIndex CPU that executed
     * @param instructionsInQuantum Instructions executed in current quantum
     * @return true to continue, false to preempt
     */
    bool invokeAfterInstructionCallbacks(int cpuIndex, uint64_t instructionsInQuantum);
    
    /**
     * Invoke preemption callbacks on quantum expiration
     * 
     * @param cpuIndex CPU whose quantum expired
     * @param bundlesExecuted Bundles executed in quantum
     */
    void invokeQuantumExpiredCallbacks(int cpuIndex, uint64_t bundlesExecuted);
};

} // namespace ia64
