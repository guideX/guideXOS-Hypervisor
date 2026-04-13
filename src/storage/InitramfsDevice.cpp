#include "InitramfsDevice.h"
#include "memory.h"
#include <fstream>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace ia64 {

// ============================================================================
// Constructor - from file
// ============================================================================

InitramfsDevice::InitramfsDevice(const std::string& deviceId,
                                 const std::string& imagePath,
                                 uint64_t physicalAddress,
                                 uint32_t blockSize)
    : deviceId_(deviceId)
    , imagePath_(imagePath)
    , blockSize_(blockSize)
    , connected_(false)
    , buffer_()
    , physicalAddress_(physicalAddress != 0 ? physicalAddress : DEFAULT_IA64_ADDRESS)
    , loadedInMemory_(false)
    , totalReads_(0)
    , bytesRead_(0)
{
    if (!imagePath_.empty()) {
        if (!loadFileInternal(imagePath_)) {
            throw std::runtime_error("Failed to load initramfs from: " + imagePath_);
        }
    }
}

// ============================================================================
// Constructor - from memory buffer
// ============================================================================

InitramfsDevice::InitramfsDevice(const std::string& deviceId,
                                 const uint8_t* buffer,
                                 uint64_t size,
                                 uint64_t physicalAddress,
                                 uint32_t blockSize)
    : deviceId_(deviceId)
    , imagePath_()
    , blockSize_(blockSize)
    , connected_(false)
    , buffer_()
    , physicalAddress_(physicalAddress != 0 ? physicalAddress : DEFAULT_IA64_ADDRESS)
    , loadedInMemory_(false)
    , totalReads_(0)
    , bytesRead_(0)
{
    if (buffer != nullptr && size > 0) {
        initializeFromBuffer(buffer, size);
    } else {
        throw std::runtime_error("Invalid buffer or size for initramfs");
    }
}

// ============================================================================
// IStorageDevice Interface Implementation
// ============================================================================

StorageDeviceInfo InitramfsDevice::getInfo() const {
    StorageDeviceInfo info;
    info.deviceId = deviceId_;
    info.type = StorageDeviceType::MEMORY_BACKED;
    info.sizeBytes = buffer_.size();
    info.blockSize = blockSize_;
    info.readOnly = true;
    info.imagePath = imagePath_;
    info.connected = connected_;
    return info;
}

int64_t InitramfsDevice::readBlocks(uint64_t blockNumber,
                                    uint64_t blockCount,
                                    uint8_t* buffer) {
    if (!connected_) {
        return -1;  // Device not connected
    }
    
    if (buffer == nullptr) {
        return -1;  // Invalid buffer
    }
    
    // Calculate byte offset and size
    uint64_t byteOffset = blockNumber * blockSize_;
    uint64_t byteCount = blockCount * blockSize_;
    
    // Check bounds
    if (byteOffset >= buffer_.size()) {
        return 0;  // Read beyond end
    }
    
    // Adjust read size if necessary
    if (byteOffset + byteCount > buffer_.size()) {
        byteCount = buffer_.size() - byteOffset;
        blockCount = (byteCount + blockSize_ - 1) / blockSize_;
    }
    
    // Copy data
    std::memcpy(buffer, buffer_.data() + byteOffset, byteCount);
    
    // Update statistics
    totalReads_++;
    bytesRead_ += byteCount;
    
    return static_cast<int64_t>(blockCount);
}

int64_t InitramfsDevice::writeBlocks(uint64_t blockNumber,
                                     uint64_t blockCount,
                                     const uint8_t* buffer) {
    // Initramfs is read-only
    (void)blockNumber;
    (void)blockCount;
    (void)buffer;
    return -1;  // Write not supported
}

bool InitramfsDevice::connect() {
    if (buffer_.empty()) {
        return false;  // No data to serve
    }
    
    connected_ = true;
    return true;
}

void InitramfsDevice::disconnect() {
    connected_ = false;
    loadedInMemory_ = false;
}

std::string InitramfsDevice::getStatistics() const {
    std::ostringstream oss;
    oss << "InitramfsDevice Statistics:\n";
    oss << "  Device ID: " << deviceId_ << "\n";
    oss << "  Size: " << buffer_.size() << " bytes\n";
    oss << "  Physical Address: 0x" << std::hex << physicalAddress_ << std::dec << "\n";
    oss << "  Format: " << getFormatType() << "\n";
    oss << "  Loaded in Memory: " << (loadedInMemory_ ? "yes" : "no") << "\n";
    oss << "  Total Reads: " << totalReads_ << "\n";
    oss << "  Bytes Read: " << bytesRead_ << "\n";
    return oss.str();
}

