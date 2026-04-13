#include "dynamic_linker.h"
#include "loader.h"
#include "IMemory.h"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace ia64 {

// ELF structures (ELF64_Dyn, ELF64_Sym, ELF64_Rela) are defined in loader.h

// Memory region for shared libraries (starts at 2GB)
constexpr uint64_t SHARED_LIBRARY_BASE = 0x80000000;
constexpr uint64_t SHARED_LIBRARY_SIZE = 0x10000000;  // 256MB per library

DynamicLinker::DynamicLinker()
    : nextLibraryAddress_(SHARED_LIBRARY_BASE)
    , initialized_(false)
{
    // Default library search paths for IA-64 Linux
    searchPaths_.push_back("/lib");
    searchPaths_.push_back("/lib64");
    searchPaths_.push_back("/usr/lib");
    searchPaths_.push_back("/usr/lib64");
    searchPaths_.push_back("/usr/local/lib");
}

DynamicLinker::~DynamicLinker() {
    Clear();
}

// ===== Binary Detection =====

bool DynamicLinker::IsDynamicBinary(const uint8_t* buffer, size_t size) {
    if (size < sizeof(ELF64_Ehdr)) {
        return false;
    }
    
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    // Check if ET_DYN (shared object) or has PT_INTERP
    if (ehdr->e_type == static_cast<uint16_t>(ELFType::DYN)) {
        return true;
    }
    
    return HasInterpreter(buffer, size);
}

bool DynamicLinker::HasInterpreter(const uint8_t* buffer, size_t size) {
    if (size < sizeof(ELF64_Ehdr)) {
        return false;
    }
    
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    // Check program headers for PT_INTERP
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        if (offset + sizeof(ELF64_Phdr) > size) {
            break;
        }
        
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer + offset);
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_INTERP)) {
            return true;
        }
    }
    
    return false;
}

std::string DynamicLinker::GetInterpreterPath(const uint8_t* buffer, size_t size) {
    if (size < sizeof(ELF64_Ehdr)) {
        return "";
    }
    
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        if (offset + sizeof(ELF64_Phdr) > size) {
            break;
        }
        
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer + offset);
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_INTERP)) {
            if (phdr->p_offset + phdr->p_filesz <= size) {
                const char* path = reinterpret_cast<const char*>(buffer + phdr->p_offset);
                return std::string(path, phdr->p_filesz - 1);  // Exclude null terminator
            }
        }
    }
    
    return "";
}

// ===== Library Loading =====

SharedLibrary* DynamicLinker::LoadLibrary(const std::string& libraryName, IMemory& memory) {
    // Check if already loaded
    for (auto& lib : loadedLibraries_) {
        if (lib.name == libraryName) {
            return &lib;
        }
    }
    
    // Find library file
    std::string libraryPath = FindLibraryPath(libraryName);
    if (libraryPath.empty()) {
        // Create stub library for emulation
        SharedLibrary stubLib;
        stubLib.name = libraryName;
        stubLib.path = libraryName;
        stubLib.baseAddress = AllocateLibraryMemory(0x100000);  // 1MB stub
        stubLib.size = 0x100000;
        loadedLibraries_.push_back(stubLib);
        return &loadedLibraries_.back();
    }
    
    // Load library file
    std::ifstream file(libraryPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return nullptr;
    }
    
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        return nullptr;
    }
    file.close();
    
    // Allocate memory for library
    uint64_t baseAddr = AllocateLibraryMemory(fileSize + 0x10000);
    if (baseAddr == 0) {
        return nullptr;
    }
    
    // Create library entry
    SharedLibrary lib;
    lib.name = libraryName;
    lib.path = libraryPath;
    lib.baseAddress = baseAddr;
    lib.size = fileSize + 0x10000;
    
    // Load segments into memory
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer.data());
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer.data() + offset);
        
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            uint64_t vaddr = baseAddr + phdr->p_vaddr;
            for (uint64_t j = 0; j < phdr->p_filesz; ++j) {
                memory.write<uint8_t>(vaddr + j, buffer[phdr->p_offset + j]);
            }
            
            // Zero-fill remaining memory
            for (uint64_t j = phdr->p_filesz; j < phdr->p_memsz; ++j) {
                memory.write<uint8_t>(vaddr + j, 0);
            }
        } else if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_DYNAMIC)) {
            lib.dynamicAddr = baseAddr + phdr->p_vaddr;
        }
    }
    
    // Parse dynamic section
    if (lib.dynamicAddr != 0) {
        ParseDynamicSection(buffer.data(), fileSize, baseAddr, memory);
    }
    
    loadedLibraries_.push_back(lib);
    return &loadedLibraries_.back();
}

