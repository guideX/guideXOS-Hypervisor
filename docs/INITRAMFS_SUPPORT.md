# Virtual Initramfs Support

## Overview

The Virtual Initramfs feature provides support for loading initial ramdisk (initramfs) images into the virtual machine's memory and exposing them to the guest kernel during boot. This is essential for booting Linux kernels that require an initramfs for early userspace initialization.

## Features

- **Load from File or Memory**: Create initramfs devices from disk files or in-memory buffers
- **Physical Address Mapping**: Control where the initramfs is loaded in VM physical memory
- **Format Detection**: Automatic detection of initramfs formats (cpio, cpio.gz, cpio.xz, etc.)
- **Boot Protocol Integration**: Seamlessly integrates with IA-64 Linux kernel boot protocol
- **Read-Only Access**: Initramfs is exposed as a read-only storage device
- **Memory Validation**: Verify initramfs integrity before and after loading

## Architecture

### Components

1. **InitramfsDevice**: Storage device implementation for initramfs
2. **KernelBootstrapConfig**: Extended with initramfs address/size fields
3. **WriteKernelBootParameters**: Helper to write boot parameters including initramfs info
4. **BootConfiguration**: VM configuration with initrdPath support

### Memory Layout

```
Physical Memory Layout:
+------------------+
| 0x000000         | Reserved/BIOS
| ...              |
+------------------+
| 0x100000         | Kernel Image (typical)
| ...              |
+------------------+
| 0x2000000        | Initramfs (default, 32 MB)
| ...              |
+------------------+
| Higher addresses |
+------------------+
```

### Boot Parameter Structure

The initramfs information is passed to the kernel via a boot parameter structure in memory:

```cpp
struct BootParameters {
    uint64_t command_line;        // 0x00: Command line address
    uint32_t command_line_size;   // 0x08: Command line size
    uint32_t reserved1;           // 0x0C: Padding
    uint64_t initramfs_start;     // 0x10: Initramfs physical address
    uint64_t initramfs_size;      // 0x18: Initramfs size in bytes
    uint64_t memory_map;          // 0x20: Memory map address
    uint64_t memory_map_size;     // 0x28: Memory map size
    uint64_t efi_system_table;    // 0x30: EFI system table
    uint64_t reserved2;           // 0x38: Reserved
};
```

The address of this structure is passed to the kernel in register r28.

## Usage

### Basic Usage

```cpp
#include "InitramfsDevice.h"
#include "bootstrap.h"
#include "memory.h"

// Create initramfs device from file
auto initramfs = std::make_unique<InitramfsDevice>(
    "initramfs0",
    "/path/to/initramfs.cpio.gz",
    0x2000000  // Load at 32 MB
);

// Load into VM memory
Memory memory;
if (!initramfs->loadIntoMemory(memory)) {
    std::cerr << "Failed to load initramfs into memory" << std::endl;
    return false;
}

// Configure kernel bootstrap
KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.globalPointer = 0x600000;
config.bootParamAddress = 0x10000;
config.initramfsAddress = initramfs->getPhysicalAddress();
config.initramfsSize = initramfs->getSize();

// Write boot parameters to memory
WriteKernelBootParameters(memory, config.bootParamAddress, config);

// Initialize kernel state
CPUState cpu;
InitializeKernelBootstrapState(cpu, memory, config);
```

### Creating from Memory Buffer

```cpp
// Load initramfs from memory buffer
std::vector<uint8_t> initramfsData = LoadFromSomewhere();

auto initramfs = std::make_unique<InitramfsDevice>(
    "initramfs0",
    initramfsData.data(),
    initramfsData.size(),
    0x2000000
);
```

### Using with VMConfiguration

```cpp
VMConfiguration vmConfig;
vmConfig.name = "my-vm";
vmConfig.boot.kernelPath = "/path/to/kernel";
vmConfig.boot.initrdPath = "/path/to/initramfs.cpio.gz";
vmConfig.boot.bootArgs = "console=ttyS0 root=/dev/ram0";
vmConfig.boot.directBoot = true;

// The VMManager will automatically create and load the initramfs
```

