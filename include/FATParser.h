#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace guideXOS {

// FAT Boot Sector structures
#pragma pack(push, 1)
struct FATBootSector {
    uint8_t  jumpBoot[3];
    char     oemName[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t  numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t  mediaType;
    uint16_t sectorsPerFAT;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    
    // FAT12/16 specific
    uint8_t  driveNumber;
    uint8_t  reserved1;
    uint8_t  bootSignature;
    uint32_t volumeID;
    char     volumeLabel[11];
    char     fileSystemType[8];
};

struct FATDirectoryEntry {
    char     filename[8];
    char     extension[3];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creationTimeTenth;
    uint16_t creationTime;
    uint16_t creationDate;
    uint16_t lastAccessDate;
    uint16_t firstClusterHigh;  // FAT32 only
    uint16_t lastWriteTime;
    uint16_t lastWriteDate;
    uint16_t firstClusterLow;
    uint32_t fileSize;
};
#pragma pack(pop)

// Directory entry attributes
constexpr uint8_t ATTR_READ_ONLY = 0x01;
constexpr uint8_t ATTR_HIDDEN    = 0x02;
constexpr uint8_t ATTR_SYSTEM    = 0x04;
constexpr uint8_t ATTR_VOLUME_ID = 0x08;
constexpr uint8_t ATTR_DIRECTORY = 0x10;
constexpr uint8_t ATTR_ARCHIVE   = 0x20;
constexpr uint8_t ATTR_LONG_NAME = ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID;

struct FATFileInfo {
    std::string name;
    bool isDirectory;
    uint32_t size;
    uint32_t firstCluster;
    uint8_t attributes;
};

class FATParser {
public:
    FATParser();
    ~FATParser();
    
    // Initialize with FAT image data
    bool parse(const uint8_t* data, size_t size);
    
    // File operations
    bool findFile(const std::string& path, FATFileInfo& fileInfo);
    bool readFile(const FATFileInfo& fileInfo, std::vector<uint8_t>& data);
    
    // Directory operations
    bool listDirectory(const std::string& path, std::vector<FATFileInfo>& entries);
    
    // Get filesystem info
    bool isValid() const { return valid_; }
    uint32_t getBytesPerCluster() const;
    
private:
    bool parseBootSector();
    bool parseFAT();
    bool parseRootDirectory();
    
    uint32_t getNextCluster(uint32_t cluster);
    uint32_t getClusterOffset(uint32_t cluster);
    
    bool findFileInDirectory(const std::string& name, uint32_t dirCluster, 
                            FATFileInfo& fileInfo);
    bool readDirectoryEntries(uint32_t cluster, std::vector<FATDirectoryEntry>& entries);
    
    std::string getDOSName(const FATDirectoryEntry& entry);
    bool matchDOSName(const std::string& dosName, const std::string& searchName);
    
    const uint8_t* imageData_;
    size_t imageSize_;
    bool valid_;
    
    FATBootSector bootSector_;
    std::vector<uint8_t> fatTable_;
    
    uint32_t rootDirSector_;
    uint32_t rootDirSectors_;
    uint32_t dataSector_;
    uint32_t totalClusters_;
    
    enum class FATType {
        FAT12,
        FAT16,
        FAT32
    } fatType_;
};

} // namespace guideXOS
