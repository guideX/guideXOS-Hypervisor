# Profiler Quick Reference

## Setup

```cpp
#include "Profiler.h"

// Create with default config
ia64::Profiler profiler;

// Or with custom config
ia64::ProfilerConfig config;
config.trackInstructionFrequency = true;
config.trackRegisterPressure = true;
// ... configure other options
ia64::Profiler profiler(config);

// Attach to CPU
cpu.setProfiler(&profiler);
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `trackInstructionFrequency` | bool | true | Track instruction execution counts |
| `trackExecutionPaths` | bool | true | Track execution path sequences |
| `trackRegisterPressure` | bool | true | Track register usage |
| `trackMemoryAccess` | bool | true | Track memory reads/writes |
| `trackControlFlow` | bool | true | Build control flow graph |
| `maxPathLength` | size_t | 1000 | Max execution path length |
| `maxMemoryAccessHistory` | size_t | 10000 | Max memory access records |
| `hotPathThreshold` | size_t | 100 | Min executions for "hot" path |

## Memory Region Setup

```cpp
// Define regions for classification
profiler.setStackRegion(0x7FFF0000, 0x80000000);
profiler.setHeapRegion(0x10000000, 0x20000000);
profiler.setCodeRegion(0x00400000, 0x00500000);
```

## Recording Events (Automatic with CPU integration)

```cpp
// These are called automatically by CPU when profiler is attached
profiler.recordInstructionExecution(address, disassembly);
profiler.recordBranch(fromAddr, toAddr, taken, isConditional);
profiler.recordGeneralRegisterRead(index);
profiler.recordGeneralRegisterWrite(index);
profiler.recordMemoryRead(address, size);
profiler.recordMemoryWrite(address, size);
```

## Querying Statistics

### Basic Stats
```cpp
uint64_t total = profiler.getTotalInstructions();
uint64_t unique = profiler.getUniqueInstructionCount();
uint64_t reads = profiler.getTotalMemoryReads();
uint64_t writes = profiler.getTotalMemoryWrites();
```

### Top Instructions
```cpp
auto topInstr = profiler.getTopInstructions(10);
for (const auto& [addr, count] : topInstr) {
    std::cout << "0x" << std::hex << addr << ": " << count << "\n";
}
```

### Hot Paths
```cpp
auto hotPaths = profiler.getHotPaths(5);
for (const auto& path : hotPaths) {
    std::cout << "Executed " << path.executionCount << " times\n";
}
```

### Memory Statistics
```cpp
const auto& memStats = profiler.getMemoryStats();
auto stackStats = memStats.at(MemoryRegionType::STACK);
std::cout << "Stack reads: " << stackStats.readCount << "\n";
std::cout << "Stack writes: " << stackStats.writeCount << "\n";
```

### Register Pressure
```cpp
const RegisterPressure& pressure = profiler.getRegisterPressure();
std::cout << "GR1 reads: " << pressure.grReadCount[1] << "\n";
std::cout << "GR1 writes: " << pressure.grWriteCount[1] << "\n";
```

## Export Formats

### JSON (Complete Summary)
```cpp
std::string json = profiler.exportToJSON();
std::ofstream("profile.json") << json;
```

### DOT (Control Flow Graph)
```cpp
std::string dot = profiler.exportControlFlowGraphDOT();
std::ofstream("cfg.dot") << dot;
// Visualize: dot -Tpng cfg.dot -o cfg.png
```

### CSV (Instruction Frequency)
```cpp
std::string csv = profiler.exportInstructionFrequencyCSV();
std::ofstream("instructions.csv") << csv;
```

### CSV (Register Pressure)
```cpp
std::string csv = profiler.exportRegisterPressureCSV();
std::ofstream("registers.csv") << csv;
```

### CSV (Memory Access)
```cpp
std::string csv = profiler.exportMemoryAccessCSV();
std::ofstream("memory.csv") << csv;
```

## Control

```cpp
profiler.enable();     // Enable profiling
profiler.disable();    // Disable profiling
profiler.isEnabled();  // Check if enabled
profiler.reset();      // Clear all statistics
```

## Common Patterns

### Profile Specific Region
```cpp
profiler.disable();
// ... execute uninteresting code ...
profiler.enable();
// ... execute code of interest ...
profiler.disable();
```

### Periodic Export
```cpp
if (instructionCount % 1000000 == 0) {
    std::string json = profiler.exportToJSON();
    saveToFile("profile_" + timestamp + ".json", json);
    profiler.reset();
}
```

### Selective Tracking
```cpp
ProfilerConfig config;
config.trackInstructionFrequency = true;
config.trackRegisterPressure = false;  // Disable for performance
config.trackMemoryAccess = false;       // Disable for performance
Profiler profiler(config);
```

## Memory Region Types

- `MemoryRegionType::STACK` - Stack accesses
- `MemoryRegionType::HEAP` - Heap accesses
- `MemoryRegionType::CODE` - Code/text accesses
- `MemoryRegionType::UNKNOWN` - Unclassified accesses

## Performance Tips

1. Disable unused tracking features
2. Reduce history buffer sizes for long runs
3. Use `disable()` for uninteresting code sections
4. Export and reset periodically for very long runs
5. Profiler overhead per instruction: ~100-200ns (all features enabled)

## Example Output Files

### profile_results.json
```json
{
  "summary": {
    "totalInstructions": 10000,
    "uniqueInstructions": 150,
    "totalMemoryReads": 2500,
    "totalMemoryWrites": 1800
  },
  "topInstructions": [...],
  "memoryStats": {...},
  "registerPressure": {...},
  "hotPaths": [...]
}
```

### instruction_frequency.csv
```
Address,Count,Percentage,Disassembly
0x401000,850,8.50,"add r1 = r2, r3"
0x401010,720,7.20,"ld8 r4 = [r5]"
```

### register_pressure.csv
```
Register,Type,Reads,Writes,Total
GR1,GENERAL,150,80,230
GR2,GENERAL,120,50,170
```

### memory_access.csv
```
Timestamp,Address,Size,Type,Region
1,0x7fff1000,8,READ,STACK
2,0x10001000,4,WRITE,HEAP
```

## Visualization

### Control Flow Graph
```bash
# Generate PNG
dot -Tpng control_flow_graph.dot -o cfg.png

# Generate SVG (scalable)
dot -Tsvg control_flow_graph.dot -o cfg.svg

# Interactive viewer
xdot control_flow_graph.dot
```

### CSV Analysis (Python)
```python
import pandas as pd
import matplotlib.pyplot as plt

# Load and visualize instruction frequency
df = pd.read_csv('instruction_frequency.csv')
df.head(20).plot(x='Address', y='Count', kind='bar')
plt.savefig('top_instructions.png')

# Analyze register usage
df_regs = pd.read_csv('register_pressure.csv')
df_regs.groupby('Type')['Total'].sum().plot(kind='pie')
plt.savefig('register_usage.png')
```

## See Also

- Full documentation: `docs/PROFILER.md`
- Example: `examples/profiler_example.cpp`
- Tests: `tests/test_profiler.cpp`