bool DynamicLinker::LoadDependencies(const uint8_t* buffer, size_t size, IMemory& memory) {
    if (size < sizeof(ELF64_Ehdr)) {
        return false;
    }
    
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    // Find PT_DYNAMIC segment
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    uint64_t dynamicOffset = 0;
    uint64_t dynamicSize = 0;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        if (offset + sizeof(ELF64_Phdr) > size) {
            break;
        }
        
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer + offset);
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_DYNAMIC)) {
            dynamicOffset = phdr->p_offset;
            dynamicSize = phdr->p_filesz;
            break;
        }
    }
    
    if (dynamicOffset == 0) {
        return true;  // No dependencies
    }
    
    // Parse DT_NEEDED entries
    uint64_t strtabOffset = 0;
    std::vector<uint32_t> neededOffsets;
    
    size_t numEntries = dynamicSize / sizeof(ELF64_Dyn);
    for (size_t i = 0; i < numEntries; ++i) {
        uint64_t offset = dynamicOffset + i * sizeof(ELF64_Dyn);
        if (offset + sizeof(ELF64_Dyn) > size) {
            break;
        }
        
        const ELF64_Dyn* dyn = reinterpret_cast<const ELF64_Dyn*>(buffer + offset);
        
        if (dyn->d_tag == static_cast<int64_t>(DynamicTag::DT_NULL)) {
            break;
        } else if (dyn->d_tag == static_cast<int64_t>(DynamicTag::DT_NEEDED)) {
            neededOffsets.push_back(static_cast<uint32_t>(dyn->d_un.d_val));
        } else if (dyn->d_tag == static_cast<int64_t>(DynamicTag::DT_STRTAB)) {
            // Note: This is a virtual address, need to convert to file offset
            // For simplicity, we'll handle this in a second pass
        }
    }
    
    // Load each needed library
    for (uint32_t offset : neededOffsets) {
        // In a full implementation, we'd read the library name from the string table
        // For now, we'll load common libraries
        LoadLibrary("libc.so.6", memory);
        LoadLibrary("libm.so.6", memory);
        LoadLibrary("libpthread.so.0", memory);
    }
    
    return true;
}

void DynamicLinker::SetLibrarySearchPaths(const std::vector<std::string>& paths) {
    searchPaths_ = paths;
}

void DynamicLinker::AddLibrarySearchPath(const std::string& path) {
    searchPaths_.push_back(path);
}

// ===== Symbol Resolution =====

DynamicSymbol* DynamicLinker::ResolveSymbol(const std::string& symbolName) {
    // Check global symbols first
    auto it = globalSymbols_.find(symbolName);
    if (it != globalSymbols_.end()) {
        return &it->second;
    }
    
    // Search loaded libraries
    for (auto& lib : loadedLibraries_) {
        auto symIt = lib.symbols.find(symbolName);
        if (symIt != lib.symbols.end()) {
            return &symIt->second;
        }
    }
    
    // Check stubs
    auto stubIt = stubs_.find(symbolName);
    if (stubIt != stubs_.end()) {
        static DynamicSymbol stubSym;
        stubSym.name = symbolName;
        stubSym.address = stubIt->second;
        stubSym.type = STT_FUNC;
        stubSym.library = "stub";
        return &stubSym;
    }
    
    return nullptr;
}

