#pragma once


#include <cstdint>
#include <string>
#include <vector>

namespace ia64 {

// Forward declarations
class Memory;
using MemorySystem = Memory; // Alias for backward compatibility
class CPUState;
class DynamicLinker;
class KernelValidator;

// ELF header constants (IA-64 specific)
constexpr uint8_t ELFMAG0 = 0x7F;
constexpr uint8_t ELFMAG1 = 'E';
constexpr uint8_t ELFMAG2 = 'L';
constexpr uint8_t ELFMAG3 = 'F';

constexpr uint16_t EM_IA_64 = 50;  // Intel IA-64
constexpr uint8_t ELFCLASS64 = 2;   // 64-bit objects
constexpr uint8_t ELFDATA2LSB = 1;  // Little-endian
constexpr uint8_t EV_CURRENT = 1;   // Current ELF version

// ELF identification indices
constexpr size_t EI_MAG0 = 0;
constexpr size_t EI_MAG1 = 1;
constexpr size_t EI_MAG2 = 2;
constexpr size_t EI_MAG3 = 3;
constexpr size_t EI_CLASS = 4;
constexpr size_t EI_DATA = 5;
constexpr size_t EI_VERSION = 6;
constexpr size_t EI_OSABI = 7;
constexpr size_t EI_ABIVERSION = 8;
constexpr size_t EI_NIDENT = 16;

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
    PT_TLS = 7,         // Thread-local storage
    PT_IA_64_ARCHEXT = 0x70000000,  // IA-64 architecture extension
    PT_IA_64_UNWIND = 0x70000001    // IA-64 unwind information
};

// Segment flags
constexpr uint32_t PF_X = 0x1;  // Execute
constexpr uint32_t PF_W = 0x2;  // Write
constexpr uint32_t PF_R = 0x4;  // Read

// Dynamic table tags
enum class DynamicTag : int64_t {
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_PLTGOT = 3,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_INIT = 12,
    DT_FINI = 13,
    DT_SONAME = 14,
    DT_RPATH = 15,
    DT_SYMBOLIC = 16,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELENT = 19,
    DT_PLTREL = 20,
    DT_DEBUG = 21,
    DT_TEXTREL = 22,
    DT_JMPREL = 23
};

// IA-64 relocation types
enum class RelocationType : uint32_t {
    R_IA64_NONE = 0x00,
    R_IA64_IMM14 = 0x21,
    R_IA64_IMM22 = 0x22,
    R_IA64_IMM64 = 0x23,
    R_IA64_DIR32MSB = 0x24,
    R_IA64_DIR32LSB = 0x25,
    R_IA64_DIR64MSB = 0x26,
    R_IA64_DIR64LSB = 0x27,
    R_IA64_FPTR64I = 0x43,
    R_IA64_FPTR32MSB = 0x44,
    R_IA64_FPTR32LSB = 0x45,
    R_IA64_FPTR64MSB = 0x46,
    R_IA64_FPTR64LSB = 0x47,
    R_IA64_PCREL60B = 0x48,
    R_IA64_PCREL21B = 0x49,
    R_IA64_PCREL21M = 0x4a,
    R_IA64_PCREL21F = 0x4b,
    R_IA64_LTOFF22 = 0x52,
    R_IA64_LTOFF64I = 0x53
};

// Pack structures for binary compatibility
#pragma pack(push, 1)

// ELF64 file header
struct ELF64_Ehdr {
    uint8_t  e_ident[EI_NIDENT];  // ELF identification
    uint16_t e_type;               // Object file type
    uint16_t e_machine;            // Machine type
    uint32_t e_version;            // Object file version
    uint64_t e_entry;              // Entry point address
    uint64_t e_phoff;              // Program header offset
    uint64_t e_shoff;              // Section header offset
    uint32_t e_flags;              // Processor-specific flags
    uint16_t e_ehsize;             // ELF header size
    uint16_t e_phentsize;          // Program header entry size
    uint16_t e_phnum;              // Program header count
    uint16_t e_shentsize;          // Section header entry size
    uint16_t e_shnum;              // Section header count
    uint16_t e_shstrndx;           // Section header string table index
};

// ELF64 program header
struct ELF64_Phdr {
    uint32_t p_type;               // Segment type
    uint32_t p_flags;              // Segment flags
    uint64_t p_offset;             // Segment file offset
    uint64_t p_vaddr;              // Segment virtual address
    uint64_t p_paddr;              // Segment physical address
    uint64_t p_filesz;             // Segment size in file
    uint64_t p_memsz;              // Segment size in memory
    uint64_t p_align;              // Segment alignment
};

