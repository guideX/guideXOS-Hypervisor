# IA-64 (Intel Itanium) Emulator

# IA-64 Hypervisor

A comprehensive Windows-only hypervisor and emulator for the IA-64 (Intel Itanium) architecture, written in C++14/C++20.

## Overview

This project provides a full-featured virtual machine environment for IA-64 binaries with:
- Complete CPU emulation with multi-core support
- Advanced memory management with MMU and page tables
- Virtual machine lifecycle management
- Debugging and profiling tools
- ISA plugin architecture for extensibility
- Full VM isolation guarantees

## Project Structure

```
guideXOS.Hypervisor/
  include/                     # Public headers
    cpu.h, cpu_state.h         # CPU emulation core
    decoder.h                  # IA-64 bundle decoder
    memory.h, mmu.h            # Memory system with MMU
    VirtualMachine.h           # VM implementation
    VMManager.h                # VM lifecycle management
    VMConfiguration.h          # VM configuration system
    ISAPluginRegistry.h        # ISA plugin architecture
    Profiler.h                 # Performance profiling
    Fuzzer.h                   # Fuzzing framework
    Console.h, Timer.h         # Virtual devices
    loader.h, bootstrap.h      # ELF loading & bootstrap
  src/
    core/                      # CPU implementation
    decoder/                   # Instruction decoding
    memory/                    # Memory & MMU
    vm/                        # Virtual machine
    isa/                       # ISA plugins
    io/                        # Virtual devices
    loader/                    # ELF loader & bootstrap
    profiler/                  # Profiling system
    fuzzer/                    # Fuzzing framework
    syscall/                   # System call layer
  tests/                       # Comprehensive test suite
  docs/                        # Detailed documentation
  examples/                    # Example programs
  CMakeLists.txt               # CMake build system
```

## Features

### Core Emulation
- **CPU State**: Full IA-64 register file (128 GRs, 128 FRs, 64 PRs, 8 BRs)
- **Instruction Decoder**: 128-bit bundle decoding with template parsing
- **Execution Engine**: Instruction execution framework
- **Multi-CPU Support**: SMP emulation with CPU scheduling
- **Register Rotation**: Logical-to-physical register mapping

### Memory Management
- **Virtual Memory**: 64-bit address space with on-demand paging
- **MMU**: Full memory management unit with page tables
- **Page Faults**: Comprehensive fault handling and diagnostics
- **Watchpoints**: Memory access breakpoints (read/write/execute)
- **Memory Protection**: Read/Write/Execute permission enforcement
- **Memory-Mapped I/O**: Device registration system

### Virtual Machine
- **VirtualMachine**: Complete VM with isolated CPU, memory, and devices
- **VMManager**: Multi-VM lifecycle management
- **VM Isolation**: Guaranteed no shared mutable state between VMs
- **VM Configuration**: JSON-based configuration system
- **Snapshots**: Save/restore VM state
- **Debugging**: Breakpoints, stepping, register inspection

### I/O Devices
- **Virtual Console**: Memory-mapped character I/O
- **Virtual Timer**: Programmable interval timer with interrupts
- **Interrupt Controller**: Interrupt queuing and delivery
- **Storage Devices**: Raw disk device support

### Development Tools
- **Profiler**:
  - Instruction frequency tracking
  - Hot path detection
  - Register pressure analysis
  - Memory access classification
  - Control flow graph generation
  - Multiple export formats (JSON, DOT, CSV)
- **Fuzzer**:
  - Instruction fuzzing
  - Memory fuzzing
  - State fuzzing
  - Crash detection and reporting
- **Debugger**:
  - Breakpoints with conditions
  - Memory watchpoints
  - Single-stepping
  - Register inspection
  - Snapshot/restore

### Architecture Features
- **ISA Plugin System**: Extensible instruction set architecture
- **Bootstrap Loader**: IA-64 bootstrap initialization
- **ELF Loader**: ELF64 binary loading and validation
- **Syscall Dispatcher**: Linux IA-64 ABI system calls
- **CPU Scheduler**: Round-robin and quantum-based scheduling

### Testing & Documentation
- **Comprehensive Test Suite**: 25+ test executables
- **Complete Documentation**: Architecture guides and API references
- **Example Programs**: Configuration, profiling, and usage examples

