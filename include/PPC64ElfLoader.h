#pragma once

#include "PPC64Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ia64 {
class VirtualMachine;

namespace ppc64 {

struct PPC64ElfLoadSegment {
    uint64_t fileOffset = 0;
    uint64_t virtualAddress = 0;
    uint64_t physicalAddress = 0;
    uint64_t fileSize = 0;
    uint64_t memorySize = 0;
    uint64_t alignment = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> fileBytes;
};

struct PPC64ElfImage {
    uint64_t virtualEntry = 0;
    uint64_t physicalEntry = 0;
    uint64_t physicalBase = 0;
    uint64_t physicalEnd = 0;
    uint32_t flags = 0;
    std::vector<PPC64ElfLoadSegment> loadSegments;
};

// Parses only the bounded ELF64 BE PPC64 ET_EXEC subset used by the Phase 20
// direct-entry loader. It never consults section headers or applies ELF
// relocations. The result is replaced only after the complete image validates.
bool parsePPC64Elf64BE(const std::vector<uint8_t>& image,
                       PPC64ElfImage& result, std::string& reason);

// Parse, validate against the production VM's guest RAM/reservations, and
// atomically publish file-backed PT_LOAD bytes, BSS, FDT, and optional RTAS.
bool loadPPC64Elf(VirtualMachine& vm, const std::vector<uint8_t>& image,
                  PPC64ElfImage& result, std::string& reason);

} // namespace ppc64
} // namespace ia64
