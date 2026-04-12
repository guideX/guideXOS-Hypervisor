#pragma once


#include <cstdint>
#include <string>
#include <vector>

namespace ia64 {

// Forward declarations
class Memory;
using MemorySystem = Memory; // Alias for backward compatibility
class CPUState;

// ELF header constants (IA-64 specific)
constexpr uint8_t ELFMAG0 = 0x7F;
constexpr uint8_t ELFMAG1 = 'E';
constexpr uint8_t ELFMAG2 = 'L';
constexpr uint8_t ELFMAG3 = 'F';

constexpr uint16_t EM_IA_64 = 50;  // Intel IA-64
constexpr uint8_t ELFCLASS64 = 2;   // 64-bit objects

// ELF file types
enum class ELFType : uint16_t {
    NONE = 0,
    REL = 1,        // Relocatable file
    EXEC = 2,       // Executable file
    DYN = 3,        // Shared object file
    CORE = 4        // Core file
};

// Program segment types
enum class SegmentType : uint32_t {
    PT_NULL = 0,
    PT_LOAD = 1,        // Loadable segment
    PT_DYNAMIC = 2,     // Dynamic linking info
    PT_INTERP = 3,      // Interpreter path
    PT_NOTE = 4,        // Auxiliary info
    PT_SHLIB = 5,
    PT_PHDR = 6,        // Program header table
    PT_TLS = 7          // Thread-local storage
};

// Segment flags
constexpr uint32_t PF_X = 0x1;  // Execute
constexpr uint32_t PF_W = 0x2;  // Write
constexpr uint32_t PF_R = 0x4;  // Read

// Represents a loaded ELF segment
struct LoadedSegment {
    uint64_t virtualAddress;
    uint64_t size;
    uint32_t flags;
    SegmentType type;
};

// ELF loader for IA-64 binaries
class ELFLoader {
public:
    ELFLoader();
    ~ELFLoader() = default;

    // Load ELF file into memory
    // Returns entry point address on success, throws on failure
    uint64_t LoadFile(const std::string& filePath, MemorySystem& memory, CPUState& cpu);

    // Load ELF from memory buffer
    uint64_t LoadBuffer(const uint8_t* buffer, size_t size, MemorySystem& memory, CPUState& cpu);

    // Get information about loaded segments
    const std::vector<LoadedSegment>& GetSegments() const { return segments_; }

    // Get entry point of loaded binary
    uint64_t GetEntryPoint() const { return entryPoint_; }

    // Validate if buffer contains valid IA-64 ELF
    bool ValidateELF(const uint8_t* buffer, size_t size) const;

private:
    // Parse ELF header
    bool ParseHeader(const uint8_t* buffer, size_t size);

    // Load program segments
    bool LoadSegments(const uint8_t* buffer, size_t size, MemorySystem& memory);

    // Initialize CPU state for execution
    void InitializeCPU(CPUState& cpu);

    std::vector<LoadedSegment> segments_;
    uint64_t entryPoint_;
    bool isLoaded_;
};

} // namespace ia64
