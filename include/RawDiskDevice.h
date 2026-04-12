#pragma once

#include "IStorageDevice.h"
#include <string>
#include <fstream>
#include <vector>
#include <memory>

namespace ia64 {

/**
 * RawDiskDevice - Raw disk image storage device
 * 
 * This class implements block storage backed by a raw disk image file.
 * Supports:
 * - Read/write operations
 * - Read-only mode
 * - Auto-creation of new disk images
 * - File-based or memory-backed storage
 * 
 * Thread Safety:
 * - Not thread-safe (caller must synchronize)
 */
class RawDiskDevice : public IStorageDevice {
public:
    /**
     * Constructor - create from existing file
     * 
     * @param deviceId Device identifier
     * @param imagePath Path to disk image file
     * @param sizeBytes Size in bytes (0 = auto-detect from file)
     * @param readOnly Read-only mode
     * @param blockSize Block size in bytes (default 512)
     */
    RawDiskDevice(const std::string& deviceId,
                  const std::string& imagePath,
                  uint64_t sizeBytes = 0,
                  bool readOnly = false,
                  uint32_t blockSize = 512);
    
    /**
     * Destructor
     */
    ~RawDiskDevice() override;
    
    // ========================================================================
    // IStorageDevice Interface Implementation
    // ========================================================================
    
    StorageDeviceInfo getInfo() const override;
    std::string getDeviceId() const override { return deviceId_; }
    uint64_t getSize() const override { return sizeBytes_; }
    uint32_t getBlockSize() const override { return blockSize_; }
    bool isReadOnly() const override { return readOnly_; }
    bool isConnected() const override { return connected_; }
    
    int64_t readBlocks(uint64_t blockNumber, 
                      uint64_t blockCount,
                      uint8_t* buffer) override;
    
    int64_t writeBlocks(uint64_t blockNumber,
                       uint64_t blockCount,
                       const uint8_t* buffer) override;
    
    bool flush() override;
    
    bool connect() override;
    void disconnect() override;
    
    std::string getStatistics() const override;
    
    // ========================================================================
    // RawDiskDevice Specific Methods
    // ========================================================================
    
    /**
     * Create a new disk image file
     * 
     * @param path Path to new disk image
     * @param sizeBytes Size in bytes
     * @param fillByte Fill byte (default 0)
     * @return True if successful
     */
    static bool createDiskImage(const std::string& path, 
                                uint64_t sizeBytes,
                                uint8_t fillByte = 0);
    
    /**
     * Get file path
     */
    std::string getImagePath() const { return imagePath_; }
    
    /**
     * Get read/write statistics
     */
    uint64_t getTotalReads() const { return totalReads_; }
    uint64_t getTotalWrites() const { return totalWrites_; }
    uint64_t getBytesRead() const { return bytesRead_; }
    uint64_t getBytesWritten() const { return bytesWritten_; }
    
private:
    // Device identification
    std::string deviceId_;
    std::string imagePath_;
    
    // Device parameters
    uint64_t sizeBytes_;
    uint32_t blockSize_;
    bool readOnly_;
    bool connected_;
    
    // File handle
    std::fstream file_;
    
    // Statistics
    uint64_t totalReads_;
    uint64_t totalWrites_;
    uint64_t bytesRead_;
    uint64_t bytesWritten_;
    
    // Helper methods
    bool openFile();
    void closeFile();
    uint64_t detectFileSize();
};

/**
 * MemoryBackedDisk - Memory-backed storage device for testing
 * 
 * This class implements block storage backed by a memory buffer.
 * Useful for testing and temporary storage that doesn't require persistence.
 */
class MemoryBackedDisk : public IStorageDevice {
public:
    /**
     * Constructor
     * 
     * @param deviceId Device identifier
     * @param sizeBytes Size in bytes
     * @param blockSize Block size in bytes (default 512)
     */
    MemoryBackedDisk(const std::string& deviceId,
                     uint64_t sizeBytes,
                     uint32_t blockSize = 512);
    
    ~MemoryBackedDisk() override = default;
    
    // ========================================================================
    // IStorageDevice Interface Implementation
    // ========================================================================
    
    StorageDeviceInfo getInfo() const override;
    std::string getDeviceId() const override { return deviceId_; }
    uint64_t getSize() const override { return buffer_.size(); }
    uint32_t getBlockSize() const override { return blockSize_; }
    bool isReadOnly() const override { return false; }
    bool isConnected() const override { return connected_; }
    
    int64_t readBlocks(uint64_t blockNumber,
                      uint64_t blockCount,
                      uint8_t* buffer) override;
    
    int64_t writeBlocks(uint64_t blockNumber,
                       uint64_t blockCount,
                       const uint8_t* buffer) override;
    
    bool flush() override { return true; }  // Always flushed (in memory)
    
    bool connect() override;
    void disconnect() override;
    
    // ========================================================================
    // MemoryBackedDisk Specific Methods
    // ========================================================================
    
    /**
     * Get direct access to memory buffer
     */
    const uint8_t* getBuffer() const { return buffer_.data(); }
    uint8_t* getBuffer() { return buffer_.data(); }
    
    /**
     * Fill entire disk with a byte value
     */
    void fill(uint8_t value);
    
    /**
     * Clear disk (fill with zeros)
     */
    void clear() { fill(0); }
    
private:
    std::string deviceId_;
    uint32_t blockSize_;
    bool connected_;
    std::vector<uint8_t> buffer_;
};

} // namespace ia64