### Format Detection

```cpp
InitramfsDevice device("initramfs0", "/path/to/initramfs.img");

if (!device.validateFormat()) {
    std::cerr << "Invalid initramfs format" << std::endl;
    return false;
}

std::string format = device.getFormatType();
std::cout << "Detected format: " << format << std::endl;
// Outputs: "cpio-newc", "cpio.gz", "cpio.xz", etc.
```

### Custom Physical Address

```cpp
// Different architectures may prefer different addresses
InitramfsDevice device("initramfs0", "/path/to/initramfs.img");

#ifdef IA64
    device.setPhysicalAddress(InitramfsDevice::DEFAULT_IA64_ADDRESS);
#elif X86_64
    device.setPhysicalAddress(InitramfsDevice::DEFAULT_X86_64_ADDRESS);
#elif ARM64
    device.setPhysicalAddress(InitramfsDevice::DEFAULT_ARM64_ADDRESS);
#endif

device.loadIntoMemory(memory);
```

## Supported Formats

The InitramfsDevice can detect and handle the following formats:

1. **CPIO Archives**
   - Old ASCII format (magic: "070707")
   - New ASCII format (magic: "070701")
   - CRC format (magic: "070702")

2. **Compressed Formats**
   - Gzip-compressed CPIO (.cpio.gz)
   - XZ-compressed CPIO (.cpio.xz)
   - LZMA-compressed CPIO (.cpio.lzma)

Note: The device stores the compressed data as-is. The kernel is responsible for decompression.

## Physical Address Selection

Default addresses for different architectures:

| Architecture | Default Address | Hex        | Decimal  |
|-------------|-----------------|------------|----------|
| IA-64       | 32 MB           | 0x2000000  | 33554432 |
| x86-64      | 32 MB           | 0x2000000  | 33554432 |
| ARM64       | 64 MB           | 0x4000000  | 67108864 |

### Address Constraints

- Must be page-aligned (typically 4 KB)
- Should not overlap with kernel image
- Should be within physical memory range
- Should leave room for kernel BSS and early allocations

## API Reference

### InitramfsDevice Class

#### Constructors

```cpp
// Create from file
InitramfsDevice(const std::string& deviceId,
                const std::string& imagePath,
                uint64_t physicalAddress = 0,
                uint32_t blockSize = 4096);

// Create from memory buffer
InitramfsDevice(const std::string& deviceId,
                const uint8_t* buffer,
                uint64_t size,
                uint64_t physicalAddress = 0,
                uint32_t blockSize = 4096);
```

#### Key Methods

```cpp
// Load initramfs into VM memory
bool loadIntoMemory(Memory& memory);

// Get/set physical address
uint64_t getPhysicalAddress() const;
void setPhysicalAddress(uint64_t address);

// Format detection
bool validateFormat() const;
std::string getFormatType() const;

// Check load status
bool isLoadedInMemory() const;

// Get device information
StorageDeviceInfo getInfo() const;
std::string getStatistics() const;
```

### KernelBootstrapConfig Extension

```cpp
struct KernelBootstrapConfig {
    // ... existing fields ...
    
    // Initramfs configuration
    uint64_t initramfsAddress;  // Physical address of initramfs
    uint64_t initramfsSize;     // Size of initramfs in bytes
    
    // ... other fields ...
};
```

### WriteKernelBootParameters Function

```cpp
// Write boot parameters to memory
uint64_t WriteKernelBootParameters(
    MemorySystem& memory,
    uint64_t address,
    const KernelBootstrapConfig& config
);
```

## Examples

### Example 1: Simple Direct Boot

