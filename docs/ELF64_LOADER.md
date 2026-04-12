# ELF64 Loader Implementation for IA-64

## Overview

This document describes the complete ELF64 loader implementation for IA-64 binaries in the guideXOS Hypervisor. The loader handles parsing, validation, segment loading, relocations, and CPU initialization for both static executables and position-independent executables (PIE).

## Features

### 1. **Complete ELF64 Format Support**
- Full ELF64 header parsing (64-byte header)
- Program header table parsing
- Section header structures (for future use)
- Symbol table and relocation table structures

### 2. **Binary Validation**
- Magic number verification (0x7F 'E' 'L' 'F')
- ELF class validation (64-bit only)
- Endianness checking (little-endian)
- Machine type verification (EM_IA_64 = 50)
- File type validation (EXEC or DYN)
- Bounds checking for all headers

### 3. **Segment Loading**
- PT_LOAD segment processing
- Virtual address mapping
- File data loading into memory
- .bss section zero-initialization
- Page-aligned memory allocation
- PIE/shared object support with base address relocation

### 4. **Relocation Processing**
- R_IA64_NONE - No operation
- R_IA64_DIR64LSB - 64-bit absolute address (little-endian)
- R_IA64_DIR32LSB - 32-bit absolute address (little-endian)
- R_IA64_PCREL21B - PC-relative branch (stub)
- R_IA64_PCREL60B - PC-relative branch (stub)
- Base relocation for PIE executables

### 5. **Stack Initialization**
- Proper stack layout with argc/argv/envp
- Auxiliary vector (auxv) support:
  - AT_PAGESZ - Page size (4096 bytes)
  - AT_PHDR - Program header address
  - AT_PHENT - Program header entry size
  - AT_PHNUM - Number of program headers
  - AT_BASE - Base address for dynamic executables
  - AT_ENTRY - Entry point address
  - AT_UID/AT_EUID - User IDs
  - AT_GID/AT_EGID - Group IDs
- 16-byte stack alignment
- String data placement

### 6. **CPU State Initialization**
- Instruction pointer (IP) set to entry point
- Stack pointer (GR12) initialized
- Backing store pointer (AR.BSP, AR36) configured
- RSE configuration (AR.RSC, AR16) set
- Current Frame Marker (CFM) initialized
- Processor Status Register (PSR) configured
- Predicate register PR0 set to true

## Architecture

### Data Structures

```cpp
// ELF64 File Header (64 bytes)
struct ELF64_Ehdr {
    uint8_t  e_ident[16];      // Magic + class + encoding
    uint16_t e_type;           // ET_EXEC or ET_DYN
    uint16_t e_machine;        // EM_IA_64 (50)
    uint32_t e_version;        // EV_CURRENT (1)
    uint64_t e_entry;          // Entry point address
    uint64_t e_phoff;          // Program header offset
    uint64_t e_shoff;          // Section header offset
    uint32_t e_flags;          // Processor flags
    uint16_t e_ehsize;         // ELF header size
    uint16_t e_phentsize;      // Program header size
    uint16_t e_phnum;          // Program header count
    uint16_t e_shentsize;      // Section header size
    uint16_t e_shnum;          // Section header count
    uint16_t e_shstrndx;       // String table index
};

// ELF64 Program Header (56 bytes)
struct ELF64_Phdr {
    uint32_t p_type;           // PT_LOAD, PT_DYNAMIC, etc.
    uint32_t p_flags;          // PF_R | PF_W | PF_X
    uint64_t p_offset;         // File offset
    uint64_t p_vaddr;          // Virtual address
    uint64_t p_paddr;          // Physical address
    uint64_t p_filesz;         // Size in file
    uint64_t p_memsz;          // Size in memory
    uint64_t p_align;          // Alignment
};
```

### Loading Process

```
1. File Reading
   ??? LoadFile() reads binary from disk
       ??? LoadBuffer() processes in-memory data

2. Validation
   ??? ValidateELF() checks magic, class, machine type
       ??? Bounds checking for all offsets

3. Header Parsing
   ??? ParseHeader() extracts ELF header
       ??? Parses program header table
       ??? Identifies dynamic section

4. Segment Loading
   ??? LoadSegments() for each PT_LOAD:
       ??? Calculate load address (with PIE offset)
       ??? Copy file data to memory
       ??? Zero-fill .bss sections
       ??? Record segment information

5. Relocation (if needed)
   ??? ProcessRelocations() for PIE/DYN
       ??? ApplyRelocation() for each entry

6. Stack Setup
   ??? SetupStack() creates initial stack:
       ??? argc value
       ??? argv[] pointers
       ??? envp[] pointers
       ??? auxv[] entries
       ??? String data

7. CPU Initialization
   ??? InitializeCPU() sets:
       ??? IP = entry point
       ??? GR12 = stack pointer
       ??? AR.BSP = backing store
       ??? AR.RSC = RSE config
       ??? CFM = frame marker
       ??? PSR = processor status
```

## Memory Layout

### Typical Executable Layout

```
0x0000000000000000  ???????????????????????
                    ?   NULL page         ?
0x0000000000400000  ???????????????????????
                    ?   .text (code)      ? PF_R | PF_X
                    ???????????????????????
                    ?   .rodata (const)   ? PF_R
                    ???????????????????????
                    ?   .data (init data) ? PF_R | PF_W
                    ???????????????????????
                    ?   .bss (zero data)  ? PF_R | PF_W
0x0000080000000000  ???????????????????????
                    ?   RSE Backing Store ? (grows upward)
0x00007FFFFFFFF000  ???????????????????????
                    ?   Stack             ? (grows downward)
0x00007FFFFFFFFFF0  ???????????????????????
```

