# Memory Watchpoint System

## Overview

The memory watchpoint system provides advanced debugging capabilities for the IA-64 hypervisor. Watchpoints allow you to monitor and intercept memory accesses at specific addresses or ranges with sophisticated triggering conditions and instruction-level tracing.

## Features

- **Address and Range Watchpoints**: Monitor single addresses or address ranges
- **Access Type Filtering**: Trigger on read, write, or any access
- **Conditional Triggering**: Advanced conditions (value equals, changed, greater/less than)
- **Instruction Trace Capture**: Automatically capture instruction execution history on trigger
- **Hit Counting**: Auto-disable after N triggers
- **Access Control**: Block or allow memory access from callbacks
- **CPU State Integration**: Access full CPU state when watchpoints trigger

## Basic Usage

### 1. Simple Write Watchpoint

```cpp
#include "memory.h"
#include "Watchpoint.h"

// Create memory system
Memory memory(64 * 1024 * 1024);  // 64MB
MMU& mmu = memory.GetMMU();

// Create a watchpoint that triggers on writes to 0x1000
Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Write detected at 0x" << std::hex << ctx.address 
                  << " value: 0x" << ctx.newValue << std::endl;
    });

// Register the watchpoint
size_t wpId = mmu.RegisterWatchpoint(wp);

// This write will trigger the watchpoint
memory.write<uint32_t>(0x1000, 0xDEADBEEF);

// Unregister when done
mmu.UnregisterWatchpoint(wpId);
```

### 2. Read Watchpoint

```cpp
// Monitor reads from a specific address
Watchpoint readWp = CreateAddressWatchpoint(0x2000, WatchpointType::READ,
    [](WatchpointContext& ctx) {
        std::cout << "Read from 0x" << std::hex << ctx.address << std::endl;
    });

mmu.RegisterWatchpoint(readWp);

// This read will trigger
uint32_t value = memory.read<uint32_t>(0x2000);
```

### 3. Access Watchpoint (Read or Write)

```cpp
// Monitor any access (read or write)
Watchpoint accessWp = CreateAddressWatchpoint(0x3000, WatchpointType::ACCESS,
    [](WatchpointContext& ctx) {
        std::cout << (ctx.isWrite ? "WRITE" : "READ") << " at 0x" 
                  << std::hex << ctx.address << std::endl;
    });

mmu.RegisterWatchpoint(accessWp);
```

## Advanced Features

### Range Watchpoints

Monitor a range of addresses:

```cpp
// Watch a 256-byte buffer
Watchpoint rangeWp = CreateRangeWatchpoint(
    0x10000,     // Start address (inclusive)
    0x10100,     // End address (exclusive)
    WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Write in range at offset: 0x" 
                  << std::hex << (ctx.address - ctx.watchpointStart) << std::endl;
    }
);

mmu.RegisterWatchpoint(rangeWp);

// Any write in the range will trigger
memory.write<uint32_t>(0x10050, 0x12345678);
```

### Conditional Watchpoints

#### Value Equals

```cpp
// Trigger only when specific value is written
Watchpoint valueWp = CreateAddressWatchpoint(0x4000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Magic value 0x42 written!" << std::endl;
    });
valueWp.condition = WatchpointCondition::VALUE_EQUALS;
valueWp.conditionValue = 0x42;

mmu.RegisterWatchpoint(valueWp);

memory.write<uint32_t>(0x4000, 0x41);  // No trigger
memory.write<uint32_t>(0x4000, 0x42);  // Triggers!
```

#### Value Changed

```cpp
// Trigger only when value changes
Watchpoint changeWp = CreateAddressWatchpoint(0x5000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Value changed from 0x" << std::hex << ctx.oldValue 
                  << " to 0x" << ctx.newValue << std::endl;
    });
changeWp.condition = WatchpointCondition::VALUE_CHANGED;

mmu.RegisterWatchpoint(changeWp);

memory.write<uint32_t>(0x5000, 100);  // No trigger (first write)
memory.write<uint32_t>(0x5000, 100);  // No trigger (same value)
memory.write<uint32_t>(0x5000, 200);  // Triggers! (value changed)
```

#### Value Greater/Less Than