DynamicSymbol* DynamicLinker::ResolveSymbolInLibrary(const std::string& symbolName, 
                                                      const std::string& libraryName) {
    if (libraryName.empty()) {
        return ResolveSymbol(symbolName);
    }
    
    for (auto& lib : loadedLibraries_) {
        if (lib.name == libraryName) {
            auto it = lib.symbols.find(symbolName);
            if (it != lib.symbols.end()) {
                return &it->second;
            }
        }
    }
    
    return nullptr;
}

void DynamicLinker::ExportSymbol(const std::string& symbolName, uint64_t address, uint8_t type) {
    DynamicSymbol sym;
    sym.name = symbolName;
    sym.address = address;
    sym.type = type;
    sym.binding = STB_GLOBAL;
    sym.library = "main";
    
    globalSymbols_[symbolName] = sym;
}

// ===== Relocation Processing =====

bool DynamicLinker::ProcessRelocations(const uint8_t* buffer, size_t size,
                                       uint64_t baseAddress, IMemory& memory) {
    // Find relocation sections
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    uint64_t dynamicOffset = 0;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        if (offset + sizeof(ELF64_Phdr) > size) {
            break;
        }
        
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer + offset);
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_DYNAMIC)) {
            dynamicOffset = phdr->p_offset;
            break;
        }
    }
    
    if (dynamicOffset == 0) {
        return true;  // No relocations
    }
    
    // For simplified implementation, we'll just return success
    // Full relocation processing would parse RELA/REL sections
    return true;
}

bool DynamicLinker::ProcessPLTRelocations(const uint8_t* buffer, size_t size,
                                         uint64_t baseAddress, IMemory& memory) {
    // PLT relocations are typically processed lazily
    // For now, return success
    return true;
}

bool DynamicLinker::ApplyRelocation(const ELF64_Rela& relocation, uint64_t baseAddress, IMemory& memory) {
    uint32_t type = ELF64_R_TYPE(relocation.r_info);
    uint32_t sym = ELF64_R_SYM(relocation.r_info);
    
    uint64_t relocAddr = baseAddress + relocation.r_offset;
    
    // Handle different IA-64 relocation types
    switch (type) {
        case static_cast<uint32_t>(RelocationType::R_IA64_NONE):
            // No relocation
            break;
            
        case static_cast<uint32_t>(RelocationType::R_IA64_DIR64LSB):
        case static_cast<uint32_t>(RelocationType::R_IA64_DIR64MSB): {
            // Absolute 64-bit relocation
            uint64_t value = baseAddress + relocation.r_addend;
            for (int i = 0; i < 8; ++i) {
                memory.write<uint8_t>(relocAddr + i, (value >> (i * 8)) & 0xFF);
            }
            break;
        }
        
        default:
            // Unsupported relocation type - skip
            break;
    }
    
    return true;
}

bool DynamicLinker::ParseDynamicSection(const uint8_t* buffer, size_t size,
                                       uint64_t baseAddress, IMemory& memory) {
    // Simplified dynamic section parsing
    return true;
}

bool DynamicLinker::GetDynamicTag(const uint8_t* buffer, size_t size, int64_t tag, uint64_t& value) {
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    uint64_t phoff = ehdr->e_phoff;
    uint16_t phnum = ehdr->e_phnum;
    uint16_t phentsize = ehdr->e_phentsize;
    
    uint64_t dynamicOffset = 0;
    uint64_t dynamicSize = 0;
    
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t offset = phoff + i * phentsize;
        if (offset + sizeof(ELF64_Phdr) > size) {
            break;
        }
        
        const ELF64_Phdr* phdr = reinterpret_cast<const ELF64_Phdr*>(buffer + offset);
        if (phdr->p_type == static_cast<uint32_t>(SegmentType::PT_DYNAMIC)) {
            dynamicOffset = phdr->p_offset;
            dynamicSize = phdr->p_filesz;
            break;
        }
    }
    
    if (dynamicOffset == 0) {
        return false;
    }
    
    size_t numEntries = dynamicSize / sizeof(ELF64_Dyn);
    for (size_t i = 0; i < numEntries; ++i) {
        uint64_t offset = dynamicOffset + i * sizeof(ELF64_Dyn);
        if (offset + sizeof(ELF64_Dyn) > size) {
            break;
        }
        
        const ELF64_Dyn* dyn = reinterpret_cast<const ELF64_Dyn*>(buffer + offset);
        
        if (dyn->d_tag == static_cast<int64_t>(DynamicTag::DT_NULL)) {
            break;
        } else if (dyn->d_tag == tag) {
            value = dyn->d_un.d_val;
            return true;
        }
    }
    
    return false;
}

