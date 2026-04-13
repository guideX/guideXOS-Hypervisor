#pragma once

#include "VirtualBlockDevice.h"
#include "IFilesystem.h"
#include "FilesystemMount.h"
#include "PathResolver.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdint>

namespace ia64 {

/**
 * VirtualDiskManager - Manages virtual disks and filesystem mounts
 * 
 * This class orchestrates:
 * - Multiple virtual block devices
 * - Multiple filesystem mounts
 * - Path resolution and translation
 * - Secure access to guest filesystem
 * 
 * Design Philosophy:
 * - Centralized management of storage subsystem
 * - Secure path resolution with sandboxing
 * - Simple API for guest filesystem access
 * - Support for multiple concurrent mounts
 * 
 * Features:
 * - Create and attach virtual disks
 * - Mount filesystems at arbitrary paths
 * - Resolve guest paths to filesystem operations
 * - Read/write files through unified API
 * - List directory contents
 * - Query file attributes
 * 
 * Thread Safety:
 * - Thread-safe for read operations
 * - Synchronization required for mount/unmount operations
 */
class VirtualDiskManager {
public:
    /**
     * Constructor
     */
    VirtualDiskManager();
    
    /**
     * Destructor
     */
    ~VirtualDiskManager();
    
    // ========================================================================
    // Device Management
    // ========================================================================
    
    /**
     * Create and register a memory-backed virtual disk
     * 
     * @param deviceId Unique device identifier
     * @param sizeBytes Size in bytes
     * @param sectorSize Sector size (default 512)
     * @return Shared pointer to device, or nullptr on error
     */
    std::shared_ptr<VirtualBlockDevice> createMemoryDisk(
        const std::string& deviceId,
        uint64_t sizeBytes,
        uint32_t sectorSize = 512);
    
    /**
     * Create and register a file-backed virtual disk
     * 
     * @param deviceId Unique device identifier
     * @param imagePath Path to disk image file
     * @param sizeBytes Size in bytes (0 = auto-detect)
     * @param readOnly Read-only flag
     * @param sectorSize Sector size (default 512)
     * @return Shared pointer to device, or nullptr on error
     */
    std::shared_ptr<VirtualBlockDevice> createFileDisk(
        const std::string& deviceId,
        const std::string& imagePath,
        uint64_t sizeBytes = 0,
        bool readOnly = false,
        uint32_t sectorSize = 512);
    
    /**
     * Register an existing device
     * 
     * @param device Device to register
     * @return True if successful
     */
    bool registerDevice(std::shared_ptr<VirtualBlockDevice> device);
    
    /**
     * Unregister a device
     * 
     * @param deviceId Device identifier
     * @return True if successful
     */
    bool unregisterDevice(const std::string& deviceId);
    
    /**
     * Get device by ID
     * 
     * @param deviceId Device identifier
     * @return Device pointer, or nullptr if not found
     */
    std::shared_ptr<VirtualBlockDevice> getDevice(const std::string& deviceId) const;
    
    /**
     * Get all registered devices
     * 
     * @return Vector of devices
     */
    std::vector<std::shared_ptr<VirtualBlockDevice>> getDevices() const;
    
    // ========================================================================
    // Filesystem Management
    // ========================================================================
    
    /**
     * Mount a filesystem on a device at a mount point
     * 
     * @param mountPoint Guest path for mount point (e.g., "/mnt/disk0")
     * @param deviceId Device to mount
     * @param filesystem Filesystem implementation
     * @param options Mount options
     * @return True if successful
     */
    bool mount(const std::string& mountPoint,
               const std::string& deviceId,
               std::shared_ptr<IFilesystem> filesystem,
               const FilesystemMount::MountOptions& options = FilesystemMount::MountOptions());
    