```cpp
// Trigger when value exceeds threshold
Watchpoint thresholdWp = CreateAddressWatchpoint(0x6000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Threshold exceeded: 0x" << std::hex << ctx.newValue << std::endl;
    });
thresholdWp.condition = WatchpointCondition::VALUE_GREATER;
thresholdWp.conditionValue = 1000;

mmu.RegisterWatchpoint(thresholdWp);

memory.write<uint32_t>(0x6000, 999);   // No trigger
memory.write<uint32_t>(0x6000, 1001);  // Triggers!
```

### Access Control

Block or allow memory access from watchpoint callback:

```cpp
// Create a read-only watchpoint by blocking writes
Watchpoint readOnlyWp = CreateAddressWatchpoint(0x7000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Blocking write to protected address!" << std::endl;
        *ctx.skipAccess = true;  // Block the write
    });

mmu.RegisterWatchpoint(readOnlyWp);

memory.write<uint32_t>(0x7000, 0x12345678);  // Write is blocked
uint32_t value = memory.read<uint32_t>(0x7000);  // Still 0, write was prevented
```

### Hit Counting and Auto-Disable

```cpp
// Trigger only first 3 times, then auto-disable
Watchpoint limitedWp = CreateAddressWatchpoint(0x8000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Trigger #" << ctx.watchpointStart << std::endl;
    });
limitedWp.maxTriggers = 3;

size_t wpId = mmu.RegisterWatchpoint(limitedWp);

for (int i = 0; i < 5; i++) {
    memory.write<uint32_t>(0x8000, i);  // Only first 3 trigger
}

// Watchpoint is now disabled
const Watchpoint* wp = mmu.GetWatchpoint(wpId);
assert(!wp->enabled);
assert(wp->triggerCount == 3);
```

### Manual Enable/Disable

```cpp
size_t wpId = mmu.RegisterWatchpoint(someWatchpoint);

// Temporarily disable
mmu.SetWatchpointEnabled(wpId, false);
// ... do some work without triggering ...

// Re-enable
mmu.SetWatchpointEnabled(wpId, true);
```

## Instruction Tracing

### Basic Instruction Trace Capture

```cpp
// Enable instruction tracing in MMU
mmu.SetInstructionTraceDepth(100);  // Keep last 100 instructions

// Somewhere in your CPU execution loop:
void executeBundleWithTrace(CPU& cpu, MMU& mmu) {
    // Before executing each instruction
    InstructionTrace trace;
    trace.instructionPointer = cpu.getIP();
    trace.bundleAddress = cpu.getBundleAddress();
    trace.disassembly = cpu.disassembleCurrentInstruction();
    trace.timestamp = cpu.getCycleCount();
    
    mmu.AddInstructionTrace(trace);
    
    // Execute instruction...
}
```

### Watchpoint with Instruction Trace

```cpp
// Create watchpoint that captures instruction history
Watchpoint traceWp = CreateAddressWatchpoint(0x9000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        std::cout << "Write at 0x" << std::hex << ctx.address << std::endl;
        std::cout << "Recent instruction history:" << std::endl;
        
        for (const auto& trace : ctx.instructionTrace) {
            std::cout << "  IP: 0x" << std::hex << trace.instructionPointer
                      << " - " << trace.disassembly << std::endl;
        }
    });

// Enable instruction capture for this watchpoint
traceWp.captureInstructions = true;
traceWp.instructionTraceDepth = 10;  // Capture last 10 instructions

mmu.RegisterWatchpoint(traceWp);
```

### CPU State Integration

```cpp
// Provide CPU state reference for watchpoints
CPUState* cpuState = &cpu.getState();
mmu.SetCPUStateReference(cpuState);

// Now watchpoints can access CPU state
Watchpoint cpuWp = CreateAddressWatchpoint(0xA000, WatchpointType::WRITE,
    [](WatchpointContext& ctx) {
        if (ctx.cpuState) {
            std::cout << "Write occurred at IP: 0x" << std::hex 
                      << ctx.cpuState->ip << std::endl;
            std::cout << "Register r1: 0x" << ctx.cpuState->gr[1] << std::endl;
        }
    });

mmu.RegisterWatchpoint(cpuWp);
```

## Complete Example: Debugging Buffer Overflow

