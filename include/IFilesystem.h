#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace ia64 {

// Forward declarations
class IStorageDevice;

/**
 * FileType - Type of filesystem entry
 */
enum class FileType {
    REGULAR_FILE,
    DIRECTORY,
    SYMBOLIC_LINK,
    BLOCK_DEVICE,
    CHARACTER_DEVICE,
    FIFO,
    SOCKET,
    UNKNOWN
};

/**
 * FileAttributes - File metadata
 */
struct FileAttributes {
    std::string name;           // File name
    FileType type;              // File type
    uint64_t size;              // File size in bytes
    uint64_t creationTime;      // Creation timestamp
    uint64_t modificationTime;  // Last modification timestamp
    uint64_t accessTime;        // Last access timestamp
    uint32_t permissions;       // Unix-style permissions (rwxrwxrwx)
    uint32_t uid;               // User ID
    uint32_t gid;               // Group ID
    uint64_t inodeNumber;       // Inode number (if applicable)
    
    FileAttributes()
        : name()
        , type(FileType::UNKNOWN)
        , size(0)
        , creationTime(0)
        , modificationTime(0)
        , accessTime(0)
        , permissions(0)
        , uid(0)
        , gid(0)
        , inodeNumber(0)
    {}
};

/**
 * DirectoryEntry - Entry in a directory listing
 */
struct DirectoryEntry {
    std::string name;
    FileType type;
    uint64_t size;
    
    DirectoryEntry()
        : name()
        , type(FileType::UNKNOWN)
        , size(0)
    {}
    
    DirectoryEntry(const std::string& n, FileType t, uint64_t s)
        : name(n), type(t), size(s)
    {}
};

/**
 * IFilesystem - Abstract interface for virtual filesystems
 * 
 * This interface defines the operations supported by virtual filesystems
 * that can be mounted on virtual block devices.
 * 
 * Design Philosophy:
 * - Read operations are mandatory
 * - Write operations are optional (can return false/error)
 * - Path-based operations (Unix-style paths)
 * - Sandboxing enforced at this layer
 * 
 * Thread Safety:
 * - Implementations should be thread-safe
 * - Multiple concurrent reads allowed
 * - Exclusive writes
 */
class IFilesystem {
public:
    virtual ~IFilesystem() = default;
    
    // ========================================================================
    // Filesystem Information
    // ========================================================================
    
    /**
     * Get filesystem type/name
     * @return Filesystem type (e.g., "FAT16", "EXT2", "SimpleRO")
     */
    virtual std::string getType() const = 0;
    
    /**
     * Check if filesystem is read-only
     * @return True if read-only
     */
    virtual bool isReadOnly() const = 0;
    
    /**
     * Get total filesystem size
     * @return Size in bytes
     */
    virtual uint64_t getTotalSize() const = 0;
    
    /**
     * Get available free space
     * @return Free space in bytes
     */
    virtual uint64_t getFreeSize() const = 0;
    
    /**
     * Get block/cluster size
     * @return Block size in bytes
     */
    virtual uint32_t getBlockSize() const = 0;
    
    // ========================================================================
    // Filesystem Lifecycle
    // ========================================================================
    
    /**
     * Mount the filesystem
     * @param device Storage device to mount
     * @return True if successful
     */
    virtual bool mount(std::shared_ptr<IStorageDevice> device) = 0;
    
    /**
     * Unmount the filesystem
     */
    virtual void unmount() = 0;
    
    /**
     * Check if filesystem is mounted
     * @return True if mounted
     */
    virtual bool isMounted() const = 0;
    
    // ========================================================================
    // File Operations
    // ========================================================================
    
    /**
     * Check if a file or directory exists
     * 
     * @param path Absolute path within filesystem
     * @return True if exists
     */
    virtual bool exists(const std::string& path) const = 0;
    
    /**
     * Check if path is a directory
     * 
     * @param path Absolute path within filesystem
     * @return True if directory
     */
    virtual bool isDirectory(const std::string& path) const = 0;
    
    /**
     * Check if path is a regular file
     * 
     * @param path Absolute path within filesystem
     * @return True if regular file
     */
    virtual bool isFile(const std::string& path) const = 0;
    
    /**
     * Get file attributes
     * 
     * @param path Absolute path within filesystem
     * @param attrs Output attributes
     * @return True if successful
     */
    virtual bool getAttributes(const std::string& path, FileAttributes& attrs) const = 0;
    
    /**
     * Get file size
     * 
     * @param path Absolute path within filesystem
     * @return File size in bytes, or -1 on error
     */
    virtual int64_t getFileSize(const std::string& path) const = 0;
    
    /**
     * Read file content
     * 
     * @param path Absolute path within filesystem
     * @param buffer Output buffer
     * @param size Number of bytes to read
     * @param offset Offset within file
     * @return Number of bytes read, or -1 on error
     */
    virtual int64_t readFile(const std::string& path,
                            uint8_t* buffer,
                            uint64_t size,
                            uint64_t offset = 0) const = 0;
    
    /**
     * Read entire file into vector
     * 
     * @param path Absolute path within filesystem
     * @param data Output vector
     * @return True if successful
     */
    virtual bool readFileAll(const std::string& path, std::vector<uint8_t>& data) const;
    
    /**
     * Write file content (optional - may not be supported)
     * 
     * @param path Absolute path within filesystem
     * @param buffer Input buffer
     * @param size Number of bytes to write
     * @param offset Offset within file
     * @return Number of bytes written, or -1 on error
     */
    virtual int64_t writeFile(const std::string& path,
                             const uint8_t* buffer,
                             uint64_t size,
                             uint64_t offset = 0) {
        return -1;  // Default: not supported
    }
    
    // ========================================================================
    // Directory Operations
    // ========================================================================
    
    /**
     * List directory contents
     * 
     * @param path Absolute path to directory
     * @param entries Output vector of directory entries
     * @return True if successful
     */
    virtual bool listDirectory(const std::string& path,
                              std::vector<DirectoryEntry>& entries) const = 0;
    
    /**
     * Create directory (optional - may not be supported)
     * 
     * @param path Absolute path for new directory
     * @return True if successful
     */
    virtual bool createDirectory(const std::string& path) {
        return false;  // Default: not supported
    }
    
    /**
     * Delete file or directory (optional - may not be supported)
     * 
     * @param path Absolute path to delete
     * @return True if successful
     */
    virtual bool deleteEntry(const std::string& path) {
        return false;  // Default: not supported
    }
    
protected:
    IFilesystem() = default;
    
    // Prevent copying and moving of interface
    IFilesystem(const IFilesystem&) = delete;
    IFilesystem& operator=(const IFilesystem&) = delete;
    IFilesystem(IFilesystem&&) = delete;
    IFilesystem& operator=(IFilesystem&&) = delete;
};

/**
 * Default implementation: Read entire file
 */
inline bool IFilesystem::readFileAll(const std::string& path, std::vector<uint8_t>& data) const {
    int64_t size = getFileSize(path);
    if (size < 0) {
        return false;
    }
    
    data.resize(size);
    if (size == 0) {
        return true;
    }
    
    int64_t bytesRead = readFile(path, data.data(), size, 0);
    return bytesRead == size;
}

} // namespace ia64
