#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace ia64 {

/**
 * StorageDeviceType - Types of storage devices
 */
enum class StorageDeviceType {
    RAW_DISK,           // Raw disk image
    QCOW2,              // QCOW2 format
    VDI,                // VirtualBox Disk Image
    VMDK,               // VMware Disk
    MEMORY_BACKED,      // Memory-backed (for testing)
    NETWORK_BLOCK       // Network block device (iSCSI, NBD)
};

/**
 * StorageDeviceInfo - Storage device information
 */
struct StorageDeviceInfo {
    std::string deviceId;           // Device identifier
    StorageDeviceType type;         // Device type
    uint64_t sizeBytes;             // Total size in bytes
    uint32_t blockSize;             // Block size in bytes
    bool readOnly;                  // Read-only flag
    std::string imagePath;          // Path to backing file (if applicable)
    bool connected;                 // Device connected status
    
    StorageDeviceInfo()
        : deviceId()
        , type(StorageDeviceType::RAW_DISK)
        , sizeBytes(0)
        , blockSize(512)
        , readOnly(false)
        , imagePath()
        , connected(false) {}
};

/**
 * IStorageDevice - Abstract interface for virtual storage devices
 * 
 * This interface provides block-level access to virtual storage devices.
 * Implementations can back storage with:
 * - Raw disk image files
 * - Advanced formats (QCOW2, VDI, VMDK)
 * - Memory buffers (for testing)
 * - Network block devices
 * 
 * Design Philosophy:
 * - Block-oriented interface (not byte-oriented)
 * - Async I/O support (future)
 * - Metadata queries
 * - Hot-plug support
 * 
 * Thread Safety:
 * - Implementations must be thread-safe for concurrent access
 * - Multiple readers allowed
 * - Exclusive writer or multiple readers
 */
class IStorageDevice {
public:
    virtual ~IStorageDevice() = default;
    
    // ========================================================================
    // Device Information
    // ========================================================================
    
    /**
     * Get device information
     * @return Device info structure
     */
    virtual StorageDeviceInfo getInfo() const = 0;
    
    /**
     * Get device identifier
     * @return Unique device ID
     */
    virtual std::string getDeviceId() const = 0;
    
    /**
     * Get total device size in bytes
     * @return Size in bytes
     */
    virtual uint64_t getSize() const = 0;
    
    /**
     * Get block size in bytes
     * @return Block size (typically 512 or 4096)
     */
    virtual uint32_t getBlockSize() const = 0;
    
    /**
     * Get number of blocks
     * @return Total block count
     */
    virtual uint64_t getBlockCount() const {
        return getSize() / getBlockSize();
    }
    
    /**
     * Check if device is read-only
     * @return True if read-only
     */
    virtual bool isReadOnly() const = 0;
    
    /**
     * Check if device is connected
     * @return True if connected and ready
     */
    virtual bool isConnected() const = 0;
    
    // ========================================================================
    // Block I/O Operations
    // ========================================================================
    
    /**
     * Read blocks from device
     * 
     * @param blockNumber Starting block number
     * @param blockCount Number of blocks to read
     * @param buffer Output buffer (must be at least blockCount * blockSize bytes)
     * @return Number of blocks actually read, or -1 on error
     */
    virtual int64_t readBlocks(uint64_t blockNumber, 
                               uint64_t blockCount, 
                               uint8_t* buffer) = 0;
    
    /**
     * Write blocks to device
     * 
     * @param blockNumber Starting block number
     * @param blockCount Number of blocks to write
     * @param buffer Input buffer (must be at least blockCount * blockSize bytes)
     * @return Number of blocks actually written, or -1 on error
     */
    virtual int64_t writeBlocks(uint64_t blockNumber,
                                uint64_t blockCount,
                                const uint8_t* buffer) = 0;
    
    /**
     * Read single block (convenience method)
     * 
     * @param blockNumber Block number to read
     * @param buffer Output buffer (must be at least blockSize bytes)
     * @return True if successful
     */
    virtual bool readBlock(uint64_t blockNumber, uint8_t* buffer) {
        return readBlocks(blockNumber, 1, buffer) == 1;
    }
    
    /**
     * Write single block (convenience method)
     * 
     * @param blockNumber Block number to write
     * @param buffer Input buffer (must be at least blockSize bytes)
     * @return True if successful
     */
    virtual bool writeBlock(uint64_t blockNumber, const uint8_t* buffer) {
        return writeBlocks(blockNumber, 1, buffer) == 1;
    }
    
    /**
     * Flush pending writes to storage
     * @return True if successful
     */
    virtual bool flush() = 0;
    
