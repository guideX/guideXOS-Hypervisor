#include "FATParser.h"
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <cstring>

using ia64::LOG_ERROR;
using ia64::LOG_INFO;
using ia64::LOG_WARN;

namespace guideXOS {

FATParser::FATParser() 
    : imageData_(nullptr)
    , imageSize_(0)
    , valid_(false)
    , rootDirSector_(0)
    , rootDirSectors_(0)
    , dataSector_(0)
    , totalClusters_(0)
    , fatType_(FATType::FAT12) {
}

FATParser::~FATParser() {
}

bool FATParser::parse(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(FATBootSector)) {
        LOG_ERROR("Invalid FAT image data");
        return false;
    }
    
    imageData_ = data;
    imageSize_ = size;
    
    if (!parseBootSector()) {
        LOG_ERROR("Failed to parse FAT boot sector");
        return false;
    }
    
    if (!parseFAT()) {
        LOG_ERROR("Failed to parse FAT table");
        return false;
    }
    
    valid_ = true;
    LOG_INFO("FAT filesystem parsed successfully");
    LOG_INFO("  Type: FAT" + std::to_string(fatType_ == FATType::FAT12 ? 12 : 
                                            fatType_ == FATType::FAT16 ? 16 : 32));
    LOG_INFO("  Bytes per cluster: " + std::to_string(getBytesPerCluster()));
    LOG_INFO("  Total clusters: " + std::to_string(totalClusters_));
    
    return true;
}

bool FATParser::LooksLikeFAT(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(FATBootSector)) {
        return false;
    }

    const FATBootSector* boot = reinterpret_cast<const FATBootSector*>(data);
    if (boot->bytesPerSector != 512 && boot->bytesPerSector != 1024 &&
        boot->bytesPerSector != 2048 && boot->bytesPerSector != 4096) {
        return false;
    }

    if (boot->sectorsPerCluster == 0 || boot->numFATs == 0) {
        return false;
    }

    return true;
}

std::string FATParser::getSummary() const {
    if (!valid_) {
        return "FAT{invalid}";
    }

    return "FAT{" + std::string(fatType_ == FATType::FAT12 ? "FAT12" :
                                 fatType_ == FATType::FAT16 ? "FAT16" : "FAT32") +
           ", bytesPerSector=" + std::to_string(bootSector_.bytesPerSector) +
           ", sectorsPerCluster=" + std::to_string(bootSector_.sectorsPerCluster) +
           ", rootEntries=" + std::to_string(bootSector_.rootEntryCount) +
           ", totalClusters=" + std::to_string(totalClusters_) + "}";
}

bool FATParser::parseBootSector() {
    std::memcpy(&bootSector_, imageData_, sizeof(FATBootSector));
    
    // Basic validation
    if (bootSector_.bytesPerSector != 512 && bootSector_.bytesPerSector != 1024 &&
        bootSector_.bytesPerSector != 2048 && bootSector_.bytesPerSector != 4096) {
        LOG_ERROR("Invalid bytes per sector: " + std::to_string(bootSector_.bytesPerSector));
        return false;
    }
    
    // Calculate FAT type
    uint32_t totalSectors = bootSector_.totalSectors16 != 0 ? 
                           bootSector_.totalSectors16 : bootSector_.totalSectors32;
    
    rootDirSectors_ = ((bootSector_.rootEntryCount * 32) + 
                      (bootSector_.bytesPerSector - 1)) / bootSector_.bytesPerSector;
    
    uint32_t fatSz = bootSector_.sectorsPerFAT;
    uint32_t dataSectors = totalSectors - (bootSector_.reservedSectors + 
                          (bootSector_.numFATs * fatSz) + rootDirSectors_);
    
    totalClusters_ = dataSectors / bootSector_.sectorsPerCluster;
    
    // Determine FAT type based on cluster count
    if (totalClusters_ < 4085) {
        fatType_ = FATType::FAT12;
    } else if (totalClusters_ < 65525) {
        fatType_ = FATType::FAT16;
    } else {
        fatType_ = FATType::FAT32;
    }
    
    // Calculate important sector numbers
    rootDirSector_ = bootSector_.reservedSectors + (bootSector_.numFATs * fatSz);
    dataSector_ = rootDirSector_ + rootDirSectors_;
    
    return true;
}

