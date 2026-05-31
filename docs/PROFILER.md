# IA-64 Hypervisor Profiler

## Overview

The profiler is a comprehensive runtime analysis system for the IA-64 hypervisor that tracks performance characteristics, execution patterns, and resource utilization during program execution.

## Features

### 1. Instruction Frequency Tracking
- Counts execution frequency for every unique instruction address
- Tracks total instruction count and unique instruction count
- Identifies hot instructions (most frequently executed)
- Associates disassembly with each instruction for readability

### 2. Execution Path Analysis
- Records sequences of executed instructions
- Detects hot paths (frequently executed code sequences)
- Configurable path length and hot path threshold
- Useful for identifying optimization opportunities

### 3. Register Pressure Tracking
- Monitors usage of all register types:
  - General registers (GR0-GR127)
  - Floating-point registers (FR0-FR127)
  - Predicate registers (PR0-PR63)
  - Branch registers (BR0-BR7)
  - Application registers (AR)
- Tracks both read and write operations
- Identifies which registers are most heavily used
- Helps detect register allocation issues

### 4. Memory Access Classification
- Classifies memory accesses into regions:
  - **Stack**: Stack region accesses
  - **Heap**: Heap region accesses
  - **Code**: Code/text region accesses
  - **Unknown**: Unclassified regions
- Tracks read/write counts per region
- Records unique addresses accessed per region
- Maintains configurable history of recent memory accesses

### 5. Control Flow Graph (CFG) Generation
- Builds runtime control flow graph
- Tracks execution counts for basic blocks
- Records edge traversal frequencies
- Distinguishes between:
  - Sequential execution (fall-through)
  - Unconditional branches
  - Conditional branches
- Identifies hot edges (frequently taken paths)

## Usage

### Basic Setup

```cpp
#include "Profiler.h"

// Create profiler with default configuration
ia64::Profiler profiler;

// Configure CPU to use profiler
cpu.setProfiler(&profiler);

// Run your program
while (cpu.step()) {
    // Profiler automatically tracks execution
}

// Get results
std::string json = profiler.exportToJSON();
std::cout << json << std::endl;
```

### Advanced Configuration

```cpp
ia64::ProfilerConfig config;

// Enable/disable specific tracking features
config.trackInstructionFrequency = true;
config.trackExecutionPaths = true;
config.trackRegisterPressure = true;
config.trackMemoryAccess = true;
config.trackControlFlow = true;

// Configure thresholds
config.hotPathThreshold = 100;      // Minimum executions to be "hot"
config.maxPathLength = 1000;        // Max path length to track
config.maxMemoryAccessHistory = 10000;  // Max memory access records

// Define memory regions for classification
config.stackStart = 0x7FFF0000;
config.stackEnd = 0x80000000;
config.heapStart = 0x10000000;
config.heapEnd = 0x20000000;
config.codeStart = 0x00400000;
config.codeEnd = 0x00500000;

ia64::Profiler profiler(config);
```

### Querying Statistics

```cpp
// Instruction statistics
uint64_t totalInstr = profiler.getTotalInstructions();
uint64_t uniqueInstr = profiler.getUniqueInstructionCount();

// Get top 10 most executed instructions
auto topInstr = profiler.getTopInstructions(10);
for (const auto& pair : topInstr) {
    std::cout << "Address: 0x" << std::hex << pair.first 
              << " Count: " << std::dec << pair.second << std::endl;
}

// Get hot execution paths
auto hotPaths = profiler.getHotPaths(5);
for (const auto& path : hotPaths) {
    std::cout << "Executed " << path.executionCount << " times, "
              << path.addresses.size() << " instructions" << std::endl;
}

// Memory statistics
uint64_t memReads = profiler.getTotalMemoryReads();
uint64_t memWrites = profiler.getTotalMemoryWrites();

const auto& memStats = profiler.getMemoryStats();
std::cout << "Stack reads: " 
          << memStats.at(ia64::MemoryRegionType::STACK).readCount 
          << std::endl;

// Register pressure
const ia64::RegisterPressure& pressure = profiler.getRegisterPressure();
std::cout << "GR1 reads: " << pressure.grReadCount[1] << std::endl;
std::cout << "GR1 writes: " << pressure.grWriteCount[1] << std::endl;
```

### Control Profiling

```cpp
// Temporarily disable profiling
profiler.disable();

// ... execute some code without profiling ...

// Re-enable profiling
profiler.enable();

// Reset all statistics
profiler.reset();
```

## Export Formats

### JSON Export

Complete profiling summary in JSON format, including:
- Overall statistics
- Top instructions
- Memory access statistics by region
- Register pressure summary
- Hot paths

```cpp
std::string json = profiler.exportToJSON();

// Save to file
std::ofstream outFile("profile.json");
outFile << json;
outFile.close();
```

Example output:
```json
{
  "summary": {
    "totalInstructions": 10000,
    "uniqueInstructions": 150,
    "totalMemoryReads": 2500,
    "totalMemoryWrites": 1800
  },
  "topInstructions": [
    {
      "address": "0x401000",
      "count": 850,
      "disassembly": "add r1 = r2, r3"
    }
  ],
  "memoryStats": {
    "stack": {
      "reads": 1200,
      "writes": 1500,
      "totalBytes": 21600,
      "uniqueAddresses": 150
    }
  }
}
```

