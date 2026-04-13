# Boot Trace System and Kernel Panic Detection

## Overview

The guideXOS Hypervisor provides comprehensive boot tracing and kernel panic detection capabilities that record every step of the virtual machine lifecycle from `POWER_ON` through `USERSPACE_RUNNING`, including detailed diagnostics when system failures occur.

## Features

### Boot Trace System

The **Boot Trace System** records:
- **Power-on and initialization** events
- **Memory allocation and mapping** operations
- **Boot state transitions** (POWER_ON ? FIRMWARE_INIT ? ... ? USERSPACE_RUNNING)
- **Kernel loading and entry point** execution
- **Syscall execution** (first syscall and subsequent calls)
- **CPU execution milestones** (every N instructions)
- **Interrupts and exceptions**
- **Boot failures and panics**

Features:
- **Circular buffer** prevents memory exhaustion (configurable size)
- **Verbosity levels** (critical, major milestones, all events)
- **Timeline reconstruction** showing boot sequence
- **Statistics collection** (boot time, syscall count, etc.)
- **Query interface** for event analysis

### Kernel Panic Detection

The **Kernel Panic Detection System** captures:
- **Full register dump** at moment of failure
  - All general registers (GR0-GR127)
  - Branch registers (BR0-BR7)
  - Application registers (AR)
  - Special registers (IP, CFM, PSR)
- **Stack trace** with frame pointers
- **Last executed instruction bundle** with slot information
- **Recent boot trace events** leading to panic
- **Memory state** (faulting address, MMU status)

Panic triggers:
- Invalid opcodes
- Illegal instructions
- Page faults
- Memory access violations
- Unhandled exceptions
- CPU execution failures
- Manual panic triggers

## Architecture

```
VirtualMachine
??? BootTraceSystem (10K event circular buffer)
?   ??? Records events from all subsystems
?   ??? Tracks boot state transitions
?   ??? Provides query and reporting
?
??? KernelPanicDetector
?   ??? Monitors CPU execution
?   ??? Captures system state on failure
?   ??? Generates diagnostic reports
?
??? Integration Points
    ??? VM initialization
    ??? CPU execution (stepCPU)
    ??? SyscallDispatcher
    ??? Boot state machine
```

## Usage

### Basic Boot Tracing

```cpp
#include "VirtualMachine.h"
#include "BootTraceSystem.h"

// Create VM
VirtualMachine vm(16 * 1024 * 1024);  // 16MB memory

// Initialize (records POWER_ON, FIRMWARE_INIT)
vm.init();

// Load program (records KERNEL_LOAD)
vm.loadProgram(data, size, 0x1000);

// Set entry point (records KERNEL_ENTRY)
vm.setEntryPoint(0x1000);

// Execute (records milestones, syscalls, etc.)
vm.run(10000);

// Get boot trace report
std::cout << vm.getBootTraceReport();

// Get boot timeline
std::cout << vm.getBootTimeline();

// Get syscall summary
std::cout << vm.getSyscallSummary();
```

### Configuring Boot Trace

```cpp
// Set verbosity level
vm.setBootTraceVerbosity(2);  // 0=critical, 1=major, 2=all (default)

// Enable/disable tracing
vm.setBootTraceEnabled(true);

// Access boot trace system directly
auto& bootTrace = vm.getBootTraceSystem();

// Get statistics
auto stats = bootTrace.getStatistics();
std::cout << "Total boot cycles: " << stats.totalBootCycles << "\n";
std::cout << "Syscalls executed: " << stats.syscallCount << "\n";
```

### Querying Boot Events

```cpp
auto& bootTrace = vm.getBootTraceSystem();

// Get all events
auto allEvents = bootTrace.getAllEvents();

// Get events by type
auto stateChanges = bootTrace.getEventsByType(BootTraceEventType::BOOT_STATE_CHANGE);
auto syscalls = bootTrace.getEventsByType(BootTraceEventType::SYSCALL_EXECUTED);

// Get events by boot state
auto kernelEvents = bootTrace.getEventsByBootState(VMBootState::KERNEL_INIT);

// Get events in time range
auto recentEvents = bootTrace.getEventsInRange(startCycle, endCycle);

// Get last N events (useful for panic analysis)
auto lastTen = bootTrace.getLastEvents(10);
```

### Kernel Panic Detection

```cpp
// Execute VM - panic detection is automatic
for (int i = 0; i < 1000; i++) {
    if (!vm.step()) {
        // Check if panic occurred
        if (vm.hasKernelPanic()) {
            std::cout << "KERNEL PANIC DETECTED!\n";
            
            // Get panic report
            std::cout << vm.getLastPanicReport();
            
            // Access panic structure
            const KernelPanic* panic = vm.getLastPanic();
            std::cout << "Panic reason: " 
                     << kernelPanicReasonToString(panic->reason) << "\n";
            std::cout << "Faulting IP: 0x" << std::hex 
                     << panic->registers.instructionPointer << "\n";
            
            break;
        }
    }
}
```

