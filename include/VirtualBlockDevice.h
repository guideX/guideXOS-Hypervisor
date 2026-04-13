#pragma once

#include "IStorageDevice.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <fstream>

namespace ia64 {

/**
 * VirtualBlockDevice - Enhanced block device with sector-level APIs
 * 
 * Provides a flexible virtual block device that supports multiple backing storage types:
 * - Memory buffers (volatile, for testing)
 * - File-backed storage (persistent)
 * - Disk image files (raw, QCOW2, etc.)
 * 
 * Features:
 * - Sector-level read/write operations
 * - Block-level operations (inherited from IStorageDevice)
 * - Multiple backing storage options
 * - Statistics and monitoring
 * - Hot-plug support
 */
class VirtualBlockDevice : public IStorageDevice {
public:
    /**
     * Backing storage type
     */
    enum class BackingType {
        MEMORY,     // In-memory buffer (volatile)
        FILE,       // File-backed storage
        DISK_IMAGE  // Disk image file (raw format)
    };
    
    /**
     * Constructor for memory-backed device
     * 
     * @param deviceId Unique device identifier
     * @param sizeBytes Total device size in bytes
     * @param sectorSize Sector size in bytes (default 512)
     */
    VirtualBlockDevice(const std::string& deviceId,
                      uint64_t sizeBytes,
                      uint32_t sectorSize = 512);
    
    /**
     * Constructor for file-backed or disk image device
     * 
     * @param deviceId Unique device identifier
     * @param backingPath Path to backing file or disk image
     * @param backingType Type of backing storage
     * @param sizeBytes Size in bytes (0 = auto-detect from file)
     * @param readOnly Read-only flag
     * @param sectorSize Sector size in bytes (default 512)
     */
    VirtualBlockDevice(const std::string& deviceId,
                      const std::string& backingPath,
                      BackingType backingType,
                      uint64_t sizeBytes = 0,
                      bool readOnly = false,
                      uint32_t sectorSize = 512);
    
    virtual ~VirtualBlockDevice();
    
    // ========================================================================
    // IStorageDevice Interface Implementation
    // ========================================================================
    
    StorageDeviceInfo getInfo() const override;
    std::string getDeviceId() const override { return deviceId_; }
    uint64_t getSize() const override { return sizeBytes_; }
    uint32_t getBlockSize() const override { return sectorSize_; }
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
    // Sector-Level Operations
    // ========================================================================
    
    /**
     * Read sectors from device
     * 
     * @param sectorNumber Starting sector number
     * @param sectorCount Number of sectors to read
     * @param buffer Output buffer (must be at least sectorCount * sectorSize bytes)
     * @return Number of sectors actually read, or -1 on error
     */
    int64_t readSectors(uint64_t sectorNumber,
                       uint64_t sectorCount,
                       uint8_t* buffer);
    
    /**
     * Write sectors to device
     * 
     * @param sectorNumber Starting sector number
     * @param sectorCount Number of sectors to write
     * @param buffer Input buffer (must be at least sectorCount * sectorSize bytes)
     * @return Number of sectors actually written, or -1 on error
     */
    int64_t writeSectors(uint64_t sectorNumber,
                        uint64_t sectorCount,
                        const uint8_t* buffer);
    
    /**
     * Read single sector (convenience method)
     * 
     * @param sectorNumber Sector number to read
     * @param buffer Output buffer (must be at least sectorSize bytes)
     * @return True if successful
     */
    bool readSector(uint64_t sectorNumber, uint8_t* buffer);
    
    /**
     * Write single sector (convenience method)
     * 
     * @param sectorNumber Sector number to write
     * @param buffer Input buffer (must be at least sectorSize bytes)
     * @return True if successful
     */
    bool writeSector(uint64_t sectorNumber, const uint8_t* buffer);
    
    /**
     * Get sector count
     * @return Total number of sectors
     */
    uint64_t getSectorCount() const {
        return sizeBytes_ / sectorSize_;
    }
    
    /**
     * Get sector size
     * @return Sector size in bytes
     */
    uint32_t getSectorSize() const {
        return sectorSize_;
    }
    
    // ========================================================================
    // Device Management
    // ========================================================================
    
    /**
     * Get backing type
     * @return Backing storage type
     */
    BackingType getBackingType() const { return backingType_; }
    
    /**
     * Get backing path (for file/disk image backed devices)
     * @return Path to backing storage, or empty string for memory-backed
     */
    std::string getBackingPath() const { return backingPath_; }
    
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
     * Format the device with zeros
     * @return True if successful
     */
    bool format();
    
protected:
    // Device identification
    std::string deviceId_;
    std::string backingPath_;
    
    // Device parameters
    uint64_t sizeBytes_;
    uint32_t sectorSize_;
    bool readOnly_;
    bool connected_;
    BackingType backingType_;
    
    // Backing storage
    std::vector<uint8_t> memoryBuffer_;  // For memory-backed storage
    std::fstream fileStream_;             // For file/disk image backed storage
    
    // Statistics
    uint64_t totalReads_;
    uint64_t totalWrites_;
    uint64_t bytesRead_;
    uint64_t bytesWritten_;
    
    // Helper methods
    bool openBackingStorage();
    void closeBackingStorage();
    uint64_t detectFileSize();
    bool validateAccess(uint64_t offset, uint64_t size) const;
};

} // namespace ia64
