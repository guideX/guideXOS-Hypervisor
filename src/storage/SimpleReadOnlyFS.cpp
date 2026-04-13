#include "SimpleReadOnlyFS.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace ia64 {

// ============================================================================
// Superblock Implementation
// ============================================================================

SimpleReadOnlyFS::Superblock::Superblock()
    : magic{'S', 'R', 'O', 'F', 'S', '1', '.', '0'}
    , version(VERSION)
    , blockSize(512)
    , totalBlocks(0)
    , dirBlock(1)
    , dirEntries(0)
    , reserved1(0)
    , dataBlock(0)
    , reserved()
{
    std::memset(reserved, 0, sizeof(reserved));
}

bool SimpleReadOnlyFS::Superblock::isValid() const {
    return std::memcmp(magic, MAGIC, 8) == 0 && version == VERSION;
}

void SimpleReadOnlyFS::Superblock::serialize(uint8_t* buffer) const {
    std::memcpy(buffer, magic, 8);
    std::memcpy(buffer + 8, &version, 4);
    std::memcpy(buffer + 12, &blockSize, 4);
    std::memcpy(buffer + 16, &totalBlocks, 8);
    std::memcpy(buffer + 24, &dirBlock, 8);
    std::memcpy(buffer + 32, &dirEntries, 4);
    std::memcpy(buffer + 36, &reserved1, 4);
    std::memcpy(buffer + 40, &dataBlock, 8);
    std::memcpy(buffer + 48, reserved, sizeof(reserved));
}

bool SimpleReadOnlyFS::Superblock::deserialize(const uint8_t* buffer) {
    std::memcpy(magic, buffer, 8);
    std::memcpy(&version, buffer + 8, 4);
    std::memcpy(&blockSize, buffer + 12, 4);
    std::memcpy(&totalBlocks, buffer + 16, 8);
    std::memcpy(&dirBlock, buffer + 24, 8);
    std::memcpy(&dirEntries, buffer + 32, 4);
    std::memcpy(&reserved1, buffer + 36, 4);
    std::memcpy(&dataBlock, buffer + 40, 8);
    std::memcpy(reserved, buffer + 48, sizeof(reserved));
    
    return isValid();
}

// ============================================================================
// Directory Entry Implementation
// ============================================================================

SimpleReadOnlyFS::DirEntry::DirEntry()
    : name()
    , type(0)
    , reserved()
    , size(0)
    , dataBlock(0)
    , reserved2(0)
{
    std::memset(name, 0, sizeof(name));
    std::memset(reserved, 0, sizeof(reserved));
}

void SimpleReadOnlyFS::DirEntry::serialize(uint8_t* buffer) const {
    std::memcpy(buffer, name, MAX_FILENAME_LENGTH);
    buffer[48] = type;
    std::memcpy(buffer + 49, reserved, 3);
    std::memcpy(buffer + 52, &size, 8);
    std::memcpy(buffer + 60, &dataBlock, 4);
}

bool SimpleReadOnlyFS::DirEntry::deserialize(const uint8_t* buffer) {
    std::memcpy(name, buffer, MAX_FILENAME_LENGTH);
    name[MAX_FILENAME_LENGTH - 1] = '\0';  // Ensure null-terminated
    type = buffer[48];
    std::memcpy(reserved, buffer + 49, 3);
    std::memcpy(&size, buffer + 52, 8);
    std::memcpy(&dataBlock, buffer + 60, 4);
    return true;
}

std::string SimpleReadOnlyFS::DirEntry::getName() const {
    return std::string(name);
}

void SimpleReadOnlyFS::DirEntry::setName(const std::string& newName) {
    std::memset(name, 0, sizeof(name));
    size_t copyLen = std::min(newName.size(), static_cast<size_t>(MAX_FILENAME_LENGTH - 1));
    std::memcpy(name, newName.c_str(), copyLen);
}

// ============================================================================
// SimpleReadOnlyFS Implementation
// ============================================================================

SimpleReadOnlyFS::SimpleReadOnlyFS()
    : device_(nullptr)
    , superblock_()
    , directory_()
    , mounted_(false)
{
}

SimpleReadOnlyFS::~SimpleReadOnlyFS() {
    if (mounted_) {
        unmount();
    }
}

// ============================================================================
// IFilesystem Interface Implementation
// ============================================================================

uint64_t SimpleReadOnlyFS::getTotalSize() const {
    if (!mounted_) {
        return 0;
    }
    return superblock_.totalBlocks * superblock_.blockSize;
}

uint32_t SimpleReadOnlyFS::getBlockSize() const {
    return mounted_ ? superblock_.blockSize : 512;
}

bool SimpleReadOnlyFS::mount(std::shared_ptr<IStorageDevice> device) {
    if (mounted_) {
        return true;
    }
    
    if (!device || !device->isConnected()) {
        return false;
    }
    
    device_ = device;
    
    // Read and validate superblock
    if (!readSuperblock()) {
        device_ = nullptr;
        return false;
    }
    
    // Read directory table
    if (!readDirectory()) {
        device_ = nullptr;
        return false;
    }
    
    mounted_ = true;
    return true;
}

