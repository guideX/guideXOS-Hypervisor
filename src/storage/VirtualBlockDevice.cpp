#include "VirtualBlockDevice.h"
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ia64 {

// ============================================================================
// Constructor for memory-backed device
// ============================================================================

VirtualBlockDevice::VirtualBlockDevice(const std::string& deviceId,
                                     uint64_t sizeBytes,
                                     uint32_t sectorSize)
    : deviceId_(deviceId)
    , backingPath_()
    , sizeBytes_(sizeBytes)
    , sectorSize_(sectorSize)
    , readOnly_(false)
    , connected_(false)
    , backingType_(BackingType::MEMORY)
    , memoryBuffer_()
    , fileStream_()
    , totalReads_(0)
    , totalWrites_(0)
    , bytesRead_(0)
    , bytesWritten_(0)
{
    // Allocate memory buffer
    memoryBuffer_.resize(sizeBytes, 0);
}

// ============================================================================
// Constructor for file-backed or disk image device
// ============================================================================

VirtualBlockDevice::VirtualBlockDevice(const std::string& deviceId,
                                     const std::string& backingPath,
                                     BackingType backingType,
                                     uint64_t sizeBytes,
                                     bool readOnly,
                                     uint32_t sectorSize)
    : deviceId_(deviceId)
    , backingPath_(backingPath)
    , sizeBytes_(sizeBytes)
    , sectorSize_(sectorSize)
    , readOnly_(readOnly)
    , connected_(false)
    , backingType_(backingType)
    , memoryBuffer_()
    , fileStream_()
    , totalReads_(0)
    , totalWrites_(0)
    , bytesRead_(0)
    , bytesWritten_(0)
{
    // Size will be detected on connect if not specified
}

// ============================================================================
// Destructor
// ============================================================================

VirtualBlockDevice::~VirtualBlockDevice() {
    if (connected_) {
        disconnect();
    }
}

// ============================================================================
// IStorageDevice Interface Implementation
// ============================================================================

StorageDeviceInfo VirtualBlockDevice::getInfo() const {
    StorageDeviceInfo info;
    info.deviceId = deviceId_;
    info.type = (backingType_ == BackingType::MEMORY) 
                ? StorageDeviceType::MEMORY_BACKED 
                : StorageDeviceType::RAW_DISK;
    info.sizeBytes = sizeBytes_;
    info.blockSize = sectorSize_;
    info.readOnly = readOnly_;
    info.imagePath = backingPath_;
    info.connected = connected_;
    return info;
}

int64_t VirtualBlockDevice::readBlocks(uint64_t blockNumber,
                                      uint64_t blockCount,
                                      uint8_t* buffer) {
    return readSectors(blockNumber, blockCount, buffer);
}

int64_t VirtualBlockDevice::writeBlocks(uint64_t blockNumber,
                                       uint64_t blockCount,
                                       const uint8_t* buffer) {
    return writeSectors(blockNumber, blockCount, buffer);
}

bool VirtualBlockDevice::flush() {
    if (!connected_) {
        return false;
    }
    
    if (backingType_ == BackingType::MEMORY) {
        return true;  // Memory is always "flushed"
    }
    
    if (fileStream_.is_open()) {
        fileStream_.flush();
        return fileStream_.good();
    }
    
    return false;
}

bool VirtualBlockDevice::connect() {
    if (connected_) {
        return true;
    }
    
    if (backingType_ == BackingType::MEMORY) {
        // Memory-backed device is always ready
        connected_ = true;
        return true;
    }
    
    // File-backed or disk image
    if (!openBackingStorage()) {
        return false;
    }
    
    // Detect size if not specified
    if (sizeBytes_ == 0) {
        sizeBytes_ = detectFileSize();
    }
    
    connected_ = true;
    return true;
}

void VirtualBlockDevice::disconnect() {
    if (!connected_) {
        return;
    }
    
    if (backingType_ != BackingType::MEMORY) {
        closeBackingStorage();
    }
    
    connected_ = false;
}

std::string VirtualBlockDevice::getStatistics() const {
    std::ostringstream oss;
    oss << "Virtual Block Device Statistics:\n"
        << "  Device ID: " << deviceId_ << "\n"
        << "  Backing Type: " << (backingType_ == BackingType::MEMORY ? "Memory" : 
                                  backingType_ == BackingType::FILE ? "File" : "Disk Image") << "\n"
        << "  Size: " << sizeBytes_ << " bytes\n"
        << "  Sector Size: " << sectorSize_ << " bytes\n"
        << "  Sector Count: " << getSectorCount() << "\n"
        << "  Total Reads: " << totalReads_ << "\n"
        << "  Total Writes: " << totalWrites_ << "\n"
        << "  Bytes Read: " << bytesRead_ << "\n"
        << "  Bytes Written: " << bytesWritten_ << "\n";
    return oss.str();
}

// ============================================================================
// Sector-Level Operations
// ============================================================================

int64_t VirtualBlockDevice::readSectors(uint64_t sectorNumber,
                                       uint64_t sectorCount,
                                       uint8_t* buffer) {
    if (!connected_ || !buffer || sectorCount == 0) {
        return -1;
    }
    
    uint64_t offset = sectorNumber * sectorSize_;
    uint64_t size = sectorCount * sectorSize_;
    
    if (!validateAccess(offset, size)) {
        return -1;
    }
    
    if (backingType_ == BackingType::MEMORY) {
        // Read from memory buffer
        std::memcpy(buffer, memoryBuffer_.data() + offset, size);
    } else {
        // Read from file
        fileStream_.seekg(offset, std::ios::beg);
        if (!fileStream_.good()) {
            return -1;
        }
        
        fileStream_.read(reinterpret_cast<char*>(buffer), size);
        if (!fileStream_.good() && !fileStream_.eof()) {
            return -1;
        }
        
        // Return actual sectors read
        uint64_t bytesRead = fileStream_.gcount();
        sectorCount = bytesRead / sectorSize_;
    }
    
    // Update statistics
    totalReads_++;
    bytesRead_ += sectorCount * sectorSize_;
    
    return sectorCount;
}

int64_t VirtualBlockDevice::writeSectors(uint64_t sectorNumber,
                                        uint64_t sectorCount,
                                        const uint8_t* buffer) {
    if (!connected_ || !buffer || sectorCount == 0 || readOnly_) {
        return -1;
    }
    
    uint64_t offset = sectorNumber * sectorSize_;
    uint64_t size = sectorCount * sectorSize_;
    
    if (!validateAccess(offset, size)) {
        return -1;
    }
    
    if (backingType_ == BackingType::MEMORY) {
        // Write to memory buffer
        std::memcpy(memoryBuffer_.data() + offset, buffer, size);
    } else {
        // Write to file
        fileStream_.seekp(offset, std::ios::beg);
        if (!fileStream_.good()) {
            return -1;
        }
        
        fileStream_.write(reinterpret_cast<const char*>(buffer), size);
        if (!fileStream_.good()) {
            return -1;
        }
    }
    
    // Update statistics
    totalWrites_++;
    bytesWritten_ += sectorCount * sectorSize_;
    
    return sectorCount;
}

bool VirtualBlockDevice::readSector(uint64_t sectorNumber, uint8_t* buffer) {
    return readSectors(sectorNumber, 1, buffer) == 1;
}

bool VirtualBlockDevice::writeSector(uint64_t sectorNumber, const uint8_t* buffer) {
    return writeSectors(sectorNumber, 1, buffer) == 1;
}

// ============================================================================
// Device Management
// ============================================================================

bool VirtualBlockDevice::createDiskImage(const std::string& path,
                                        uint64_t sizeBytes,
                                        uint8_t fillByte) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    
    // Write in chunks to avoid excessive memory usage
    const size_t chunkSize = 1024 * 1024; // 1MB chunks
    std::vector<uint8_t> chunk(chunkSize, fillByte);
    
    uint64_t remaining = sizeBytes;
    while (remaining > 0) {
        size_t writeSize = std::min(static_cast<uint64_t>(chunkSize), remaining);
        file.write(reinterpret_cast<const char*>(chunk.data()), writeSize);
        if (!file.good()) {
            file.close();
            return false;
        }
        remaining -= writeSize;
    }
    
    file.close();
    return true;
}

