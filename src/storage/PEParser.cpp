#include "PEParser.h"
#include "PEParser.h"
#include "logger.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

using ia64::LOG_ERROR;
using ia64::LOG_INFO;
using ia64::LOG_WARN;

namespace guideXOS {

PEParser::PEParser()
    : imageData_(nullptr)
    , imageSize_(0)
    , valid_(false) {
}

PEParser::~PEParser() {
}

bool PEParser::parse(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(DOSHeader)) {
        LOG_ERROR("Invalid PE image data");
        return false;
    }
    
    imageData_ = data;
    imageSize_ = size;
    
    if (!parseDOSHeader()) {
        LOG_ERROR("Failed to parse DOS header");
        return false;
    }
    
    if (!parsePEHeader()) {
        LOG_ERROR("Failed to parse PE header");
        return false;
    }
    
    if (!parseCOFFHeader()) {
        LOG_ERROR("Failed to parse COFF header");
        return false;
    }
    
    if (!parseOptionalHeader()) {
        LOG_ERROR("Failed to parse optional header");
        return false;
    }
    
    if (!parseSections()) {
        LOG_ERROR("Failed to parse sections");
        return false;
    }
    
    valid_ = true;
    LOG_INFO("PE/COFF image parsed successfully");
    
    // Use std::ostringstream for proper hex formatting
    std::ostringstream oss;
    oss << "  Machine: 0x" << std::hex << imageInfo_.machine << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Subsystem: " + std::to_string(imageInfo_.subsystem));
    
    oss.str("");
    oss << "  Image base: 0x" << std::hex << imageInfo_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point: 0x" << std::hex << imageInfo_.entryPoint << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Sections: " + std::to_string(imageInfo_.sections.size()));
    
    return true;
}

bool PEParser::parseDOSHeader() {
    if (imageSize_ < sizeof(DOSHeader)) {
        return false;
    }
    
    std::memcpy(&dosHeader_, imageData_, sizeof(DOSHeader));
    
    // Check MZ signature
    if (dosHeader_.e_magic != 0x5A4D) { // "MZ"
        LOG_ERROR("Invalid DOS header magic (expected MZ)");
        return false;
    }
    
    // Validate PE header offset
    if (dosHeader_.e_lfanew == 0 || dosHeader_.e_lfanew >= imageSize_) {
        LOG_ERROR("Invalid PE header offset");
        return false;
    }
    
    return true;
}

bool PEParser::parsePEHeader() {
    uint32_t peOffset = dosHeader_.e_lfanew;
    
    if (peOffset + sizeof(PEHeader) > imageSize_) {
        return false;
    }
    
    std::memcpy(&peHeader_, imageData_ + peOffset, sizeof(PEHeader));
    
    // Check PE signature
    if (peHeader_.signature != 0x00004550) { // "PE\0\0"
        LOG_ERROR("Invalid PE signature");
        return false;
    }
    
    return true;
}

bool PEParser::parseCOFFHeader() {
    uint32_t coffOffset = dosHeader_.e_lfanew + sizeof(PEHeader);
    
    if (coffOffset + sizeof(COFFHeader) > imageSize_) {
        return false;
    }
    
    std::memcpy(&coffHeader_, imageData_ + coffOffset, sizeof(COFFHeader));
    
    imageInfo_.machine = coffHeader_.machine;
    
    LOG_INFO("COFF Header:");
    LOG_INFO("  Machine: 0x" + std::to_string(coffHeader_.machine));
    LOG_INFO("  Number of sections: " + std::to_string(coffHeader_.numberOfSections));
    LOG_INFO("  Optional header size: " + std::to_string(coffHeader_.sizeOfOptionalHeader));
    
    return true;
}

