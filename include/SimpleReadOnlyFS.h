#pragma once

#include "IFilesystem.h"
#include "IStorageDevice.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace ia64 {

/**
 * SimpleReadOnlyFS - Simple read-only filesystem implementation
 * 
 * This is a minimal filesystem format designed for simple use cases:
 * - Read-only access
 * - Flat directory structure (single level directories)
 * - Fixed-size entries
 * - Simple linear layout
 * 
 * Filesystem Layout:
 * - Superblock (sector 0): Filesystem metadata
 * - Directory Table: Array of directory entries
 * - File Data: Contiguous file data blocks
 * 
 * Superblock Format (512 bytes):
 * - Magic: "SROFS1.0" (8 bytes)
 * - Version: 1 (4 bytes)
 * - Block Size: typically 512 or 4096 (4 bytes)
 * - Total Blocks: (8 bytes)
 * - Directory Block: Starting block of directory (8 bytes)
 * - Directory Entries: Number of directory entries (4 bytes)
 * - Data Block: Starting block of file data (8 bytes)
 * - Reserved: (remaining bytes)
 * 
 * Directory Entry Format (64 bytes):
 * - Name: Null-terminated string (48 bytes)
 * - Type: File type (1 byte) 0=file, 1=directory
 * - Reserved: (3 bytes for alignment)
 * - Size: File size in bytes (8 bytes)
 * - Data Block: Starting block of file data (4 bytes)
 * 
 * This filesystem is designed for:
 * - Bootable disk images
 * - Embedded resources
 * - Testing and demonstrations
 */
class SimpleReadOnlyFS : public IFilesystem {
public:
    /**
     * Magic number for filesystem identification
     */
    static constexpr const char* MAGIC = "SROFS1.0";
    static constexpr uint32_t VERSION = 1;
    static constexpr uint32_t SUPERBLOCK_SIZE = 512;
    static constexpr uint32_t DIRENT_SIZE = 64;
    static constexpr uint32_t MAX_FILENAME_LENGTH = 48;
    
    /**
     * Superblock structure
     */
    struct Superblock {
        char magic[8];          // Magic number "SROFS1.0"
        uint32_t version;       // Version number
        uint32_t blockSize;     // Block size in bytes
        uint64_t totalBlocks;   // Total number of blocks
        uint64_t dirBlock;      // Starting block of directory table
        uint32_t dirEntries;    // Number of directory entries
        uint32_t reserved1;     // Reserved for alignment
        uint64_t dataBlock;     // Starting block of file data
        uint8_t reserved[464];  // Reserved space (total = 512 bytes)
        
        Superblock();
        bool isValid() const;
        void serialize(uint8_t* buffer) const;
        bool deserialize(const uint8_t* buffer);
    };
    
    /**
     * Directory entry structure
     */
    struct DirEntry {
        char name[MAX_FILENAME_LENGTH];  // Null-terminated filename
        uint8_t type;                    // 0=file, 1=directory
        uint8_t reserved[3];             // Reserved
        uint64_t size;                   // File size in bytes
        uint32_t dataBlock;              // Starting block of file data
        uint32_t reserved2;              // Reserved for alignment
        
        DirEntry();
        void serialize(uint8_t* buffer) const;
        bool deserialize(const uint8_t* buffer);
        std::string getName() const;
        void setName(const std::string& name);
    };
    
    /**
     * Constructor
     */
    SimpleReadOnlyFS();
    
    /**
     * Destructor
     */
    ~SimpleReadOnlyFS() override;
    
    // ========================================================================
    // IFilesystem Interface Implementation
    // ========================================================================
    
    std::string getType() const override { return "SimpleReadOnlyFS"; }
    bool isReadOnly() const override { return true; }
    uint64_t getTotalSize() const override;
    uint64_t getFreeSize() const override { return 0; }  // Read-only, no free space
    uint32_t getBlockSize() const override;
    
    bool mount(std::shared_ptr<IStorageDevice> device) override;
    void unmount() override;
    bool isMounted() const override { return mounted_; }
    
    bool exists(const std::string& path) const override;
    bool isDirectory(const std::string& path) const override;
    bool isFile(const std::string& path) const override;
    bool getAttributes(const std::string& path, FileAttributes& attrs) const override;
    int64_t getFileSize(const std::string& path) const override;
    
    int64_t readFile(const std::string& path,
                    uint8_t* buffer,
                    uint64_t size,
                    uint64_t offset = 0) const override;
    
    bool listDirectory(const std::string& path,
                      std::vector<DirectoryEntry>& entries) const override;
    
    // ========================================================================
    // SimpleReadOnlyFS Specific Methods
    // ========================================================================
    
    /**
     * Create a new SimpleReadOnlyFS image
     * 
     * @param device Storage device to format
     * @param files Map of path -> file content
     * @param blockSize Block size (default 512)
     * @return True if successful
     */
    static bool createFilesystem(std::shared_ptr<IStorageDevice> device,
                                 const std::map<std::string, std::vector<uint8_t>>& files,
                                 uint32_t blockSize = 512);
    
    /**
     * Get superblock
     * @return Superblock structure
     */
    const Superblock& getSuperblock() const { return superblock_; }
    
private:
    std::shared_ptr<IStorageDevice> device_;
    Superblock superblock_;
    std::vector<DirEntry> directory_;
    bool mounted_;
    
    // Helper methods
    bool readSuperblock();
    bool readDirectory();
    const DirEntry* findEntry(const std::string& path) const;
    std::string normalizePath(const std::string& path) const;
};

} // namespace ia64