### Manual Panic Trigger

```cpp
// Trigger panic manually (for testing or assertion failures)
auto panic = vm.triggerKernelPanic(
    KernelPanicReason::ASSERTION_FAILURE,
    "Expected condition X, got Y"
);

// Generate report
auto& detector = vm.getKernelPanicDetector();
std::cout << detector.generatePanicReport(panic);
```

## Boot Trace Event Types

### Power and Initialization
- `POWER_ON` - VM powered on
- `HARDWARE_RESET` - Hardware reset

### Memory Initialization
- `MEMORY_ALLOCATED` - Memory system allocated
- `MEMORY_REGION_MAPPED` - Memory region mapped
- `MEMORY_CLEARED` - Memory cleared
- `PAGE_TABLE_INIT` - Page table initialized

### Boot State Transitions
- `BOOT_STATE_CHANGE` - Boot state transition occurred

### Firmware/BIOS
- `FIRMWARE_START` - Firmware initialization started
- `FIRMWARE_POST` - Power-on self-test
- `FIRMWARE_DEVICE_ENUM` - Device enumeration
- `FIRMWARE_COMPLETE` - Firmware initialization complete

### Bootloader
- `BOOTLOADER_START` - Bootloader started
- `BOOTLOADER_LOAD_KERNEL` - Loading kernel
- `BOOTLOADER_JUMP` - Jumping to kernel

### Kernel
- `KERNEL_LOADED` - Kernel loaded into memory
- `KERNEL_ENTRY_POINT` - Kernel entry point reached
- `KERNEL_EARLY_INIT` - Early kernel initialization
- `KERNEL_SUBSYSTEM_INIT` - Kernel subsystem initialization
- `KERNEL_MOUNT_ROOT` - Root filesystem mounted
- `KERNEL_START_INIT` - Starting init process

### Syscalls
- `SYSCALL_FIRST` - First syscall executed
- `SYSCALL_EXECUTED` - Syscall executed

### Init and Userspace
- `INIT_PROCESS_START` - Init process started
- `USERSPACE_READY` - Userspace ready

### CPU Execution
- `CPU_INSTRUCTION_EXECUTED` - Instruction milestone
- `CPU_BUNDLE_EXECUTED` - Bundle executed
- `CPU_CONTEXT_SWITCH` - Context switch

### Interrupts and Exceptions
- `INTERRUPT_RAISED` - Interrupt raised
- `INTERRUPT_HANDLED` - Interrupt handled
- `EXCEPTION_OCCURRED` - Exception occurred

### Error Conditions
- `BOOT_FAILURE` - Boot process failed
- `KERNEL_PANIC` - Kernel panic
- `EMERGENCY_HALT` - Emergency halt

## Kernel Panic Reasons

- `INVALID_OPCODE` - Invalid instruction opcode
- `ILLEGAL_INSTRUCTION` - Illegal instruction
- `PAGE_FAULT` - Page fault
- `GENERAL_PROTECTION_FAULT` - General protection fault
- `DIVISION_BY_ZERO` - Division by zero
- `INVALID_MEMORY_ACCESS` - Invalid memory access
- `STACK_OVERFLOW` - Stack overflow
- `DOUBLE_FAULT` - Double fault
- `INVALID_STATE` - Invalid CPU state
- `ASSERTION_FAILURE` - Assertion failure
- `HARDWARE_FAILURE` - Hardware failure
- `WATCHDOG_TIMEOUT` - Watchdog timeout
- `UNHANDLED_EXCEPTION` - Unhandled exception
- `CUSTOM` - Custom panic reason

## Panic Report Format

A kernel panic report includes:

```
================================================================================
                           KERNEL PANIC DETECTED                                
================================================================================

Panic Reason: INVALID_OPCODE
Description:  Invalid opcode encountered at slot 1 in bundle at 0x0000000000001010
Timestamp:    1234567 cycles
Total Cycles: 1234567
Faulting Addr: 0x0000000000000000

--- CPU STATE AT PANIC ---
Instruction Pointer: 0x0000000000001010
Current Frame Marker: 0x0000000000000000
Processor Status: 0x0000000000000000

General Registers:
  r0-r3: 0x0000000000000000 0x0000000000000001 ...
  ...

Branch Registers:
  b0: 0x0000000000001000
  ...

--- LAST EXECUTED BUNDLE ---
Address: 0x0000000000001010
Bundle Raw: 0x0000000000000004
Template: 0x4 (4)
Slots:
  Slot 0: 0x00000000001
  Slot 1: 0x00000000000
  Slot 2: 0x00000000000
Last Slot: 1

--- STACK TRACE ---
Stack Depth: 1

#  Frame Pointer      Instruction Ptr     Return Address      Function
--------------------------------------------------------------------------------
0  0x0000000000fff000 0x0000000000001010 0x0000000000001000 current

--- RECENT BOOT EVENTS (Last 20 events) ---
Timestamp    | Event                         | Description
--------------------------------------------------------------------------------
0            | POWER_ON                      | Virtual machine powered on
0            | MEMORY_ALLOCATED              | Memory allocated: 16777216 bytes
...

================================================================================
                          END OF PANIC REPORT                                  
================================================================================
```