bool PEParser::parseOptionalHeader() {
    uint32_t optOffset = dosHeader_.e_lfanew + sizeof(PEHeader) + sizeof(COFFHeader);
    
    if (coffHeader_.sizeOfOptionalHeader == 0) {
        LOG_WARN("No optional header present");
        return true;
    }
    
    if (optOffset + sizeof(PEOptionalHeader64) > imageSize_) {
        return false;
    }
    
    std::memcpy(&optionalHeader_, imageData_ + optOffset, sizeof(PEOptionalHeader64));
    
    // Validate magic (0x10B for PE32, 0x20B for PE32+)
    if (optionalHeader_.magic != 0x20B && optionalHeader_.magic != 0x10B) {
        LOG_ERROR("Invalid optional header magic: 0x" + std::to_string(optionalHeader_.magic));
        return false;
    }
    
    imageInfo_.imageBase = optionalHeader_.imageBase;
    imageInfo_.entryPoint = optionalHeader_.imageBase + optionalHeader_.addressOfEntryPoint;
    imageInfo_.subsystem = optionalHeader_.subsystem;
    imageInfo_.sectionAlignment = optionalHeader_.sectionAlignment;
    imageInfo_.fileAlignment = optionalHeader_.fileAlignment;
    imageInfo_.sizeOfImage = optionalHeader_.sizeOfImage;
    imageInfo_.sizeOfHeaders = optionalHeader_.sizeOfHeaders;
    imageInfo_.globalPointer = 0;
    imageInfo_.hasGlobalPointer = false;
    
    LOG_INFO("Optional Header:");
    
    std::ostringstream oss;
    oss << "  Magic: 0x" << std::hex << optionalHeader_.magic << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point RVA: 0x" << std::hex << optionalHeader_.addressOfEntryPoint << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Image base: 0x" << std::hex << optionalHeader_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Section alignment: " + std::to_string(optionalHeader_.sectionAlignment));
    LOG_INFO("  Subsystem: " + std::to_string(optionalHeader_.subsystem));
    
    return true;
}

bool PEParser::parseSections() {
    uint32_t sectionOffset = dosHeader_.e_lfanew + sizeof(PEHeader) + 
                             sizeof(COFFHeader) + coffHeader_.sizeOfOptionalHeader;
    
    imageInfo_.sections.clear();
    
    for (uint16_t i = 0; i < coffHeader_.numberOfSections; i++) {
        if (sectionOffset + sizeof(PESectionHeader) > imageSize_) {
            LOG_ERROR("Section header extends beyond image size");
            return false;
        }
        
        PESectionHeader sectionHeader;
        std::memcpy(&sectionHeader, imageData_ + sectionOffset, sizeof(PESectionHeader));
        
        PESectionInfo section;
        section.name = std::string(sectionHeader.name, 
                                   std::find(sectionHeader.name, sectionHeader.name + 8, '\0'));
        section.virtualAddress = sectionHeader.virtualAddress;
        section.virtualSize = sectionHeader.virtualSize;
        section.rawDataOffset = sectionHeader.pointerToRawData;
        section.rawDataSize = sectionHeader.sizeOfRawData;
        section.characteristics = sectionHeader.characteristics;
        
        imageInfo_.sections.push_back(section);
        
        LOG_INFO("Section " + std::to_string(i) + ": " + section.name);
        LOG_INFO("  Virtual address: 0x" + std::to_string(section.virtualAddress));
        LOG_INFO("  Virtual size: " + std::to_string(section.virtualSize));
        LOG_INFO("  Raw data offset: 0x" + std::to_string(section.rawDataOffset));
        LOG_INFO("  Raw data size: " + std::to_string(section.rawDataSize));
        
        sectionOffset += sizeof(PESectionHeader);
    }
    
    return true;
}

