#include "ISO9660Parser.h"
#include "IStorageDevice.h"
#include "logger.h"
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
    
    // Parse validation entry
    ElToritoValidationEntry* validation = 
        reinterpret_cast<ElToritoValidationEntry*>(catalogBuffer);
    
    if (validation->headerID != 1) {
        LOG_ERROR("Invalid validation entry header ID");
        return false;
    }
    
    if (validation->key55 != 0x55 || validation->keyAA != 0xAA) {
        LOG_ERROR("Invalid validation entry key bytes");
        return false;
    }
    
    LOG_INFO("Validation entry verified");
    LOG_INFO("  Platform ID: 0x" + std::to_string(validation->platformID));
    
    // Parse default boot entry (follows validation entry)
    ElToritoBootEntry* defaultEntry = 
        reinterpret_cast<ElToritoBootEntry*>(catalogBuffer + 32);
    
    if (defaultEntry->bootIndicator == 0x88) {
        LOG_INFO("Default boot entry found:");
        LOG_INFO("  Bootable: Yes");
        LOG_INFO("  Media type: 0x" + std::to_string(defaultEntry->bootMediaType));
        LOG_INFO("  Load LBA: " + std::to_string(defaultEntry->loadLBA));
        LOG_INFO("  Sector count: " + std::to_string(defaultEntry->sectorCount));
        
        // Check if this is EFI boot (platform ID 0xEF or media type indicates EFI)
        if (validation->platformID == 0xEF || defaultEntry->bootMediaType == 0) {
            LOG_INFO("EFI boot entry detected");
            
            efiBootEntry_.isBootable = true;
            efiBootEntry_.platformID = validation->platformID;
            efiBootEntry_.loadLBA = defaultEntry->loadLBA;
            efiBootEntry_.sectorCount = defaultEntry->sectorCount > 0 ? 
                                       defaultEntry->sectorCount : 1;
            efiBootEntry_.mediaType = defaultEntry->bootMediaType;
            
            return true;
        }
    }
    
    // Look for additional boot entries
    // El Torito supports multiple entries for different platforms
    for (size_t offset = 64; offset < sizeof(catalogBuffer); offset += 32) {
        ElToritoBootEntry* entry = 
            reinterpret_cast<ElToritoBootEntry*>(catalogBuffer + offset);
        
        // Check for section header (0x90, 0x91) or extension (0x44)
        if (entry->bootIndicator == 0x90 || entry->bootIndicator == 0x91) {
            // Section header - indicates more entries follow
            LOG_INFO("Found boot catalog section at offset " + std::to_string(offset));
            continue;
        }
        
        if (entry->bootIndicator == 0x88) {
            // Another bootable entry
            LOG_INFO("Additional boot entry found at offset " + std::to_string(offset));
            LOG_INFO("  Load LBA: " + std::to_string(entry->loadLBA));
            LOG_INFO("  Sector count: " + std::to_string(entry->sectorCount));
            
            // Prefer EFI entries
            if (!efiBootEntry_.isBootable) {
                efiBootEntry_.isBootable = true;
                efiBootEntry_.platformID = validation->platformID;
                efiBootEntry_.loadLBA = entry->loadLBA;
                efiBootEntry_.sectorCount = entry->sectorCount > 0 ? 
                                           entry->sectorCount : 1;
                efiBootEntry_.mediaType = entry->bootMediaType;
            }
        }
        else if (entry->bootIndicator == 0x00) {
            // End of catalog or empty entry
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
    
    LOG_WARN("No EFI boot entry found in catalog");
    return false;
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

} // namespace ia64
