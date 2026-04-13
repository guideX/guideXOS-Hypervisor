#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include "VirtualBlockDevice.h"
#include "VirtualDiskManager.h"
#include "SimpleReadOnlyFS.h"
#include "FilesystemMount.h"
#include "PathResolver.h"

using namespace ia64;

/**
 * Example: Virtual Block Device and Filesystem Mounting
 * 
 * This example demonstrates:
 * 1. Creating virtual block devices (memory and file-backed)
 * 2. Creating a simple read-only filesystem
 * 3. Mounting filesystems at guest paths
 * 4. Performing file operations through the VirtualDiskManager
 * 5. Path resolution with sandboxing
 */

void printSeparator(const std::string& title) {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << title << "\n";
    std::cout << "========================================\n\n";
}

void example1_MemoryBackedDisk() {
    printSeparator("Example 1: Memory-Backed Virtual Disk");
    
    // Create a memory-backed virtual disk
    std::string deviceId = "memdisk0";
    uint64_t diskSize = 1024 * 1024;  // 1 MB
    uint32_t sectorSize = 512;
    
    VirtualBlockDevice disk(deviceId, diskSize, sectorSize);
    
    std::cout << "Created memory-backed disk:\n";
    std::cout << "  Device ID: " << disk.getDeviceId() << "\n";
    std::cout << "  Size: " << disk.getSize() << " bytes\n";
    std::cout << "  Sector Size: " << disk.getSectorSize() << " bytes\n";
    std::cout << "  Sector Count: " << disk.getSectorCount() << "\n\n";
    
    // Connect the disk
    if (disk.connect()) {
        std::cout << "Disk connected successfully\n";
    } else {
        std::cout << "Failed to connect disk\n";
        return;
    }
    
    // Write some data
    std::vector<uint8_t> writeData(sectorSize, 0xAB);
    if (disk.writeSector(0, writeData.data())) {
        std::cout << "Wrote sector 0 with pattern 0xAB\n";
    }
    
    // Read it back
    std::vector<uint8_t> readData(sectorSize, 0);
    if (disk.readSector(0, readData.data())) {
        std::cout << "Read sector 0: ";
        for (size_t i = 0; i < 16; ++i) {
            printf("%02X ", readData[i]);
        }
        std::cout << "...\n";
    }
    
    std::cout << "\n" << disk.getStatistics() << "\n";
}

void example2_FileBackedDisk() {
    printSeparator("Example 2: File-Backed Virtual Disk");
    
    std::string imagePath = "test_disk.img";
    uint64_t diskSize = 2 * 1024 * 1024;  // 2 MB
    
    // Create the disk image file
    std::cout << "Creating disk image: " << imagePath << "\n";
    if (VirtualBlockDevice::createDiskImage(imagePath, diskSize)) {
        std::cout << "Disk image created successfully\n\n";
    } else {
        std::cout << "Failed to create disk image\n";
        return;
    }
    
    // Create file-backed virtual disk
    VirtualBlockDevice disk(
        "filedisk0",
        imagePath,
        VirtualBlockDevice::BackingType::DISK_IMAGE,
        diskSize,
        false,  // not read-only
        512
    );
    
    if (disk.connect()) {
        std::cout << "File-backed disk connected\n";
        std::cout << "  Size: " << disk.getSize() << " bytes\n";
        std::cout << "  Backing: " << disk.getBackingPath() << "\n\n";
        
        // Write and read test
        std::string testData = "Hello, Virtual Disk!";
        std::vector<uint8_t> writeData(512, 0);
        std::copy(testData.begin(), testData.end(), writeData.begin());
        
        disk.writeSector(10, writeData.data());
        std::cout << "Wrote data to sector 10\n";
        
        std::vector<uint8_t> readData(512, 0);
        disk.readSector(10, readData.data());
        std::cout << "Read data from sector 10: " 
                  << std::string(readData.begin(), readData.begin() + testData.size()) 
                  << "\n\n";
    }
}

void example3_SimpleFilesystem() {
    printSeparator("Example 3: Simple Read-Only Filesystem");
    
    // Create a memory-backed device
    auto device = std::make_shared<VirtualBlockDevice>(
        "fsdisk0",
        512 * 1024,  // 512 KB
        512
    );
    
    device->connect();
    
    // Create filesystem with some files
    std::map<std::string, std::vector<uint8_t>> files;
    
    // File 1: greeting.txt
    std::string greeting = "Hello from the virtual filesystem!";
    files["greeting.txt"] = std::vector<uint8_t>(greeting.begin(), greeting.end());
    
    // File 2: config.txt
    std::string config = "setting1=value1\nsetting2=value2\nsetting3=value3\n";
    files["config.txt"] = std::vector<uint8_t>(config.begin(), config.end());
    
    // File 3: data.bin
    std::vector<uint8_t> binaryData(1024, 0);
    for (size_t i = 0; i < binaryData.size(); ++i) {
        binaryData[i] = static_cast<uint8_t>(i % 256);
    }
    files["data.bin"] = binaryData;
    
    std::cout << "Creating SimpleReadOnlyFS with " << files.size() << " files\n";
    
    // Create filesystem
    if (SimpleReadOnlyFS::createFilesystem(device, files, 512)) {
        std::cout << "Filesystem created successfully\n\n";
    } else {
        std::cout << "Failed to create filesystem\n";
        return;
    }
    
    // Mount and read
    auto fs = std::make_shared<SimpleReadOnlyFS>();
    
    if (fs->mount(device)) {
        std::cout << "Filesystem mounted\n";
        std::cout << "  Type: " << fs->getType() << "\n";
        std::cout << "  Total Size: " << fs->getTotalSize() << " bytes\n";
        std::cout << "  Block Size: " << fs->getBlockSize() << " bytes\n\n";
        
        // List directory
        std::vector<DirectoryEntry> entries;
        if (fs->listDirectory("/", entries)) {
            std::cout << "Directory listing:\n";
            for (const auto& entry : entries) {
                std::cout << "  " << entry.name 
                          << " (" << (entry.type == FileType::REGULAR_FILE ? "file" : "dir")
                          << ", " << entry.size << " bytes)\n";
            }
            std::cout << "\n";
        }
        
        // Read a file
        std::vector<uint8_t> content;
        if (fs->readFileAll("/greeting.txt", content)) {
            std::cout << "Content of greeting.txt:\n";
            std::cout << "  " << std::string(content.begin(), content.end()) << "\n\n";
        }
        
        fs->unmount();
    }
}

