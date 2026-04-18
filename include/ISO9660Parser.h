#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace ia64 {

// Forward declaration
class IStorageDevice;

/**
 * ISO 9660 Filesystem Parser
 * 
 * Parses ISO 9660 CD-ROM filesystem to extract files, particularly
 * the EFI bootloader for IA-64 boot.
 */

// ISO 9660 Volume Descriptor Types
enum class VolumeDescriptorType : uint8_t {
    BOOT_RECORD = 0,
    PRIMARY_VOLUME = 1,
    SUPPLEMENTARY_VOLUME = 2,
    VOLUME_PARTITION = 3,
    TERMINATOR = 255
};

// ISO 9660 Directory Record (simplified)
#pragma pack(push, 1)
struct ISO9660DirectoryRecord {
    uint8_t length;                     // Length of directory record
    uint8_t extendedAttrLength;         // Extended attribute record length
    uint32_t extentLBA_LE;              // Location of extent (LBA) - little endian
    uint32_t extentLBA_BE;              // Location of extent (LBA) - big endian
    uint32_t dataLength_LE;             // Data length - little endian
    uint32_t dataLength_BE;             // Data length - big endian
    uint8_t recordingDateTime[7];       // Recording date and time
    uint8_t fileFlags;                  // File flags
    uint8_t fileUnitSize;               // File unit size
    uint8_t interleaveGap;              // Interleave gap size
    uint16_t volumeSeqNumber_LE;        // Volume sequence number - LE
    uint16_t volumeSeqNumber_BE;        // Volume sequence number - BE
    uint8_t fileIdentifierLength;       // Length of file identifier
    // Followed by: file identifier, padding, system use
};

// ISO 9660 Primary Volume Descriptor
struct ISO9660PrimaryVolumeDescriptor {
    uint8_t type;                       // Volume descriptor type (1)
    char identifier[5];                 // "CD001"
    uint8_t version;                    // Volume descriptor version (1)
    uint8_t unused1;                    // Unused (0)
    char systemIdentifier[32];          // System identifier
    char volumeIdentifier[32];          // Volume identifier
    uint8_t unused2[8];                 // Unused
    uint32_t volumeSpaceSize_LE;        // Volume space size - LE
    uint32_t volumeSpaceSize_BE;        // Volume space size - BE
    uint8_t unused3[32];                // Unused
    uint16_t volumeSetSize_LE;          // Volume set size - LE
    uint16_t volumeSetSize_BE;          // Volume set size - BE
    uint16_t volumeSeqNumber_LE;        // Volume sequence number - LE
    uint16_t volumeSeqNumber_BE;        // Volume sequence number - BE
    uint16_t logicalBlockSize_LE;       // Logical block size - LE
    uint16_t logicalBlockSize_BE;       // Logical block size - BE
    uint32_t pathTableSize_LE;          // Path table size - LE
    uint32_t pathTableSize_BE;          // Path table size - BE
    uint32_t typeLPathTable;            // Type L path table location
    uint32_t optTypeLPathTable;         // Optional type L path table
    uint32_t typeMPathTable;            // Type M path table location
    uint32_t optTypeMPathTable;         // Optional type M path table
    ISO9660DirectoryRecord rootDirectory; // Root directory record (34 bytes)
    uint8_t rootDirPadding[34 - sizeof(ISO9660DirectoryRecord)]; // Padding to 34 bytes
    char volumeSetIdentifier[128];      // Volume set identifier
    char publisherIdentifier[128];      // Publisher identifier
    char dataPreparerIdentifier[128];   // Data preparer identifier
    char applicationIdentifier[128];    // Application identifier
    char copyrightFileIdentifier[37];   // Copyright file identifier
    char abstractFileIdentifier[37];    // Abstract file identifier
    char bibliographicFileIdentifier[37]; // Bibliographic file identifier
    char volumeCreationDateTime[17];    // Volume creation date/time
    char volumeModificationDateTime[17]; // Volume modification date/time
    char volumeExpirationDateTime[17];  // Volume expiration date/time
    char volumeEffectiveDateTime[17];   // Volume effective date/time
    uint8_t fileStructureVersion;       // File structure version (1)
    uint8_t unused4;                    // Unused
    uint8_t applicationUse[512];        // Application use
    uint8_t reserved[653];              // Reserved
};

// El Torito Boot Record
struct ElToritoBootRecord {
    uint8_t type;                       // Boot record indicator (0)
    char identifier[5];                 // "CD001"
    uint8_t version;                    // Version (1)
    char bootSystemIdentifier[32];      // "EL TORITO SPECIFICATION"
    uint8_t unused[32];                 // Unused (0)
    uint32_t bootCatalogLBA;            // Absolute pointer to boot catalog
    uint8_t unused2[1973];              // Unused
};

