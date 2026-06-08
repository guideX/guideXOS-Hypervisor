#include "ISO9660Parser.h"
#include "IStorageDevice.h"
#include "logger.h"
#include "FATParser.h"
#include "BootStageTrace.h"
#include <cstring>
#include <sstream>

namespace ia64 {

namespace {

#pragma pack(push, 1)
struct MBRPartitionEntry {
    uint8_t status;
    uint8_t chsFirst[3];
    uint8_t partitionType;
    uint8_t chsLast[3];
    uint32_t lbaFirst;
    uint32_t sectorCount;
};

struct GPTHeader {
    uint64_t signature;
    uint32_t revision;
    uint32_t headerSize;
    uint32_t headerCrc32;
    uint32_t reserved;
    uint64_t currentLba;
    uint64_t backupLba;
    uint64_t firstUsableLba;
    uint64_t lastUsableLba;
    uint8_t diskGuid[16];
    uint64_t partitionEntriesLba;
    uint32_t numberOfPartitionEntries;
    uint32_t sizeOfPartitionEntry;
    uint32_t partitionEntriesCrc32;
};

struct GPTPartitionEntry {
    uint8_t partitionTypeGuid[16];
    uint8_t uniquePartitionGuid[16];
    uint64_t firstLba;
    uint64_t lastLba;
    uint64_t attributes;
    uint16_t name[36];
};
#pragma pack(pop)

constexpr uint8_t MBR_PARTITION_EFI = 0xEF;

bool isZeroGuid(const uint8_t* guid, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

bool isEfiSystemPartitionGuid(const uint8_t guid[16]) {
    static constexpr uint8_t espGuid[16] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };
    return std::memcmp(guid, espGuid, sizeof(espGuid)) == 0;
}

std::string trimPathComponent(std::string value) {
    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }
    return value;
}

} // namespace

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
    lastBootMediaDiagnostics_.clear();
    
    if (!device_) {
        LOG_ERROR("No storage device provided");
        return false;
    }

    const StorageDeviceInfo info = device_->getInfo();
    std::ostringstream stage;
    stage << "device=\"" << info.deviceId << "\""
          << " imagePath=\"" << info.imagePath << "\""
          << " sizeBytes=" << info.sizeBytes
          << " blockSize=" << info.blockSize
          << " connected=" << (info.connected ? "true" : "false");
    BootStageTrace::Stage(10, "ISO opened", stage.str());
    
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
                BootStageTrace::Stage(20, "El Torito catalog found",
                                      "catalogLBA=" + std::to_string(bootCatalogLBA_));
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
BootStageTrace::Stage(20, "El Torito catalog found",
                      "catalogLBA=" + std::to_string(catalogLBA));
    
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
            LOG_WARN("  ISO boot catalog is for platform 0x" + 
                    std::to_string(validation->platformID) + 
                    (validation->platformID == 0 ? " (x86/BIOS)" : ""));
            LOG_WARN("  This ISO is NOT designed for EFI boot (IA-64 requires EFI)");
            LOG_INFO("  Scanning for additional entries...");
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
        std::ostringstream ctx;
        ctx << "platform=" << BootStageTrace::Hex(efiBootEntry_.platformID)
            << " loadLBA=" << efiBootEntry_.loadLBA
            << " sectorCount=" << efiBootEntry_.sectorCount
            << " mediaType=" << BootStageTrace::Hex(efiBootEntry_.mediaType);
        BootStageTrace::Event("EL_TORITO_EFI_ENTRY", ctx.str());
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
    std::string resolvedPath;
    static const char* isoPaths[] = {
        "/EFI/BOOT/BOOTIA64.EFI",
        "EFI/BOOT/BOOTIA64.EFI",
        "/BOOTIA64.EFI",
        "BOOTIA64.EFI",
        "/EFI/BOOT/ELILO.EFI",
        "EFI/BOOT/ELILO.EFI",
        "/ELILO.EFI",
        "ELILO.EFI"
    };

    for (const char* path : isoPaths) {
        LOG_INFO("Searching ISO filesystem for: " + std::string(path));
        if (extractFile(path, executableData)) {
            resolvedPath = path;
            lastBootMediaDiagnostics_ = "BOOTIA64.EFI found at path " + resolvedPath + " (ISO9660)";
            BootStageTrace::Stage(50, "BOOTIA64.EFI found",
                                  std::string("path=\"") + path + "\" source=ISO9660");
            return true;
        }
    }

    if (!efiBootEntry_.isBootable) {
        lastBootMediaDiagnostics_ = "No El Torito EFI boot image found and ISO probing failed";
        LOG_WARN(lastBootMediaDiagnostics_);
        return false;
    }

    LOG_INFO("Extracting EFI executable from boot image...");
    std::vector<uint8_t> bootImage;
    int64_t bytesRead = readFile(efiBootEntry_.loadLBA, efiBootEntry_.sectorCount, bootImage);
    if (bytesRead <= 0 || bootImage.empty()) {
        lastBootMediaDiagnostics_ = "EFI boot image found but could not be read";
        LOG_ERROR(lastBootMediaDiagnostics_);
        return false;
    }

    {
        std::ostringstream ctx;
        ctx << "loadLBA=" << efiBootEntry_.loadLBA
            << " sectorCount=" << efiBootEntry_.sectorCount
            << " bytesRead=" << bytesRead;
        BootStageTrace::Stage(30, "EFI boot image extracted", ctx.str());
    }

    static const char* efiPaths[] = {
        "EFI/BOOT/BOOTIA64.EFI",
        "efi/boot/bootia64.efi",
        "EFI\\BOOT\\BOOTIA64.EFI",
        "BOOTIA64.EFI",
        "ELILO.EFI",
        "elilo.efi"
    };

    auto tryFatSource = [&](const uint8_t* fatData,
                            size_t fatSize,
                            const std::string& source,
                            std::string& failureReason) -> bool {
        FATParser fatParser;
        if (!fatParser.parse(fatData, fatSize)) {
            failureReason = "FAT parse failed for source " + source;
            return false;
        }

        BootStageTrace::Stage(40, "FAT image mounted",
                              "source=" + source + " bytes=" + std::to_string(fatSize));

        FATFileInfo fileInfo;
        for (const char* path : efiPaths) {
            if (fatParser.findFile(path, fileInfo) &&
                !fileInfo.isDirectory &&
                fatParser.readFile(fileInfo, executableData)) {
                resolvedPath = path;
                BootStageTrace::Stage(50, "BOOTIA64.EFI found",
                                      std::string("path=\"") + path + "\" source=" + source);
                return true;
            }
        }

        failureReason = "FAT parsed but BOOTIA64.EFI/ELILO.EFI not found in " + source;
        return false;
    };

    std::string fatFailure;
    if (tryFatSource(bootImage.data(), bootImage.size(), "ElToritoBootImage", fatFailure)) {
        lastBootMediaDiagnostics_ = "BOOTIA64.EFI found at path " + resolvedPath + " (El Torito FAT)";
        return true;
    }

    if (bootImage.size() >= 512 &&
        bootImage[510] == 0x55 &&
        bootImage[511] == 0xAA) {
        if (bootImage.size() >= 1024) {
            const GPTHeader* gpt = reinterpret_cast<const GPTHeader*>(bootImage.data() + 512);
            static constexpr uint64_t GPT_SIGNATURE = 0x5452415020494645ULL; // "EFI PART"
            if (gpt->signature == GPT_SIGNATURE && gpt->sizeOfPartitionEntry >= sizeof(GPTPartitionEntry)) {
                const size_t entryTableOffset = static_cast<size_t>(gpt->partitionEntriesLba) * 512;
                for (uint32_t i = 0; i < gpt->numberOfPartitionEntries; ++i) {
                    const size_t entryOffset = entryTableOffset + static_cast<size_t>(i) * gpt->sizeOfPartitionEntry;
                    if (entryOffset + sizeof(GPTPartitionEntry) > bootImage.size()) {
                        break;
                    }

                    const GPTPartitionEntry* entry =
                        reinterpret_cast<const GPTPartitionEntry*>(bootImage.data() + entryOffset);
                    if (isZeroGuid(entry->partitionTypeGuid, sizeof(entry->partitionTypeGuid))) {
                        continue;
                    }
                    if (!isEfiSystemPartitionGuid(entry->partitionTypeGuid)) {
                        continue;
                    }
                    if (entry->lastLba < entry->firstLba) {
                        continue;
                    }

                    const size_t espOffset = static_cast<size_t>(entry->firstLba) * 512;
                    const size_t espSectors = static_cast<size_t>(entry->lastLba - entry->firstLba + 1);
                    const size_t espSize = espSectors * 512;
                    if (espOffset >= bootImage.size()) {
                        continue;
                    }
                    const size_t available = bootImage.size() - espOffset;
                    const size_t probeSize = espSize < available ? espSize : available;
                    if (tryFatSource(bootImage.data() + espOffset, probeSize, "GPT/ESP", fatFailure)) {
                        lastBootMediaDiagnostics_ = "BOOTIA64.EFI found at path " + resolvedPath + " (GPT ESP)";
                        return true;
                    }
                }
            }
        }

        const MBRPartitionEntry* mbr =
            reinterpret_cast<const MBRPartitionEntry*>(bootImage.data() + 446);
        for (int i = 0; i < 4; ++i) {
            const MBRPartitionEntry& entry = mbr[i];
            if (entry.partitionType != MBR_PARTITION_EFI || entry.lbaFirst == 0 || entry.sectorCount == 0) {
                continue;
            }

            const size_t espOffset = static_cast<size_t>(entry.lbaFirst) * 512;
            const size_t espSize = static_cast<size_t>(entry.sectorCount) * 512;
            if (espOffset >= bootImage.size()) {
                continue;
            }
            const size_t available = bootImage.size() - espOffset;
            const size_t probeSize = espSize < available ? espSize : available;
            if (tryFatSource(bootImage.data() + espOffset, probeSize, "MBR/ESP", fatFailure)) {
                lastBootMediaDiagnostics_ = "BOOTIA64.EFI found at path " + resolvedPath + " (MBR ESP)";
                return true;
            }
        }
    }

    lastBootMediaDiagnostics_ = fatFailure.empty()
        ? "EFI boot image found but FAT root could not be parsed"
        : fatFailure;
    LOG_ERROR(lastBootMediaDiagnostics_);
    return false;

    auto tryExtractFromISO = [this, &executableData]() -> bool {
        static const char* isoPaths[] = {
            "/EFI/BOOT/BOOTIA64.EFI",
            "EFI/BOOT/BOOTIA64.EFI",
            "/BOOTIA64.EFI",
            "BOOTIA64.EFI",
            "/EFI/BOOT/ELILO.EFI",
            "EFI/BOOT/ELILO.EFI",
            "/ELILO.EFI",
            "ELILO.EFI"
        };

        for (const char* path : isoPaths) {
            LOG_INFO("Searching ISO filesystem for: " + std::string(path));
            if (extractFile(path, executableData)) {
                LOG_INFO("? EFI executable extracted directly from ISO: " +
                        std::to_string(executableData.size()) + " bytes");
                return true;
            }
        }

        return false;
    };
    
    if (!efiBootEntry_.isBootable) {
        LOG_WARN("No EFI boot entry available, trying direct ISO filesystem lookup");
        return tryExtractFromISO();
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
    {
        std::ostringstream ctx;
        ctx << "loadLBA=" << efiBootEntry_.loadLBA
            << " sectorCount=" << efiBootEntry_.sectorCount
            << " bytesRead=" << bytesRead;
        BootStageTrace::Stage(30, "EFI boot image extracted", ctx.str());
    }
    
    // Parse as FAT filesystem
    FATParser fatParser;
    if (!fatParser.parse(bootImage.data(), bootImage.size())) {
        LOG_ERROR("Failed to parse EFI boot image as FAT filesystem");
        return false;
    }
    
    LOG_INFO("FAT filesystem parsed successfully");
    BootStageTrace::Stage(40, "FAT image mounted",
                          "source=ElToritoBootImage bytes=" + std::to_string(bootImage.size()));
    
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
            std::ostringstream ctx;
            ctx << "path=\"" << path << "\""
                << " fatName=\"" << fileInfo.name << "\""
                << " size=" << fileInfo.size
                << " firstCluster=" << fileInfo.firstCluster;
            BootStageTrace::Stage(50, "BOOTIA64.EFI found", ctx.str());
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
                                uint32_t& lba, uint32_t& fileSize, 
                                int depth, int maxDepth) {
    std::set<uint32_t> visited;
    return searchDirectoryInternal(dirLBA, dirSize, fileName, lba, fileSize, visited, depth, maxDepth);
}

