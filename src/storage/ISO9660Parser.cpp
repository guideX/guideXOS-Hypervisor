#include "ISO9660Parser.h"
#include "IStorageDevice.h"
#include "logger.h"
#include "FATParser.h"
#include <cstring>

namespace ia64 {

ISO9660Parser::ISO9660Parser(IStorageDevice* device)
    : device_(device)
    , pvdValid_(false)
    , blockSize_(2048)  // Standard CD-ROM block size
    , bootCatalogFound_(false)
    , bootCatalogLBA_(0)
{
    std::memset(&pvd_, 0, sizeof(pvd_));
}

bool ISO9660Parser::readSector(uint32_t lba, uint8_t* buffer, size_t bufferSize) {
    if (!device_ || !buffer) {
        return false;
    }
    
    // Calculate byte offset
    uint64_t offset = static_cast<uint64_t>(lba) * blockSize_;
    
    // Read from storage device
    int64_t bytesRead = device_->readBytes(offset, bufferSize, buffer);
    return bytesRead == static_cast<int64_t>(bufferSize);
}

bool ISO9660Parser::parse() {
    LOG_INFO("Parsing ISO 9660 filesystem...");
    
    if (!device_) {
        LOG_ERROR("No storage device provided");
        return false;
    }
    
    // Read volume descriptors starting at sector 16
    uint8_t sectorBuffer[2048];
    
    for (uint32_t sector = 16; sector < 32; ++sector) {
        if (!readSector(sector, sectorBuffer, sizeof(sectorBuffer))) {
            LOG_ERROR("Failed to read volume descriptor at sector " + std::to_string(sector));
            return false;
        }
        
        uint8_t type = sectorBuffer[0];
        
        // Check for "CD001" identifier
        if (std::memcmp(&sectorBuffer[1], "CD001", 5) != 0) {
            LOG_ERROR("Invalid volume descriptor identifier at sector " + std::to_string(sector));
            continue;
        }
        
        LOG_INFO("Found volume descriptor type " + std::to_string(type) + 
                " at sector " + std::to_string(sector));
        
        if (type == static_cast<uint8_t>(VolumeDescriptorType::PRIMARY_VOLUME)) {
            // Found primary volume descriptor
            std::memcpy(&pvd_, sectorBuffer, sizeof(pvd_));
            pvdValid_ = true;
            
            // Extract volume information
            std::string volumeID(pvd_.volumeIdentifier, 32);
            volumeID.erase(volumeID.find_last_not_of(' ') + 1);
            
            LOG_INFO("Primary Volume Descriptor found:");
            LOG_INFO("  Volume: " + volumeID);
            LOG_INFO("  Block size: " + std::to_string(pvd_.logicalBlockSize_LE));
            LOG_INFO("  Volume size: " + std::to_string(pvd_.volumeSpaceSize_LE) + " blocks");
            
            blockSize_ = pvd_.logicalBlockSize_LE;
        }
        else if (type == static_cast<uint8_t>(VolumeDescriptorType::BOOT_RECORD)) {
            // Found El Torito boot record
            ElToritoBootRecord* bootRecord = reinterpret_cast<ElToritoBootRecord*>(sectorBuffer);
            
            // Verify El Torito signature
            if (std::memcmp(bootRecord->bootSystemIdentifier, "EL TORITO SPECIFICATION", 23) == 0) {
                bootCatalogLBA_ = bootRecord->bootCatalogLBA;
                bootCatalogFound_ = true;
                
                LOG_INFO("El Torito Boot Record found:");
                LOG_INFO("  Boot catalog at LBA: " + std::to_string(bootCatalogLBA_));
            }
        }
        else if (type == static_cast<uint8_t>(VolumeDescriptorType::TERMINATOR)) {
            // End of volume descriptors
            break;
        }
    }
    
    if (!pvdValid_) {
        LOG_ERROR("Primary Volume Descriptor not found");
        return false;
    }
    
    LOG_INFO("ISO 9660 filesystem parsed successfully");
    return true;
}

bool ISO9660Parser::findBootCatalog() {
    if (!bootCatalogFound_) {
        LOG_WARN("El Torito boot record not found, looking for boot catalog...");
        return false;
    }
    
    // Parse boot catalog
    return parseBootCatalog(bootCatalogLBA_);
}

bool ISO9660Parser::parseBootCatalog(uint32_t catalogLBA) {
LOG_INFO("Parsing El Torito boot catalog at LBA " + std::to_string(catalogLBA));
    
uint8_t catalogBuffer[2048];
if (!readSector(catalogLBA, catalogBuffer, sizeof(catalogBuffer))) {
    LOG_ERROR("Failed to read boot catalog");
    return false;
}
    
// Debug: Dump first 256 bytes of boot catalog
LOG_INFO("Boot catalog hex dump (first 256 bytes):");
for (size_t i = 0; i < 256 && i < sizeof(catalogBuffer); i += 32) {
    std::string hexLine = "  [" + std::to_string(i) + "]: ";
    for (size_t j = 0; j < 32 && (i + j) < 256; ++j) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", catalogBuffer[i + j]);
        hexLine += hex;
    }
    LOG_INFO(hexLine);
}
    
// Parse validation entry
ElToritoValidationEntry* validation = 
    reinterpret_cast<ElToritoValidationEntry*>(catalogBuffer);
    
if (validation->headerID != 1) {
    LOG_ERROR("Invalid validation entry header ID: " + std::to_string(validation->headerID));
    return false;
}
    
if (validation->key55 != 0x55 || validation->keyAA != 0xAA) {
    LOG_ERROR("Invalid validation entry key bytes: 0x" + 
             std::to_string(validation->key55) + ", 0x" + std::to_string(validation->keyAA));
    return false;
}
    
LOG_INFO("? Validation entry verified");
LOG_INFO("  Platform ID: 0x" + std::to_string(validation->platformID) + 
         (validation->platformID == 0xEF ? " (EFI)" : 
          validation->platformID == 0 ? " (x86)" : ""));
    