void SimpleReadOnlyFS::unmount() {
    if (!mounted_) {
        return;
    }
    
    directory_.clear();
    device_ = nullptr;
    mounted_ = false;
}

bool SimpleReadOnlyFS::exists(const std::string& path) const {
    if (!mounted_) {
        return false;
    }
    
    std::string normalized = normalizePath(path);
    
    // Root always exists
    if (normalized == "/") {
        return true;
    }
    
    return findEntry(normalized) != nullptr;
}

bool SimpleReadOnlyFS::isDirectory(const std::string& path) const {
    if (!mounted_) {
        return false;
    }
    
    std::string normalized = normalizePath(path);
    
    // Root is a directory
    if (normalized == "/") {
        return true;
    }
    
    const DirEntry* entry = findEntry(normalized);
    return entry && entry->type == 1;
}

bool SimpleReadOnlyFS::isFile(const std::string& path) const {
    if (!mounted_) {
        return false;
    }
    
    const DirEntry* entry = findEntry(path);
    return entry && entry->type == 0;
}

bool SimpleReadOnlyFS::getAttributes(const std::string& path, FileAttributes& attrs) const {
    if (!mounted_) {
        return false;
    }
    
    std::string normalized = normalizePath(path);
    
    // Handle root directory
    if (normalized == "/") {
        attrs.name = "/";
        attrs.type = FileType::DIRECTORY;
        attrs.size = 0;
        attrs.permissions = 0555;  // r-xr-xr-x
        return true;
    }
    
    const DirEntry* entry = findEntry(normalized);
    if (!entry) {
        return false;
    }
    
    attrs.name = entry->getName();
    attrs.type = entry->type == 0 ? FileType::REGULAR_FILE : FileType::DIRECTORY;
    attrs.size = entry->size;
    attrs.permissions = entry->type == 0 ? 0444 : 0555;  // Read-only
    
    return true;
}

int64_t SimpleReadOnlyFS::getFileSize(const std::string& path) const {
    if (!mounted_) {
        return -1;
    }
    
    const DirEntry* entry = findEntry(path);
    if (!entry || entry->type != 0) {
        return -1;
    }
    
    return static_cast<int64_t>(entry->size);
}

int64_t SimpleReadOnlyFS::readFile(const std::string& path,
                                   uint8_t* buffer,
                                   uint64_t size,
                                   uint64_t offset) const {
    if (!mounted_ || !buffer || size == 0) {
        return -1;
    }
    
    const DirEntry* entry = findEntry(path);
    if (!entry || entry->type != 0) {
        return -1;
    }
    
    // Check bounds
    if (offset >= entry->size) {
        return 0;
    }
    
    uint64_t bytesToRead = std::min(size, entry->size - offset);
    
    // Calculate block and offset
    uint64_t startBlock = entry->dataBlock;
    uint64_t fileOffset = offset;
    uint64_t blockOffset = fileOffset % superblock_.blockSize;
    uint64_t currentBlock = startBlock + (fileOffset / superblock_.blockSize);
    
    uint64_t bytesRead = 0;
    
    while (bytesRead < bytesToRead) {
        // Read block
        std::vector<uint8_t> blockData(superblock_.blockSize);
        int64_t result = device_->readBlocks(currentBlock, 1, blockData.data());
        
        if (result != 1) {
            return -1;
        }
        
        // Copy data from block
        uint64_t copyOffset = (bytesRead == 0) ? blockOffset : 0;
        uint64_t copySize = std::min(
            superblock_.blockSize - copyOffset,
            bytesToRead - bytesRead
        );
        
        std::memcpy(buffer + bytesRead, blockData.data() + copyOffset, copySize);
        
        bytesRead += copySize;
        currentBlock++;
    }
    
    return static_cast<int64_t>(bytesRead);
}

bool SimpleReadOnlyFS::listDirectory(const std::string& path,
                                    std::vector<DirectoryEntry>& entries) const {
    if (!mounted_) {
        return false;
    }
    
    std::string normalized = normalizePath(path);
    
    // Only root directory is supported in this simple implementation
    if (normalized != "/") {
        return false;
    }
    
    entries.clear();
    
    for (const auto& dirEntry : directory_) {
        if (dirEntry.name[0] == '\0') {
            continue;  // Skip empty entries
        }
        
        DirectoryEntry entry;
        entry.name = dirEntry.getName();
        entry.type = dirEntry.type == 0 ? FileType::REGULAR_FILE : FileType::DIRECTORY;
        entry.size = dirEntry.size;
        entries.push_back(entry);
    }
    
    return true;
}

// ============================================================================
// SimpleReadOnlyFS Specific Methods
// ============================================================================

