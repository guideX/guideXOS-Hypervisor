#include "RawDiskDevice.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace ia64 {

// ============================================================================
// RawDiskDevice Implementation
// ============================================================================

RawDiskDevice::RawDiskDevice(const std::string& deviceId,
                             const std::string& imagePath,
                             uint64_t sizeBytes,
                             bool readOnly,
                             uint32_t blockSize)
    : deviceId_(deviceId)
    , imagePath_(imagePath)
    , sizeBytes_(sizeBytes)
    , blockSize_(blockSize)
    , readOnly_(readOnly)
    , connected_(false)
    , file_()
    , totalReads_(0)
    , totalWrites_(0)
    , bytesRead_(0)
    , bytesWritten_(0) {
}

RawDiskDevice::~RawDiskDevice() {
    disconnect();
}

StorageDeviceInfo RawDiskDevice::getInfo() const {
    StorageDeviceInfo info;
    info.deviceId = deviceId_;
    info.type = StorageDeviceType::RAW_DISK;
    info.sizeBytes = sizeBytes_;
    info.blockSize = blockSize_;
    info.readOnly = readOnly_;
    info.imagePath = imagePath_;
    info.connected = connected_;
    return info;
}

int64_t RawDiskDevice::readBlocks(uint64_t blockNumber,
                                  uint64_t blockCount,
                                  uint8_t* buffer) {
    if (!connected_ || !buffer) {
        return -1;
    }
    
    // Validate block range
    uint64_t totalBlocks = sizeBytes_ / blockSize_;
    if (blockNumber >= totalBlocks) {
        return -1;
    }
    
    // Clamp block count
    uint64_t actualBlockCount = blockCount;
    if (blockNumber + blockCount > totalBlocks) {
        actualBlockCount = totalBlocks - blockNumber;
    }
    
    // Calculate file position
    uint64_t fileOffset = blockNumber * blockSize_;
    uint64_t readSize = actualBlockCount * blockSize_;
    
    try {
        file_.seekg(fileOffset, std::ios::beg);
        if (!file_.good()) {
            std::cerr << "Error seeking to position " << fileOffset << "\n";
            return -1;
        }
        
        file_.read(reinterpret_cast<char*>(buffer), readSize);
        std::streamsize bytesRead = file_.gcount();
        
        // Update statistics
        totalReads_++;
        bytesRead_ += bytesRead;
        
        return bytesRead / blockSize_;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception reading blocks: " << e.what() << "\n";
        return -1;
    }
}

int64_t RawDiskDevice::writeBlocks(uint64_t blockNumber,
                                   uint64_t blockCount,
                                   const uint8_t* buffer) {
    if (!connected_ || !buffer || readOnly_) {
        return -1;
    }
    
    // Validate block range
    uint64_t totalBlocks = sizeBytes_ / blockSize_;
    if (blockNumber >= totalBlocks) {
        return -1;
    }
    
    // Clamp block count
    uint64_t actualBlockCount = blockCount;
    if (blockNumber + blockCount > totalBlocks) {
        actualBlockCount = totalBlocks - blockNumber;
    }
    
    // Calculate file position
    uint64_t fileOffset = blockNumber * blockSize_;
    uint64_t writeSize = actualBlockCount * blockSize_;
    
    try {
        file_.seekp(fileOffset, std::ios::beg);
        if (!file_.good()) {
            std::cerr << "Error seeking to position " << fileOffset << "\n";
            return -1;
        }
        
        file_.write(reinterpret_cast<const char*>(buffer), writeSize);
        if (!file_.good()) {
            std::cerr << "Error writing " << writeSize << " bytes\n";
            return -1;
        }
        
        // Update statistics
        totalWrites_++;
        bytesWritten_ += writeSize;
        
        return actualBlockCount;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception writing blocks: " << e.what() << "\n";
        return -1;
    }
}

bool RawDiskDevice::flush() {
    if (!connected_) {
        return false;
    }
    
    try {
        file_.flush();
        return file_.good();
    } catch (const std::exception& e) {
        std::cerr << "Exception flushing: " << e.what() << "\n";
        return false;
    }
}

bool RawDiskDevice::connect() {
    if (connected_) {
        return true;
    }
    
    if (!openFile()) {
        return false;
    }
    
    // Detect size if not specified
    if (sizeBytes_ == 0) {
        sizeBytes_ = detectFileSize();
        if (sizeBytes_ == 0) {
            closeFile();
            return false;
        }
    }
    
    connected_ = true;
    return true;
}

void RawDiskDevice::disconnect() {
    if (connected_) {
        closeFile();
        connected_ = false;
    }
}

std::string RawDiskDevice::getStatistics() const {
    std::ostringstream oss;
    oss << "RawDiskDevice Statistics:\n";
    oss << "  Device ID: " << deviceId_ << "\n";
    oss << "  Image Path: " << imagePath_ << "\n";
    oss << "  Size: " << (sizeBytes_ / (1024 * 1024)) << " MB\n";
    oss << "  Block Size: " << blockSize_ << " bytes\n";
    oss << "  Total Blocks: " << (sizeBytes_ / blockSize_) << "\n";
    oss << "  Read Operations: " << totalReads_ << "\n";
    oss << "  Write Operations: " << totalWrites_ << "\n";
    oss << "  Bytes Read: " << (bytesRead_ / 1024) << " KB\n";
    oss << "  Bytes Written: " << (bytesWritten_ / 1024) << " KB\n";
    return oss.str();
}