    // Parse default boot entry (follows validation entry)
    ElToritoBootEntry* defaultEntry = 
        reinterpret_cast<ElToritoBootEntry*>(catalogBuffer + 32);
    
    // Debug: Show raw bytes at offset 32
    LOG_INFO("Default boot entry raw bytes:");
    std::string rawBytes = "  [32-47]: ";
    for (int i = 0; i < 16; ++i) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", catalogBuffer[32 + i]);
        rawBytes += hex;
    }
    LOG_INFO(rawBytes);
    LOG_INFO("  bootIndicator offset: " + std::to_string((uint8_t*)&defaultEntry->bootIndicator - catalogBuffer));
    LOG_INFO("  bootIndicator value (raw byte): 0x" + std::to_string((int)catalogBuffer[32]));
    LOG_INFO("  bootIndicator value (struct): 0x" + std::to_string((int)defaultEntry->bootIndicator));
    
    LOG_INFO("Default boot entry:");
    LOG_INFO("  Boot indicator: 0x" + std::to_string((int)defaultEntry->bootIndicator) + 
             (defaultEntry->bootIndicator == 0x88 ? " (Bootable)" : 
              defaultEntry->bootIndicator == 0x00 ? " (Not bootable)" : ""));
    
    if (defaultEntry->bootIndicator == 0x88) {
        LOG_INFO("  Media type: 0x" + std::to_string(defaultEntry->bootMediaType));
        LOG_INFO("  Load LBA: " + std::to_string(defaultEntry->loadLBA));
        LOG_INFO("  Sector count: " + std::to_string(defaultEntry->sectorCount));
        
        // Check if this is EFI boot (platform ID 0xEF from validation entry)
        if (validation->platformID == 0xEF) {
            LOG_INFO("? EFI boot entry detected in default entry");
            
            efiBootEntry_.isBootable = true;
            efiBootEntry_.platformID = validation->platformID;
            efiBootEntry_.loadLBA = defaultEntry->loadLBA;
            efiBootEntry_.sectorCount = defaultEntry->sectorCount > 0 ? 
                                       defaultEntry->sectorCount : 1;
            efiBootEntry_.mediaType = defaultEntry->bootMediaType;
            
            return true;
        } else {
            LOG_INFO("  Default entry is not EFI (platform 0x" + std::to_string(validation->platformID) + 
                    "), scanning for additional entries...");
        }
    }
    
    // Look for additional boot entries
    // El Torito supports multiple entries for different platforms
    // Section headers (0x90=more headers follow, 0x91=last header) contain platform ID at offset +1
    uint8_t currentPlatformID = validation->platformID;
    
    for (size_t offset = 64; offset < sizeof(catalogBuffer); offset += 32) {
        uint8_t* entryBytes = catalogBuffer + offset;
        uint8_t headerByte = entryBytes[0];
        
        LOG_INFO("Entry at offset " + std::to_string(offset) + ": header byte 0x" + 
                std::to_string(headerByte));
        
        // Check for section header (0x90, 0x91)
        if (headerByte == 0x90 || headerByte == 0x91) {
            // Section header format: [0]=0x90/0x91, [1]=platform ID, [2-3]=section entries count
            currentPlatformID = entryBytes[1];
            uint16_t sectionEntries = *reinterpret_cast<uint16_t*>(entryBytes + 2);
            
            LOG_INFO("? Section header found (" + std::string(headerByte == 0x90 ? "more follow" : "final") + ")");
            LOG_INFO("  Platform ID: 0x" + std::to_string(currentPlatformID) + 
                     (currentPlatformID == 0xEF ? " (EFI)" : 
                      currentPlatformID == 0 ? " (x86)" : ""));
            LOG_INFO("  Section entries: " + std::to_string(sectionEntries));
            continue;
        }
        
        // Check for boot entry (0x88=bootable, 0x00=not bootable)
        if (headerByte == 0x88) {
            ElToritoBootEntry* entry = reinterpret_cast<ElToritoBootEntry*>(entryBytes);
            
            LOG_INFO("? Bootable entry found");
            LOG_INFO("  Platform: 0x" + std::to_string(currentPlatformID) + 
                     (currentPlatformID == 0xEF ? " (EFI)" : 
                      currentPlatformID == 0 ? " (x86)" : ""));
            LOG_INFO("  Media type: 0x" + std::to_string(entry->bootMediaType));
            LOG_INFO("  Load LBA: " + std::to_string(entry->loadLBA));
            LOG_INFO("  Sector count: " + std::to_string(entry->sectorCount));
            
            // Check if this is an EFI entry
            if (currentPlatformID == 0xEF && !efiBootEntry_.isBootable) {
                LOG_INFO("??? EFI boot entry found! ???");
                
                efiBootEntry_.isBootable = true;
                efiBootEntry_.platformID = currentPlatformID;
                efiBootEntry_.loadLBA = entry->loadLBA;
                efiBootEntry_.sectorCount = entry->sectorCount > 0 ? 
                                           entry->sectorCount : 1;
                efiBootEntry_.mediaType = entry->bootMediaType;
                // Don't return yet, continue scanning to log all entries
            }
        }
        else if (headerByte == 0x00) {
            // End of catalog or empty entry
            LOG_INFO("End of boot catalog entries");
            break;
        }
    }
    
    if (efiBootEntry_.isBootable) {
        LOG_INFO("? EFI boot entry configured:");
        LOG_INFO("  Platform: 0x" + std::to_string(efiBootEntry_.platformID));
        LOG_INFO("  LBA: " + std::to_string(efiBootEntry_.loadLBA));
        LOG_INFO("  Size: " + std::to_string(efiBootEntry_.sectorCount) + " sectors");
        return true;
    }
    
    LOG_WARN("No EFI boot entry found in El Torito boot catalog");
    LOG_INFO("Will attempt to find EFI bootloader in filesystem instead");
    return true;  // Return true anyway - we can still search filesystem
}