bool VirtualBlockDevice::format() {
    if (!connected_ || readOnly_) {
        return false;
    }
    
    if (backingType_ == BackingType::MEMORY) {
        std::fill(memoryBuffer_.begin(), memoryBuffer_.end(), 0);
        return true;
    }
    
    // For file-backed, write zeros in chunks
    const size_t chunkSize = 64 * 1024; // 64KB chunks
    std::vector<uint8_t> zeroChunk(chunkSize, 0);
    
    uint64_t remaining = sizeBytes_;
    uint64_t offset = 0;
    
    while (remaining > 0) {
        size_t writeSize = std::min(static_cast<uint64_t>(chunkSize), remaining);
        
        fileStream_.seekp(offset, std::ios::beg);
        fileStream_.write(reinterpret_cast<const char*>(zeroChunk.data()), writeSize);
        
        if (!fileStream_.good()) {
            return false;
        }
        
        offset += writeSize;
        remaining -= writeSize;
    }
    
    fileStream_.flush();
    return true;
}

// ============================================================================
// Helper Methods
// ============================================================================

bool VirtualBlockDevice::openBackingStorage() {
    if (backingType_ == BackingType::MEMORY) {
        return true;
    }
    
    std::ios::openmode mode = std::ios::binary;
    
    if (readOnly_) {
        mode |= std::ios::in;
    } else {
        mode |= std::ios::in | std::ios::out;
    }
    
    fileStream_.open(backingPath_, mode);
    
    // If file doesn't exist and we're not read-only, create it
    if (!fileStream_.is_open() && !readOnly_) {
        // Try creating the file
        std::ofstream createFile(backingPath_, std::ios::binary | std::ios::trunc);
        if (createFile.is_open()) {
            createFile.close();
            fileStream_.open(backingPath_, mode);
        }
    }
    
    return fileStream_.is_open();
}

void VirtualBlockDevice::closeBackingStorage() {
    if (fileStream_.is_open()) {
        fileStream_.close();
    }
}

uint64_t VirtualBlockDevice::detectFileSize() {
    if (!fileStream_.is_open()) {
        return 0;
    }
    
    // Get current position
    std::streampos current = fileStream_.tellg();
    
    // Seek to end
    fileStream_.seekg(0, std::ios::end);
    std::streampos size = fileStream_.tellg();
    
    // Restore position
    fileStream_.seekg(current, std::ios::beg);
    
    return static_cast<uint64_t>(size);
}

bool VirtualBlockDevice::validateAccess(uint64_t offset, uint64_t size) const {
    if (offset + size > sizeBytes_) {
        return false;
    }
    return true;
}

} // namespace ia64
