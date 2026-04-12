#include "loader.h"
#include "loader.h"
#include "memory.h"
#include "cpu_state.h"
#include "bootstrap.h"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace ia64 {

// IA-64 specific constants
constexpr uint64_t PAGE_SIZE = 4096;

ELFLoader::ELFLoader()
    : entryPoint_(0)
    , baseAddress_(0)
    , dynamicAddr_(0)
    , dynamicSize_(0)
    , isLoaded_(false)
    , elfType_(ELFType::NONE)
{
    std::memset(&header_, 0, sizeof(header_));
}

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

    // Parse header and program headers
    if (!ParseHeader(buffer, size)) {
        throw std::runtime_error("Failed to parse ELF header");
    }

    // Load segments into memory
    if (!LoadSegments(buffer, size, memory)) {
        throw std::runtime_error("Failed to load ELF segments");
    }

    // Process relocations (for PIE/shared objects)
    if (!ProcessRelocations(buffer, size, memory)) {
        throw std::runtime_error("Failed to process relocations");
    }

    // Setup bootstrap configuration
    BootstrapConfig config;
    config.entryPoint = entryPoint_;
    config.argv = { "program" };  // Default argv[0]
    config.envp = {};  // Empty environment for now
    config.globalPointer = 0;  // For static executables
    config.programHeaderAddr = baseAddress_ + header_.e_phoff;
    config.programHeaderEntrySize = header_.e_phentsize;
    config.programHeaderCount = header_.e_phnum;
    config.baseAddress = baseAddress_;

    // Initialize CPU and memory state using bootstrap module
    InitializeBootstrapState(cpu, memory, config);

    isLoaded_ = true;
    return entryPoint_;
}

bool ELFLoader::ValidateELF(const uint8_t* buffer, size_t size) const {
    // Check minimum size for ELF64 header
    if (size < sizeof(ELF64_Ehdr)) {
        return false;
    }

    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);

    // Check magic number
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || 
        ehdr->e_ident[EI_MAG1] != ELFMAG1 || 
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || 
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }

    // Check class (64-bit)
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }

    // Check data encoding (little-endian)
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    // Check version
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        return false;
    }

    // Check machine type (IA-64)
    if (ehdr->e_machine != EM_IA_64) {
        return false;
    }

    // Check file type (executable or shared object)
    if (ehdr->e_type != static_cast<uint16_t>(ELFType::EXEC) && 
        ehdr->e_type != static_cast<uint16_t>(ELFType::DYN)) {
        return false;
    }

    // Check program header table is within bounds
    if (ehdr->e_phoff > 0 && ehdr->e_phnum > 0) {
        uint64_t phdrEnd = ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize);
        if (phdrEnd > size) {
            return false;
        }
    }

    return true;
}

bool ELFLoader::ParseHeader(const uint8_t* buffer, size_t size) {
    if (size < sizeof(ELF64_Ehdr)) {
        return false;
    }

    // Copy header
    std::memcpy(&header_, buffer, sizeof(ELF64_Ehdr));

    // Extract basic information
    entryPoint_ = header_.e_entry;
    elfType_ = static_cast<ELFType>(header_.e_type);

    // For PIE/shared objects, we may need to adjust base address
    if (elfType_ == ELFType::DYN) {
        baseAddress_ = 0;  // Will be determined during segment loading
    } else {
        baseAddress_ = 0;  // Static executable uses absolute addresses
    }

    // Parse program headers
    if (header_.e_phoff > 0 && header_.e_phnum > 0) {
        const uint8_t* phdrStart = buffer + header_.e_phoff;
        
        for (uint16_t i = 0; i < header_.e_phnum; ++i) {
            const uint8_t* phdrData = phdrStart + (i * header_.e_phentsize);
            
            if (phdrData + sizeof(ELF64_Phdr) > buffer + size) {
                return false;
            }

            ELF64_Phdr phdr;
            std::memcpy(&phdr, phdrData, sizeof(ELF64_Phdr));
            programHeaders_.push_back(phdr);

            // Track dynamic section
            if (phdr.p_type == static_cast<uint32_t>(SegmentType::PT_DYNAMIC)) {
                dynamicAddr_ = phdr.p_vaddr;
                dynamicSize_ = phdr.p_memsz;
            }
        }
    }

    return true;
}

