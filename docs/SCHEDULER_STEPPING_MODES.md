# Scheduler Interface and Stepping Modes

## Overview

The guideXOS Hypervisor provides a comprehensive scheduler interface that allows fine-grained control over virtual CPU execution. This document describes the enhanced stepping modes and preemption support added to the scheduler system.

## Architecture

### Components

```
???????????????????????????????????????????????????????????
?                    VirtualMachine                        ?
?  ?????????????????????????????????????????????????????? ?
?  ?              ICPUScheduler Interface               ? ?
?  ?  ???????????????????????????????????????????????? ? ?
?  ?  ?         SimpleCPUScheduler Impl             ? ? ?
?  ?  ?  - Round-robin scheduling                   ? ? ?
?  ?  ?  - Bundle quantum tracking                  ? ? ?
?  ?  ?  - Preemption callbacks                     ? ? ?
?  ?  ???????????????????????????????????????????????? ? ?
?  ?????????????????????????????????????????????????????? ?
?                                                          ?
?  ??????????  ??????????  ??????????                    ?
?  ? CPU 0  ?  ? CPU 1  ?  ? CPU N  ?                    ?
?  ??????????  ??????????  ??????????                    ?
???????????????????????????????????????????????????????????
```

### Key Interfaces

1. **ICPUScheduler** - Abstract scheduler interface
2. **IPreemptionCallback** - Hooks for preemption logic
3. **IVirtualMachine** - Extended VM execution control

## Stepping Modes

### 1. Step Single CPU (`stepCPU`)

Executes a single instruction on a specific CPU, bypassing the scheduler's normal policy.

**Use Cases:**
- Debugging specific CPUs
- Single-stepping through code on one processor
- Testing CPU-specific behavior

**Example:**
```cpp
VirtualMachine vm(16 * 1024 * 1024, 4);  // 4 CPUs
vm.init();

// Step CPU 2 specifically
bool success = vm.stepCPU(2);
if (success) {
    std::cout << "CPU 2 executed one instruction\n";
}
```

**Implementation Details:**
- Validates CPU index
- Checks CPU state (enabled, running)
- Executes single instruction
- Updates CPU statistics
- Returns success/failure status

### 2. Step All CPUs (`stepAllCPUs`)

Executes one instruction on each enabled/running CPU in sequence.

**Use Cases:**
- Simulating parallel execution
- Fair time-sharing across all CPUs
- Testing multi-CPU interactions

**Example:**
```cpp
VirtualMachine vm(16 * 1024 * 1024, 4);  // 4 CPUs
vm.init();

// Enable all CPUs
for (int i = 0; i < 4; i++) {
    vm.getCPUContext(i)->enabled = true;
    vm.getCPUContext(i)->state = CPUExecutionState::RUNNING;
}

// Step all CPUs once
int count = vm.stepAllCPUs();
std::cout << count << " CPUs executed\n";
```

**Implementation Details:**
- Iterates through all CPU contexts (0 to N-1)
- Skips disabled or non-running CPUs
- Executes one instruction per enabled CPU
- Returns count of successful executions

### 3. Step by Bundle Quantum (`stepQuantum`)

Executes a specified number of IA-64 instruction bundles on a CPU.

**Use Cases:**
- Time-sliced scheduling
- Quantum-based preemption
- Performance testing with controlled execution bursts

**IA-64 Bundle Structure:**
- Each bundle is 128 bits (16 bytes)
- Contains 3 instructions (41 bits each)
- Plus 5-bit template field
- Bundle quantum of N = 3*N instructions

**Example:**
```cpp
VirtualMachine vm(16 * 1024 * 1024, 1);
vm.init();

// Set quantum size to 10 bundles (30 instructions)
vm.setQuantumSize(10);

// Execute 5 bundles on CPU 0
uint64_t executed = vm.stepQuantum(0, 5);
std::cout << "Executed " << executed << " bundles\n";
std::cout << "  = " << (executed * 3) << " instructions\n";
```

**Implementation Details:**
- Validates CPU index and state
- Loops through N bundles
- Executes 3 instructions per bundle
- Invokes preemption callbacks at each step
- Stops early if CPU halts or preemption requested
- Returns actual bundles executed

## Preemption Support

### IPreemptionCallback Interface

The scheduler provides hooks for external preemption logic:

```cpp
class IPreemptionCallback {
public:
    // Called before scheduling decision
    virtual bool onBeforeSchedule(int currentCPU, 
                                  const std::vector<CPUContext*>& cpus);
    
    // Called after each instruction execution
    virtual bool onAfterInstruction(int cpuIndex, 
                                    uint64_t instructionsInQuantum);
    
    // Called when quantum expires
    virtual void onQuantumExpired(int cpuIndex, 
                                  uint64_t bundlesExecuted);
};
```

### Callback Invocation Points

1. **Before Schedule**: Before selecting next CPU
   - Can force preemption
   - Can inspect all CPU states
   - Returns false to preempt

2. **After Instruction**: After each instruction in quantum
   - Can count instructions
   - Can enforce time limits
   - Returns false to preempt

3. **Quantum Expired**: After quantum completes or preempts
   - Notification only
   - Can update statistics
   - Can log performance data

### Example: Time-Limit Preemption

