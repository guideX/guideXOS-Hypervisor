# Initramfs Quick Reference

## Quick Start

### Load Initramfs from File
```cpp
#include "InitramfsDevice.h"

auto initramfs = std::make_unique<InitramfsDevice>(
    "initramfs0",
    "/path/to/initramfs.cpio.gz",
    0x2000000  // 32 MB
);
initramfs->loadIntoMemory(memory);
```

### Boot Kernel with Initramfs
```cpp
KernelBootstrapConfig config;
config.entryPoint = kernelEntry;
config.bootParamAddress = 0x10000;
config.initramfsAddress = initramfs->getPhysicalAddress();
config.initramfsSize = initramfs->getSize();

WriteKernelBootParameters(memory, config.bootParamAddress, config);
InitializeKernelBootstrapState(cpu, memory, config);
```

## Common Patterns

### Pattern: VM Configuration
```cpp
VMConfiguration vmConfig;
vmConfig.boot.kernelPath = "/boot/vmlinux";
vmConfig.boot.initrdPath = "/boot/initramfs.cpio.gz";
vmConfig.boot.directBoot = true;
```

### Pattern: Custom Physical Address
```cpp
InitramfsDevice device("initramfs0", path);
device.setPhysicalAddress(0x4000000);  // 64 MB
device.loadIntoMemory(memory);
```

### Pattern: Format Validation
```cpp
if (!device.validateFormat()) {
    std::cerr << "Invalid format" << std::endl;
}
std::cout << "Format: " << device.getFormatType() << std::endl;
```

## Default Addresses

| Architecture | Address | Constant |
|-------------|---------|----------|
| IA-64 | 0x2000000 (32 MB) | `InitramfsDevice::DEFAULT_IA64_ADDRESS` |
| x86-64 | 0x2000000 (32 MB) | `InitramfsDevice::DEFAULT_X86_64_ADDRESS` |
| ARM64 | 0x4000000 (64 MB) | `InitramfsDevice::DEFAULT_ARM64_ADDRESS` |

## Boot Parameter Offsets

| Field | Offset | Type |
|-------|--------|------|
| command_line | 0x00 | uint64_t |
| command_line_size | 0x08 | uint32_t |
| initramfs_start | 0x10 | uint64_t |
| initramfs_size | 0x18 | uint64_t |
| memory_map | 0x20 | uint64_t |
| efi_system_table | 0x30 | uint64_t |

## Supported Formats

- `cpio-old` - ASCII format (magic: 070707)
- `cpio-newc` - New ASCII (magic: 070701)
- `cpio-crc` - CRC format (magic: 070702)
- `cpio.gz` - Gzip compressed
- `cpio.xz` - XZ compressed
- `cpio.lzma` - LZMA compressed

## Key Methods

```cpp
class InitramfsDevice {
    // Load into VM memory
    bool loadIntoMemory(Memory& memory);
    
    // Get/set physical address
    uint64_t getPhysicalAddress() const;
    void setPhysicalAddress(uint64_t address);
    
    // Format detection
    bool validateFormat() const;
    std::string getFormatType() const;
    
    // Status
    bool isLoadedInMemory() const;
    bool isConnected() const;
    
    // Information
    uint64_t getSize() const;
    std::string getStatistics() const;
};
```

## Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| "Failed to load" | File not found | Check path |
| validateFormat() = false | Invalid format | Verify CPIO archive |
| Kernel can't find initramfs | Not in boot params | Call WriteKernelBootParameters() |
| Memory corruption | Address overlap | Plan memory layout |

## Minimal Example

```cpp
// Complete minimal example
#include "InitramfsDevice.h"
#include "bootstrap.h"
#include "memory.h"

Memory memory;
CPUState cpu;

// Load initramfs
auto initramfs = std::make_unique<InitramfsDevice>(
    "initramfs0", "/boot/initramfs.cpio.gz", 0x2000000
);
initramfs->loadIntoMemory(memory);

// Configure boot
KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.bootParamAddress = 0x10000;
config.initramfsAddress = initramfs->getPhysicalAddress();
config.initramfsSize = initramfs->getSize();

// Initialize
WriteKernelBootParameters(memory, config.bootParamAddress, config);
InitializeKernelBootstrapState(cpu, memory, config);
```

## See Also

- [Full Documentation](INITRAMFS_SUPPORT.md)
- [Kernel Bootstrap](BOOTSTRAP_INITIALIZATION.md)
- [VM Configuration](VM_CONFIGURATION_SCHEMA.md)