```cpp
#include "VirtualMachine.h"
#include "InitramfsDevice.h"

void BootWithInitramfs() {
    // Create VM
    VirtualMachine vm;
    vm.Initialize(256 * 1024 * 1024);  // 256 MB RAM
    
    // Load kernel (ELF file)
    uint64_t kernelEntry = LoadKernelELF(vm.GetMemory(), "/path/to/vmlinux");
    
    // Create and load initramfs
    auto initramfs = std::make_unique<InitramfsDevice>(
        "initramfs0",
        "/path/to/initramfs.cpio.gz",
        0x2000000
    );
    initramfs->loadIntoMemory(vm.GetMemory());
    
    // Setup boot parameters
    KernelBootstrapConfig config;
    config.entryPoint = kernelEntry;
    config.globalPointer = 0x600000;  // From ELF
    config.bootParamAddress = 0x10000;
    config.initramfsAddress = initramfs->getPhysicalAddress();
    config.initramfsSize = initramfs->getSize();
    config.commandLineAddress = 0x11000;
    
    // Write command line
    std::string cmdline = "console=ttyS0 root=/dev/ram0 rw";
    WriteStringToMemory(vm.GetMemory(), config.commandLineAddress, cmdline);
    
    // Write boot parameters
    WriteKernelBootParameters(vm.GetMemory(), config.bootParamAddress, config);
    
    // Initialize CPU state
    InitializeKernelBootstrapState(vm.GetCPU(0), vm.GetMemory(), config);
    
    // Start VM
    vm.Run();
}
```

### Example 2: Multi-Stage Boot

```cpp
void MultiStageBoot() {
    Memory memory;
    CPUState cpu;
    
    // Stage 1: Load bootloader
    uint64_t bootloaderEntry = LoadBootloader(memory, 0x100000);
    
    // Stage 2: Prepare kernel and initramfs
    uint64_t kernelAddr = 0x200000;  // 2 MB
    uint64_t initramfsAddr = 0x2000000;  // 32 MB
    
    LoadKernel(memory, kernelAddr);
    
    auto initramfs = std::make_unique<InitramfsDevice>(
        "initramfs0",
        "/path/to/initramfs.cpio.gz",
        initramfsAddr
    );
    initramfs->loadIntoMemory(memory);
    
    // Stage 3: Bootloader will jump to kernel
    // Prepare boot info structure for bootloader
    BootInfoStruct info;
    info.kernelAddress = kernelAddr;
    info.initramfsAddress = initramfsAddr;
    info.initramfsSize = initramfs->getSize();
    WriteBootInfo(memory, 0x8000, info);
    
    // Start at bootloader
    cpu.SetIP(bootloaderEntry);
}
```

### Example 3: Testing with Custom Initramfs

```cpp
void TestCustomInitramfs() {
    // Create a custom initramfs in memory
    std::vector<uint8_t> customInitramfs;
    
    // Build CPIO archive programmatically
    CPIOArchiveBuilder builder;
    builder.addFile("init", 0755, "#!/bin/sh\necho Hello from initramfs\n");
    builder.addFile("etc/config", 0644, "key=value\n");
    customInitramfs = builder.build();
    
    // Create device
    auto initramfs = std::make_unique<InitramfsDevice>(
        "custom_initramfs",
        customInitramfs.data(),
        customInitramfs.size(),
        0x2000000
    );
    
    // Validate format
    assert(initramfs->validateFormat());
    assert(initramfs->getFormatType() == "cpio-newc");
    
    // Use in VM
    Memory memory;
    initramfs->loadIntoMemory(memory);
}
```

## Integration with VM Manager

The VMManager automatically handles initramfs when configured via BootConfiguration:

```cpp
VMManager manager;

VMConfiguration config;
config.name = "linux-vm";
config.cpu.cpuCount = 4;
config.memory.memorySize = 512 * 1024 * 1024;

// Configure boot with initramfs
config.boot.kernelPath = "/boot/vmlinux";
config.boot.initrdPath = "/boot/initramfs.cpio.gz";
config.boot.bootArgs = "console=ttyS0 root=/dev/ram0";
config.boot.directBoot = true;

std::string vmId = manager.createVM(config);
manager.startVM(vmId);
```

