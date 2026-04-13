# Virtual Block Device Quick Reference

## Quick Start

### 1. Create a Memory Disk

```cpp
#include "VirtualDiskManager.h"
#include "SimpleReadOnlyFS.h"

VirtualDiskManager manager;
auto disk = manager.createMemoryDisk("disk0", 1024 * 1024, 512);
```

### 2. Create Files and Filesystem

```cpp
std::map<std::string, std::vector<uint8_t>> files;
std::string content = "Hello, World!";
files["hello.txt"] = std::vector<uint8_t>(content.begin(), content.end());

SimpleReadOnlyFS::createFilesystem(disk, files, 512);
```

### 3. Mount and Access

```cpp
auto fs = std::make_shared<SimpleReadOnlyFS>();
manager.mount("/mnt", disk, fs);

std::vector<uint8_t> data;
manager.readFileAll("/mnt/hello.txt", data);
```

## Common Operations

### Check if File Exists
```cpp
if (manager.exists("/mnt/file.txt")) {
    // File exists
}
```

### Read File
```cpp
std::vector<uint8_t> content;
if (manager.readFileAll("/mnt/file.txt", content)) {
    std::string text(content.begin(), content.end());
}
```

### List Directory
```cpp
std::vector<DirectoryEntry> entries;
if (manager.listDirectory("/mnt", entries)) {
    for (const auto& entry : entries) {
        std::cout << entry.name << " (" << entry.size << " bytes)\n";
    }
}
```

### Get File Size
```cpp
int64_t size = manager.getFileSize("/mnt/file.txt");
```

### Get File Attributes
```cpp
FileAttributes attrs;
if (manager.getAttributes("/mnt/file.txt", attrs)) {
    std::cout << "Size: " << attrs.size << "\n";
    std::cout << "Type: " << (attrs.type == FileType::REGULAR_FILE ? "file" : "dir") << "\n";
}
```

## Low-Level Block Device API

### Direct Sector Access
```cpp
VirtualBlockDevice disk("disk0", 1024 * 1024, 512);
disk.connect();

// Write sector
std::vector<uint8_t> data(512, 0xFF);
disk.writeSector(0, data.data());

// Read sector
std::vector<uint8_t> buffer(512);
disk.readSector(0, buffer.data());
```

### Multiple Sectors
```cpp
// Write 10 sectors
std::vector<uint8_t> data(5120, 0xAB);  // 10 * 512
disk.writeSectors(0, 10, data.data());

// Read 10 sectors
std::vector<uint8_t> buffer(5120);
disk.readSectors(0, 10, buffer.data());
```

## Creating File-Backed Disks

### Create Disk Image File
```cpp
VirtualBlockDevice::createDiskImage("disk.img", 10 * 1024 * 1024);
```

### Use File-Backed Disk
```cpp
auto disk = manager.createFileDisk(
    "filedisk",
    "disk.img",
    0,      // auto-detect size
    false,  // read-write
    512
);
```

## Path Resolution

### Normalize Paths
```cpp
PathResolver resolver;
std::string clean = resolver.normalize("/foo/bar/../baz");  // "/foo/baz"
```

### Validate Paths
```cpp
std::string error;
bool safe = resolver.validate(path, error);
if (!safe) {
    std::cout << "Invalid path: " << error << "\n";
}
```

### Path Utilities
```cpp
std::string dir = PathResolver::dirname("/foo/bar/file.txt");    // "/foo/bar"
std::string name = PathResolver::basename("/foo/bar/file.txt");  // "file.txt"
auto parts = PathResolver::split("/foo/bar/baz");  // ["foo", "bar", "baz"]
```

## Mount Options

```cpp
FilesystemMount::MountOptions options;
options.readOnly = true;
options.noExec = true;
options.noSuid = true;

manager.mount("/mnt", disk, fs, options);
```

## Statistics

### Device Statistics
```cpp
std::cout << disk->getStatistics();
```

### Manager Statistics
```cpp
std::cout << manager.getStatistics();
```

### Mount Information
```cpp
std::cout << manager.getMountInfo();
```

## Error Handling

```cpp
// Check return values
if (!manager.mount("/mnt", disk, fs)) {
    std::cerr << "Mount failed\n";
}

// Check file operations
int64_t bytesRead = manager.readFile(path, buffer, size, offset);
if (bytesRead < 0) {
    std::cerr << "Read failed\n";
}

// Check existence before operations
if (!manager.exists(path)) {
    std::cerr << "File not found\n";
}
```

## Complete Example

```cpp
#include "VirtualDiskManager.h"
#include "SimpleReadOnlyFS.h"
#include <iostream>
#include <vector>
#include <map>

int main() {
    VirtualDiskManager manager;
    
    // Create disk
    auto disk = manager.createMemoryDisk("disk0", 2 * 1024 * 1024, 512);
    
    // Create files
    std::map<std::string, std::vector<uint8_t>> files;
    
    std::string readme = "# My Virtual Disk\nThis is a test.\n";
    files["README.md"] = std::vector<uint8_t>(readme.begin(), readme.end());
    
    std::string config = "key=value\n";
    files["config.ini"] = std::vector<uint8_t>(config.begin(), config.end());
    
    // Create filesystem
    SimpleReadOnlyFS::createFilesystem(disk, files, 512);
    
    // Mount
    auto fs = std::make_shared<SimpleReadOnlyFS>();
    manager.mount("/data", disk, fs);
    
    // List files
    std::vector<DirectoryEntry> entries;
    if (manager.listDirectory("/data", entries)) {
        std::cout << "Files:\n";
        for (const auto& entry : entries) {
            std::cout << "  " << entry.name << " - " << entry.size << " bytes\n";
        }
    }
    
    // Read file
    std::vector<uint8_t> content;
    if (manager.readFileAll("/data/README.md", content)) {
        std::cout << "\nREADME.md:\n";
        std::cout << std::string(content.begin(), content.end());
    }
    
    // Statistics
    std::cout << "\n" << manager.getStatistics();
    
    return 0;
}
```

## Key Classes

| Class | Purpose |
|-------|---------|
| `VirtualBlockDevice` | Low-level block device with sector I/O |
| `SimpleReadOnlyFS` | Simple read-only filesystem |
| `FilesystemMount` | Mount point management |
| `PathResolver` | Path normalization and sandboxing |
| `VirtualDiskManager` | High-level orchestration and file API |

## Header Files to Include

```cpp
#include "VirtualBlockDevice.h"      // For low-level block I/O
#include "VirtualDiskManager.h"      // For high-level file operations
#include "SimpleReadOnlyFS.h"        // For creating filesystems
#include "IFilesystem.h"             // For filesystem interface
#include "FilesystemMount.h"         // For mount options
#include "PathResolver.h"            // For path utilities
```

## Best Practices

1. **Always check return values** - Most operations return bool or int64_t (-1 on error)
2. **Use VirtualDiskManager** - Higher-level API is safer and easier
3. **Validate paths** - Use PathResolver for untrusted input
4. **Set proper mount options** - Use read-only when appropriate
5. **Check existence first** - Avoid errors by checking `exists()` before operations
6. **Handle errors gracefully** - Check for nullptr, -1, false returns

## Limitations (Current Implementation)

- SimpleReadOnlyFS is **read-only** (no writes)
- Single-level directories only (no subdirectories)
- No symbolic links
- No permissions enforcement (read-only for all)
- No timestamps (placeholders only)
- Maximum filename: 47 characters
- Block-aligned I/O is most efficient

## Building and Running

```bash
# Configure
cmake -B build -S .

# Build
cmake --build build

# Run example
./build/bin/virtual_disk_example
```