bool RawDiskDevice::createDiskImage(const std::string& path,
                                    uint64_t sizeBytes,
                                    uint8_t fillByte) {
    try {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "Failed to create disk image: " << path << "\n";
            return false;
        }
        
        // Write in chunks to avoid memory issues
        const size_t chunkSize = 1024 * 1024;  // 1 MB chunks
        std::vector<uint8_t> chunk(chunkSize, fillByte);
        
        uint64_t remaining = sizeBytes;
        while (remaining > 0) {
            size_t writeSize = remaining > chunkSize ? chunkSize : static_cast<size_t>(remaining);
            file.write(reinterpret_cast<const char*>(chunk.data()), writeSize);
            if (!file.good()) {
                std::cerr << "Error writing disk image\n";
                return false;
            }
            remaining -= writeSize;
        }
        
        file.close();
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception creating disk image: " << e.what() << "\n";
        return false;
    }
}

bool RawDiskDevice::openFile() {
    try {
        std::ios::openmode mode = std::ios::binary;
        
        if (readOnly_) {
            mode |= std::ios::in;
        } else {
            mode |= std::ios::in | std::ios::out;
        }
        
        file_.open(imagePath_, mode);
        
        if (!file_) {
            std::cerr << "Failed to open disk image: " << imagePath_ << "\n";
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception opening file: " << e.what() << "\n";
        return false;
    }
}

void RawDiskDevice::closeFile() {
    if (file_.is_open()) {
        file_.close();
    }
}

uint64_t RawDiskDevice::detectFileSize() {
    try {
        file_.seekg(0, std::ios::end);
        std::streampos size = file_.tellg();
        file_.seekg(0, std::ios::beg);
        
        return static_cast<uint64_t>(size);
        
    } catch (const std::exception& e) {
        std::cerr << "Exception detecting file size: " << e.what() << "\n";
        return 0;
    }
}

// ============================================================================
// MemoryBackedDisk Implementation
// ============================================================================

MemoryBackedDisk::MemoryBackedDisk(const std::string& deviceId,
                                   uint64_t sizeBytes,
                                   uint32_t blockSize)
    : deviceId_(deviceId)
    , blockSize_(blockSize)
    , connected_(false)
    , buffer_(sizeBytes, 0) {
}

StorageDeviceInfo MemoryBackedDisk::getInfo() const {
    StorageDeviceInfo info;
    info.deviceId = deviceId_;
    info.type = StorageDeviceType::MEMORY_BACKED;
    info.sizeBytes = buffer_.size();
    info.blockSize = blockSize_;
    info.readOnly = false;
    info.imagePath = "<memory>";
    info.connected = connected_;
    return info;
}

int64_t MemoryBackedDisk::readBlocks(uint64_t blockNumber,
                                     uint64_t blockCount,
                                     uint8_t* buffer) {
    if (!connected_ || !buffer) {
        return -1;
    }
    
    // Validate block range
    uint64_t totalBlocks = buffer_.size() / blockSize_;
    if (blockNumber >= totalBlocks) {
        return -1;
    }
    
    // Clamp block count
    uint64_t actualBlockCount = blockCount;
    if (blockNumber + blockCount > totalBlocks) {
        actualBlockCount = totalBlocks - blockNumber;
    }
    
    // Calculate offset
    uint64_t offset = blockNumber * blockSize_;
    uint64_t readSize = actualBlockCount * blockSize_;
    
    // Copy data
    std::memcpy(buffer, buffer_.data() + offset, readSize);
    
    return actualBlockCount;
}

int64_t MemoryBackedDisk::writeBlocks(uint64_t blockNumber,
                                      uint64_t blockCount,
                                      const uint8_t* buffer) {
    if (!connected_ || !buffer) {
        return -1;
    }
    
    // Validate block range
    uint64_t totalBlocks = buffer_.size() / blockSize_;
    if (blockNumber >= totalBlocks) {
        return -1;
    }
    
    // Clamp block count
    uint64_t actualBlockCount = blockCount;
    if (blockNumber + blockCount > totalBlocks) {
        actualBlockCount = totalBlocks - blockNumber;
    }
    
    // Calculate offset
    uint64_t offset = blockNumber * blockSize_;
    uint64_t writeSize = actualBlockCount * blockSize_;
    
    // Copy data
    std::memcpy(buffer_.data() + offset, buffer, writeSize);
    
    return actualBlockCount;
}

bool MemoryBackedDisk::connect() {
    connected_ = true;
    return true;
}

void MemoryBackedDisk::disconnect() {
    connected_ = false;
}

void MemoryBackedDisk::fill(uint8_t value) {
    std::fill(buffer_.begin(), buffer_.end(), value);
}

} // namespace ia64