// ============================================================================
// InitramfsDevice Specific Methods
// ============================================================================

bool InitramfsDevice::loadIntoMemory(Memory& memory) {
    if (buffer_.empty()) {
        return false;  // No data to load
    }
    
    if (loadedInMemory_) {
        return true;  // Already loaded
    }
    
    // Copy initramfs to VM physical memory
    try {
        for (size_t i = 0; i < buffer_.size(); ++i) {
            memory.write<uint8_t>(physicalAddress_ + i, buffer_[i]);
        }
        
        loadedInMemory_ = true;
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

bool InitramfsDevice::loadFromFile(const std::string& path) {
    imagePath_ = path;
    loadedInMemory_ = false;  // Need to reload into memory
    return loadFileInternal(path);
}

bool InitramfsDevice::validateFormat() const {
    if (buffer_.size() < 6) {
        return false;  // Too small
    }
    
    // Check for CPIO magic numbers
    // ASCII cpio: "070707" or "070701" or "070702"
    if (buffer_.size() >= 6) {
        if (std::memcmp(buffer_.data(), "070707", 6) == 0 ||
            std::memcmp(buffer_.data(), "070701", 6) == 0 ||
            std::memcmp(buffer_.data(), "070702", 6) == 0) {
            return true;  // ASCII cpio
        }
    }
    
    // Check for gzip magic (0x1f, 0x8b)
    if (buffer_.size() >= 2) {
        if (buffer_[0] == 0x1f && buffer_[1] == 0x8b) {
            return true;  // Gzip-compressed (likely cpio.gz)
        }
    }
    
    // Check for XZ magic (0xfd, '7', 'z', 'X', 'Z', 0x00)
    if (buffer_.size() >= 6) {
        if (buffer_[0] == 0xfd && buffer_[1] == '7' && buffer_[2] == 'z' &&
            buffer_[3] == 'X' && buffer_[4] == 'Z' && buffer_[5] == 0x00) {
            return true;  // XZ-compressed
        }
    }
    
    // Check for LZMA magic
    if (buffer_.size() >= 6) {
        if (buffer_[0] == 0x5d && buffer_[1] == 0x00 && buffer_[2] == 0x00) {
            return true;  // LZMA-compressed
        }
    }
    
    return false;
}

std::string InitramfsDevice::getFormatType() const {
    if (buffer_.size() < 2) {
        return "unknown";
    }
    
    // Check for CPIO magic
    if (buffer_.size() >= 6) {
        if (std::memcmp(buffer_.data(), "070707", 6) == 0) {
            return "cpio-old";
        }
        if (std::memcmp(buffer_.data(), "070701", 6) == 0) {
            return "cpio-newc";
        }
        if (std::memcmp(buffer_.data(), "070702", 6) == 0) {
            return "cpio-crc";
        }
    }
    
    // Check for compressed formats
    if (buffer_[0] == 0x1f && buffer_[1] == 0x8b) {
        return "cpio.gz";
    }
    
    if (buffer_.size() >= 6) {
        if (buffer_[0] == 0xfd && buffer_[1] == '7' && buffer_[2] == 'z' &&
            buffer_[3] == 'X' && buffer_[4] == 'Z' && buffer_[5] == 0x00) {
            return "cpio.xz";
        }
    }
    
    if (buffer_.size() >= 3) {
        if (buffer_[0] == 0x5d && buffer_[1] == 0x00 && buffer_[2] == 0x00) {
            return "cpio.lzma";
        }
    }
    
    return "unknown";
}

// ============================================================================
// Private Helper Methods
// ============================================================================

bool InitramfsDevice::loadFileInternal(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    
    // Get file size
    std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    
    file.seekg(0, std::ios::beg);
    
    // Read file into buffer
    buffer_.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer_.data()), size)) {
        buffer_.clear();
        return false;
    }
    
    return true;
}

void InitramfsDevice::initializeFromBuffer(const uint8_t* buffer, uint64_t size) {
    buffer_.resize(size);
    std::memcpy(buffer_.data(), buffer, size);
}

} // namespace ia64