## Boot Statistics

The boot trace system collects statistics:

```cpp
struct BootTraceStatistics {
    uint64_t totalEvents;           // Total events recorded
    uint64_t powerOnTimestamp;      // When VM powered on
    uint64_t kernelEntryTimestamp;  // When kernel entry reached
    uint64_t userSpaceTimestamp;    // When userspace ready
    uint64_t totalBootCycles;       // Total cycles from power-on to userspace
    uint64_t syscallCount;          // Number of syscalls executed
    uint64_t interruptCount;        // Number of interrupts raised
    uint64_t exceptionCount;        // Number of exceptions
    bool bootCompleted;             // Whether boot completed successfully
    bool panicOccurred;             // Whether panic occurred
};
```

## Performance Considerations

### Boot Trace

- **Circular buffer** (default 10,000 events) prevents unbounded memory growth
- **Verbosity filtering** reduces overhead for production use
- **Event importance levels** allow selective tracing
- **Minimal overhead** when disabled

### Panic Detection

- **Zero overhead** during normal execution
- **Immediate capture** on failure with no additional execution
- **Stack trace depth** configurable (default 32 frames)

## Integration with SyscallDispatcher

The boot trace system automatically integrates with the syscall dispatcher:

```cpp
// Syscall dispatcher records to boot trace when connected
vm.init();  // Syscall dispatcher automatically linked to boot trace

// First syscall is specially marked
// Subsequent syscalls are recorded

// Get syscall summary
std::cout << vm.getSyscallSummary();
```

## Best Practices

1. **Set appropriate verbosity** for your use case:
   - Production: level 0 or 1 (critical/major events only)
   - Development: level 2 (all events)
   - Debugging: level 2 with increased buffer size

2. **Query specific event types** rather than dumping all events for performance

3. **Check for panic** after each execution phase in critical sections

4. **Save panic reports** for post-mortem analysis

5. **Use boot timeline** for boot performance optimization

6. **Monitor boot statistics** for regression detection

## Example Output

See `examples/boot_trace_example.cpp` for complete working examples demonstrating:
- Basic boot tracing
- Detailed trace reports
- Kernel panic detection
- Manual panic triggers
- Boot trace querying and analysis

## API Reference

### VirtualMachine Methods

```cpp
// Boot Trace
BootTraceSystem& getBootTraceSystem();
std::string getBootTraceReport() const;
std::string getBootTimeline() const;
std::string getSyscallSummary() const;
void setBootTraceEnabled(bool enabled);
bool isBootTraceEnabled() const;
void setBootTraceVerbosity(int level);

// Kernel Panic
KernelPanicDetector& getKernelPanicDetector();
KernelPanic triggerKernelPanic(KernelPanicReason reason, const std::string& description);
bool hasKernelPanic() const;
const KernelPanic* getLastPanic() const;
std::string getLastPanicReport() const;
void clearPanicState();
```

### BootTraceSystem Methods

```cpp
void recordEvent(BootTraceEventType type, uint64_t timestamp, ...);
void recordBootStateChange(VMBootState from, VMBootState to, ...);
void recordMemoryAllocation(uint64_t size, uint64_t timestamp);
void recordKernelEntry(uint64_t entryPoint, uint64_t timestamp);
void recordSyscall(uint64_t number, const std::string& name, ...);

std::vector<BootTraceEntry> getAllEvents() const;
std::vector<BootTraceEntry> getEventsByType(BootTraceEventType type) const;
std::vector<BootTraceEntry> getLastEvents(size_t count) const;

BootTraceStatistics getStatistics() const;
std::string generateTraceReport() const;
std::string generateBootTimeline() const;
```

### KernelPanicDetector Methods

```cpp
KernelPanic triggerPanic(KernelPanicReason reason, ...);
KernelPanic captureInvalidOpcodePanic(...);
KernelPanic captureMemoryAccessPanic(...);
KernelPanic captureExceptionPanic(...);

std::string generatePanicReport(const KernelPanic& panic) const;
std::string generateRegisterDump(const RegisterDump& dump) const;
std::string generateStackTrace(const StackTrace& trace) const;
```

## Future Enhancements

Planned improvements:
- Symbol table integration for function names in stack traces
- Compressed event storage for longer traces
- Event filtering by subsystem
- Real-time event streaming
- Panic handler callbacks
- Boot trace serialization/deserialization
- Performance profiling integration