const BootEntryInfo* ISO9660Parser::getEFIBootEntry() const {
    return efiBootEntry_.isBootable ? &efiBootEntry_ : nullptr;
}

int64_t ISO9660Parser::readFile(uint32_t lba, uint32_t sectorCount, 
                                 std::vector<uint8_t>& buffer) {
    if (!device_) {
        return -1;
    }
    
    size_t totalSize = static_cast<size_t>(sectorCount) * blockSize_;
    buffer.resize(totalSize);
    
    uint64_t offset = static_cast<uint64_t>(lba) * blockSize_;
    int64_t bytesRead = device_->readBytes(offset, totalSize, buffer.data());
    
    if (bytesRead > 0) {
        buffer.resize(static_cast<size_t>(bytesRead));
        LOG_INFO("Read " + std::to_string(bytesRead) + " bytes from LBA " + 
                std::to_string(lba));
    }
    
    return bytesRead;
}

bool ISO9660Parser::extractEFIExecutable(std::vector<uint8_t>& executableData) {
    using namespace guideXOS;
    
    executableData.clear();
    
    if (!efiBootEntry_.isBootable) {
        LOG_ERROR("No EFI boot entry available");
        return false;
    }
    
    LOG_INFO("Extracting EFI executable from boot image...");
    
    // Read the EFI boot image (FAT filesystem)
    std::vector<uint8_t> bootImage;
    int64_t bytesRead = readFile(efiBootEntry_.loadLBA, efiBootEntry_.sectorCount, bootImage);
    
    if (bytesRead <= 0 || bootImage.empty()) {
        LOG_ERROR("Failed to read EFI boot image");
        return false;
    }
    
    LOG_INFO("Read " + std::to_string(bytesRead) + " bytes of EFI boot image");
    
    // Parse as FAT filesystem
    FATParser fatParser;
    if (!fatParser.parse(bootImage.data(), bootImage.size())) {
        LOG_ERROR("Failed to parse EFI boot image as FAT filesystem");
        return false;
    }
    
    LOG_INFO("FAT filesystem parsed successfully");
    
    // Try common EFI bootloader paths for IA-64
    const char* efiPaths[] = {
        "EFI/BOOT/BOOTIA64.EFI",
        "efi/boot/bootia64.efi",
        "EFI\\BOOT\\BOOTIA64.EFI",
        "BOOTIA64.EFI",
        "bootia64.efi"
    };
    
    FATFileInfo fileInfo;
    bool found = false;
    
    for (const char* path : efiPaths) {
        LOG_INFO("Searching for: " + std::string(path));
        if (fatParser.findFile(path, fileInfo)) {
            LOG_INFO("? Found EFI executable: " + fileInfo.name);
            LOG_INFO("  Size: " + std::to_string(fileInfo.size) + " bytes");
            found = true;
            break;
        }
    }
    
    if (!found) {
        LOG_ERROR("Could not find EFI bootloader in boot image");
        
        // List root directory to help debug
        std::vector<FATFileInfo> rootEntries;
        if (fatParser.listDirectory("/", rootEntries)) {
            LOG_INFO("Root directory contents:");
            for (const auto& entry : rootEntries) {
                LOG_INFO("  " + entry.name + (entry.isDirectory ? " [DIR]" : ""));
            }
        }
        
        return false;
    }
    
    // Read the EFI executable
    if (!fatParser.readFile(fileInfo, executableData)) {
        LOG_ERROR("Failed to read EFI executable from FAT filesystem");
        return false;
    }
    
    LOG_INFO("? EFI executable extracted successfully: " + 
            std::to_string(executableData.size()) + " bytes");
    
    return true;
}

