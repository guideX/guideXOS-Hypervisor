#pragma once

#include "IFilesystem.h"
#include "IStorageDevice.h"
#include <string>
#include <memory>

namespace ia64 {

/**
 * FilesystemMount - Represents a mounted filesystem at a mount point
 * 
 * This class manages the association between a filesystem implementation,
 * its backing storage device, and the mount point in the guest's virtual
 * filesystem namespace.
 * 
 * Features:
 * - Mount point management
 * - Filesystem lifecycle control
 * - Access statistics
 * - Mount options (read-only, etc.)
 */
class FilesystemMount {
public:
    /**
     * Mount options
     */
    struct MountOptions {
        bool readOnly;          // Force read-only access
        bool noExec;            // Disable execution of binaries
        bool noSuid;            // Ignore suid/sgid bits
        std::string fsType;     // Filesystem type hint
        
        MountOptions()
            : readOnly(false)
            , noExec(false)
            , noSuid(false)
            , fsType()
        {}
    };
    
    /**
     * Constructor
     * 
     * @param mountPoint Guest path where filesystem is mounted (e.g., "/mnt/disk0")
     * @param filesystem Filesystem implementation
     * @param device Backing storage device
     * @param options Mount options
     */
    FilesystemMount(const std::string& mountPoint,
                   std::shared_ptr<IFilesystem> filesystem,
                   std::shared_ptr<IStorageDevice> device,
                   const MountOptions& options = MountOptions());
    
    ~FilesystemMount();
    
    // ========================================================================
    // Mount Information
    // ========================================================================
    
    /**
     * Get mount point
     * @return Mount point path
     */
    std::string getMountPoint() const { return mountPoint_; }
    
    /**
     * Get filesystem
     * @return Filesystem implementation
     */
    std::shared_ptr<IFilesystem> getFilesystem() const { return filesystem_; }
    
    /**
     * Get storage device
     * @return Backing storage device
     */
    std::shared_ptr<IStorageDevice> getDevice() const { return device_; }
    
    /**
     * Get mount options
     * @return Mount options
     */
    const MountOptions& getOptions() const { return options_; }
    
    /**
     * Check if mounted
     * @return True if mounted
     */
    bool isMounted() const { return mounted_; }
    
    /**
     * Check if read-only
     * @return True if read-only
     */
    bool isReadOnly() const {
        return options_.readOnly || (filesystem_ && filesystem_->isReadOnly());
    }
    
    // ========================================================================
    // Mount Operations
    // ========================================================================
    
    /**
     * Mount the filesystem
     * @return True if successful
     */
    bool mount();
    
    /**
     * Unmount the filesystem
     */
    void unmount();
    
    // ========================================================================
    // Path Translation
    // ========================================================================
    
    /**
     * Check if a guest path belongs to this mount
     * 
     * @param guestPath Absolute guest path
     * @return True if path is under this mount point
     */
    bool containsPath(const std::string& guestPath) const;
    
    /**
     * Translate guest path to filesystem-relative path
     * 
     * Example: guestPath="/mnt/disk0/etc/config" with mountPoint="/mnt/disk0"
     *          returns "/etc/config"
     * 
     * @param guestPath Absolute guest path
     * @return Filesystem-relative path, or empty if not under this mount
     */
    std::string translatePath(const std::string& guestPath) const;
    
    // ========================================================================
    // Statistics
    // ========================================================================
    
    /**
     * Get access count
     * @return Number of accesses
     */
    uint64_t getAccessCount() const { return accessCount_; }
    
    /**
     * Increment access count
     */
    void incrementAccessCount() { accessCount_++; }
    
    /**
     * Get mount information as string
     * @return Mount info string
     */
    std::string getInfo() const;
    
private:
    std::string mountPoint_;                    // Mount point in guest filesystem
    std::shared_ptr<IFilesystem> filesystem_;   // Filesystem implementation
    std::shared_ptr<IStorageDevice> device_;    // Backing storage
    MountOptions options_;                       // Mount options
    bool mounted_;                              // Mount status
    uint64_t accessCount_;                      // Access statistics
    
    // Helper: Normalize path (remove trailing slashes, etc.)
    static std::string normalizePath(const std::string& path);
};

} // namespace ia64