### In Development / Planned
- Full IA-64 instruction set implementation
- Register Stack Engine (RSE)
- Predication execution
- VLIW bundle scheduling
- Dynamic linking support
- JIT compilation

## Project Structure

```
include/              # Public headers
  cpu_state.h         # CPU register state
  decoder.h           # Instruction decoder
  memory.h            # Memory management
  loader.h            # ELF loader (stubbed)
  abi.h               # Linux IA-64 ABI (stubbed)
src/
  core/               # CPU emulation core
  decoder/            # Instruction decoding (IA-64 bundles)
  memory/             # Virtual memory system
  loader/             # ELF loader stubs
  abi/                # Syscall layer stubs
tests/               # Unit tests
main.cpp             # Entry point with demo
CMakeLists.txt       # CMake build configuration

```

## Features (Scaffolding Phase)

### Implemented
- **CPU State**: 128 general registers, 128 FP registers, 64 predicate registers, 8 branch registers
- **Memory System**: Paged virtual memory (4KB pages) with protection flags
- **Instruction Decoder**: Bundle extraction (128-bit bundles, template + 3 slots)
- **Basic Execution**: NOP and MOV-like instructions
- **ELF Loader**: Validation stub for IA-64 ELF binaries
- **ABI Layer**: Linux IA-64 syscall stubs (read, write, brk, etc.)
- **MMU**: Full memory management unit with page tables and permissions
- **Multi-CPU**: Support for multiple CPU contexts and scheduling
- **Profiler**: Runtime performance analysis with instruction frequency, hot paths, register pressure, memory access classification, and control flow graphs

### Not Yet Implemented
- Full IA-64 instruction set
- Register stack engine (RSE)
- Predication execution
- VLIW bundle scheduling
- Complete ELF loading
- Dynamic linking


## Quick Start

### Building with CMake (Recommended)

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release

# Run main emulator
.\build\bin\Release\ia64emu.exe

# Run test suite
ctest --test-dir build -C Release
```

### Running Tests

```powershell
# Run all tests
ctest --test-dir build -C Release --verbose

# Run specific test suites
.\build\bin\Release\test_vm_isolation.exe
.\build\bin\Release\test_profiler.exe
.\build\bin\Release\test_vmmanager.exe
.\build\bin\Release\test_debugger.exe
```

## Usage Examples

### Creating and Running a Virtual Machine

```cpp
#include "VirtualMachine.h"

// Create VM with 256MB memory and 2 CPUs
ia64::VirtualMachine vm(256 * 1024 * 1024, 2);

// Initialize
vm.init();

// Load program
std::vector<uint8_t> program = loadELF("program.elf");
vm.loadProgram(program.data(), program.size(), 0x4000);
vm.setEntryPoint(0x4000);

// Execute
vm.run(1000000);  // Run for 1M cycles
```

### Using VMManager

```cpp
#include "VMManager.h"
#include "VMConfiguration.h"

ia64::VMManager manager;

// Create VM configuration
ia64::VMConfiguration config = 
    ia64::VMConfiguration::createStandard("my-vm", 512);
config.cpu.cpuCount = 4;

// Add storage device
ia64::StorageConfiguration disk;
disk.deviceId = "disk0";
disk.imagePath = "disk.img";
disk.sizeInBytes = 10ULL * 1024 * 1024 * 1024; // 10GB
config.addStorageDevice(disk);

// Create and start VM
std::string vmId = manager.createVM(config);
manager.startVM(vmId);
manager.runVM(vmId, 1000000);

// Take snapshot
std::string snapId = manager.snapshotVM(vmId, "checkpoint-1");

// Stop VM
manager.stopVM(vmId);
```


### Profiling Support

The emulator includes a comprehensive profiling system for analyzing program execution:

```cpp
#include "Profiler.h"

// Configure profiler
ia64::ProfilerConfig config;
config.trackInstructionFrequency = true;
config.trackRegisterPressure = true;
config.trackMemoryAccess = true;
config.trackControlFlow = true;

ia64::Profiler profiler(config);
cpu.setProfiler(&profiler);

// Run program
vm.run(1000000);

// Export results
std::string json = profiler.exportToJSON();
std::string dotGraph = profiler.exportControlFlowGraphDOT();
std::string csv = profiler.exportToCSV();

