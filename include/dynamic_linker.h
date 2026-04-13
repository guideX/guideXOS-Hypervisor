#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace ia64 {

// Forward declarations
class IMemory;
class ELFLoader;

// ELF structures are already defined in loader.h
// We just need forward declarations here
struct ELF64_Dyn;
struct ELF64_Sym;
struct ELF64_Rela;
struct ELF64_Rel;

/**
 * @file dynamic_linker.h
 * @brief Dynamic Linker for IA-64 Shared Libraries
 * 
 * This module provides dynamic linking support for IA-64 ELF binaries:
 * - Detects dynamically linked binaries (ET_DYN type or PT_INTERP segment)
 * - Loads and resolves shared library dependencies
 * - Performs runtime symbol resolution
 * - Applies relocations for position-independent code
 * - Implements lazy binding for PLT (Procedure Linkage Table)
 */

// Symbol binding types
constexpr uint8_t STB_LOCAL = 0;
constexpr uint8_t STB_GLOBAL = 1;
constexpr uint8_t STB_WEAK = 2;

// Symbol types
constexpr uint8_t STT_NOTYPE = 0;
constexpr uint8_t STT_OBJECT = 1;
constexpr uint8_t STT_FUNC = 2;
constexpr uint8_t STT_SECTION = 3;
constexpr uint8_t STT_FILE = 4;
constexpr uint8_t STT_TLS = 6;

// Symbol visibility
constexpr uint8_t STV_DEFAULT = 0;
constexpr uint8_t STV_INTERNAL = 1;
constexpr uint8_t STV_HIDDEN = 2;
constexpr uint8_t STV_PROTECTED = 3;

// Helper macros for symbol info (already defined in loader.h, but repeated for clarity)
#ifndef ELF64_ST_BIND
#define ELF64_ST_BIND(i) ((i) >> 4)
#endif

#ifndef ELF64_ST_TYPE
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#endif

#ifndef ELF64_ST_INFO
#define ELF64_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
#endif

// Helper macros for relocation info (already defined in loader.h, but repeated for clarity)
#ifndef ELF64_R_SYM
#define ELF64_R_SYM(i) ((i) >> 32)
#endif

#ifndef ELF64_R_TYPE
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)
#endif

#ifndef ELF64_R_INFO
#define ELF64_R_INFO(s, t) (((s) << 32) + ((t) & 0xffffffffL))
#endif

/**
 * @struct DynamicSymbol
 * @brief Represents a resolved dynamic symbol
 */
struct DynamicSymbol {
    std::string name;           // Symbol name
    uint64_t address;           // Resolved address
    uint64_t size;              // Symbol size
    uint8_t type;               // Symbol type (STT_*)
    uint8_t binding;            // Symbol binding (STB_*)
    std::string library;        // Source library name
    
    DynamicSymbol() : address(0), size(0), type(STT_NOTYPE), binding(STB_LOCAL) {}
};

/**
 * @struct SharedLibrary
 * @brief Represents a loaded shared library
 */
struct SharedLibrary {
    std::string name;           // Library name (e.g., "libc.so.6")
    std::string path;           // Full path to library
    uint64_t baseAddress;       // Load address in guest memory
    uint64_t size;              // Total size in memory
    
    // Dynamic section pointers
    uint64_t dynamicAddr;       // Address of _DYNAMIC section
    uint64_t symbolTableAddr;   // Address of symbol table
    uint64_t stringTableAddr;   // Address of string table
    uint64_t hashTableAddr;     // Address of symbol hash table
    uint64_t relaAddr;          // Address of RELA relocations
    uint64_t relaSize;          // Size of RELA relocations
    uint64_t pltRelaAddr;       // Address of PLT relocations
    uint64_t pltRelaSize;       // Size of PLT relocations
    uint64_t initFuncAddr;      // Initialization function
    uint64_t finiFuncAddr;      // Finalization function
    
    std::unordered_map<std::string, DynamicSymbol> symbols;  // Exported symbols
    
    SharedLibrary() : baseAddress(0), size(0), dynamicAddr(0),
                      symbolTableAddr(0), stringTableAddr(0), hashTableAddr(0),
                      relaAddr(0), relaSize(0), pltRelaAddr(0), pltRelaSize(0),
                      initFuncAddr(0), finiFuncAddr(0) {}
};

/**
 * @class DynamicLinker
 * @brief Implements dynamic linking and symbol resolution for IA-64 binaries
 * 
 * The dynamic linker handles:
 * 1. Detection of dynamically linked binaries
 * 2. Loading shared library dependencies
 * 3. Symbol resolution and relocation
 * 4. Lazy binding support
 * 5. Global Offset Table (GOT) and Procedure Linkage Table (PLT) setup
 */
class DynamicLinker {
public:
    DynamicLinker();
    ~DynamicLinker();

    // ===== Binary Detection =====
    
    /**
     * Check if ELF binary is dynamically linked
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @return true if binary requires dynamic linking
     */
    bool IsDynamicBinary(const uint8_t* buffer, size_t size);
    
    /**
     * Check if binary has PT_INTERP segment (dynamic linker path)
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @return true if PT_INTERP segment exists
     */
    bool HasInterpreter(const uint8_t* buffer, size_t size);
    
    /**
     * Get interpreter path from PT_INTERP segment
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @return Interpreter path (e.g., "/lib/ld-linux-ia64.so.2")
     */
    std::string GetInterpreterPath(const uint8_t* buffer, size_t size);

    // ===== Library Loading =====
    