    // ========================================================================
    // Byte-Level I/O (Optional - implemented via block I/O)
    // ========================================================================
    
    /**
     * Read bytes from device
     * 
     * @param offset Byte offset from start of device
     * @param size Number of bytes to read
     * @param buffer Output buffer
     * @return Number of bytes actually read, or -1 on error
     */
    virtual int64_t readBytes(uint64_t offset, uint64_t size, uint8_t* buffer);
    
    /**
     * Write bytes to device
     * 
     * @param offset Byte offset from start of device
     * @param size Number of bytes to write
     * @param buffer Input buffer
     * @return Number of bytes actually written, or -1 on error
     */
    virtual int64_t writeBytes(uint64_t offset, uint64_t size, const uint8_t* buffer);
    
    // ========================================================================
    // Device Lifecycle
    // ========================================================================
    
    /**
     * Connect/initialize the device
     * @return True if successful
     */
    virtual bool connect() = 0;
    
    /**
     * Disconnect/cleanup the device
     */
    virtual void disconnect() = 0;
    
    /**
     * Reset device to initial state
     * @return True if successful
     */
    virtual bool reset() {
        disconnect();
        return connect();
    }
    
    // ========================================================================
    // Advanced Features (Optional)
    // ========================================================================
    
    /**
     * Trim/discard blocks (for SSD optimization)
     * 
     * @param blockNumber Starting block number
     * @param blockCount Number of blocks to trim
     * @return True if successful
     */
    virtual bool trimBlocks(uint64_t blockNumber, uint64_t blockCount) {
        // Default: not supported
        return false;
    }
    
    /**
     * Get device statistics
     * @return Statistics string
     */
    virtual std::string getStatistics() const {
        return "Statistics not available";
    }
    
protected:
    IStorageDevice() = default;
    
    // Prevent copying and moving of interface
    IStorageDevice(const IStorageDevice&) = delete;
    IStorageDevice& operator=(const IStorageDevice&) = delete;
    IStorageDevice(IStorageDevice&&) = delete;
    IStorageDevice& operator=(IStorageDevice&&) = delete;
};

/**
 * Default implementation of byte-level I/O using block I/O
 */
inline int64_t IStorageDevice::readBytes(uint64_t offset, uint64_t size, uint8_t* buffer) {
    if (!buffer || size == 0) {
        return 0;
    }
    
    uint32_t blockSize = getBlockSize();
    uint64_t startBlock = offset / blockSize;
    uint64_t endBlock = (offset + size - 1) / blockSize;
    uint64_t numBlocks = endBlock - startBlock + 1;
    
    // Allocate temporary block buffer
    std::vector<uint8_t> blockBuffer(numBlocks * blockSize);
    
    // Read blocks
    int64_t blocksRead = readBlocks(startBlock, numBlocks, blockBuffer.data());
    if (blocksRead <= 0) {
        return -1;
    }
    
    // Calculate offset within first block
    uint64_t blockOffset = offset % blockSize;
    
    // Copy requested bytes
    uint64_t bytesToCopy = std::min(size, static_cast<uint64_t>(blocksRead * blockSize - blockOffset));
    std::memcpy(buffer, blockBuffer.data() + blockOffset, bytesToCopy);
    
    return bytesToCopy;
}

inline int64_t IStorageDevice::writeBytes(uint64_t offset, uint64_t size, const uint8_t* buffer) {
    if (!buffer || size == 0 || isReadOnly()) {
        return 0;
    }
    
    uint32_t blockSize = getBlockSize();
    uint64_t startBlock = offset / blockSize;
    uint64_t endBlock = (offset + size - 1) / blockSize;
    uint64_t numBlocks = endBlock - startBlock + 1;
    
    // Allocate temporary block buffer
    std::vector<uint8_t> blockBuffer(numBlocks * blockSize);
    
    // Read existing blocks if we're doing partial block writes
    uint64_t blockOffset = offset % blockSize;
    bool partialFirst = (blockOffset != 0);
    bool partialLast = ((offset + size) % blockSize != 0);
    
    if (partialFirst || partialLast) {
        int64_t blocksRead = readBlocks(startBlock, numBlocks, blockBuffer.data());
        if (blocksRead <= 0) {
            return -1;
        }
    }
    
    // Merge new data into block buffer
    std::memcpy(blockBuffer.data() + blockOffset, buffer, size);
    
    // Write blocks
    int64_t blocksWritten = writeBlocks(startBlock, numBlocks, blockBuffer.data());
    if (blocksWritten <= 0) {
        return -1;
    }
    
    return size;
}

} // namespace ia64
