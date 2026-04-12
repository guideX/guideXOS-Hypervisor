#include "loader.h"
#include "memory.h"
#include "cpu_state.h"
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace ia64 {

ELFLoader::ELFLoader()
    : entryPoint_(0)
    , isLoaded_(false)
{}

uint64_t ELFLoader::LoadFile(const std::string& filePath, MemorySystem& memory, CPUState& cpu) {
    // Open file
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open ELF file: " + filePath);
    }

    // Get file size
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Read entire file into buffer
    std::vector<uint8_t> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        throw std::runtime_error("Failed to read ELF file: " + filePath);
    }

    file.close();

    // Load from buffer
    return LoadBuffer(buffer.data(), buffer.size(), memory, cpu);
}

uint64_t ELFLoader::LoadBuffer(const uint8_t* buffer, size_t size, MemorySystem& memory, CPUState& cpu) {
    // Validate ELF header
    if (!ValidateELF(buffer, size)) {
        throw std::runtime_error("Invalid IA-64 ELF file");
    }

    // Parse header
    if (!ParseHeader(buffer, size)) {
        throw std::runtime_error("Failed to parse ELF header");
    }

    // Load segments
    if (!LoadSegments(buffer, size, memory)) {
        throw std::runtime_error("Failed to load ELF segments");
    }

    // Initialize CPU
    InitializeCPU(cpu);

    isLoaded_ = true;
    return entryPoint_;
}

bool ELFLoader::ValidateELF(const uint8_t* buffer, size_t size) const {
    // Check minimum size for ELF64 header (64 bytes)
    if (size < 64) {
        return false;
    }

    // Check magic number
    if (buffer[0] != ELFMAG0 || buffer[1] != ELFMAG1 || 
        buffer[2] != ELFMAG2 || buffer[3] != ELFMAG3) {
        return false;
    }

    // Check class (64-bit)
    if (buffer[4] != ELFCLASS64) {
        return false;
    }

    // Check machine type (IA-64)
    uint16_t machine = *reinterpret_cast<const uint16_t*>(&buffer[18]);
    if (machine != EM_IA_64) {
        return false;
    }

    return true;
}

bool ELFLoader::ParseHeader(const uint8_t* buffer, size_t size) {
    // This is a STUB implementation
    // Real ELF parsing would extract:
    // - Entry point from offset 24 (8 bytes)
    // - Program header offset from offset 32 (8 bytes)
    // - Section header offset from offset 40 (8 bytes)
    // - Program header entry size and count
    // etc.

    // For now, just extract entry point (assuming little-endian)
    if (size >= 32) {
        std::memcpy(&entryPoint_, &buffer[24], 8);
    } else {
        return false;
    }

    // Stub: no actual segment parsing yet
    return true;
}

bool ELFLoader::LoadSegments(const uint8_t* buffer, size_t size, MemorySystem& memory) {
    // This is a STUB implementation
    // Real implementation would:
    // 1. Parse program header table
    // 2. For each PT_LOAD segment:
    //    - Map memory region
    //    - Copy segment data from file
    //    - Set appropriate permissions (R/W/X)
    // 3. Handle .bss sections (zero-initialized)
    // 4. Process dynamic linking information if present

    // For now, just create a stub segment
    LoadedSegment stubSegment;
    stubSegment.virtualAddress = 0x10000;  // Standard load address
    stubSegment.size = 0x10000;            // 64KB
    stubSegment.flags = PF_R | PF_W | PF_X;
    stubSegment.type = SegmentType::PT_LOAD;

    segments_.push_back(stubSegment);

    // Note: Memory class auto-allocates on write, no need to map region
    // Future: Add memory protection when implementing page tables

    return true;
}

void ELFLoader::InitializeCPU(CPUState& cpu) {
    // Initialize CPU state for program execution
    cpu.Reset();
    
    // Set instruction pointer to entry point
    cpu.SetIP(entryPoint_);
    
    // Set up initial stack pointer (application register AR12 = RSE BSP)
    // For stub, use a simple stack at high address
    uint64_t stackTop = 0x7FFFFFF0000ULL;
    cpu.SetGR(12, stackTop);  // Stack pointer (by convention)
    
    // Initialize predicate register 0 (always true)
    cpu.SetPR(0, true);
}

} // namespace ia64