std::string FATParser::normalizeSearchComponent(const std::string& value) {
    std::string result;
    for (char ch : value) {
        if (ch == '/' || ch == '\\') {
            continue;
        }
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool FATParser::isEndOfDirectoryMarker(const FATDirectoryEntry& entry) {
    return entry.filename[0] == 0x00;
}

bool FATParser::parseFAT() {
    uint32_t fatSize = bootSector_.sectorsPerFAT * bootSector_.bytesPerSector;
    uint32_t fatOffset = bootSector_.reservedSectors * bootSector_.bytesPerSector;
    
    if (fatOffset + fatSize > imageSize_) {
        LOG_ERROR("FAT extends beyond image size");
        return false;
    }
    
    fatTable_.resize(fatSize);
    std::memcpy(fatTable_.data(), imageData_ + fatOffset, fatSize);
    
    return true;
}

bool FATParser::parseRootDirectory() {
    // Root directory is already accessible via sector calculations
    return true;
}

uint32_t FATParser::getNextCluster(uint32_t cluster) {
    if (cluster < 2 || cluster >= totalClusters_ + 2) {
        return 0xFFFFFFFF; // Invalid cluster
    }
    
    uint32_t nextCluster = 0;
    
    if (fatType_ == FATType::FAT12) {
        uint32_t fatOffset = cluster + (cluster / 2);
        if (fatOffset + 1 >= fatTable_.size()) {
            return 0xFFFFFFFF;
        }
        
        uint16_t fatValue = *reinterpret_cast<uint16_t*>(fatTable_.data() + fatOffset);
        
        if (cluster & 1) {
            nextCluster = fatValue >> 4;
        } else {
            nextCluster = fatValue & 0x0FFF;
        }
        
        if (nextCluster >= 0x0FF8) {
            nextCluster = 0xFFFFFFFF; // End of chain
        }
    } else if (fatType_ == FATType::FAT16) {
        uint32_t fatOffset = cluster * 2;
        if (fatOffset + 1 >= fatTable_.size()) {
            return 0xFFFFFFFF;
        }
        
        nextCluster = *reinterpret_cast<uint16_t*>(fatTable_.data() + fatOffset);
        
        if (nextCluster >= 0xFFF8) {
            nextCluster = 0xFFFFFFFF; // End of chain
        }
    } else {
        // FAT32
        uint32_t fatOffset = cluster * 4;
        if (fatOffset + 3 >= fatTable_.size()) {
            return 0xFFFFFFFF;
        }
        
        nextCluster = *reinterpret_cast<uint32_t*>(fatTable_.data() + fatOffset) & 0x0FFFFFFF;
        
        if (nextCluster >= 0x0FFFFFF8) {
            nextCluster = 0xFFFFFFFF; // End of chain
        }
    }
    
    return nextCluster;
}

uint32_t FATParser::getClusterOffset(uint32_t cluster) {
    if (cluster < 2) {
        return 0;
    }
    
    uint32_t sector = dataSector_ + ((cluster - 2) * bootSector_.sectorsPerCluster);
    return sector * bootSector_.bytesPerSector;
}

uint32_t FATParser::getBytesPerCluster() const {
    return bootSector_.sectorsPerCluster * bootSector_.bytesPerSector;
}

std::string FATParser::getDOSName(const FATDirectoryEntry& entry) {
    std::string name;
    
    // Extract filename (8 chars)
    for (int i = 0; i < 8 && entry.filename[i] != ' '; i++) {
        name += static_cast<char>(entry.filename[i]);
    }
    
    // Add extension if present (3 chars)
    bool hasExt = false;
    for (int i = 0; i < 3; i++) {
        if (entry.extension[i] != ' ') {
            if (!hasExt) {
                name += '.';
                hasExt = true;
            }
            name += static_cast<char>(entry.extension[i]);
        }
    }
    
    return name;
}

std::string FATParser::getLongName(const FATDirectoryEntry& entry) {
    const auto* raw = reinterpret_cast<const uint8_t*>(&entry);
    std::u16string codeUnits;
    static constexpr size_t kOffsets[] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30,
    };

    for (const size_t offset : kOffsets) {
        const uint16_t codeUnit = static_cast<uint16_t>(raw[offset]) |
                                  (static_cast<uint16_t>(raw[offset + 1]) << 8);
        if (codeUnit == 0x0000) {
            break;
        }
        if (codeUnit != 0xFFFF) {
            codeUnits.push_back(static_cast<char16_t>(codeUnit));
        }
    }

    std::string result;
    for (size_t index = 0; index < codeUnits.size(); ++index) {
        uint32_t codePoint = codeUnits[index];
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < codeUnits.size()) {
            const uint32_t low = codeUnits[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }

        if (codePoint <= 0x7F) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    return result;
}

bool FATParser::matchDOSName(const std::string& dosName, const std::string& searchName) {
    // Case-insensitive comparison
    if (dosName.length() != searchName.length()) {
        return false;
    }
    
    for (size_t i = 0; i < dosName.length(); i++) {
        if (std::toupper(dosName[i]) != std::toupper(searchName[i])) {
            return false;
        }
    }
    
    return true;
}

bool FATParser::readDirectoryEntries(uint32_t cluster,
                                     std::vector<DirectoryEntryInfo>& entries) {
    entries.clear();

    auto appendEntries = [&](const FATDirectoryEntry* dirEntries,
                             uint32_t entryCount) {
        std::vector<std::string> longNameParts;
        for (uint32_t i = 0; i < entryCount; ++i) {
            const FATDirectoryEntry& entry = dirEntries[i];
            if (isEndOfDirectoryMarker(entry)) {
                return true;
            }
            if (entry.filename[0] == static_cast<char>(0xE5)) {
                longNameParts.clear();
                continue;
            }
            if (entry.attributes == ATTR_LONG_NAME) {
                // FAT stores the last LFN fragment first. Insert each new
                // fragment at the front to restore the Unicode filename.
                longNameParts.insert(longNameParts.begin(), getLongName(entry));
                continue;
            }

            std::string longName;
            for (const auto& part : longNameParts) {
                longName += part;
            }
            entries.push_back({entry, std::move(longName)});
            longNameParts.clear();
        }
        return false;
    };

    // Handle root directory (cluster 0 means root for FAT12/16)
    if (cluster == 0 && fatType_ != FATType::FAT32) {
        uint32_t rootOffset = rootDirSector_ * bootSector_.bytesPerSector;
        uint32_t rootSize = bootSector_.rootEntryCount * sizeof(FATDirectoryEntry);
        
        if (rootOffset + rootSize > imageSize_) {
            return false;
        }
        
        const auto* dirEntries = reinterpret_cast<const FATDirectoryEntry*>(imageData_ + rootOffset);
        appendEntries(dirEntries, bootSector_.rootEntryCount);
        return true;
    }
    
    // Read cluster chain for subdirectories
    uint32_t currentCluster = cluster;
    uint32_t bytesPerCluster = getBytesPerCluster();
    
    while (currentCluster != 0xFFFFFFFF && currentCluster >= 2) {
        uint32_t offset = getClusterOffset(currentCluster);
        
        if (offset + bytesPerCluster > imageSize_) {
            LOG_ERROR("Cluster offset beyond image size");
            return false;
        }
        
        const auto* dirEntries = reinterpret_cast<const FATDirectoryEntry*>(imageData_ + offset);

        uint32_t entriesPerCluster = bytesPerCluster / sizeof(FATDirectoryEntry);

        if (appendEntries(dirEntries, entriesPerCluster)) {
            return true;
        }

        currentCluster = getNextCluster(currentCluster);
    }
    
    return true;
}

bool FATParser::findFileInDirectory(const std::string& name, uint32_t dirCluster, 
                                    FATFileInfo& fileInfo) {
    std::vector<DirectoryEntryInfo> entries;
    
    if (!readDirectoryEntries(dirCluster, entries)) {
        return false;
    }
    
    for (const auto& entryInfo : entries) {
        const FATDirectoryEntry& entry = entryInfo.entry;
        const std::string dosName = getDOSName(entry);
        const std::string& lookupName = entryInfo.longName.empty()
            ? dosName
            : entryInfo.longName;
        
        if (matchDOSName(lookupName, normalizeSearchComponent(name)) ||
            matchDOSName(dosName, normalizeSearchComponent(name))) {
            fileInfo.name = lookupName;
            fileInfo.isDirectory = (entry.attributes & ATTR_DIRECTORY) != 0;
            fileInfo.size = entry.fileSize;
            fileInfo.firstCluster = entry.firstClusterLow | 
                                   (static_cast<uint32_t>(entry.firstClusterHigh) << 16);
            fileInfo.attributes = entry.attributes;
            return true;
        }
    }
    
    return false;
}

bool FATParser::findFile(const std::string& path, FATFileInfo& fileInfo) {
    if (!valid_) {
        return false;
    }
    
    // Split path into components
    std::vector<std::string> components;
    std::string current;
    
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                components.push_back(normalizeSearchComponent(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        components.push_back(normalizeSearchComponent(current));
    }
    
    if (components.empty()) {
        return false;
    }
    
    // Start from root directory
    uint32_t currentCluster = 0;
    
    for (size_t i = 0; i < components.size(); i++) {
        if (!findFileInDirectory(components[i], currentCluster, fileInfo)) {
            return false;
        }
        
        // If not the last component, must be a directory
        if (i < components.size() - 1) {
            if (!fileInfo.isDirectory) {
                return false;
            }
            currentCluster = fileInfo.firstCluster;
        }
    }
    
    return true;
}

bool FATParser::readFile(const FATFileInfo& fileInfo, std::vector<uint8_t>& data) {
    if (!valid_ || fileInfo.isDirectory) {
        return false;
    }
    
    data.clear();
    data.reserve(fileInfo.size);
    
    uint32_t currentCluster = fileInfo.firstCluster;
    uint32_t bytesRead = 0;
    uint32_t bytesPerCluster = getBytesPerCluster();
    
    while (currentCluster != 0xFFFFFFFF && currentCluster >= 2 && bytesRead < fileInfo.size) {
        uint32_t offset = getClusterOffset(currentCluster);
        uint32_t bytesToRead = std::min(bytesPerCluster, fileInfo.size - bytesRead);
        
        if (offset + bytesToRead > imageSize_) {
            LOG_ERROR("File data extends beyond image size");
            return false;
        }
        
        data.insert(data.end(), imageData_ + offset, imageData_ + offset + bytesToRead);
        bytesRead += bytesToRead;
        
        currentCluster = getNextCluster(currentCluster);
    }
    
    return bytesRead == fileInfo.size;
}

bool FATParser::listDirectory(const std::string& path, std::vector<FATFileInfo>& entries) {
    if (!valid_) {
        return false;
    }
    
    entries.clear();
    
    uint32_t dirCluster = 0;
    
    if (!path.empty() && path != "/" && path != "\\") {
        FATFileInfo dirInfo;
        if (!findFile(path, dirInfo) || !dirInfo.isDirectory) {
            return false;
        }
        dirCluster = dirInfo.firstCluster;
    }
    
    std::vector<DirectoryEntryInfo> dirEntries;
    if (!readDirectoryEntries(dirCluster, dirEntries)) {
        return false;
    }
    
    for (const auto& entryInfo : dirEntries) {
        const FATDirectoryEntry& entry = entryInfo.entry;
        // Skip . and .. entries
        if (entry.filename[0] == '.') {
            continue;
        }
        
        FATFileInfo fileInfo;
        fileInfo.name = entryInfo.longName.empty()
            ? getDOSName(entry)
            : entryInfo.longName;
        fileInfo.isDirectory = (entry.attributes & ATTR_DIRECTORY) != 0;
        fileInfo.size = entry.fileSize;
        fileInfo.firstCluster = entry.firstClusterLow | 
                               (static_cast<uint32_t>(entry.firstClusterHigh) << 16);
        fileInfo.attributes = entry.attributes;
        
        entries.push_back(fileInfo);
    }
    
    return true;
}

} // namespace guideXOS