```cpp
#include "memory.h"
#include "Watchpoint.h"
#include <iostream>

void setupBufferOverflowDetection(Memory& memory, uint64_t bufferStart, size_t bufferSize) {
    MMU& mmu = memory.GetMMU();
    
    // Enable instruction tracing
    mmu.SetInstructionTraceDepth(50);
    
    // Watchpoint for valid buffer range
    Watchpoint validRangeWp = CreateRangeWatchpoint(
        bufferStart,
        bufferStart + bufferSize,
        WatchpointType::WRITE,
        [bufferStart, bufferSize](WatchpointContext& ctx) {
            std::cout << "[INFO] Valid write to buffer at offset "
                      << (ctx.address - bufferStart) << std::endl;
        }
    );
    validRangeWp.description = "Valid buffer range monitor";
    mmu.RegisterWatchpoint(validRangeWp);
    
    // Watchpoint for overflow detection (guard page after buffer)
    Watchpoint overflowWp = CreateRangeWatchpoint(
        bufferStart + bufferSize,      // Right after buffer
        bufferStart + bufferSize + 16, // 16-byte guard zone
        WatchpointType::WRITE,
        [bufferStart, bufferSize](WatchpointContext& ctx) {
            std::cerr << "!!! BUFFER OVERFLOW DETECTED !!!" << std::endl;
            std::cerr << "Buffer: 0x" << std::hex << bufferStart 
                      << " - 0x" << (bufferStart + bufferSize) << std::endl;
            std::cerr << "Overflow write at: 0x" << ctx.address << std::endl;
            std::cerr << "Overflow offset: +" << std::dec 
                      << (ctx.address - (bufferStart + bufferSize)) << " bytes" << std::endl;
            
            // Print instruction trace
            std::cerr << "\nRecent instructions:" << std::endl;
            for (const auto& trace : ctx.instructionTrace) {
                std::cerr << "  0x" << std::hex << trace.instructionPointer 
                          << ": " << trace.disassembly << std::endl;
            }
            
            // Print CPU state if available
            if (ctx.cpuState) {
                std::cerr << "\nCPU State:" << std::endl;
                std::cerr << "  IP: 0x" << std::hex << ctx.cpuState->ip << std::endl;
            }
            
            // Block the overflow write
            *ctx.skipAccess = true;
            
            // Optionally halt execution
            // *ctx.continueExecution = false;
        }
    );
    overflowWp.description = "Buffer overflow guard";
    overflowWp.captureInstructions = true;
    overflowWp.instructionTraceDepth = 20;
    
    mmu.RegisterWatchpoint(overflowWp);
}

int main() {
    Memory memory(64 * 1024 * 1024);
    
    uint64_t bufferAddr = 0x10000;
    size_t bufferSize = 256;
    
    setupBufferOverflowDetection(memory, bufferAddr, bufferSize);
    
    // Normal write - OK
    memory.write<uint32_t>(bufferAddr + 100, 0x12345678);
    
    // Overflow write - DETECTED AND BLOCKED
    memory.write<uint32_t>(bufferAddr + bufferSize + 4, 0xDEADBEEF);
    
    return 0;
}
```

## Watchpoint Management

### Query Watchpoints

```cpp
// Get specific watchpoint
const Watchpoint* wp = mmu.GetWatchpoint(wpId);
if (wp) {
    std::cout << "Watchpoint " << wp->id << ":" << std::endl;
    std::cout << "  Range: 0x" << std::hex << wp->addressStart 
              << " - 0x" << wp->addressEnd << std::endl;
    std::cout << "  Type: " << WatchpointTypeToString(wp->type) << std::endl;
    std::cout << "  Enabled: " << (wp->enabled ? "Yes" : "No") << std::endl;
    std::cout << "  Triggers: " << std::dec << wp->triggerCount << std::endl;
}

// Get all watchpoints
auto allWatchpoints = mmu.GetAllWatchpoints();
for (const auto& wp : allWatchpoints) {
    std::cout << "Watchpoint " << wp.id << ": " 
              << wp.description << std::endl;
}

// Get count
size_t count = mmu.GetWatchpointCount();
std::cout << "Active watchpoints: " << count << std::endl;
```

### Clear All Watchpoints

```cpp
// Remove all watchpoints at once
mmu.ClearWatchpoints();
```

## Best Practices

1. **Use Descriptive Names**: Always set the `description` field for easier debugging
   ```cpp
   wp.description = "Stack pointer overflow detection";
   ```

2. **Limit Trace Depth**: Don't capture excessive instruction history unless needed
   ```cpp
   wp.instructionTraceDepth = 10;  // Usually sufficient
   ```