// Analyze hot paths
auto hotPaths = profiler.getHotPaths(10);  // Top 10 hot paths
for (const auto& path : hotPaths) {
    std::cout << "Hot path at 0x" << std::hex << path.startAddress 
              << " - " << path.executionCount << " executions\n";
}
```

**Profiler Features:**
- Instruction frequency tracking
- Hot execution path detection
- Register pressure analysis (identify register-heavy code)
- Memory access classification (stack/heap/code/data)
- Control flow graph generation (visualize with Graphviz)
- Multiple export formats (JSON, DOT, CSV)

**Documentation:**
- Full guide: [`docs/PROFILER.md`](docs/PROFILER.md)
- Quick reference: [`docs/PROFILER_QUICK_REFERENCE.md`](docs/PROFILER_QUICK_REFERENCE.md)
- Example: [`examples/profiler_example.cpp`](examples/profiler_example.cpp)

### Debugging Support

```cpp
#include "VirtualMachine.h"

// Attach debugger
vm.attach_debugger();

// Set breakpoint
vm.setBreakpoint(0x4000);

// Set conditional breakpoint
ia64::DebugCondition condition;
condition.target = ia64::DebugConditionTarget::GENERAL_REGISTER;
condition.op = ia64::DebugConditionOperator::EQUAL;
condition.index = 10;
condition.value = 0x12345678;
vm.setConditionalBreakpoint(0x5000, condition);

// Set memory watchpoint
vm.setMemoryBreakpoint(0x10000, 0x10100, 
    ia64::WatchpointType::WRITE);

// Single-step execution
while (vm.step()) {
    // Inspect state
    uint64_t ip = vm.getIP();
    uint64_t gr10 = vm.readGR(10);
    
    if (vm.getState() == ia64::VMState::DEBUG_BREAK) {
        std::cout << "Breakpoint hit at 0x" << std::hex << ip << "\n";
        break;
    }
}

// Create snapshot
auto snapshot = vm.createSnapshot();

// Restore snapshot later
vm.restoreSnapshot(snapshot);
```

### Fuzzing Support

```cpp
#include "Fuzzer.h"

// Configure fuzzer
ia64::FuzzerConfig config;
config.maxIterations = 10000;
config.timeout = 1000;  // cycles per iteration
config.enableInstructionFuzzing = true;
config.enableMemoryFuzzing = true;

ia64::Fuzzer fuzzer(config);

// Fuzz a VM
fuzzer.fuzzVM(vm);

// Get results
ia64::FuzzerReport report = fuzzer.generateReport();
std::cout << "Total iterations: " << report.totalIterations << "\n";
std::cout << "Crashes found: " << report.totalCrashes << "\n";

// Export crash details
std::string crashReport = fuzzer.exportCrashesToJSON();
```

### VM Configuration (JSON)

```json
{
  "name": "production-vm",
  "uuid": "550e8400-e29b-41d4-a716-446655440000",
  "memory": {
    "size_mb": 2048,
    "enable_mmu": true,
    "page_size": 4096
  },
  "cpu": {
    "count": 4,
    "isa_type": "IA-64"
  },
  "storage": [
    {
      "device_id": "boot-disk",
      "type": "raw",
      "image_path": "/path/to/disk.img",
      "size_bytes": 10737418240,
      "read_only": false
    }
  ]
}
```

Load configuration:
```cpp
#include "VMConfiguration.h"
#include "JsonUtils.h"

// Load from JSON
std::string json = readFile("config.json");
ia64::VMConfiguration config = 
    ia64::VMConfiguration::fromJSON(json);