bool SimpleReadOnlyFS::createFilesystem(std::shared_ptr<IStorageDevice> device,
                                       const std::map<std::string, std::vector<uint8_t>>& files,
                                       uint32_t blockSize) {
    if (!device || !device->isConnected()) {
        return false;
    }
    
    // Create superblock
    Superblock sb;
    sb.blockSize = blockSize;
    sb.dirBlock = 1;  // Directory starts at block 1
    sb.dirEntries = static_cast<uint32_t>(files.size());
    
    // Calculate directory size in blocks
    uint32_t dirBlocks = (sb.dirEntries * DIRENT_SIZE + blockSize - 1) / blockSize;
    sb.dataBlock = sb.dirBlock + dirBlocks;
    
    // Calculate total size needed
    uint64_t dataSize = 0;
    for (const auto& file : files) {
        dataSize += file.second.size();
    }
    uint64_t dataBlocks = (dataSize + blockSize - 1) / blockSize;
    sb.totalBlocks = sb.dataBlock + dataBlocks;
    
    // Write superblock
    std::vector<uint8_t> blockBuffer(blockSize, 0);
    sb.serialize(blockBuffer.data());
    
    if (device->writeBlocks(0, 1, blockBuffer.data()) != 1) {
        return false;
    }
    
    // Write directory entries
    uint32_t currentDataBlock = static_cast<uint32_t>(sb.dataBlock);
    std::vector<DirEntry> dirEntries;
    
    for (const auto& file : files) {
        DirEntry entry;
        entry.setName(file.first);
        entry.type = 0;  // File
        entry.size = file.second.size();
        entry.dataBlock = currentDataBlock;
        
        uint32_t fileBlocks = static_cast<uint32_t>((file.second.size() + blockSize - 1) / blockSize);
        currentDataBlock += fileBlocks;
        
        dirEntries.push_back(entry);
    }
    
    // Write directory table
    uint64_t currentBlock = sb.dirBlock;
    for (size_t i = 0; i < dirEntries.size(); ) {
        std::vector<uint8_t> dirBlock(blockSize, 0);
        size_t entriesInBlock = 0;
        
        while (i < dirEntries.size() && (entriesInBlock + 1) * DIRENT_SIZE <= blockSize) {
            dirEntries[i].serialize(dirBlock.data() + entriesInBlock * DIRENT_SIZE);
            i++;
            entriesInBlock++;
        }
        
        if (device->writeBlocks(currentBlock, 1, dirBlock.data()) != 1) {
            return false;
        }
        
        currentBlock++;
    }
    
    // Write file data
    for (const auto& entry : dirEntries) {
        const auto& fileData = files.at(entry.getName());
        
        uint64_t offset = 0;
        uint64_t block = entry.dataBlock;
        
        while (offset < fileData.size()) {
            std::vector<uint8_t> dataBlock(blockSize, 0);
            uint64_t copySize = std::min(
                static_cast<uint64_t>(blockSize),
                fileData.size() - offset
            );
            
            std::memcpy(dataBlock.data(), fileData.data() + offset, copySize);
            
            if (device->writeBlocks(block, 1, dataBlock.data()) != 1) {
                return false;
            }
            
            offset += copySize;
            block++;
        }
    }
    
    device->flush();
    return true;
}

// ============================================================================
// Helper Methods
// ============================================================================

bool SimpleReadOnlyFS::readSuperblock() {
    if (!device_) {
        return false;
    }
    
    std::vector<uint8_t> buffer(SUPERBLOCK_SIZE);
    
    if (device_->readBlocks(0, 1, buffer.data()) != 1) {
        return false;
    }
    
    return superblock_.deserialize(buffer.data());
}

bool SimpleReadOnlyFS::readDirectory() {
    if (!device_) {
        return false;
    }
    
    directory_.clear();
    
    // Calculate number of blocks for directory
    uint32_t dirBlocks = (superblock_.dirEntries * DIRENT_SIZE + superblock_.blockSize - 1) 
                        / superblock_.blockSize;
    
    // Read directory blocks
    std::vector<uint8_t> dirData(dirBlocks * superblock_.blockSize);
    
    if (device_->readBlocks(superblock_.dirBlock, dirBlocks, dirData.data()) 
        != static_cast<int64_t>(dirBlocks)) {
        return false;
    }
    
    // Parse directory entries
    for (uint32_t i = 0; i < superblock_.dirEntries; ++i) {
        DirEntry entry;
        entry.deserialize(dirData.data() + i * DIRENT_SIZE);
        
        if (entry.name[0] != '\0') {
            directory_.push_back(entry);
        }
    }
    
    return true;
}

const SimpleReadOnlyFS::DirEntry* SimpleReadOnlyFS::findEntry(const std::string& path) const {
    std::string normalized = normalizePath(path);
    
    // Remove leading slash for comparison
    if (!normalized.empty() && normalized[0] == '/') {
        normalized = normalized.substr(1);
    }
    
    if (normalized.empty()) {
        return nullptr;  // Root has no entry
    }
    
    for (const auto& entry : directory_) {
        if (entry.getName() == normalized) {
            return &entry;
        }
    }
    
    return nullptr;
}

std::string SimpleReadOnlyFS::normalizePath(const std::string& path) const {
    if (path.empty() || path == "/") {
        return "/";
    }
    
    std::string result = path;
    
    // Ensure starts with /
    if (result[0] != '/') {
        result = "/" + result;
    }
    
    // Remove trailing slashes
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

} // namespace ia64