```cpp
class TimeLimitCallback : public IPreemptionCallback {
    uint64_t maxInstructions = 100;
    
public:
    bool onAfterInstruction(int cpuIndex, uint64_t instructionsInQuantum) override {
        if (instructionsInQuantum >= maxInstructions) {
            std::cout << "Time limit reached, preempting CPU " << cpuIndex << "\n";
            return false;  // Preempt
        }
        return true;  // Continue
    }
    
    // ... other methods
};

// Usage:
TimeLimitCallback callback;
// Register with scheduler (requires access to scheduler instance)
// scheduler->registerPreemptionCallback(&callback);
```

## Quantum Size Control

### Set Quantum Size

Controls how many bundles are executed in `stepQuantum`:

```cpp
vm.setQuantumSize(20);  // 20 bundles = 60 instructions
```

### Get Quantum Size

Query current quantum setting:

```cpp
uint64_t size = vm.getQuantumSize();
std::cout << "Quantum: " << size << " bundles\n";
```

### Default Quantum

The default quantum size is **10 bundles** (30 instructions).

## Scheduling Policies

### SimpleCPUScheduler (Round-Robin)

The default scheduler implements round-robin scheduling:

1. Maintains `lastCPUIndex_` to track last executed CPU
2. `selectNextCPU()` finds next runnable CPU after last
3. Wraps around to CPU 0 after last CPU
4. Skips disabled or non-running CPUs

**Characteristics:**
- Fair time distribution
- Deterministic order
- Low overhead
- Suitable for most use cases

### Future Schedulers

The pluggable `ICPUScheduler` interface allows custom schedulers:

- **PriorityScheduler**: Weighted CPU priorities
- **LoadBalancedScheduler**: Balance instruction counts
- **RealTimeScheduler**: Guaranteed time slices
- **InteractiveScheduler**: Boost responsive CPUs

## Multi-CPU Execution Model

### Interleaved Simulation

The hypervisor simulates parallel execution by interleaving:

```
Time   CPU 0       CPU 1       CPU 2
????   ?????       ?????       ?????
  0    INSTR 0
  1                INSTR 0
  2                            INSTR 0
  3    INSTR 1
  4                INSTR 1
  5                            INSTR 1
```

### CPU Isolation

Each CPU has completely isolated state:
- Independent register files (128 GR, 128 FR, 64 PR)
- Separate instruction pointer (IP)
- Isolated processor status register (PSR)
- Independent register frames (CFM)

### Shared Resources

All CPUs share:
- Physical memory system
- Instruction decoder (stateless)
- Breakpoints (VM-level debugging)

## Performance Considerations

### Bundle Quantum Efficiency

Larger quantum = fewer scheduler invocations:
- 1 bundle quantum: High overhead, fine-grained control
- 10 bundle quantum (default): Balanced
- 100 bundle quantum: Low overhead, coarse control

### Callback Overhead

Preemption callbacks are invoked frequently:
- Before each schedule decision
- After each instruction in quantum
- On quantum expiration

Keep callbacks lightweight for performance.

### Statistics Tracking

The scheduler maintains per-CPU counters:
- `cyclesExecuted`: Total cycles
- `instructionsExecuted`: Total instructions
- `cpuBundleCounters_`: Bundles executed (scheduler-level)

## API Reference

### VirtualMachine Methods

```cpp
// Step specific CPU
bool stepCPU(int cpuIndex);

// Step all CPUs
int stepAllCPUs();

// Step by bundle quantum
uint64_t stepQuantum(int cpuIndex, uint64_t bundleCount);

// Get/Set quantum size
uint64_t getQuantumSize() const;
void setQuantumSize(uint64_t bundleCount);
```

### ICPUScheduler Methods

```cpp
// Extended stepping
bool stepCPU(std::vector<CPUContext*>& cpus, int cpuIndex);
int stepAllCPUs(std::vector<CPUContext*>& cpus);
uint64_t stepQuantum(std::vector<CPUContext*>& cpus, 
                     int cpuIndex, 
                     uint64_t bundleCount);

// Preemption support
void registerPreemptionCallback(IPreemptionCallback* callback);
void unregisterPreemptionCallback(IPreemptionCallback* callback);
uint64_t getQuantumSize() const;
void setQuantumSize(uint64_t bundleCount);
```

## Testing

See `tests/test_scheduler_stepping.cpp` for comprehensive examples:

1. **test_stepCPU**: Single CPU stepping
2. **test_stepAllCPUs**: All CPU stepping
3. **test_stepQuantum**: Bundle quantum execution
4. **test_preemptionCallbacks**: Callback infrastructure
5. **test_quantumSizeControl**: Quantum size management

## Future Enhancements

### Planned Features

1. **Interrupt Injection**
   - Preempt CPU on virtual interrupt
   - Inter-processor interrupts (IPI)

2. **CPU Affinity**
   - Pin tasks to specific CPUs
   - NUMA-aware scheduling

3. **Performance Monitoring**
   - Per-CPU performance counters
   - Instruction-level profiling

4. **Advanced Scheduling**
   - Completely Fair Scheduler (CFS)
   - Real-time guarantees
   - SMP load balancing

5. **Migration Support**
   - CPU context checkpointing
   - Live migration between CPUs

## References

- `include/ICPUScheduler.h` - Scheduler interface
- `include/SimpleCPUScheduler.h` - Round-robin implementation
- `include/IVirtualMachine.h` - VM execution interface
- `docs/IA64_BUNDLE_DECODER.md` - IA-64 bundle format
- `docs/CPU_IMPLEMENTATION.md` - CPU architecture

## Version History

- **v1.0** (Current): Initial implementation
  - stepCPU, stepAllCPUs, stepQuantum
  - Preemption callback infrastructure
  - Quantum size control
  - Round-robin scheduler
