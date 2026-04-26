#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace guideXOS {

// PE/COFF structures
#pragma pack(push, 1)
struct DOSHeader {
    uint16_t e_magic;      // Magic number (MZ)
    uint16_t e_cblp;       // Bytes on last page
    uint16_t e_cp;         // Pages in file
    uint16_t e_crlc;       // Relocations
    uint16_t e_cparhdr;    // Size of header in paragraphs
    uint16_t e_minalloc;   // Minimum extra paragraphs needed
    uint16_t e_maxalloc;   // Maximum extra paragraphs needed
    uint16_t e_ss;         // Initial SS value
    uint16_t e_sp;         // Initial SP value
    uint16_t e_csum;       // Checksum
    uint16_t e_ip;         // Initial IP value
    uint16_t e_cs;         // Initial CS value
    uint16_t e_lfarlc;     // File address of relocation table
    uint16_t e_ovno;       // Overlay number
    uint16_t e_res[4];     // Reserved words
    uint16_t e_oemid;      // OEM identifier
    uint16_t e_oeminfo;    // OEM information
    uint16_t e_res2[10];   // Reserved words
    uint32_t e_lfanew;     // File address of new exe header
};

struct PEHeader {
    uint32_t signature;           // PE signature (PE\0\0)
};

struct COFFHeader {
    uint16_t machine;             // Machine type (0x200 for IA-64)
    uint16_t numberOfSections;
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numberOfSymbols;
    uint16_t sizeOfOptionalHeader;
    uint16_t characteristics;
};

struct PEOptionalHeader64 {
    uint16_t magic;                     // 0x20B for PE32+
    uint8_t  majorLinkerVersion;
    uint8_t  minorLinkerVersion;
    uint32_t sizeOfCode;
    uint32_t sizeOfInitializedData;
    uint32_t sizeOfUninitializedData;
    uint32_t addressOfEntryPoint;
    uint32_t baseOfCode;
    uint64_t imageBase;
    uint32_t sectionAlignment;
    uint32_t fileAlignment;
    uint16_t majorOperatingSystemVersion;
    uint16_t minorOperatingSystemVersion;
    uint16_t majorImageVersion;
    uint16_t minorImageVersion;
    uint16_t majorSubsystemVersion;
    uint16_t minorSubsystemVersion;
    uint32_t win32VersionValue;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint32_t checkSum;
    uint16_t subsystem;
    uint16_t dllCharacteristics;
    uint64_t sizeOfStackReserve;
    uint64_t sizeOfStackCommit;
    uint64_t sizeOfHeapReserve;
    uint64_t sizeOfHeapCommit;
    uint32_t loaderFlags;
    uint32_t numberOfRvaAndSizes;
};

struct PEDataDirectory {
    uint32_t virtualAddress;
    uint32_t size;
};

struct PESectionHeader {
    char     name[8];
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;
    uint32_t pointerToRelocations;
    uint32_t pointerToLinenumbers;
    uint16_t numberOfRelocations;
    uint16_t numberOfLinenumbers;
    uint32_t characteristics;
};
#pragma pack(pop)

// Machine types
constexpr uint16_t IMAGE_FILE_MACHINE_IA64 = 0x200;
constexpr uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
constexpr uint16_t IMAGE_FILE_MACHINE_I386 = 0x14C;

// Subsystem types
constexpr uint16_t IMAGE_SUBSYSTEM_EFI_APPLICATION = 10;
constexpr uint16_t IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER = 11;
constexpr uint16_t IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER = 12;

// Section characteristics
constexpr uint32_t IMAGE_SCN_CNT_CODE = 0x00000020;
constexpr uint32_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
constexpr uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
constexpr uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
constexpr uint32_t IMAGE_SCN_MEM_WRITE = 0x80000000;

// PE Base Relocation types
constexpr uint16_t IMAGE_REL_BASED_ABSOLUTE = 0;
constexpr uint16_t IMAGE_REL_BASED_DIR64 = 10;

// ELF Relocation types for IA-64
constexpr uint32_t R_IA64_NONE = 0;
constexpr uint32_t R_IA64_DIR64LSB = 0x27;
constexpr uint32_t R_IA64_FPTR64LSB = 0x47;
constexpr uint32_t R_IA64_PCREL64LSB = 0x4f;
constexpr uint32_t R_IA64_SEGREL64LSB = 0x5f;

// PE Base Relocation Block Header
struct PEBaseRelocationBlock {
    uint32_t virtualAddress;
    uint32_t sizeOfBlock;
};

// ELF RELA Relocation Entry (IA-64)
struct ELFRelaEntry {
    uint64_t offset;      // Location to apply relocation
    uint64_t info;        // Relocation type and symbol index
    int64_t addend;       // Constant addend
};

struct PESectionInfo {
    std::string name;
    uint64_t virtualAddress;
    uint64_t virtualSize;
    uint32_t rawDataOffset;
    uint32_t rawDataSize;
    uint32_t characteristics;
};

struct PEImageInfo {
    uint16_t machine;
    uint16_t subsystem;
    uint64_t imageBase;
    uint64_t entryPoint;
    uint32_t sectionAlignment;
    uint32_t fileAlignment;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint64_t globalPointer;
    bool hasGlobalPointer;
    std::vector<PESectionInfo> sections;
};

class PEParser {
public:
    PEParser();
    ~PEParser();
    
    // Parse PE/COFF executable
    bool parse(const uint8_t* data, size_t size);
    
    // Get image information
    const PEImageInfo& getImageInfo() const { return imageInfo_; }
    
    // Load sections into memory buffer
    bool loadImage(std::vector<uint8_t>& imageBuffer, uint64_t& loadAddress, uint64_t& entryPoint);
    
    // Apply relocations to loaded image
    bool applyRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress);
    
    // Validate for specific architecture
    bool isIA64() const { return imageInfo_.machine == IMAGE_FILE_MACHINE_IA64; }
    bool isEFI() const { 
        return imageInfo_.subsystem >= IMAGE_SUBSYSTEM_EFI_APPLICATION && 
               imageInfo_.subsystem <= IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER; 
    }
    
    bool isValid() const { return valid_; }
    
    
private:
bool parseDOSHeader();
bool parsePEHeader();
bool parseCOFFHeader();
bool parseOptionalHeader();
bool parseSections();
    
// Helper function to dump memory at entry point for debugging
void dumpMemoryAtEntryPoint(const std::vector<uint8_t>& imageBuffer, uint64_t entryPointRVA) const;
    
// Relocation helper functions
bool applyPEBaseRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress);
bool applyELFRelocations(std::vector<uint8_t>& imageBuffer, uint64_t loadAddress);
    
const PESectionInfo* findSectionByName(const std::string& name) const;
    
const uint8_t* imageData_;
size_t imageSize_;
bool valid_;
    
DOSHeader dosHeader_;
PEHeader peHeader_;
COFFHeader coffHeader_;
PEOptionalHeader64 optionalHeader_;
    
PEImageInfo imageInfo_;
};

} // namespace guideXOS

