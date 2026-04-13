#pragma once

#include "IStorageDevice.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace ia64 {

/**
 * InitramfsDevice - Virtual initramfs (initial ramdisk) storage device
 * 
 * This class implements a special storage device that loads an initramfs image
 * into memory and exposes it to the guest kernel during boot. The initramfs
 * appears as a memory-backed device at a fixed physical address known to the
 * kernel boot protocol.
 * 
 * Key Features:
 * - Loads initramfs from file or memory buffer
 * - Exposes initramfs at a fixed physical address
 * - Provides address/size info to kernel via boot parameters
 * - Read-only access (initramfs is immutable after load)
 * - Integrates with kernel bootstrap process
 * 
 * Boot Protocol:
 * The initramfs address and size are passed to the kernel via:
 * - KernelBootstrapConfig.initramfsAddress (physical address)
 * - KernelBootstrapConfig.initramfsSize (size in bytes)
 * - Optionally via boot parameter structure (platform-dependent)
 * 
 * Memory Layout:
 * The initramfs is loaded at a configurable physical address, typically
 * just after the kernel image. Common addresses:
 * - 0x2000000 (32 MB) - default for IA-64 Linux
 * - 0x4000000 (64 MB) - alternative location
 * 
 * Usage Example:
 * ```
 * // Create initramfs device from file
 * auto initramfs = std::make_unique<InitramfsDevice>(
 *     "initramfs0",
 *     "/path/to/initramfs.img",
 *     0x2000000  // Load at 32 MB
 * );
 * 
 * // Load into VM memory
 * initramfs->loadIntoMemory(memory);
 * 
 * // Configure kernel bootstrap
 * KernelBootstrapConfig config;
 * config.initramfsAddress = initramfs->getPhysicalAddress();
 * config.initramfsSize = initramfs->getSize();
 * ```
 * 
 * Thread Safety:
 * - Not thread-safe (caller must synchronize)
 */
class InitramfsDevice : public IStorageDevice {
public:
    /**
     * Constructor - create from file
     * 
     * @param deviceId Device identifier (e.g., "initramfs0")
     * @param imagePath Path to initramfs image file
     * @param physicalAddress Physical address to load at (0 = default)
     * @param blockSize Block size in bytes (default 4096 for ramdisk)
     */
    InitramfsDevice(const std::string& deviceId,
                    const std::string& imagePath,
                    uint64_t physicalAddress = 0,
                    uint32_t blockSize = 4096);
    
    /**
     * Constructor - create from memory buffer
     * 
     * @param deviceId Device identifier
     * @param buffer Pointer to initramfs data
     * @param size Size of initramfs data in bytes
     * @param physicalAddress Physical address to load at (0 = default)
     * @param blockSize Block size in bytes (default 4096)
     */
    InitramfsDevice(const std::string& deviceId,
                    const uint8_t* buffer,
                    uint64_t size,
                    uint64_t physicalAddress = 0,
                    uint32_t blockSize = 4096);
    
    /**
     * Destructor
     */
    ~InitramfsDevice() override = default;
    
    // ========================================================================
    // IStorageDevice Interface Implementation
    // ========================================================================
    
    StorageDeviceInfo getInfo() const override;
    std::string getDeviceId() const override { return deviceId_; }
    uint64_t getSize() const override { return buffer_.size(); }
    uint32_t getBlockSize() const override { return blockSize_; }
    bool isReadOnly() const override { return true; }  // Always read-only
    bool isConnected() const override { return connected_; }
    
    int64_t readBlocks(uint64_t blockNumber,
                      uint64_t blockCount,
                      uint8_t* buffer) override;
    
    int64_t writeBlocks(uint64_t blockNumber,
                       uint64_t blockCount,
                       const uint8_t* buffer) override;
    
    bool flush() override { return true; }  // No-op (read-only)
    
    bool connect() override;
    void disconnect() override;
    
    std::string getStatistics() const override;
    
    // ========================================================================
    // InitramfsDevice Specific Methods
    // ========================================================================
    
    /**
     * Load initramfs into virtual machine memory
     * 
     * Copies the initramfs image to the configured physical address in the
     * VM's memory space. This must be called before starting the kernel.
     * 
     * @param memory VM memory system
     * @return True if successful
     */
    bool loadIntoMemory(class Memory& memory);
    
    /**
     * Get physical address where initramfs is loaded
     * 
     * @return Physical address in VM memory space
     */
    uint64_t getPhysicalAddress() const { return physicalAddress_; }
    
    /**
     * Set physical address for initramfs
     * 
     * Must be called before loadIntoMemory().
     * 
     * @param address Physical address
     */
    void setPhysicalAddress(uint64_t address) { physicalAddress_ = address; }
    
    /**
     * Get image file path (if loaded from file)
     * 
     * @return Image path, or empty string if loaded from memory
     */
    std::string getImagePath() const { return imagePath_; }
    
    /**
     * Check if initramfs has been loaded into VM memory
     * 
     * @return True if loaded
     */
    bool isLoadedInMemory() const { return loadedInMemory_; }
    
    /**
     * Get direct access to initramfs buffer
     * 
     * @return Pointer to initramfs data
     */
    const uint8_t* getBuffer() const { return buffer_.data(); }
    
    /**
     * Get read statistics
     */
    uint64_t getTotalReads() const { return totalReads_; }
    uint64_t getBytesRead() const { return bytesRead_; }
    
    /**
     * Load initramfs image from file
     * 
     * @param path Path to initramfs image file
     * @return True if successful
     */
    bool loadFromFile(const std::string& path);
    
    /**
     * Validate initramfs format
     * 
     * Checks for common initramfs formats (cpio, gzip-compressed cpio).
     * 
     * @return True if valid initramfs detected
     */
    bool validateFormat() const;
    
    /**
     * Get format type
     * 
     * @return Format type string ("cpio", "cpio.gz", "unknown")
     */
    std::string getFormatType() const;
    
    /**
     * Default physical addresses for common platforms
     */
    static constexpr uint64_t DEFAULT_IA64_ADDRESS = 0x2000000;  // 32 MB
    static constexpr uint64_t DEFAULT_X86_64_ADDRESS = 0x2000000;  // 32 MB
    static constexpr uint64_t DEFAULT_ARM64_ADDRESS = 0x4000000;  // 64 MB
    
private:
    // Device identification
    std::string deviceId_;
    std::string imagePath_;
    
    // Device parameters
    uint32_t blockSize_;
    bool connected_;
    
    // Initramfs data
    std::vector<uint8_t> buffer_;
    uint64_t physicalAddress_;
    bool loadedInMemory_;
    
    // Statistics
    uint64_t totalReads_;
    uint64_t bytesRead_;
    
    // Helper methods
    bool loadFileInternal(const std::string& path);
    void initializeFromBuffer(const uint8_t* buffer, uint64_t size);
};

} // namespace ia64