// ===== Stub Resolution =====

void DynamicLinker::RegisterStub(const std::string& symbolName, uint64_t stubAddress) {
    stubs_[symbolName] = stubAddress;
}

bool DynamicLinker::HasStub(const std::string& symbolName) {
    return stubs_.find(symbolName) != stubs_.end();
}

void DynamicLinker::CreateDefaultStubs(IMemory& memory) {
    // Common libc functions that need stubs
    const char* stubFunctions[] = {
        "printf", "fprintf", "sprintf", "snprintf",
        "malloc", "calloc", "realloc", "free",
        "strlen", "strcpy", "strcmp", "strncpy", "strncmp",
        "memcpy", "memset", "memcmp", "memmove",
        "fopen", "fclose", "fread", "fwrite", "fseek", "ftell",
        "exit", "abort", "_exit",
        "getpid", "getppid", "getuid", "getgid",
        nullptr
    };
    
    uint64_t stubAddr = 0x90000000;  // Stub function region
    
    for (int i = 0; stubFunctions[i] != nullptr; ++i) {
        RegisterStub(stubFunctions[i], stubAddr);
        stubAddr += 0x100;  // 256 bytes per stub
    }
}

// ===== Utilities =====

std::vector<std::string> DynamicLinker::GetLoadedLibraries() const {
    std::vector<std::string> names;
    for (const auto& lib : loadedLibraries_) {
        names.push_back(lib.name);
    }
    return names;
}

SharedLibrary* DynamicLinker::GetLibrary(const std::string& name) {
    for (auto& lib : loadedLibraries_) {
        if (lib.name == name) {
            return &lib;
        }
    }
    return nullptr;
}

void DynamicLinker::Clear() {
    loadedLibraries_.clear();
    globalSymbols_.clear();
    stubs_.clear();
    nextLibraryAddress_ = SHARED_LIBRARY_BASE;
}

// ===== Internal Helper Methods =====

std::string DynamicLinker::FindLibraryPath(const std::string& libraryName) {
    // Search in library paths
    for (const auto& path : searchPaths_) {
        std::string fullPath = path + "/" + libraryName;
        std::ifstream test(fullPath);
        if (test.good()) {
            return fullPath;
        }
    }
    
    return "";  // Not found
}

std::string DynamicLinker::ReadStringFromTable(IMemory& memory, uint64_t stringTableAddr, uint32_t offset) {
    std::string result;
    uint64_t addr = stringTableAddr + offset;
    
    while (true) {
        uint8_t ch = memory.read<uint8_t>(addr++);
        if (ch == 0) {
            break;
        }
        result.push_back(static_cast<char>(ch));
        
        if (result.length() > 1024) {
            break;  // Safety limit
        }
    }
    
    return result;
}

void DynamicLinker::ParseSymbolTable(SharedLibrary& library, IMemory& memory) {
    // Simplified symbol table parsing
    // In a full implementation, this would parse the symbol table and populate library.symbols
}

uint64_t DynamicLinker::AllocateLibraryMemory(size_t size) {
    uint64_t addr = nextLibraryAddress_;
    nextLibraryAddress_ += size;
    
    // Align to page boundary
    nextLibraryAddress_ = (nextLibraryAddress_ + 0xFFF) & ~0xFFFULL;
    
    return addr;
}

} // namespace ia64