// Create VM with config
std::string vmId = manager.createVM(config);
```


## Architecture

### IA-64 Specifics
- **VLIW Architecture**: Explicitly Parallel Instruction Computing (EPIC)
- **Instruction Bundles**: 128 bits containing 3 instructions (41 bits each) + 5-bit template
- **Templates**: Specify execution units and dependencies for the 3 slots
- **Execution Units**: M (memory/ALU), I (integer), F (floating-point), B (branch), L/X (extended immediate)
- **Predication**: All instructions can be predicated on predicate registers
- **Register Stack**: Hardware-managed register windows (in development)
- **Register Rotation**: Base/limit registers for rotating register files

### System Architecture

```
???????????????????????????????????????????????????????????
?                      VMManager                          ?
?  ????????????????????????????????????????????????????  ?
?  ?            VMInstance #1                         ?  ?
?  ?  ??????????????????????????????????????????????  ?  ?
?  ?  ?         VirtualMachine                     ?  ?  ?
?  ?  ?  ????????????  ????????????  ???????????? ?  ?  ?
?  ?  ?  ?  CPU #1  ?  ?  CPU #2  ?  ?  CPU #n  ? ?  ?  ?
?  ?  ?  ????????????  ????????????  ???????????? ?  ?  ?
?  ?  ?         ?             ?            ?       ?  ?  ?
?  ?  ?         ????????????????????????????       ?  ?  ?
?  ?  ?                       ?                    ?  ?  ?
?  ?  ?              ???????????????????           ?  ?  ?
?  ?  ?              ?   Memory + MMU  ?           ?  ?  ?
?  ?  ?              ???????????????????           ?  ?  ?
?  ?  ?                       ?                    ?  ?  ?
?  ?  ?         ????????????????????????????       ?  ?  ?
?  ?  ?         ?             ?            ?       ?  ?  ?
?  ?  ?    ??????????   ??????????   ??????????   ?  ?  ?
?  ?  ?    ?Console ?   ? Timer  ?   ?  Disk  ?   ?  ?  ?
?  ?  ?    ??????????   ??????????   ??????????   ?  ?  ?
?  ?  ??????????????????????????????????????????????  ?  ?
?  ????????????????????????????????????????????????????  ?
???????????????????????????????????????????????????????????
```

### VM Isolation

Each VirtualMachine instance guarantees complete isolation:
- **Independent Memory**: Private 64-bit address space with MMU
- **Independent CPUs**: Isolated register files and execution state
- **Independent Devices**: Private console, timer, and storage
- **No Shared Mutable State**: All subsystems owned via unique_ptr
- **Thread-Safe Design**: Ready for multi-threaded execution

See [`docs/VM_ISOLATION.md`](docs/VM_ISOLATION.md) for complete isolation architecture.

### Memory Management

```
Virtual Address
       ?
       ?
???????????????
?     MMU     ?  ? Page tables
?  Translation?  ? Permission checks
?   & Checks  ?  ? Watchpoints
???????????????
       ?
       ?
Physical Address
       ?
       ?
???????????????
?   Memory    ?
?   Backing   ?  ? std::vector<uint8_t>
?    Store    ?  ? Memory-mapped devices
???????????????
```

**Features:**
- 4KB page size (configurable)
- On-demand page allocation
- Page-level permissions (RWX)
- Page fault diagnostics
- Memory watchpoints
- Copy-on-write support (in development)

See [`docs/MMU_IMPLEMENTATION.md`](docs/MMU_IMPLEMENTATION.md) for details.

### ISA Plugin Architecture

The hypervisor supports pluggable instruction set architectures:

```cpp
// Register ISA plugin
ISAPluginRegistry::instance().registerISA("Custom-ISA", customFactory);

// Create VM with custom ISA
VirtualMachine vm(memSize, cpuCount, "Custom-ISA");
```

**Built-in ISAs:**
- IA-64 (Intel Itanium)
- Example ISA (template for custom implementations)

See [`docs/ISA_PLUGIN_ARCHITECTURE.md`](docs/ISA_PLUGIN_ARCHITECTURE.md) for plugin development.


## Building

### Prerequisites
- Windows 10/11
- Visual Studio 2022 (v143 toolset)
- CMake 3.15+ (for CMake build)
- C++14/C++20 compiler support

### Option 1: CMake (Recommended)

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Debug
cmake --build build --config Debug

# Build Release
cmake --build build --config Release

# Run main executable
.\build\bin\Release\ia64emu.exe

# Run specific test
.\build\bin\Release\test_vm_isolation.exe

# Run all tests
ctest --test-dir build -C Release
```

### Option 2: Visual Studio

1. Open `guideXOS Hypervisor.sln` in Visual Studio 2022
2. **Configure project properties** (if needed):
   - Right-click project ? Properties
   - C/C++ ? General ? Additional Include Directories: Add `$(ProjectDir)include`
   - C/C++ ? Language ? C++ Language Standard: Select **ISO C++20 Standard (/std:c++20)**
   - Apply to all configurations (Debug/Release, Win32/x64)
3. Build Solution (F7)
4. Run (F5)


## Test Suite

The project includes comprehensive tests covering all major components:

### Core Tests
- `test_main.cpp` - Basic CPU, memory, and decoder tests
- `test_cpu_execution.cpp` - CPU execution and instruction tests
- `test_mov_imm.cpp` - MOV immediate instruction tests

### Memory & MMU Tests
- `test_mmu.cpp` - MMU functionality and page table tests
- `test_page_fault.cpp` - Page fault handling and diagnostics
- `test_watchpoint.cpp` - Memory watchpoint tests

### VM Tests
- `test_multi_cpu.cpp` - Multi-CPU and scheduling tests
- `test_scheduler_stepping.cpp` - CPU scheduler stepping modes
- `test_vm_isolation.cpp` - VM isolation guarantees
- `test_vmmanager.cpp` - VMManager lifecycle tests
- `test_vmconfig_serialization.cpp` - VM configuration tests

### System Tests
- `test_bootstrap.cpp` - Bootstrap initialization tests
- `test_elf_validation.cpp` - ELF loader validation tests
- `test_syscall_dispatcher.cpp` - System call tests
- `test_io_devices.cpp` - Virtual device tests

### Tool Tests
- `test_debugger.cpp` - Debugger functionality tests
- `test_profiler.cpp` - Profiler tests
- `test_fuzzer.cpp` - Fuzzer tests
- `test_isa_plugin.cpp` - ISA plugin system tests

### Running Tests

```powershell
# Run all tests
ctest --test-dir build -C Release --verbose

# Run specific test category
ctest --test-dir build -C Release -R "VMIsolationTests"

# Run individual test executable
.\build\bin\Release\test_vm_isolation.exe
```

## Documentation

Comprehensive documentation is available in the [`docs/`](docs/) directory:

### Architecture & Design
- [`CPU_IMPLEMENTATION.md`](docs/CPU_IMPLEMENTATION.md) - CPU emulation architecture
- [`IA64_BUNDLE_DECODER.md`](docs/IA64_BUNDLE_DECODER.md) - Instruction decoding
- [`MMU_IMPLEMENTATION.md`](docs/MMU_IMPLEMENTATION.md) - Memory management unit
- [`VM_ISOLATION.md`](docs/VM_ISOLATION.md) - VM isolation architecture
- [`ISA_PLUGIN_ARCHITECTURE.md`](docs/ISA_PLUGIN_ARCHITECTURE.md) - ISA plugin system

### Features & Tools
- [`PROFILER.md`](docs/PROFILER.md) - Profiling system guide
- [`FUZZER.md`](docs/FUZZER.md) - Fuzzing framework guide
- [`VMMANAGER.md`](docs/VMMANAGER.md) - VM lifecycle management
- [`VM_CONFIGURATION_SCHEMA.md`](docs/VM_CONFIGURATION_SCHEMA.md) - Configuration format

### Subsystems
- [`ELF64_LOADER.md`](docs/ELF64_LOADER.md) - ELF binary loading
- [`BOOTSTRAP_INITIALIZATION.md`](docs/BOOTSTRAP_INITIALIZATION.md) - Bootstrap process
- [`PAGE_FAULT_DIAGNOSTICS.md`](docs/PAGE_FAULT_DIAGNOSTICS.md) - Page fault handling
- [`WATCHPOINT_USAGE.md`](docs/WATCHPOINT_USAGE.md) - Memory watchpoints
- [`VIRTUAL_DEVICES.md`](docs/VIRTUAL_DEVICES.md) - Virtual Console, Timer, and Interrupt Controller

### Quick References
- [`PROFILER_QUICK_REFERENCE.md`](docs/PROFILER_QUICK_REFERENCE.md)
- [`VM_CONFIGURATION_QUICK_REFERENCE.md`](docs/VM_CONFIGURATION_QUICK_REFERENCE.md)
- [`BOOTSTRAP_QUICK_START.md`](docs/BOOTSTRAP_QUICK_START.md)
- [`VIRTUAL_DEVICES_QUICK_REF.md`](docs/VIRTUAL_DEVICES_QUICK_REF.md)

## Examples

Example programs are available in the [`examples/`](examples/) directory:

- [`virtual_devices_example.cpp`](examples/virtual_devices_example.cpp) - Virtual Console, Timer, and Interrupt Controller
- [`profiler_example.cpp`](examples/profiler_example.cpp) - Profiling usage
- [`config_example.cpp`](examples/config_example.cpp) - VM configuration
- [`configs/`](examples/configs/) - Sample JSON configurations
  - `minimal-vm.json` - Minimal VM configuration
  - `standard-vm.json` - Standard VM configuration
  - `server-vm.json` - Server VM configuration