bool ISO9660Parser::searchDirectoryInternal(uint32_t dirLBA, uint32_t dirSize, 
                                           const std::string& fileName,
                                           uint32_t& lba, uint32_t& fileSize,
                                           std::set<uint32_t>& visited,
                                           int depth, int maxDepth) {
if (!pvdValid_ || dirSize == 0) {
    return false;
}
    
// Prevent stack overflow from excessive recursion
if (depth >= maxDepth) {
    LOG_WARN("Maximum directory depth reached (" + std::to_string(maxDepth) + "), stopping recursion");
    return false;
}

// Check if we've already visited this directory LBA
if (visited.find(dirLBA) != visited.end()) {
    return false;
}
visited.insert(dirLBA);
    
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
        
        // Validate record length to prevent buffer overruns
        if (record->length < 33 || offset + record->length > dirData.size()) {
            break;
        }
        
        // Validate file identifier length
        if (record->fileIdentifierLength == 0 || 
            offset + 33 + record->fileIdentifierLength > dirData.size()) {
            offset += record->length;
            continue;
        }
        
        // Extract file name
        std::string recordName(reinterpret_cast<char*>(dirData.data() + offset + 33), 
                               record->fileIdentifierLength);
        
        // ISO9660 often has version suffixes like ";1" - strip them for comparison
        std::string cleanedRecordName = recordName;
        size_t semicolonPos = cleanedRecordName.find(';');
        if (semicolonPos != std::string::npos) {
            cleanedRecordName = cleanedRecordName.substr(0, semicolonPos);
        }
        
        // Remove trailing dots (ISO9660 adds a dot to directories sometimes)
        if (!cleanedRecordName.empty() && cleanedRecordName.back() == '.') {
            cleanedRecordName.pop_back();
        }
        
        // Log every filename found for debugging
        LOG_INFO("    Found entry: '" + recordName + "' (cleaned: '" + cleanedRecordName + "') " + 
                 ((record->fileFlags & 0x02) ? "[DIR]" : "[FILE]") +
                 " at LBA " + std::to_string(record->extentLBA_LE));
        
        // Convert to uppercase for comparison
        std::string recordNameUpper = cleanedRecordName;
        for (char& c : recordNameUpper) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        
        std::string fileNameUpper = fileName;
        for (char& c : fileNameUpper) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        
        // Check if this is a match
        if (recordNameUpper == fileNameUpper) {
            LOG_INFO("    MATCH found for: " + fileName);
            lba = record->extentLBA_LE;
            fileSize = record->dataLength_LE;
            return true;
        }
        
        // If this is a directory, recurse into it
        if ((record->fileFlags & 0x02) && recordName != "." && recordName != "..") {
            // Check for valid directory record before recursing
            if (record->extentLBA_LE > 0 && record->dataLength_LE > 0) {
                if (searchDirectoryInternal(record->extentLBA_LE, record->dataLength_LE, 
                                           fileName, lba, fileSize, visited, depth + 1, maxDepth)) {
                    return true;
                }
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

void ISO9660Parser::listRootDirectory() {
    if (!pvdValid_) {
        LOG_ERROR("ISO not parsed - cannot list root directory");
        return;
    }
    
    LOG_INFO("=== Root Directory Contents ===");
    uint32_t rootLBA = pvd_.rootDirectory.extentLBA_LE;
    uint32_t rootSize = pvd_.rootDirectory.dataLength_LE;
    
    // Read root directory data
    uint32_t sectorsNeeded = (rootSize + blockSize_ - 1) / blockSize_;
    std::vector<uint8_t> dirData;
    if (readFile(rootLBA, sectorsNeeded, dirData) <= 0) {
        LOG_ERROR("Failed to read root directory");
        return;
    }
    
    // Parse directory records
    size_t offset = 0;
    int entryCount = 0;
    while (offset < rootSize && offset < dirData.size()) {
        ISO9660DirectoryRecord* record = 
            reinterpret_cast<ISO9660DirectoryRecord*>(dirData.data() + offset);
        
        // Check for end of directory
        if (record->length == 0) {
            break;
        }
        
        // Validate record
        if (record->length < 33 || offset + record->length > dirData.size()) {
            break;
        }
        
        if (record->fileIdentifierLength > 0 && 
            offset + 33 + record->fileIdentifierLength <= dirData.size()) {
            
            std::string name(reinterpret_cast<char*>(dirData.data() + offset + 33), 
                           record->fileIdentifierLength);
            
            // Clean up ISO9660 version suffixes
            std::string cleanName = name;
            size_t semicolonPos = cleanName.find(';');
            if (semicolonPos != std::string::npos) {
                cleanName = cleanName.substr(0, semicolonPos);
            }
            if (!cleanName.empty() && cleanName.back() == '.') {
                cleanName.pop_back();
            }
            
            std::string typeStr = (record->fileFlags & 0x02) ? "[DIR]" : "[FILE]";
            LOG_INFO("  " + cleanName + " " + typeStr + 
                    " (LBA: " + std::to_string(record->extentLBA_LE) + 
                    ", Size: " + std::to_string(record->dataLength_LE) + ")");
            entryCount++;
        }
        
        offset += record->length;
    }
    
    LOG_INFO("=== Total entries: " + std::to_string(entryCount) + " ===");
}

} // namespace ia64