bool ISO9660Parser::searchDirectory(uint32_t dirLBA, uint32_t dirSize, 
                                    const std::string& fileName, 
                                    uint32_t& lba, uint32_t& fileSize) {
    if (!pvdValid_ || dirSize == 0) {
        return false;
    }
    
    // Read directory data
    uint32_t sectorsNeeded = (dirSize + blockSize_ - 1) / blockSize_;
    std::vector<uint8_t> dirData;
    if (readFile(dirLBA, sectorsNeeded, dirData) <= 0) {
        return false;
    }
    
    // Parse directory records
    size_t offset = 0;
    while (offset < dirSize) {
        if (offset >= dirData.size()) break;
        
        ISO9660DirectoryRecord* record = 
            reinterpret_cast<ISO9660DirectoryRecord*>(dirData.data() + offset);
        
        // Check for end of directory
        if (record->length == 0) {
            break;
        }
        
        // Extract file name
        std::string recordName(reinterpret_cast<char*>(dirData.data() + offset + 33), 
                               record->fileIdentifierLength);
        
        // Convert to uppercase for comparison
        std::string recordNameUpper = recordName;
        for (char& c : recordNameUpper) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        
        std::string fileNameUpper = fileName;
        for (char& c : fileNameUpper) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        
        // Check if this is a match
        if (recordNameUpper == fileNameUpper) {
            lba = record->extentLBA_LE;
            fileSize = record->dataLength_LE;
            return true;
        }
        
        // If this is a directory, recurse into it
        if ((record->fileFlags & 0x02) && recordName != "." && recordName != "..") {
            if (searchDirectory(record->extentLBA_LE, record->dataLength_LE, 
                              fileName, lba, fileSize)) {
                return true;
            }
        }
        
        offset += record->length;
    }
    
    return false;
}