### Stack Layout

```
High Address
    ???????????????????????????
    ? String data             ?
    ???????????????????????????
    ? Auxiliary vectors       ?
    ?   AT_NULL = 0           ?
    ?   AT_EGID = ...         ?
    ?   AT_GID = ...          ?
    ?   AT_EUID = ...         ?
    ?   AT_UID = ...          ?
    ?   AT_ENTRY = ...        ?
    ?   AT_BASE = ...         ?
    ?   AT_PHNUM = ...        ?
    ?   AT_PHENT = ...        ?
    ?   AT_PHDR = ...         ?
    ?   AT_PAGESZ = 4096      ?
    ???????????????????????????
    ? envp[n] = NULL          ?
    ? envp[n-1] ? string      ?
    ? ...                     ?
    ? envp[0] ? string        ?
    ???????????????????????????
    ? argv[argc] = NULL       ?
    ? argv[argc-1] ? string   ?
    ? ...                     ?
    ? argv[0] ? string        ?
    ???????????????????????????
SP? ? argc                    ?
    ???????????????????????????
Low Address
```

## Usage Example

```cpp
#include "loader.h"
#include "memory.h"
#include "cpu_state.h"

// Create memory system
ia64::Memory memory(64 * 1024 * 1024);  // 64 MB

// Create CPU state
ia64::CPUState cpu;

// Create loader
ia64::ELFLoader loader;

// Load ELF file
try {
    uint64_t entryPoint = loader.LoadFile("program.elf", memory, cpu);
    
    // Check loaded segments
    for (const auto& seg : loader.GetSegments()) {
        std::cout << "Segment at 0x" << std::hex << seg.virtualAddress
                  << " size " << seg.memorySize
                  << " flags " << seg.flags << std::endl;
    }
    
    // CPU is now ready to execute at entryPoint
    // cpu.GetIP() == entryPoint
    
} catch (const std::exception& e) {
    std::cerr << "Load failed: " << e.what() << std::endl;
}
```

## IA-64 Specific Considerations

### 1. **Register Stack Engine (RSE)**
- Backing store pointer (AR.BSP) placed at 0x80000000000
- RSE grows upward from backing store base
- AR.RSC configured for eager stores

### 2. **Bundle Alignment**
- IA-64 instructions are bundled in 128-bit (16-byte) units
- Entry point must be bundle-aligned
- Code segments should align to 16-byte boundaries

### 3. **Predicate Registers**
- PR0 always initialized to true (architectural requirement)
- Other predicate registers cleared on reset

### 4. **Current Frame Marker (CFM)**
- Initialized to 0 (no active frame)
- Will be set by first `alloc` instruction

### 5. **Application Registers**
- AR16 (AR.RSC) - RSE configuration
- AR36 (AR.BSP) - Backing store pointer
- Many others available for OS use

## Limitations and Future Work

### Current Limitations
1. **Dynamic Linking**: Stub implementation only
   - No shared library loading
   - No symbol resolution from external libraries
   - No PLT/GOT handling

2. **Complex Relocations**: Basic support only
   - PC-relative branch relocations need bundle patching
   - LTOFF (linkage table offset) relocations not implemented
   - Function descriptor relocations stubbed

3. **Security**: Minimal validation
   - No RELRO (Read-Only Relocations)
   - No stack canaries
   - No ASLR (Address Space Layout Randomization)

4. **Thread-Local Storage**: Not implemented
   - PT_TLS segments ignored
   - No TLS block allocation

### Future Enhancements
- [ ] Full dynamic linker implementation
- [ ] Advanced relocation types
- [ ] TLS support
- [ ] Debug symbol loading
- [ ] Memory protection via MMU
- [ ] DWARF unwind information parsing
- [ ] Lazy binding for dynamic symbols
- [ ] Prelinking support

## Testing

### Test Cases Needed
1. Simple static executable
2. PIE executable with relocations
3. Executable with .bss section
4. Multi-segment executable
5. Executable with large stack requirements
6. Malformed ELF handling

### Validation Points
- Entry point correctness
- Stack pointer alignment (16 bytes)
- Auxiliary vector values
- Segment permissions
- .bss zero-initialization
- Memory bounds

## References

1. **ELF Specification**
   - Tool Interface Standard (TIS) ELF Specification Version 1.2
   - System V Application Binary Interface

2. **IA-64 Specific**
   - Intel Itanium Software Conventions and Runtime Guide
   - IA-64 Linux Kernel ABI
   - Itanium Processor Family Software Developer's Manual

3. **Linux ABI**
   - Linux Standard Base (LSB) Specification
   - Auxiliary Vector documentation

## Implementation Files

- `include/loader.h` - ELF loader interface and structures
- `src/loader/loader.cpp` - ELF loader implementation
- `include/abi.h` - Linux ABI definitions
- `include/cpu_state.h` - CPU state management
- `include/memory.h` - Memory system interface

## Version History

- **v1.0** (Current) - Initial full implementation
  - Complete ELF64 parsing
  - Segment loading with PIE support
  - Basic relocation support
  - Stack and auxiliary vector setup
  - Full CPU initialization

---

*Last Updated: 2024*
*Author: guideXOS Hypervisor Project*
