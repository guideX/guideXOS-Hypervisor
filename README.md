# IA-64 (Intel Itanium) Emulator

A Windows-only user-mode emulator for the IA-64 (Intel Itanium) architecture, written in C++20.

## Project Structure

```
??? include/              # Public headers
?   ??? cpu_state.h      # CPU register state
?   ??? decoder.h        # Instruction decoder
?   ??? memory.h         # Memory management
?   ??? loader.h         # ELF loader (stubbed)
?   ??? abi.h            # Linux IA-64 ABI (stubbed)
??? src/
?   ??? core/            # CPU emulation core
?   ??? decoder/         # Instruction decoding (IA-64 bundles)
?   ??? memory/          # Virtual memory system
?   ??? loader/          # ELF loader stubs
?   ??? abi/             # Syscall layer stubs
??? tests/               # Unit tests
??? main.cpp             # Entry point with demo
??? CMakeLists.txt       # CMake build configuration

```

## Features (Scaffolding Phase)

### Implemented
- **CPU State**: 128 general registers, 128 FP registers, 64 predicate registers, 8 branch registers
- **Memory System**: Paged virtual memory (4KB pages) with protection flags
- **Instruction Decoder**: Bundle extraction (128-bit bundles, template + 3 slots)
- **Basic Execution**: NOP and MOV-like instructions
- **ELF Loader**: Validation stub for IA-64 ELF binaries
- **ABI Layer**: Linux IA-64 syscall stubs (read, write, brk, etc.)

### Not Yet Implemented
- Full IA-64 instruction set
- Register stack engine (RSE)
- Predication execution
- VLIW bundle scheduling
- Complete ELF loading
- Dynamic linking

## Building

### Option 1: CMake (Recommended)

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Debug

# Run
.\build\bin\Debug\ia64emu.exe

# Run tests
.\build\bin\Debug\ia64emu_tests.exe
```

### Option 2: Visual Studio

If you prefer to use the Visual Studio project directly:

1. Open `guideXOS Hypervisor.sln` in Visual Studio 2022
2. **Important**: Configure project properties:
   - Right-click project ? Properties
   - C/C++ ? General ? Additional Include Directories: Add `$(ProjectDir)include`
   - C/C++ ? Language ? C++ Language Standard: Select **ISO C++20 Standard (/std:c++20)**
   - Apply to all configurations (Debug/Release, Win32/x64)
3. Ensure all source files are added:
   - main.cpp
   - src\core\cpu.cpp
   - src\decoder\decoder.cpp
   - src\memory\memory.cpp
   - src\loader\loader.cpp
   - src\abi\abi.cpp
4. Build and run

## Usage Example

The `main.cpp` demonstrates basic emulator usage:

```cpp
// Initialize components
ia64::CPUState cpu;
ia64::MemorySystem memory(16 * 1024 * 1024);
ia64::InstructionDecoder decoder;

// Load program
memory.Write(loadAddress, programCode.data(), programCode.size());
cpu.SetIP(loadAddress);

// Execute
while (...) {
    std::vector<uint8_t> bundleData(16);
    memory.Read(cpu.GetIP(), bundleData.data(), 16);
    
    auto bundle = decoder.DecodeBundle(bundleData.data());
    
    for (const auto& insn : bundle.instructions) {
        insn.Execute(cpu, memory);
    }
    
    cpu.SetIP(cpu.GetIP() + 16);
}
```

## Architecture Notes

### IA-64 Specifics
- **VLIW Architecture**: Explicitly Parallel Instruction Computing (EPIC)
- **Instruction Bundles**: 128 bits containing 3 instructions (41 bits each) + 5-bit template
- **Templates**: Specify execution units and dependencies for the 3 slots
- **Execution Units**: M (memory/ALU), I (integer), F (floating-point), B (branch), L/X (extended immediate)
- **Predication**: All instructions can be predicated on predicate registers
- **Register Stack**: Hardware-managed register windows (not yet implemented)

### Current Limitations
- Only NOP and basic MOV instructions decoded
- No actual ELF binary loading
- Syscalls are stubbed (print debug info only)
- No RSE (Register Stack Engine)
- No advanced addressing modes

## Development Roadmap

1. **Phase 1** (Current): Scaffolding and architecture
   - ? CPU state management
   - ? Memory system
   - ? Bundle decoding framework
   - ? Basic execution loop

2. **Phase 2**: Core instructions
   - Integer ALU (ADD, SUB, AND, OR, XOR, shifts)
   - Loads/stores (LD1/2/4/8, ST1/2/4/8)
   - Branches (BR, BRL, conditional branches)
   - Compares and predicates

3. **Phase 3**: Advanced features
   - Register Stack Engine (RSE)
   - Floating-point instructions
   - Full ELF loader
   - System emulation

4. **Phase 4**: Optimization
   - Just-in-time (JIT) compilation
   - Binary translation
   - Performance profiling

## Requirements

- Windows 10/11
- Visual Studio 2022 (v143 toolset)
- C++20 compiler support
- CMake 3.20+ (for CMake build)

## License

This is a research/educational project. See appropriate licensing for production use.

## References

- Intel® Itanium® Architecture Software Developer's Manual
- IA-64 Linux ABI Specification
- ELF-64 Object File Format