bool ISO9660Parser::findFile(const std::string& path, uint32_t& lba, uint32_t& fileSize) {
    if (!pvdValid_) {
        LOG_ERROR("ISO not parsed");
        return false;
    }
    
    LOG_INFO("Searching for file: " + path);
    
    // Split path into components
    std::vector<std::string> components;
    std::string currentPath = path;
    
    // Remove leading slash if present
    if (!currentPath.empty() && currentPath[0] == '/') {
        currentPath = currentPath.substr(1);
    }
    
    // Split by '/' or '\\'
    size_t pos = 0;
    while ((pos = currentPath.find_first_of("/\\")) != std::string::npos) {
        if (pos > 0) {
            components.push_back(currentPath.substr(0, pos));
        }
        currentPath = currentPath.substr(pos + 1);
    }
    if (!currentPath.empty()) {
        components.push_back(currentPath);
    }
    
    if (components.empty()) {
        LOG_ERROR("Empty path");
        return false;
    }
    
    // Start from root directory
    uint32_t currentLBA = pvd_.rootDirectory.extentLBA_LE;
    uint32_t currentSize = pvd_.rootDirectory.dataLength_LE;
    
    // Navigate through each path component
    for (size_t i = 0; i < components.size(); ++i) {
        const std::string& component = components[i];
        LOG_INFO("  Looking for: " + component);
        
        uint32_t nextLBA = 0;
        uint32_t nextSize = 0;
        
        if (!searchDirectory(currentLBA, currentSize, component, nextLBA, nextSize)) {
            LOG_WARN("  Not found: " + component);
            return false;
        }
        
        LOG_INFO("  Found: " + component + " at LBA " + std::to_string(nextLBA));
        
        currentLBA = nextLBA;
        currentSize = nextSize;
    }
    
    lba = currentLBA;
    fileSize = currentSize;
    LOG_INFO("? File found at LBA " + std::to_string(lba) + 
             ", size: " + std::to_string(fileSize) + " bytes");
    return true;
}

bool ISO9660Parser::extractFile(const std::string& path, std::vector<uint8_t>& fileData) {
    uint32_t lba = 0;
    uint32_t fileSize = 0;
    
    if (!findFile(path, lba, fileSize)) {
        LOG_ERROR("File not found: " + path);
        return false;
    }
    
    // Calculate sectors needed
    uint32_t sectorsNeeded = (fileSize + blockSize_ - 1) / blockSize_;
    
    // Read file data
    int64_t bytesRead = readFile(lba, sectorsNeeded, fileData);
    if (bytesRead <= 0) {
        LOG_ERROR("Failed to read file data");
        return false;
    }
    
    // Trim to actual file size
    if (fileData.size() > fileSize) {
        fileData.resize(fileSize);
    }
    
    LOG_INFO("? File extracted: " + std::to_string(fileData.size()) + " bytes");
    return true;
}

} // namespace ia64