Behind the scenes, VMManager will:
1. Create an InitramfsDevice from the initrdPath
2. Load it into VM memory at the default address
3. Configure the boot parameters with initramfs info
4. Initialize the kernel bootstrap state

## Best Practices

### 1. Size Considerations

```cpp
// Check initramfs size before loading
auto initramfs = std::make_unique<InitramfsDevice>("initramfs0", path);
if (initramfs->getSize() > 128 * 1024 * 1024) {  // 128 MB limit
    std::cerr << "Warning: Large initramfs may impact boot time" << std::endl;
}
```

### 2. Address Planning

```cpp
// Plan memory layout to avoid overlaps
uint64_t kernelStart = 0x100000;   // 1 MB
uint64_t kernelSize = 8 * 1024 * 1024;  // 8 MB
uint64_t initramfsStart = kernelStart + kernelSize + PAGE_SIZE;
initramfsStart = (initramfsStart + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);  // Align
```

### 3. Error Handling

```cpp
try {
    auto initramfs = std::make_unique<InitramfsDevice>("initramfs0", path);
    
    if (!initramfs->validateFormat()) {
        throw std::runtime_error("Invalid initramfs format");
    }
    
    if (!initramfs->loadIntoMemory(memory)) {
        throw std::runtime_error("Failed to load initramfs into memory");
    }
    
} catch (const std::exception& e) {
    std::cerr << "Initramfs error: " << e.what() << std::endl;
    // Fallback to diskless boot or return error
}
```

### 4. Verification

```cpp
// Verify initramfs loaded correctly
auto initramfs = std::make_unique<InitramfsDevice>("initramfs0", path);
initramfs->loadIntoMemory(memory);

// Read back and verify magic bytes
uint8_t magic[6];
for (size_t i = 0; i < 6; ++i) {
    magic[i] = memory.read<uint8_t>(initramfs->getPhysicalAddress() + i);
}

if (memcmp(magic, "070701", 6) != 0) {
    std::cerr << "Error: Initramfs not loaded correctly" << std::endl;
}
```

## Troubleshooting

### Initramfs Not Found by Kernel

**Symptoms**: Kernel boots but cannot find initramfs

**Solutions**:
- Verify initramfsAddress and initramfsSize are correctly written to boot parameters
- Check that boot parameter address is passed in r28
- Ensure initramfs is loaded before starting the kernel
- Verify physical address doesn't overlap with kernel

### Invalid Format Error

**Symptoms**: validateFormat() returns false

**Solutions**:
- Check file is a valid CPIO archive
- Verify file is not corrupted
- Ensure compressed format is supported by kernel
- Try uncompressed CPIO format

### Memory Overlap

**Symptoms**: Kernel crashes or initramfs data corrupted

**Solutions**:
- Use getPhysicalAddress() to verify address
- Ensure sufficient gap between kernel and initramfs
- Check kernel BSS section doesn't overlap
- Plan memory layout before loading

## Performance Considerations

- **Loading Time**: Larger initramfs files take longer to load into memory
- **Memory Usage**: Initramfs occupies physical memory until kernel extracts it
- **Compression**: Kernel must decompress during boot (tradeoff: smaller file vs. CPU time)

## Limitations

1. **Read-Only**: Initramfs is always read-only once loaded
2. **No Hot-Reload**: Cannot change initramfs after VM is running
3. **Format Support**: Only detects format; kernel must support decompression
4. **Size Limits**: Limited by available physical memory

## Future Enhancements

- Automatic compression/decompression
- Support for multiple initramfs images (layered)
- Dynamic initramfs modification before boot
- Integration with snapshot/restore functionality
- Initramfs caching for faster VM creation

## See Also

- [Kernel Bootstrap Documentation](BOOTSTRAP_INITIALIZATION.md)
- [VM Configuration Schema](VM_CONFIGURATION_SCHEMA.md)
- [Storage Devices](STORAGE_DEVICES.md)
- [Memory Management](MMU_IMPLEMENTATION.md)