### DOT Export (Control Flow Graph)

GraphViz DOT format for visualizing control flow:

```cpp
std::string dot = profiler.exportControlFlowGraphDOT();

// Save to file
std::ofstream dotFile("cfg.dot");
dotFile << dot;
dotFile.close();

// Generate visualization with GraphViz:
// dot -Tpng cfg.dot -o cfg.png
```

Features:
- Nodes represent basic blocks with execution counts
- Edges show control flow with execution counts
- Hot nodes are highlighted in orange
- Conditional branches are shown as dashed red lines
- Unconditional branches are shown as bold blue lines
- Hot edges have thicker lines

### CSV Exports

#### Instruction Frequency CSV

```cpp
std::string csv = profiler.exportInstructionFrequencyCSV();
```

Format:
```
Address,Count,Percentage,Disassembly
0x401000,850,8.50,"add r1 = r2, r3"
0x401010,720,7.20,"ld8 r4 = [r5]"
```

#### Register Pressure CSV

```cpp
std::string csv = profiler.exportRegisterPressureCSV();
```

Format:
```
Register,Type,Reads,Writes,Total
GR1,GENERAL,150,80,230
GR2,GENERAL,120,50,170
PR0,PREDICATE,200,0,200
```

#### Memory Access CSV

```cpp
std::string csv = profiler.exportMemoryAccessCSV();
```

Format:
```
Timestamp,Address,Size,Type,Region
1,0x7fff1000,8,READ,STACK
2,0x10001000,4,WRITE,HEAP
3,0x401000,16,READ,CODE
```

## Integration with CPU

The profiler is designed to be automatically invoked by the CPU during execution:

```cpp
// In CPU::step()
if (isProfilingEnabled()) {
    profiler_->recordInstructionExecution(state_.GetIP(), instr.GetDisassembly());
}

// In CPU::readGR()
if (isProfilingEnabled()) {
    profiler_->recordGeneralRegisterRead(index);
}

// In CPU::writeGR()
if (isProfilingEnabled()) {
    profiler_->recordGeneralRegisterWrite(index);
}
```

## Performance Considerations

The profiler is designed to have minimal performance impact:
- All tracking is optional via configuration flags
- Can be disabled entirely during execution
- Uses efficient data structures (maps, vectors)
- History buffers are size-limited to prevent unbounded growth

Overhead estimates (when all features enabled):
- Per-instruction overhead: ~100-200ns
- Per-register access overhead: ~50-100ns
- Per-memory access overhead: ~50-100ns

For production runs, consider:
- Disabling features not needed
- Reducing history buffer sizes
- Using sampling (track every Nth instruction)

## Best Practices

1. **Memory Region Configuration**: Always configure memory regions for accurate classification
   ```cpp
   profiler.setStackRegion(stackStart, stackEnd);
   profiler.setHeapRegion(heapStart, heapEnd);
   profiler.setCodeRegion(codeStart, codeEnd);
   ```

2. **Hot Path Threshold**: Adjust based on program characteristics
   - Short programs: 3-10 executions
   - Long programs: 100-1000 executions

3. **Export Regularly**: For long-running programs, export and reset periodically
   ```cpp
   if (cpu.getTotalInstructions() % 1000000 == 0) {
       std::string json = profiler.exportToJSON();
       saveToFile("profile_" + timestamp + ".json", json);
       profiler.reset();
   }
   ```

4. **Selective Profiling**: Profile only regions of interest
   ```cpp
   if (cpu.getIP() >= interestingStart && cpu.getIP() < interestingEnd) {
       profiler.enable();
   } else {
       profiler.disable();
   }
   ```

## Visualization Tools

### Control Flow Graph

Use GraphViz to visualize the DOT export:
```bash
# Generate PNG image
dot -Tpng cfg.dot -o cfg.png

# Generate SVG (scalable)
dot -Tsvg cfg.dot -o cfg.svg

# Interactive viewer
xdot cfg.dot
```

### CSV Analysis

Use any data analysis tool:
```python
import pandas as pd
import matplotlib.pyplot as plt

# Load instruction frequency data
df = pd.read_csv('instructions.csv')

# Plot top 20 instructions
df.head(20).plot(x='Address', y='Count', kind='bar')
plt.savefig('top_instructions.png')

# Load register pressure
df_regs = pd.read_csv('registers.csv')
df_regs.groupby('Type')['Total'].sum().plot(kind='pie')
plt.savefig('register_usage.png')
```

## Examples

See `tests/test_profiler.cpp` for comprehensive usage examples including:
- Instruction frequency tracking
- Register pressure analysis
- Memory access classification
- Control flow graph generation
- Hot path detection
- All export formats

## Architecture Notes

The profiler is designed to be non-invasive:
- Optional - can be null
- No dependencies on CPU internals
- All tracking is passive (read-only access to CPU state)
- Thread-safe for single-threaded emulator use

Data structures:
- `std::map` for sparse data (instruction addresses, CFG nodes)
- `std::vector` for dense data (register pressure)
- `std::deque` for history buffers (memory accesses, paths)

## Future Enhancements

Potential additions:
- Cache simulation and miss tracking
- Branch prediction statistics
- Bundle parallelism analysis
- Predicate register effectiveness
- Register rotation utilization
- Time-series snapshots for trend analysis
- Sampling mode for reduced overhead
- Binary instrumentation support