// El Torito Validation Entry
struct ElToritoValidationEntry {
    uint8_t headerID;                   // Header ID (1)
    uint8_t platformID;                 // Platform ID (0=80x86, 1=PowerPC, 2=Mac, 0xEF=EFI)
    uint16_t reserved;                  // Reserved (0)
    char idString[24];                  // ID string
    uint16_t checksum;                  // Checksum
    uint8_t key55;                      // Key byte 1 (0x55)
    uint8_t keyAA;                      // Key byte 2 (0xAA)
};

// El Torito Initial/Default Entry
struct ElToritoBootEntry {
    uint8_t bootIndicator;              // Bootable (0x88) or not (0x00)
    uint8_t bootMediaType;              // Boot media type
    uint16_t loadSegment;               // Load segment (0 = default 0x7C0)
    uint8_t systemType;                 // System type (partition type)
    uint8_t unused;                     // Unused (0)
    uint16_t sectorCount;               // Sector count (virtual sectors to load)
    uint32_t loadLBA;                   // Load RBA (start address of virtual disk)
    uint8_t unused2[20];                // Unused
};

#pragma pack(pop)

/**
 * Parsed boot entry information
 */
struct BootEntryInfo {
    bool isBootable;
    uint8_t platformID;         // 0xEF for EFI
    uint32_t loadLBA;           // Where bootloader is located
    uint32_t sectorCount;       // Size in sectors
    uint8_t mediaType;          // Media type
    
    BootEntryInfo() 
        : isBootable(false), platformID(0), loadLBA(0), 
          sectorCount(0), mediaType(0) {}
};

/**
 * ISO9660Parser - Parses ISO 9660 filesystem
 */
class ISO9660Parser {
public:
    /**
     * Create parser for ISO storage device
     */
    explicit ISO9660Parser(IStorageDevice* device);
    
    /**
     * Parse the ISO filesystem
     * @return true if successful
     */
    bool parse();
    
    /**
     * Find El Torito boot catalog
     * @return true if found
     */
    bool findBootCatalog();
    
    /**
     * Get EFI boot entry information
     * @return Boot entry info, or nullptr if not found
     */
    const BootEntryInfo* getEFIBootEntry() const;
    
    /**
     * Read file data from ISO
     * @param lba Logical block address
     * @param sectorCount Number of sectors to read
     * @param buffer Output buffer
     * @return Number of bytes read, or -1 on error
     */
    int64_t readFile(uint32_t lba, uint32_t sectorCount, std::vector<uint8_t>& buffer);
    
    /**
     * Get primary volume descriptor
     */
    const ISO9660PrimaryVolumeDescriptor* getPrimaryVolumeDescriptor() const {
        return pvdValid_ ? &pvd_ : nullptr;
    }
    
    /**
     * Check if ISO is valid
     */
    bool isValid() const { return pvdValid_; }
    
    /**
     * Get block size (typically 2048 for CDs)
     */
    uint32_t getBlockSize() const { return blockSize_; }
    
    /**
     * Extract EFI executable from boot image
     * @param executableData Output buffer for executable
     * @return true if successful
     */
    bool extractEFIExecutable(std::vector<uint8_t>& executableData);
    
    /**
     * Find file in ISO 9660 filesystem by path
     * @param path File path (e.g., "/EFI/BOOT/BOOTIA64.EFI" or "EFI/BOOT/BOOTIA64.EFI")
     * @param lba Output: LBA where file is located
     * @param fileSize Output: File size in bytes
     * @return true if file found
     */
    bool findFile(const std::string& path, uint32_t& lba, uint32_t& fileSize);
    
    /**
     * Extract file from ISO filesystem
     * @param path File path
     * @param fileData Output buffer for file data
     * @return true if successful
     */
    bool extractFile(const std::string& path, std::vector<uint8_t>& fileData);

private:
    IStorageDevice* device_;
    ISO9660PrimaryVolumeDescriptor pvd_;
    bool pvdValid_;
    uint32_t blockSize_;
    
    // Boot catalog information
    bool bootCatalogFound_;
    uint32_t bootCatalogLBA_;
    BootEntryInfo efiBootEntry_;
    
    /**
     * Read a sector from the ISO
     */
    bool readSector(uint32_t lba, uint8_t* buffer, size_t bufferSize);
    
    /**
     * Parse El Torito boot catalog
     */
    bool parseBootCatalog(uint32_t catalogLBA);
    
    /**
     * Search for file in directory
     * @param dirLBA Directory location
     * @param dirSize Directory size in bytes
     * @param fileName File name to search for (case-insensitive)
     * @param lba Output: File LBA
     * @param fileSize Output: File size
     * @return true if found
     */
    bool searchDirectory(uint32_t dirLBA, uint32_t dirSize, const std::string& fileName, 
                         uint32_t& lba, uint32_t& fileSize);
};

} // namespace ia64