bool PEParser::loadImage(std::vector<uint8_t>& imageBuffer, uint64_t& loadAddress, 
                        uint64_t& entryPoint) {
    if (!valid_) {
        LOG_ERROR("PE image not parsed or invalid");
        return false;
    }
    
    LOG_INFO("=== PE/COFF Image Loader ===");
    LOG_INFO("Loading EFI executable (IA-64)");
    
    std::ostringstream oss;
    
    // Log image metadata
    oss << "Image metadata:";
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  SizeOfImage: 0x" << std::hex << imageInfo_.sizeOfImage << std::dec 
        << " (" << imageInfo_.sizeOfImage << " bytes)";
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  SizeOfHeaders: 0x" << std::hex << imageInfo_.sizeOfHeaders << std::dec 
        << " (" << imageInfo_.sizeOfHeaders << " bytes)";
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  ImageBase: 0x" << std::hex << imageInfo_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  AddressOfEntryPoint: 0x" << std::hex << (imageInfo_.entryPoint - imageInfo_.imageBase) << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Number of sections: " + std::to_string(imageInfo_.sections.size()));
    
    // STEP 1: Allocate memory using SizeOfImage
    LOG_INFO("");
    LOG_INFO("Step 1: Allocating image buffer...");
    imageBuffer.clear();
    imageBuffer.resize(imageInfo_.sizeOfImage, 0);  // Zero-initialize
    LOG_INFO("  Allocated " + std::to_string(imageInfo_.sizeOfImage) + " bytes (zero-initialized)");
    
    // STEP 2: Copy headers
    LOG_INFO("");
    LOG_INFO("Step 2: Copying PE headers...");
    if (imageInfo_.sizeOfHeaders > imageSize_) {
        LOG_ERROR("  ERROR: Headers size exceeds file size");
        return false;
    }
    
    std::memcpy(imageBuffer.data(), imageData_, imageInfo_.sizeOfHeaders);
    oss.str("");
    oss << "  Copied 0x" << std::hex << imageInfo_.sizeOfHeaders << std::dec 
        << " (" << imageInfo_.sizeOfHeaders << ") bytes of headers";
    LOG_INFO(oss.str());
    
    // STEP 3: Map each section
    LOG_INFO("");
    LOG_INFO("Step 3: Mapping sections into memory...");
    
    for (size_t i = 0; i < imageInfo_.sections.size(); ++i) {
        const auto& section = imageInfo_.sections[i];
        
        LOG_INFO("");
        LOG_INFO("  Section " + std::to_string(i) + ": " + section.name);
        
        oss.str("");
        oss << "    VirtualAddress (RVA): 0x" << std::hex << section.virtualAddress << std::dec;
        LOG_INFO(oss.str());
        
        oss.str("");
        oss << "    VirtualSize: 0x" << std::hex << section.virtualSize << std::dec 
            << " (" << section.virtualSize << " bytes)";
        LOG_INFO(oss.str());
        
        oss.str("");
        oss << "    PointerToRawData: 0x" << std::hex << section.rawDataOffset << std::dec;
        LOG_INFO(oss.str());
        
        oss.str("");
        oss << "    SizeOfRawData: 0x" << std::hex << section.rawDataSize << std::dec 
            << " (" << section.rawDataSize << " bytes)";
        LOG_INFO(oss.str());
        
        oss.str("");
        oss << "    Characteristics: 0x" << std::hex << section.characteristics << std::dec;
        LOG_INFO(oss.str());
        
        // Handle BSS sections (no raw data)
        if (section.rawDataSize == 0) {
            LOG_INFO("    -> BSS section (no raw data, already zero-initialized)");
            continue;
        }
        
        // Validate section is within file
        if (section.rawDataOffset + section.rawDataSize > imageSize_) {
            LOG_ERROR("    ERROR: Section raw data extends beyond file size");
            LOG_ERROR("      File size: " + std::to_string(imageSize_));
            LOG_ERROR("      Section end: " + std::to_string(section.rawDataOffset + section.rawDataSize));
            return false;
        }
        
        // Validate section fits in image buffer
        // Use VirtualSize for the check, not RawDataSize
        uint64_t sectionMemorySize = std::max(section.virtualSize, static_cast<uint64_t>(section.rawDataSize));
        if (section.virtualAddress + sectionMemorySize > imageInfo_.sizeOfImage) {
            LOG_ERROR("    ERROR: Section extends beyond allocated image size");
            LOG_ERROR("      Image size: " + std::to_string(imageInfo_.sizeOfImage));
            LOG_ERROR("      Section end: " + std::to_string(section.virtualAddress + sectionMemorySize));
            return false;
        }
        
        // Copy raw data from file to virtual address in memory
        uint8_t* destPtr = imageBuffer.data() + section.virtualAddress;
        const uint8_t* srcPtr = imageData_ + section.rawDataOffset;
        
        std::memcpy(destPtr, srcPtr, section.rawDataSize);
        
        oss.str("");
        oss << "    -> Copied " << section.rawDataSize << " bytes from file offset 0x" 
            << std::hex << section.rawDataOffset << " to RVA 0x" << section.virtualAddress << std::dec;
        LOG_INFO(oss.str());
        
        // If VirtualSize > SizeOfRawData, the remaining bytes are already zero (BSS)
        if (section.virtualSize > section.rawDataSize) {
            uint32_t bssSize = section.virtualSize - section.rawDataSize;
            oss.str("");
            oss << "    -> BSS padding: " << bssSize << " bytes (already zero-initialized)";
            LOG_INFO(oss.str());
        }
    }
    
    // STEP 4: Set load address and entry point
    LOG_INFO("");
    LOG_INFO("Step 4: Setting load address and entry point...");
    
    loadAddress = imageInfo_.imageBase;
    entryPoint = imageInfo_.entryPoint;
    
    oss.str("");
    oss << "  Load address (ImageBase): 0x" << std::hex << loadAddress << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
    LOG_INFO(oss.str());
    
    // STEP 5: Validate entry point
    LOG_INFO("");
    LOG_INFO("Step 5: Validating entry point...");
    
    uint64_t entryPointRVA = entryPoint - loadAddress;
    bool entryPointValid = false;
    std::string entryPointSection;
    bool entryPointInDataSection = false;
    
    for (const auto& section : imageInfo_.sections) {
        if (entryPointRVA >= section.virtualAddress && 
            entryPointRVA < section.virtualAddress + section.virtualSize) {
            entryPointValid = true;
            entryPointSection = section.name;
            
            // Check if section is executable
            const uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
            bool isExecutable = (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            
            oss.str("");
            oss << "  Entry point RVA 0x" << std::hex << entryPointRVA << std::dec 
                << " is in section: " << section.name;
            LOG_INFO(oss.str());
            
            if (isExecutable) {
                LOG_INFO("  Section is executable: YES ?");
            } else {
                LOG_WARN("  Section is executable: NO");
                LOG_WARN("  This may be an EFI descriptor - checking for indirect entry point...");
                entryPointInDataSection = true;
            }
            
            break;
        }
    }
    
    if (!entryPointValid) {
        LOG_ERROR("  ERROR: Entry point is not within any section!");
        return false;
    }
    
    // STEP 5.5: Handle EFI indirect entry point
    if (entryPointInDataSection) {
        LOG_INFO("");
        LOG_INFO("Step 5.5: Checking for EFI indirect entry point...");
        
        // IA-64 EFI entry points commonly reference a function descriptor:
        // [0] code address, [1] global pointer.
        if (entryPointRVA + 8 <= imageBuffer.size()) {
            const uint64_t descriptorRVA = entryPointRVA;
            uint64_t potentialRVA = 0;
            std::memcpy(&potentialRVA, &imageBuffer[entryPointRVA], sizeof(uint64_t));
            
            oss.str("");
            oss << "  Found value at entry point: 0x" << std::hex << potentialRVA << std::dec;
            LOG_INFO(oss.str());
            
            // Check if this points to .text section
            bool isValidCodePointer = false;
            for (const auto& section : imageInfo_.sections) {
                const uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
                bool isExecutable = (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
                
                if (isExecutable && 
                    potentialRVA >= section.virtualAddress && 
                    potentialRVA < section.virtualAddress + section.virtualSize) {
                    
                    oss.str("");
                    oss << "  ? This points to executable section: " << section.name 
                        << " (RVA 0x" << std::hex << potentialRVA << std::dec << ")";
                    LOG_INFO(oss.str());
                    
                    LOG_INFO("  Redirecting entry point to actual code location...");
                    entryPoint = loadAddress + potentialRVA;
                    entryPointRVA = potentialRVA;
                    isValidCodePointer = true;

                    if (descriptorRVA + 16 <= imageBuffer.size()) {
                        uint64_t descriptorGP = 0;
                        std::memcpy(&descriptorGP, &imageBuffer[descriptorRVA + 8], sizeof(uint64_t));
                        imageInfo_.globalPointer = loadAddress + descriptorGP;
                        imageInfo_.hasGlobalPointer = true;

                        oss.str("");
                        oss << "  Descriptor global pointer: 0x" << std::hex
                            << imageInfo_.globalPointer << std::dec;
                        LOG_INFO(oss.str());
                    }
                    
                    oss.str("");
                    oss << "  New entry point: 0x" << std::hex << entryPoint << std::dec;
                    LOG_INFO(oss.str());
                    
                    break;
                }
            }
            
            if (!isValidCodePointer) {
                LOG_WARN("  Value does not point to executable code - using original entry point");
            }
        }
    }
    
    // STEP 6: Dump entry point bytes for debugging
    LOG_INFO("");
    LOG_INFO("Step 6: Entry point memory dump...");
    dumpMemoryAtEntryPoint(imageBuffer, entryPointRVA);
    
    // STEP 7: Apply relocations
    LOG_INFO("");
    LOG_INFO("Step 7: Applying relocations...");
    if (!applyRelocations(imageBuffer, loadAddress)) {
        LOG_WARN("Failed to apply some relocations - execution may fail");
        // Don't return false - we can continue even if relocations fail
    }
    
    // Update entry point after relocations (in case it was relocated)
    entryPoint = loadAddress + entryPointRVA;
    
    LOG_INFO("");
    LOG_INFO("=== PE/COFF Image Loaded Successfully ===");
    oss.str("");
    oss << "Total image size: 0x" << std::hex << imageBuffer.size() << std::dec 
        << " (" << imageBuffer.size() << " bytes)";
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "Final entry point: 0x" << std::hex << entryPoint << std::dec;
    LOG_INFO(oss.str());
    
    return true;
}

void PEParser::dumpMemoryAtEntryPoint(const std::vector<uint8_t>& imageBuffer, uint64_t entryPointRVA) const {
    std::ostringstream oss;
    
    oss << "  Entry point RVA: 0x" << std::hex << entryPointRVA << std::dec;
    LOG_INFO(oss.str());
    
    // Check if entry point is within bounds
    if (entryPointRVA >= imageBuffer.size()) {
        LOG_ERROR("  ERROR: Entry point is beyond image buffer!");
        return;
    }
    
    // Dump 32 bytes (2 IA-64 bundles) at the entry point
    size_t bytesToDump = std::min(static_cast<size_t>(32), imageBuffer.size() - static_cast<size_t>(entryPointRVA));
    
    LOG_INFO("  First " + std::to_string(bytesToDump) + " bytes at entry point:");
    
    // Dump in rows of 16 bytes
    for (size_t offset = 0; offset < bytesToDump; offset += 16) {
        oss.str("");
        oss << "    [0x" << std::hex << std::setfill('0') << std::setw(6) 
            << (entryPointRVA + offset) << "] ";
        
        // Hex dump
        size_t rowSize = std::min(static_cast<size_t>(16), bytesToDump - offset);
        for (size_t i = 0; i < rowSize; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') 
                << static_cast<unsigned int>(imageBuffer[entryPointRVA + offset + i]) << " ";
        }
        
        // Padding for alignment
        for (size_t i = rowSize; i < 16; ++i) {
            oss << "   ";
        }
        
        // ASCII representation
        oss << " |";
        for (size_t i = 0; i < rowSize; ++i) {
            uint8_t byte = imageBuffer[entryPointRVA + offset + i];
            if (byte >= 32 && byte < 127) {
                oss << static_cast<char>(byte);
            } else {
                oss << ".";
            }
        }
        oss << "|";
        
        LOG_INFO(oss.str());
    }
    
    // Special IA-64 bundle analysis
    if (bytesToDump >= 16) {
        LOG_INFO("");
        LOG_INFO("  IA-64 Bundle Analysis (first 16 bytes):");
        
        // Read template field (first 5 bits)
        uint8_t templateField = imageBuffer[entryPointRVA] & 0x1F;
        
        oss.str("");
        oss << "    Template: 0x" << std::hex << static_cast<unsigned int>(templateField) << std::dec;
        LOG_INFO(oss.str());
        
        // Extract each 41-bit instruction slot
        uint64_t bundle[2];
        std::memcpy(bundle, &imageBuffer[entryPointRVA], 16);
        
        // Slot 0: bits 5-45
        uint64_t slot0 = (bundle[0] >> 5) & 0x1FFFFFFFFFFULL;
        oss.str("");
        oss << "    Slot 0: 0x" << std::hex << slot0 << std::dec;
        LOG_INFO(oss.str());
        
        // Slot 1: bits 46-86
        uint64_t slot1 = (bundle[0] >> 46) | ((bundle[1] & 0x7FFFFFF) << 18);
        slot1 &= 0x1FFFFFFFFFFULL;
        oss.str("");
        oss << "    Slot 1: 0x" << std::hex << slot1 << std::dec;
        LOG_INFO(oss.str());
        
        // Slot 2: bits 87-127
        uint64_t slot2 = (bundle[1] >> 23) & 0x1FFFFFFFFFFULL;
        oss.str("");
        oss << "    Slot 2: 0x" << std::hex << slot2 << std::dec;
        LOG_INFO(oss.str());
    }
}

const PESectionInfo* PEParser::findSectionByName(const std::string& name) const {
    for (const auto& section : imageInfo_.sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

bool PEParser::applyRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress) {
    if (!valid_) {
        LOG_ERROR("Cannot apply relocations: PE image not valid");
        return false;
    }
    
    LOG_INFO("");
    LOG_INFO("=== Applying PE Relocations ===");
    
    std::ostringstream oss;
    oss << "Load address: 0x" << std::hex << loadAddress << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "Image base: 0x" << std::hex << imageInfo_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    // Calculate relocation delta
    int64_t delta = static_cast<int64_t>(loadAddress) - static_cast<int64_t>(imageInfo_.imageBase);
    
    oss.str("");
    oss << "Relocation delta: 0x" << std::hex << delta << std::dec;
    LOG_INFO(oss.str());
    
    // Note: Even if delta is 0, we still need to check ELF relocations
    // because they use explicit addends that are independent of delta
    if (delta == 0) {
        LOG_INFO("Delta is 0 (loaded at preferred base address)");
        LOG_INFO("PE base relocations not needed, but checking ELF relocations...");
    }
    
    bool success = true;
    
    // Try PE-style base relocations (.reloc section) - only if delta != 0
    if (delta != 0) {
        LOG_INFO("");
        LOG_INFO("Step 1: Checking for .reloc section (PE base relocations)...");
        if (!applyPEBaseRelocations(imageBuffer, loadAddress)) {
            LOG_WARN("PE base relocations failed or not present");
            success = false;
        }
    } else {
        LOG_INFO("");
        LOG_INFO("Step 1: Skipping .reloc (delta is 0)");
    }
    
    // Try ELF-style relocations (.rela section) - ALWAYS check
    LOG_INFO("");
    LOG_INFO("Step 2: Checking for .rela section (ELF relocations)...");
    if (!applyELFRelocations(imageBuffer, loadAddress)) {
        LOG_WARN("ELF relocations failed or not present");
        success = false;
    }
    
    if (success) {
        LOG_INFO("");
        LOG_INFO("=== Relocations Applied Successfully ===");
    } else {
        LOG_WARN("");
        LOG_WARN("=== Some Relocations Failed ===");
        LOG_WARN("Image may not execute correctly!");
    }
    
    return success;
}

bool PEParser::applyPEBaseRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress) {
    const PESectionInfo* relocSection = findSectionByName(".reloc");
    
    if (!relocSection) {
        LOG_INFO("  .reloc section not found");
        return false;
    }
    
    LOG_INFO("  Found .reloc section");
    
    std::ostringstream oss;
    oss << "    VirtualAddress: 0x" << std::hex << relocSection->virtualAddress << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "    Size: " << relocSection->virtualSize << " bytes";
    LOG_INFO(oss.str());
    
    if (relocSection->virtualSize == 0) {
        LOG_INFO("  .reloc section is empty - no relocations to apply");
        return true;
    }
    
    int64_t delta = static_cast<int64_t>(loadAddress) - static_cast<int64_t>(imageInfo_.imageBase);
    size_t offset = relocSection->virtualAddress;
    size_t endOffset = offset + relocSection->virtualSize;
    int relocationCount = 0;
    
    while (offset < endOffset && offset + sizeof(PEBaseRelocationBlock) <= imageBuffer.size()) {
        PEBaseRelocationBlock block;
        std::memcpy(&block, &imageBuffer[offset], sizeof(PEBaseRelocationBlock));
        
        if (block.sizeOfBlock == 0) {
            break; // End of relocations
        }
        
        oss.str("");
        oss << "  Relocation block at page 0x" << std::hex << block.virtualAddress << std::dec;
        LOG_INFO(oss.str());
        
        size_t entriesSize = block.sizeOfBlock - sizeof(PEBaseRelocationBlock);
        size_t numEntries = entriesSize / 2;
        
        for (size_t i = 0; i < numEntries; ++i) {
            size_t entryOffset = offset + sizeof(PEBaseRelocationBlock) + (i * 2);
            if (entryOffset + 2 > imageBuffer.size()) break;
            
            uint16_t entry;
            std::memcpy(&entry, &imageBuffer[entryOffset], 2);
            
            uint16_t type = entry >> 12;
            uint16_t offsetInPage = entry & 0x0FFF;
            
            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                continue; // Skip padding entries
            }
            
            uint64_t targetRVA = block.virtualAddress + offsetInPage;
            
            if (targetRVA + 8 > imageBuffer.size()) {
                LOG_ERROR("    Relocation target out of bounds!");
                continue;
            }
            
            if (type == IMAGE_REL_BASED_DIR64) {
                uint64_t originalValue;
                std::memcpy(&originalValue, &imageBuffer[targetRVA], 8);
                
                uint64_t newValue = originalValue + delta;
                std::memcpy(&imageBuffer[targetRVA], &newValue, 8);
                
                relocationCount++;
                
                if (relocationCount <= 10) { // Log first 10 for debugging
                    oss.str("");
                    oss << "    [" << relocationCount << "] RVA 0x" << std::hex << targetRVA 
                        << ": 0x" << originalValue << " -> 0x" << newValue << std::dec;
                    LOG_INFO(oss.str());
                }
            }
        }
        
        offset += block.sizeOfBlock;
    }
    
    oss.str("");
    oss << "  Applied " << relocationCount << " PE base relocations";
    LOG_INFO(oss.str());
    
    return relocationCount > 0;
}

bool PEParser::applyELFRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress) {
    const PESectionInfo* relaSection = findSectionByName(".rela");
    
    if (!relaSection) {
        LOG_INFO("  .rela section not found");
        return false;
    }
    
    LOG_INFO("  Found .rela section");
    
    std::ostringstream oss;
    oss << "    VirtualAddress: 0x" << std::hex << relaSection->virtualAddress << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "    Size: " << relaSection->virtualSize << " bytes";
    LOG_INFO(oss.str());
    
    if (relaSection->virtualSize == 0) {
        LOG_INFO("  .rela section is empty");
        return true;
    }
    
    size_t numRelocations = relaSection->virtualSize / sizeof(ELFRelaEntry);
    
    oss.str("");
    oss << "    Number of relocations: " << numRelocations;
    LOG_INFO(oss.str());
    
    int relocationCount = 0;
    int skippedCount = 0;
    
    for (size_t i = 0; i < numRelocations; ++i) {
        size_t entryOffset = relaSection->virtualAddress + (i * sizeof(ELFRelaEntry));
        
        if (entryOffset + sizeof(ELFRelaEntry) > imageBuffer.size()) {
            LOG_ERROR("  Relocation entry out of bounds!");
            break;
        }
        
        ELFRelaEntry rela;
        std::memcpy(&rela, &imageBuffer[entryOffset], sizeof(ELFRelaEntry));
        
        uint32_t type = rela.info & 0xFFFFFFFF;
        uint32_t symbol = rela.info >> 32;
        
        // Skip NONE relocations
        if (type == R_IA64_NONE) {
            skippedCount++;
            continue;
        }
        
        if (rela.offset >= imageBuffer.size()) {
            LOG_ERROR("  Relocation offset out of bounds!");
            continue;
        }
        
        bool applied = false;
        uint64_t originalValue = 0;
        uint64_t newValue = 0;
        
        switch (type) {
            case R_IA64_DIR64LSB:
            case R_IA64_FPTR64LSB:
            {
                if (rela.offset + 8 <= imageBuffer.size()) {
                    std::memcpy(&originalValue, &imageBuffer[rela.offset], 8);
                    // ELF RELA formula: S + A (where S is the base/symbol address)
                    // For position-independent code: newValue = loadAddress + addend
                    newValue = loadAddress + rela.addend;
                    std::memcpy(&imageBuffer[rela.offset], &newValue, 8);
                    applied = true;
                }
                break;
            }
            
            case R_IA64_PCREL64LSB:
            {
                if (rela.offset + 8 <= imageBuffer.size()) {
                    std::memcpy(&originalValue, &imageBuffer[rela.offset], 8);
                    // PC-relative: S + A - P (where P is the place)
                    newValue = loadAddress + rela.addend - rela.offset;
                    std::memcpy(&imageBuffer[rela.offset], &newValue, 8);
                    applied = true;
                }
                break;
            }
            
            case R_IA64_SEGREL64LSB:
            {
                if (rela.offset + 8 <= imageBuffer.size()) {
                    std::memcpy(&originalValue, &imageBuffer[rela.offset], 8);
                    // Segment-relative: S + A - segment base
                    // For flat memory model, this is just the addend
                    newValue = rela.addend;
                    std::memcpy(&imageBuffer[rela.offset], &newValue, 8);
                    applied = true;
                }
                break;
            }
            
            default:
                if (relocationCount < 5) {
                    oss.str("");
                    oss << "  Unknown relocation type: 0x" << std::hex << type << std::dec;
                    LOG_WARN(oss.str());
                }
                skippedCount++;
                break;
        }
        
        if (applied) {
            relocationCount++;
            
            if (relocationCount <= 10) { // Log first 10 for debugging
                oss.str("");
                oss << "  [" << relocationCount << "] Type 0x" << std::hex << type 
                    << " at RVA 0x" << rela.offset 
                    << ": 0x" << originalValue << " -> 0x" << newValue 
                    << " (addend: 0x" << rela.addend << ")" << std::dec;
                LOG_INFO(oss.str());
            }
        }
    }
    
    oss.str("");
    oss << "  Applied " << relocationCount << " ELF relocations";
    LOG_INFO(oss.str());
    
    if (skippedCount > 0) {
        oss.str("");
        oss << "  Skipped " << skippedCount << " relocations (NONE or unsupported)";
        LOG_INFO(oss.str());
    }
    
    return relocationCount > 0;
}

} // namespace guideXOS