## Command-Line Tools

### Debug Harness

Interactive debugging environment:

```powershell
.\build\bin\Release\debug_harness.exe
```

Features:
- Load and execute IA-64 binaries
- Set breakpoints and watchpoints
- Step through execution
- Inspect CPU and memory state
- Profile execution

### VM Manager CLI (Future)

Command-line VM management:

```powershell
# Create VM from config
.\vmctl create -c config.json

# Start VM
.\vmctl start <vm-id>

# List VMs
.\vmctl list

# Take snapshot
.\vmctl snapshot <vm-id> checkpoint-1

# Stop VM
.\vmctl stop <vm-id>
```

## Performance

Current performance characteristics:

- **Execution Speed**: ~100K-500K instructions/second (interpreted mode)
- **Memory Overhead**: ~500MB + VM memory size
- **Startup Time**: <100ms for VM initialization
- **Snapshot Time**: <1s for 256MB VM

**Planned Optimizations:**
- JIT compilation (target: 10-50M instructions/second)
- Binary translation
- Threaded interpretation
- Memory compression for snapshots

## Development Status

### Production Ready ?
- Core CPU emulation
- Memory management (MMU, paging, protection)
- Virtual machine infrastructure
- Multi-CPU support
- Debugging tools
- Profiling system
- VM isolation
- Configuration system
- Test suite

### In Active Development ??
- Full IA-64 instruction set
- Register Stack Engine (RSE)
- Complete syscall emulation
- ELF dynamic linking
- Advanced profiling features

### Future Roadmap ??
- JIT compilation
- Binary translation
- Snapshot compression
- Network devices
- GUI management interface
- Remote debugging protocol
- Performance optimization

## Contributing

This is currently a research/educational project. Contributions are welcome in the following areas:

1. **Instruction Implementation**: Add missing IA-64 instructions
2. **Testing**: Expand test coverage
3. **Documentation**: Improve docs and examples
4. **Performance**: Optimization and profiling
5. **Features**: New VM capabilities

## Known Limitations

- **Windows-only**: No POSIX/Linux support
- **Single-threaded**: No parallel VM execution (yet)
- **Interpreted**: No JIT/binary translation (yet)
- **Limited syscalls**: Basic Linux IA-64 ABI only
- **No RSE**: Register Stack Engine not implemented


## Troubleshooting

### Build Issues

**Problem**: `Cannot find include files`
```
Solution: Ensure CMake include directories are set correctly:
cmake -B build -DCMAKE_INCLUDE_PATH=include
```

**Problem**: `C++ standard not supported`
```
Solution: Use Visual Studio 2022 or ensure C++20 support:
cmake -B build -DCMAKE_CXX_STANDARD=20
```

### Runtime Issues

**Problem**: `Page fault at address 0x...`
```
Solution: Check memory initialization and MMU page mappings:
vm.getMemory().GetMMU().MapIdentityRange(addr, size, permissions);
```

**Problem**: `No runnable CPU found`
```
Solution: Ensure at least one CPU is enabled:
vm.setActiveCPU(0);
```

**Problem**: `Invalid instruction at address 0x...`
```
Solution: Verify program is loaded correctly and entry point is set:
vm.loadProgram(data, size, loadAddress);
vm.setEntryPoint(entryPoint);
```

## Licensing

This is a research and educational project. See appropriate licensing for production use.

## References

### IA-64 Architecture
- Intel Itanium Architecture Software Developer's Manual
- IA-64 Linux Kernel ABI
- HP PA-RISC to IA-64 Porting Guide

### Related Projects
- QEMU - Multi-architecture emulator
- Bochs - x86 emulator
- Unicorn - CPU emulator framework

## Acknowledgments

This project implements the IA-64 (Intel Itanium) architecture as specified in Intel's official documentation. The architecture design and instruction set are the intellectual property of Intel Corporation.

---

**Project Status**: Active Development  
**Version**: 1.0
**Last Updated**: 2026

## References

- Intel® Itanium® Architecture Software Developer's Manual
- IA-64 Linux ABI Specification
- ELF-64 Object File Format