    /**
     * Mount a filesystem on a device at a mount point (device pointer version)
     * 
     * @param mountPoint Guest path for mount point
     * @param device Device to mount
     * @param filesystem Filesystem implementation
     * @param options Mount options
     * @return True if successful
     */
    bool mount(const std::string& mountPoint,
               std::shared_ptr<IStorageDevice> device,
               std::shared_ptr<IFilesystem> filesystem,
               const FilesystemMount::MountOptions& options = FilesystemMount::MountOptions());
    
    /**
     * Unmount a filesystem
     * 
     * @param mountPoint Guest path of mount point
     * @return True if successful
     */
    bool unmount(const std::string& mountPoint);
    
    /**
     * Get mount by mount point
     * 
     * @param mountPoint Guest path of mount point
     * @return Mount pointer, or nullptr if not found
     */
    std::shared_ptr<FilesystemMount> getMount(const std::string& mountPoint) const;
    
    /**
     * Get all mounts
     * 
     * @return Vector of mounts
     */
    std::vector<std::shared_ptr<FilesystemMount>> getMounts() const;
    
    // ========================================================================
    // File Operations (High-Level API)
    // ========================================================================
    
    /**
     * Check if a file or directory exists
     * 
     * @param guestPath Absolute guest path
     * @return True if exists
     */
    bool exists(const std::string& guestPath);
    
    /**
     * Check if path is a directory
     * 
     * @param guestPath Absolute guest path
     * @return True if directory
     */
    bool isDirectory(const std::string& guestPath);
    
    /**
     * Check if path is a file
     * 
     * @param guestPath Absolute guest path
     * @return True if file
     */
    bool isFile(const std::string& guestPath);
    
    /**
     * Get file size
     * 
     * @param guestPath Absolute guest path
     * @return File size in bytes, or -1 on error
     */
    int64_t getFileSize(const std::string& guestPath);
    
    /**
     * Get file attributes
     * 
     * @param guestPath Absolute guest path
     * @param attrs Output attributes
     * @return True if successful
     */
    bool getAttributes(const std::string& guestPath, FileAttributes& attrs);
    
    /**
     * Read file content
     * 
     * @param guestPath Absolute guest path
     * @param buffer Output buffer
     * @param size Number of bytes to read
     * @param offset Offset within file
     * @return Number of bytes read, or -1 on error
     */
    int64_t readFile(const std::string& guestPath,
                    uint8_t* buffer,
                    uint64_t size,
                    uint64_t offset = 0);
    
    /**
     * Read entire file
     * 
     * @param guestPath Absolute guest path
     * @param data Output vector
     * @return True if successful
     */
    bool readFileAll(const std::string& guestPath, std::vector<uint8_t>& data);
    
    /**
     * List directory contents
     * 
     * @param guestPath Absolute guest path to directory
     * @param entries Output vector of directory entries
     * @return True if successful
     */
    bool listDirectory(const std::string& guestPath,
                      std::vector<DirectoryEntry>& entries);
    
    // ========================================================================
    // Path Resolution
    // ========================================================================
    
    /**
     * Resolve a guest path
     * 
     * @param guestPath Guest path
     * @param currentDir Current directory for relative paths
     * @return Resolution result
     */
    PathResolver::ResolvedPath resolvePath(
        const std::string& guestPath,
        const std::string& currentDir = "/");
    
    /**
     * Get path resolver
     * 
     * @return Path resolver instance
     */
    const PathResolver& getPathResolver() const { return pathResolver_; }
    
    // ========================================================================
    // Information and Statistics
    // ========================================================================
    
    /**
     * Get manager statistics
     * 
     * @return Statistics string
     */
    std::string getStatistics() const;
    
    /**
     * Get information about all mounts
     * 
     * @return Mount information string
     */
    std::string getMountInfo() const;
    
private:
    // Registered devices
    std::map<std::string, std::shared_ptr<VirtualBlockDevice>> devices_;
    
    // Active mounts
    std::vector<std::shared_ptr<FilesystemMount>> mounts_;
    
    // Path resolver
    PathResolver pathResolver_;
    
    // Helper methods
    std::shared_ptr<FilesystemMount> findMount(const std::string& mountPoint) const;
};

} // namespace ia64