3. **Use Hit Counting**: Prevent performance degradation in loops
   ```cpp
   wp.maxTriggers = 1;  // Trigger once then auto-disable
   ```

4. **Conditional Watchpoints**: Use conditions to reduce noise
   ```cpp
   wp.condition = WatchpointCondition::VALUE_CHANGED;
   ```

5. **Clean Up**: Unregister watchpoints when done
   ```cpp
   mmu.UnregisterWatchpoint(wpId);
   ```

6. **Combine with Logging**: Integrate with the logger system
   ```cpp
   Logger::getInstance().setLogLevel(LogLevel::DEBUG);
   ```

## Performance Considerations

- Watchpoints are checked on every memory access when registered
- More watchpoints = more overhead per access
- Use range watchpoints sparingly (check overlap on each access)
- Instruction tracing adds overhead to CPU execution
- Consider using conditional watchpoints to reduce triggers
- Use `maxTriggers` for one-time debugging scenarios

## Integration with Debugger

The watchpoint system is designed to integrate with debuggers:

```cpp
class Debugger {
    Memory& memory_;
    std::map<std::string, size_t> namedWatchpoints_;
    
public:
    void setWatchpoint(const std::string& name, uint64_t addr, WatchpointType type) {
        Watchpoint wp = CreateAddressWatchpoint(addr, type,
            [this, name](WatchpointContext& ctx) {
                handleWatchpointTrigger(name, ctx);
            });
        wp.description = name;
        wp.captureInstructions = true;
        wp.instructionTraceDepth = 20;
        
        size_t id = memory_.GetMMU().RegisterWatchpoint(wp);
        namedWatchpoints_[name] = id;
    }
    
    void handleWatchpointTrigger(const std::string& name, WatchpointContext& ctx) {
        // Debugger UI integration here
        showDebugWindow(name, ctx);
    }
};
```

## API Reference

### Types

- `WatchpointType`: `READ`, `WRITE`, `ACCESS`
- `WatchpointCondition`: `ALWAYS`, `VALUE_EQUALS`, `VALUE_NOT_EQUALS`, `VALUE_CHANGED`, `VALUE_GREATER`, `VALUE_LESS`

### Structures

- `Watchpoint`: Main watchpoint configuration
- `WatchpointContext`: Information passed to callbacks
- `InstructionTrace`: Captured instruction execution data

### Helper Functions

- `CreateAddressWatchpoint(addr, type, callback)`: Create single-address watchpoint
- `CreateRangeWatchpoint(start, end, type, callback)`: Create range watchpoint
- `WatchpointTypeToString(type)`: Convert type to string
- `WatchpointConditionToString(condition)`: Convert condition to string

### MMU Methods

- `RegisterWatchpoint(watchpoint)`: Register and activate watchpoint
- `UnregisterWatchpoint(id)`: Remove watchpoint
- `SetWatchpointEnabled(id, enabled)`: Enable/disable watchpoint
- `GetWatchpoint(id)`: Get watchpoint by ID
- `GetAllWatchpoints()`: Get all registered watchpoints
- `ClearWatchpoints()`: Remove all watchpoints
- `GetWatchpointCount()`: Get number of active watchpoints
- `SetCPUStateReference(cpuState)`: Set CPU state for tracing
- `AddInstructionTrace(trace)`: Add instruction to trace buffer
- `SetInstructionTraceDepth(depth)`: Set trace buffer size

## Troubleshooting

### Watchpoint Not Triggering

1. Check if watchpoint is enabled: `wp->enabled`
2. Verify address range matches access
3. Check if condition is satisfied
4. Ensure MMU is enabled: `mmu.IsEnabled()`
5. Verify maxTriggers hasn't been reached

### Performance Issues

1. Reduce number of active watchpoints
2. Use more specific conditions
3. Disable instruction tracing if not needed
4. Set appropriate `instructionTraceDepth`

### Missing Instruction Traces

1. Ensure `captureInstructions = true`
2. Set `instructionTraceDepth > 0`
3. Verify CPU is calling `AddInstructionTrace()`
4. Check that `SetInstructionTraceDepth()` is called

## Future Enhancements

Planned features for future releases:

- Hardware breakpoint emulation
- Conditional expressions (boolean logic)
- Watchpoint hit history logging
- Automatic watchpoint suggestions
- Visual watchpoint management UI
- Export/import watchpoint configurations
- Watchpoint templates for common patterns