void example4_VirtualDiskManager() {
    printSeparator("Example 4: Virtual Disk Manager");
    
    VirtualDiskManager manager;
    
    // Create and register a memory disk
    auto disk1 = manager.createMemoryDisk("disk0", 1024 * 1024, 512);
    std::cout << "Created disk: " << disk1->getDeviceId() << "\n";
    
    // Create filesystem with files
    std::map<std::string, std::vector<uint8_t>> files;
    std::string readme = "This is the README file for the virtual disk.\n\n"
                        "Features:\n"
                        "- Virtual block devices\n"
                        "- Filesystem mounting\n"
                        "- Path resolution with sandboxing\n";
    files["README.md"] = std::vector<uint8_t>(readme.begin(), readme.end());
    
    std::string version = "Version 1.0.0";
    files["VERSION"] = std::vector<uint8_t>(version.begin(), version.end());
    
    // Format disk with filesystem
    SimpleReadOnlyFS::createFilesystem(disk1, files, 512);
    
    // Mount filesystem
    auto filesystem = std::make_shared<SimpleReadOnlyFS>();
    FilesystemMount::MountOptions options;
    options.readOnly = true;
    
    if (manager.mount("/mnt/disk0", disk1, filesystem, options)) {
        std::cout << "Mounted filesystem at /mnt/disk0\n\n";
    }
    
    // Use high-level API
    std::cout << "Using high-level file operations:\n\n";
    
    // Check if file exists
    if (manager.exists("/mnt/disk0/README.md")) {
        std::cout << "File /mnt/disk0/README.md exists\n";
        
        // Get file size
        int64_t size = manager.getFileSize("/mnt/disk0/README.md");
        std::cout << "  Size: " << size << " bytes\n";
        
        // Read file
        std::vector<uint8_t> content;
        if (manager.readFileAll("/mnt/disk0/README.md", content)) {
            std::cout << "  Content:\n";
            std::cout << std::string(content.begin(), content.end()) << "\n";
        }
    }
    
    // List directory
    std::vector<DirectoryEntry> entries;
    if (manager.listDirectory("/mnt/disk0", entries)) {
        std::cout << "\nDirectory /mnt/disk0:\n";
        for (const auto& entry : entries) {
            std::cout << "  - " << entry.name << " (" << entry.size << " bytes)\n";
        }
    }
    
    std::cout << "\n" << manager.getStatistics() << "\n";
}

void example5_PathResolution() {
    printSeparator("Example 5: Path Resolution and Sandboxing");
    
    PathResolver resolver;
    
    // Test path normalization
    std::cout << "Path Normalization:\n";
    
    std::vector<std::pair<std::string, std::string>> testPaths = {
        {"/foo/bar/../baz", "/foo/baz"},
        {"/foo/./bar", "/foo/bar"},
        {"/foo//bar", "/foo/bar"},
        {"foo/bar", "/foo/bar"},
        {"/foo/bar/", "/foo/bar"},
        {"../../etc/passwd", "INVALID"}  // Tries to escape
    };
    
    for (const auto& test : testPaths) {
        std::string normalized = resolver.normalize(test.first, "/");
        std::cout << "  " << test.first << " -> ";
        if (normalized.empty()) {
            std::cout << "[INVALID]\n";
        } else {
            std::cout << normalized << "\n";
        }
    }
    
    std::cout << "\nPath Validation:\n";
    
    std::vector<std::string> validatePaths = {
        "/valid/path/to/file",
        "/path/with/../../escape",
        "/path\0with\0nulls",
        std::string(5000, 'a')  // Too long
    };
    
    for (const auto& path : validatePaths) {
        std::string errorMsg;
        bool valid = resolver.validate(path, errorMsg);
        std::cout << "  " << (path.size() > 40 ? "[long path]" : path) << " -> "
                  << (valid ? "VALID" : "INVALID: " + errorMsg) << "\n";
    }
    
    std::cout << "\nPath Components:\n";
    auto components = PathResolver::split("/foo/bar/baz");
    std::cout << "  /foo/bar/baz -> [";
    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << components[i];
    }
    std::cout << "]\n";
    
    std::cout << "\ndirname('/foo/bar/file.txt') = " 
              << PathResolver::dirname("/foo/bar/file.txt") << "\n";
    std::cout << "basename('/foo/bar/file.txt') = " 
              << PathResolver::basename("/foo/bar/file.txt") << "\n";
}

int main() {
    std::cout << "Virtual Block Device and Filesystem Examples\n";
    std::cout << "=============================================\n";
    
    try {
        example1_MemoryBackedDisk();
        example2_FileBackedDisk();
        example3_SimpleFilesystem();
        example4_VirtualDiskManager();
        example5_PathResolution();
        
        printSeparator("All Examples Completed Successfully!");
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
