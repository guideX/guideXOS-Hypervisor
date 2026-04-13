# Boot Trace & Kernel Panic - Quick Reference

## Quick Start

```cpp
#include "VirtualMachine.h"

// Create VM
VirtualMachine vm(16 * 1024 * 1024);
vm.init();
vm.loadProgram(data, size, 0x1000);
vm.setEntryPoint(0x1000);

// Execute with boot tracing
vm.run(10000);

// Get reports
std::cout << vm.getBootTimeline();      // Boot sequence
std::cout << vm.getBootTraceReport();   // Full trace
std::cout << vm.getSyscallSummary();    // Syscalls

// Check for panic
if (vm.hasKernelPanic()) {
    std::cout << vm.getLastPanicReport();
}
```

## Boot Trace Configuration

```cpp
// Verbosity: 0=critical, 1=major, 2=all
vm.setBootTraceVerbosity(1);

// Enable/disable
vm.setBootTraceEnabled(true);

// Access trace system
auto& trace = vm.getBootTraceSystem();
```

## Common Queries

```cpp
auto& trace = vm.getBootTraceSystem();

// Statistics
auto stats = trace.getStatistics();
stats.totalBootCycles;
stats.syscallCount;

// Events by type
trace.getEventsByType(BootTraceEventType::SYSCALL_EXECUTED);
trace.getEventsByType(BootTraceEventType::KERNEL_ENTRY_POINT);

// Last N events
trace.getLastEvents(20);

// Time range
trace.getEventsInRange(startCycle, endCycle);
```

## Panic Detection

```cpp
// Automatic detection
vm.step();  // Panics captured automatically

if (vm.hasKernelPanic()) {
    auto* panic = vm.getLastPanic();
    std::cout << "Reason: " << kernelPanicReasonToString(panic->reason) << "\n";
    std::cout << "IP: 0x" << std::hex << panic->registers.instructionPointer << "\n";
    
    // Full report
    std::cout << vm.getLastPanicReport();
}

// Manual trigger
vm.triggerKernelPanic(
    KernelPanicReason::ASSERTION_FAILURE,
    "Test condition failed"
);
```

## Event Types (Common)

### Boot States
- `POWER_ON` - VM started
- `BOOT_STATE_CHANGE` - State transition
- `KERNEL_LOADED` - Kernel in memory
- `KERNEL_ENTRY_POINT` - Execution started
- `USERSPACE_READY` - Boot complete

### Memory
- `MEMORY_ALLOCATED` - Memory created
- `MEMORY_REGION_MAPPED` - Region mapped
- `PAGE_TABLE_INIT` - Paging enabled

### Execution
- `SYSCALL_FIRST` - First syscall
- `SYSCALL_EXECUTED` - Syscall
- `CPU_INSTRUCTION_EXECUTED` - Milestone
- `INTERRUPT_RAISED` - Interrupt

### Errors
- `BOOT_FAILURE` - Boot failed
- `KERNEL_PANIC` - Panic occurred
- `EXCEPTION_OCCURRED` - Exception

## Panic Reasons

- `INVALID_OPCODE` - Bad instruction
- `ILLEGAL_INSTRUCTION` - Illegal op
- `PAGE_FAULT` - Page fault
- `INVALID_MEMORY_ACCESS` - Bad memory access
- `STACK_OVERFLOW` - Stack overflow
- `DOUBLE_FAULT` - Double fault
- `ASSERTION_FAILURE` - Assert failed
- `UNHANDLED_EXCEPTION` - Unhandled exception

## Report Types

```cpp
// Boot timeline (key milestones)
vm.getBootTimeline();

// Full trace (all events)
vm.getBootTraceReport();

// Syscall summary
vm.getSyscallSummary();

// Panic report (full diagnostics)
vm.getLastPanicReport();
```

## Integration Points

Boot tracing automatically records:
1. **VM Init** - Memory allocation, power-on
2. **Program Load** - Kernel load, entry point
3. **CPU Execution** - Every 10,000 instructions
4. **Syscalls** - Via SyscallDispatcher
5. **State Changes** - Boot state machine
6. **Panics** - Exception handlers

## Performance Tips

```cpp
// Production: critical events only
vm.setBootTraceVerbosity(0);

// Development: major events
vm.setBootTraceVerbosity(1);

// Debug: all events (default)
vm.setBootTraceVerbosity(2);

// Disable tracing completely
vm.setBootTraceEnabled(false);
```

## Code Snippets

### Monitor Boot Progress

```cpp
vm.init();
vm.loadProgram(data, size, 0x1000);
vm.setEntryPoint(0x1000);

// Check boot state
while (vm.getBootState() != VMBootState::USERSPACE_RUNNING) {
    if (!vm.step()) break;
    
    // Log progress every 1000 cycles
    if (vm.getCyclesExecuted() % 1000 == 0) {
        std::cout << "Boot state: " 
                 << bootStateToString(vm.getBootState()) << "\n";
    }
}

std::cout << "Boot complete!\n";
std::cout << vm.getBootTimeline();
```

### Capture First Syscall

```cpp
vm.run(100000);

auto& trace = vm.getBootTraceSystem();
auto firstSyscall = trace.getEventsByType(BootTraceEventType::SYSCALL_FIRST);

if (!firstSyscall.empty()) {
    auto& event = firstSyscall[0];
    std::cout << "First syscall at cycle " << event.timestamp << "\n";
    std::cout << "Syscall number: " << event.data1 << "\n";
}
```

### Post-Mortem Analysis

```cpp
try {
    vm.run();
} catch (...) {
    // VM crashed
}

if (vm.hasKernelPanic()) {
    // Save panic report
    std::ofstream panic_log("panic.txt");
    panic_log << vm.getLastPanicReport();
    
    // Save boot trace
    std::ofstream boot_log("boot_trace.txt");
    boot_log << vm.getBootTraceReport();
    
    // Analyze last events
    auto last20 = vm.getBootTraceSystem().getLastEvents(20);
    std::cout << "Last 20 events before panic:\n";
    for (const auto& event : last20) {
        std::cout << event.description << "\n";
    }
}
```

## Common Patterns

### Boot Performance Analysis

```cpp
auto stats = vm.getBootTraceSystem().getStatistics();

std::cout << "Boot Performance:\n";
std::cout << "  Total cycles: " << stats.totalBootCycles << "\n";
std::cout << "  Firmware: " 
         << (stats.kernelEntryTimestamp - stats.powerOnTimestamp) << "\n";
std::cout << "  Kernel init: "
         << (stats.userSpaceTimestamp - stats.kernelEntryTimestamp) << "\n";
```

### Syscall Monitoring

```cpp
vm.run(100000);

auto& trace = vm.getBootTraceSystem();
auto syscalls = trace.getEventsByType(BootTraceEventType::SYSCALL_EXECUTED);

std::cout << "Executed " << syscalls.size() << " syscalls\n";
std::cout << vm.getSyscallSummary();
```

### Panic Recovery

```cpp
// Save state before risky operation
auto snapshot = vm.createFullSnapshot("before_operation");

// Try operation
if (!vm.step()) {
    if (vm.hasKernelPanic()) {
        // Log panic
        std::cerr << "Panic occurred:\n" << vm.getLastPanicReport();
        
        // Restore state
        vm.restoreFromSnapshot(snapshot);
        vm.clearPanicState();
        
        // Try alternative approach
        // ...
    }
}
```

## See Also

- `docs/BOOT_TRACE_AND_PANIC.md` - Full documentation
- `examples/boot_trace_example.cpp` - Complete examples
- `include/BootTraceSystem.h` - API reference
- `include/KernelPanic.h` - Panic structures