// ELF64 section header
struct ELF64_Shdr {
    uint32_t sh_name;              // Section name (string table index)
    uint32_t sh_type;              // Section type
    uint64_t sh_flags;             // Section flags
    uint64_t sh_addr;              // Section virtual address
    uint64_t sh_offset;            // Section file offset
    uint64_t sh_size;              // Section size
    uint32_t sh_link;              // Link to another section
    uint32_t sh_info;              // Additional section info
    uint64_t sh_addralign;         // Section alignment
    uint64_t sh_entsize;           // Entry size if section holds table
};

// ELF64 symbol table entry
struct ELF64_Sym {
    uint32_t st_name;              // Symbol name (string table index)
    uint8_t  st_info;              // Symbol type and binding
    uint8_t  st_other;             // Symbol visibility
    uint16_t st_shndx;             // Section index
    uint64_t st_value;             // Symbol value
    uint64_t st_size;              // Symbol size
};

// ELF64 relocation with addend
struct ELF64_Rela {
    uint64_t r_offset;             // Relocation offset
    uint64_t r_info;               // Relocation type and symbol index
    int64_t  r_addend;             // Relocation addend
};

// ELF64 dynamic table entry
struct ELF64_Dyn {
    int64_t  d_tag;                // Dynamic entry type
    union {
        uint64_t d_val;            // Integer value
        uint64_t d_ptr;            // Address value
    } d_un;
};

#pragma pack(pop)

// Helper macros for ELF64_Rela
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffffL)
#define ELF64_R_INFO(s,t) (((s) << 32) + ((t) & 0xffffffffL))


// Represents a loaded ELF segment
struct LoadedSegment {
    uint64_t virtualAddress;
    uint64_t physicalAddress;
    uint64_t fileOffset;
    uint64_t size;
    uint64_t memorySize;
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

    // Get base load address (for PIE/shared objects)
    uint64_t GetBaseAddress() const { return baseAddress_; }

    // Validate if buffer contains valid IA-64 ELF
    bool ValidateELF(const uint8_t* buffer, size_t size) const;
    
    // Enable/disable kernel validation before load
    void SetKernelValidationEnabled(bool enabled) { kernelValidationEnabled_ = enabled; }
    
    // Set kernel validator instance (optional, for custom validation)
    void SetKernelValidator(KernelValidator* validator) { kernelValidator_ = validator; }
    
    // Check if binary is dynamically linked
    bool IsDynamic() const { return elfType_ == ELFType::DYN || hasInterpreter_; }
    
    // Get interpreter path (if PT_INTERP exists)
    std::string GetInterpreterPath() const { return interpreterPath_; }
    
    // Set dynamic linker instance
    void SetDynamicLinker(DynamicLinker* linker) { dynamicLinker_ = linker; }

private:
    // Parse ELF header
    bool ParseHeader(const uint8_t* buffer, size_t size);

    // Load program segments
    bool LoadSegments(const uint8_t* buffer, size_t size, MemorySystem& memory);

    // Process relocations (if dynamic)
    bool ProcessRelocations(const uint8_t* buffer, size_t size, MemorySystem& memory);

    // Apply a single relocation
    bool ApplyRelocation(const ELF64_Rela& rela, uint64_t symValue, MemorySystem& memory);

    // Enhanced validation methods
    bool ValidateArchitecture(const ELF64_Ehdr* ehdr) const;
    bool ValidateEntryPoint(const ELF64_Ehdr* ehdr, const std::vector<ELF64_Phdr>& phdrs) const;
    bool ValidateSegmentAlignment(const ELF64_Phdr& phdr) const;
    bool ValidateMemorySafety(const ELF64_Phdr& phdr, size_t fileSize, size_t memorySize) const;

    // Parsed ELF header
    ELF64_Ehdr header_;

    // Program headers
    std::vector<ELF64_Phdr> programHeaders_;

    // Loaded segments
    std::vector<LoadedSegment> segments_;

    // Entry point
    uint64_t entryPoint_;

    // Base address (for PIE)
    uint64_t baseAddress_;

    // Dynamic section information
    uint64_t dynamicAddr_;
    uint64_t dynamicSize_;

    // Load state
    bool isLoaded_;
    ELFType elfType_;
    
    // Dynamic linking support
    bool hasInterpreter_;
    std::string interpreterPath_;
    DynamicLinker* dynamicLinker_;
    
    // Kernel validation support
    bool kernelValidationEnabled_;
    KernelValidator* kernelValidator_;
};

} // namespace ia64