    /**
     * Load shared library into memory
     * @param libraryName Library name (e.g., "libc.so.6")
     * @param memory Memory system
     * @return Pointer to loaded library info, or nullptr on failure
     */
    SharedLibrary* LoadLibrary(const std::string& libraryName, IMemory& memory);
    
    /**
     * Load all dependencies from DT_NEEDED entries
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @param memory Memory system
     * @return true if all dependencies loaded successfully
     */
    bool LoadDependencies(const uint8_t* buffer, size_t size, IMemory& memory);
    
    /**
     * Set library search paths
     * @param paths Vector of directory paths to search
     */
    void SetLibrarySearchPaths(const std::vector<std::string>& paths);
    
    /**
     * Add library search path
     * @param path Directory path to add
     */
    void AddLibrarySearchPath(const std::string& path);

    // ===== Symbol Resolution =====
    
    /**
     * Resolve a symbol by name
     * @param symbolName Symbol name to resolve
     * @return Pointer to resolved symbol, or nullptr if not found
     */
    DynamicSymbol* ResolveSymbol(const std::string& symbolName);
    
    /**
     * Resolve symbol from specific library
     * @param symbolName Symbol name
     * @param libraryName Library to search (empty for global search)
     * @return Pointer to resolved symbol, or nullptr if not found
     */
    DynamicSymbol* ResolveSymbolInLibrary(const std::string& symbolName, 
                                          const std::string& libraryName);
    
    /**
     * Export symbol for global resolution
     * @param symbolName Symbol name
     * @param address Symbol address
     * @param type Symbol type
     */
    void ExportSymbol(const std::string& symbolName, uint64_t address, uint8_t type);

    // ===== Relocation Processing =====
    
    /**
     * Process relocations for a loaded binary
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @param baseAddress Load address in guest memory
     * @param memory Memory system
     * @return true if relocations processed successfully
     */
    bool ProcessRelocations(const uint8_t* buffer, size_t size, 
                           uint64_t baseAddress, IMemory& memory);
    
    /**
     * Process PLT relocations (lazy binding)
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @param baseAddress Load address
     * @param memory Memory system
     * @return true if successful
     */
    bool ProcessPLTRelocations(const uint8_t* buffer, size_t size,
                              uint64_t baseAddress, IMemory& memory);
    
    /**
     * Apply a single relocation
     * @param relocation Relocation entry
     * @param baseAddress Load address
     * @param memory Memory system
     * @return true if successful
     */
    bool ApplyRelocation(const ELF64_Rela& relocation, uint64_t baseAddress, IMemory& memory);

    // ===== Dynamic Section Parsing =====
    
    /**
     * Parse PT_DYNAMIC segment
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @param baseAddress Load address
     * @param memory Memory system
     * @return true if successful
     */
    bool ParseDynamicSection(const uint8_t* buffer, size_t size,
                            uint64_t baseAddress, IMemory& memory);
    
    /**
     * Get dynamic tag value
     * @param buffer ELF file buffer
     * @param size Buffer size
     * @param tag Dynamic tag to search for
     * @param value Output: Tag value
     * @return true if tag found
     */
    bool GetDynamicTag(const uint8_t* buffer, size_t size, int64_t tag, uint64_t& value);

    // ===== Stub Resolution =====
    
    /**
     * Register stub implementation for unresolved symbol
     * @param symbolName Symbol name
     * @param stubAddress Address of stub function
     */
    void RegisterStub(const std::string& symbolName, uint64_t stubAddress);
    
    /**
     * Check if symbol has a registered stub
     * @param symbolName Symbol name
     * @return true if stub exists
     */
    bool HasStub(const std::string& symbolName);
    
    /**
     * Create default stubs for common libc functions
     * @param memory Memory system
     */
    void CreateDefaultStubs(IMemory& memory);

    // ===== Utilities =====
    
    /**
     * Get list of loaded libraries
     * @return Vector of library names
     */
    std::vector<std::string> GetLoadedLibraries() const;
    
    /**
     * Get library by name
     * @param name Library name
     * @return Pointer to library info, or nullptr if not found
     */
    SharedLibrary* GetLibrary(const std::string& name);
    
    /**
     * Clear all loaded libraries and symbols
     */
    void Clear();

private:
    // ===== Internal Helper Methods =====
    
    /**
     * Find library file in search paths
     * @param libraryName Library name
     * @return Full path to library, or empty if not found
     */
    std::string FindLibraryPath(const std::string& libraryName);
    
    /**
     * Read string from string table
     * @param memory Memory system
     * @param stringTableAddr Address of string table
     * @param offset Offset into string table
     * @return String value
     */
    std::string ReadStringFromTable(IMemory& memory, uint64_t stringTableAddr, uint32_t offset);
    
    /**
     * Parse symbol table
     * @param library Library to populate with symbols
     * @param memory Memory system
     */
    void ParseSymbolTable(SharedLibrary& library, IMemory& memory);
    
    /**
     * Allocate memory for library load
     * @param size Size to allocate
     * @return Guest address, or 0 on failure
     */
    uint64_t AllocateLibraryMemory(size_t size);

    // ===== Member Variables =====
    
    std::vector<SharedLibrary> loadedLibraries_;        // Loaded shared libraries
    std::unordered_map<std::string, DynamicSymbol> globalSymbols_;  // Global symbol table
    std::unordered_map<std::string, uint64_t> stubs_;   // Stub implementations
    std::vector<std::string> searchPaths_;              // Library search paths
    
    uint64_t nextLibraryAddress_;                       // Next available address for library
    bool initialized_;                                  // Initialization flag
};

} // namespace ia64