bool ELFLoader::LoadSegments(const uint8_t* buffer, size_t size, MemorySystem& memory) {
    segments_.clear();

    // Determine load base address for PIE
    uint64_t minVAddr = UINT64_MAX;
    uint64_t maxVAddr = 0;

    // First pass: determine address range
    for (const auto& phdr : programHeaders_) {
        if (phdr.p_type == static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            minVAddr = std::min(minVAddr, phdr.p_vaddr);
            maxVAddr = std::max(maxVAddr, phdr.p_vaddr + phdr.p_memsz);
        }
    }

    // For PIE, we can choose a base address
    if (elfType_ == ELFType::DYN && minVAddr > 0) {
        baseAddress_ = 0x400000;  // Standard PIE base
    }

    // Second pass: load segments
    for (const auto& phdr : programHeaders_) {
        SegmentType segType = static_cast<SegmentType>(phdr.p_type);

        if (segType != SegmentType::PT_LOAD) {
            continue;  // Only load PT_LOAD segments
        }

        // Calculate actual load address
        uint64_t loadAddr = baseAddress_ + phdr.p_vaddr;

        // Validate file offset and size
        if (phdr.p_offset + phdr.p_filesz > size) {
            throw std::runtime_error("Segment extends beyond file size");
        }

        // Align addresses to page boundaries
        uint64_t alignedAddr = loadAddr & ~(PAGE_SIZE - 1);
        uint64_t pageOffset = loadAddr - alignedAddr;
        uint64_t alignedSize = ((phdr.p_memsz + pageOffset + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

        // Load file data into memory
        if (phdr.p_filesz > 0) {
            const uint8_t* segmentData = buffer + phdr.p_offset;
            memory.loadBuffer(loadAddr, segmentData, phdr.p_filesz);
        }

        // Zero-fill remaining memory (for .bss sections)
        if (phdr.p_memsz > phdr.p_filesz) {
            uint64_t bssStart = loadAddr + phdr.p_filesz;
            uint64_t bssSize = phdr.p_memsz - phdr.p_filesz;
            
            std::vector<uint8_t> zeros(bssSize, 0);
            memory.loadBuffer(bssStart, zeros.data(), bssSize);
        }

        // Record loaded segment
        LoadedSegment segment;
        segment.virtualAddress = loadAddr;
        segment.physicalAddress = loadAddr;  // 1:1 mapping for simplicity
        segment.fileOffset = phdr.p_offset;
        segment.size = phdr.p_filesz;
        segment.memorySize = phdr.p_memsz;
        segment.flags = phdr.p_flags;
        segment.type = segType;

        segments_.push_back(segment);
    }

    // Update entry point with base address
    if (elfType_ == ELFType::DYN) {
        entryPoint_ = baseAddress_ + header_.e_entry;
    } else {
        entryPoint_ = header_.e_entry;
    }

    return !segments_.empty();
}

bool ELFLoader::ProcessRelocations(const uint8_t* buffer, size_t size, MemorySystem& memory) {
    // If no dynamic section, no relocations to process
    if (dynamicAddr_ == 0 || elfType_ == ELFType::EXEC) {
        return true;
    }

    // For now, we support only simple executables without dynamic linking
    // Full implementation would:
    // 1. Parse dynamic section to find relocation tables
    // 2. For each relocation entry:
    //    - Determine relocation type
    //    - Look up symbol if needed
    //    - Apply relocation to target address
    // 3. Handle different relocation types (R_IA64_DIR64LSB, R_IA64_RELATIVE, etc.)

    // PIE executables may have R_IA64_RELATIVE relocations
    if (elfType_ == ELFType::DYN && baseAddress_ != 0) {
        // Simple base relocation - this is a stub
        // Real implementation would parse DT_RELA entries
    }

    return true;
}

bool ELFLoader::ApplyRelocation(const ELF64_Rela& rela, uint64_t symValue, MemorySystem& memory) {
    uint64_t relocAddr = baseAddress_ + rela.r_offset;
    uint32_t relocType = ELF64_R_TYPE(rela.r_info);
    uint64_t addend = rela.r_addend;

    RelocationType type = static_cast<RelocationType>(relocType);

    switch (type) {
        case RelocationType::R_IA64_NONE:
            // No relocation needed
            break;

        case RelocationType::R_IA64_DIR64LSB: {
            // 64-bit absolute address (little-endian)
            uint64_t value = baseAddress_ + symValue + addend;
            memory.write<uint64_t>(relocAddr, value);
            break;
        }

        case RelocationType::R_IA64_DIR32LSB: {
            // 32-bit absolute address (little-endian)
            uint32_t value = static_cast<uint32_t>(baseAddress_ + symValue + addend);
            memory.write<uint32_t>(relocAddr, value);
            break;
        }

        case RelocationType::R_IA64_PCREL21B:
        case RelocationType::R_IA64_PCREL60B: {
            // PC-relative relocations for branches
            // These require patching instruction bundles - complex for IA-64
            // For now, skip or implement simplified version
            break;
        }

        default:
            // Unknown relocation type - log but don't fail
            // In a real implementation, this might need to fail
            break;
    }

    return true;
}

} // namespace ia64
