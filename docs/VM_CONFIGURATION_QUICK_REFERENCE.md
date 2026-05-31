# VM Configuration Quick Reference

## Creating Configurations

### Minimal VM (16 MB RAM, 1 CPU)
```cpp
VMConfiguration config = VMConfiguration::createMinimal("my-vm");
```

### Standard VM (256 MB RAM, 2 CPUs, debugging enabled)
```cpp
VMConfiguration config = VMConfiguration::createStandard("my-vm", 256);
```

### Custom VM
```cpp
VMConfiguration config;
config.name = "custom-vm";
config.description = "My custom VM";
config.cpu.cpuCount = 4;
config.memory.memorySize = 512 * 1024 * 1024;  // 512 MB
config.enableDebugger = true;
```

## Adding Components

### Add Storage Device
```cpp
StorageConfiguration disk("disk0", "/path/to/disk.img");
disk.sizeBytes = 10ULL * 1024 * 1024 * 1024;  // 10 GB
disk.blockSize = 4096;
config.addStorageDevice(disk);
```

### Add Network Interface
```cpp
NetworkConfiguration eth0;
eth0.interfaceId = "eth0";
eth0.interfaceType = "bridge";
eth0.macAddress = "52:54:00:12:34:56";
eth0.enableDHCP = false;
eth0.ipAddress = "192.168.1.100";
eth0.gateway = "192.168.1.1";
config.addNetworkInterface(eth0);
```

### Configure Boot
```cpp
config.boot.bootDevice = "disk0";
config.boot.directBoot = true;
config.boot.kernelPath = "/boot/vmlinuz";
config.boot.initrdPath = "/boot/initrd.img";
config.boot.bootArgs = "root=/dev/sda1 console=ttyS0";
```

## File Operations

### Save to File
```cpp
std::string error;
if (!config.saveToFile("vm-config.json", &error)) {
    std::cerr << "Save failed: " << error << "\n";
}
```

### Load from File
```cpp
std::string error;
VMConfiguration config = VMConfiguration::loadFromFile("vm-config.json", &error);
if (config.name.empty()) {
    std::cerr << "Load failed: " << error << "\n";
}
```

### Serialize to JSON String
```cpp
std::string json = config.toJson();
```

### Deserialize from JSON String
```cpp
VMConfiguration config = VMConfiguration::fromJson(jsonString);
```

## Validation

```cpp
std::string error;
if (!config.validate(&error)) {
    std::cerr << "Validation failed: " << error << "\n";
}
```

## Configuration Summary

```cpp
std::cout << config.getResourceSummary();
```

Output:
```
VM: my-vm
  CPUs: 4 x IA-64
  Memory: 512 MB
  Storage: 2 device(s)
  Network: 1 interface(s)
```

## Common Memory Sizes

```cpp
config.memory.memorySize = 16 * 1024 * 1024;        // 16 MB
config.memory.memorySize = 64 * 1024 * 1024;        // 64 MB
config.memory.memorySize = 256 * 1024 * 1024;       // 256 MB
config.memory.memorySize = 512 * 1024 * 1024;       // 512 MB
config.memory.memorySize = 1024 * 1024 * 1024;      // 1 GB
config.memory.memorySize = 2ULL * 1024 * 1024 * 1024;   // 2 GB
config.memory.memorySize = 4ULL * 1024 * 1024 * 1024;   // 4 GB
```

## Common Disk Sizes

```cpp
disk.sizeBytes = 1ULL * 1024 * 1024 * 1024;         // 1 GB
disk.sizeBytes = 10ULL * 1024 * 1024 * 1024;        // 10 GB
disk.sizeBytes = 50ULL * 1024 * 1024 * 1024;        // 50 GB
disk.sizeBytes = 100ULL * 1024 * 1024 * 1024;       // 100 GB
disk.sizeBytes = 500ULL * 1024 * 1024 * 1024;       // 500 GB
disk.sizeBytes = 1024ULL * 1024 * 1024 * 1024;      // 1 TB
```

## Clock Frequencies

```cpp
config.cpu.clockFrequency = 0;                      // Unlimited
config.cpu.clockFrequency = 1000000000;             // 1 GHz
config.cpu.clockFrequency = 2000000000;             // 2 GHz
config.cpu.clockFrequency = 2500000000;             // 2.5 GHz
config.cpu.clockFrequency = 3000000000;             // 3 GHz
```

## Complete Example

```cpp
#include "VMConfiguration.h"
#include <iostream>

int main() {
    // Create configuration
    VMConfiguration config = VMConfiguration::createStandard("web-server", 512);
    config.description = "Production web server";
    
    // Configure CPUs
    config.cpu.cpuCount = 4;
    config.cpu.clockFrequency = 2000000000;  // 2 GHz
    
    // Add system disk
    StorageConfiguration systemDisk("disk0", "/vm/web-system.img");
    systemDisk.sizeBytes = 20ULL * 1024 * 1024 * 1024;  // 20 GB
    systemDisk.blockSize = 4096;
    config.addStorageDevice(systemDisk);
    
    // Add data disk
    StorageConfiguration dataDisk("disk1", "/vm/web-data.img");
    dataDisk.sizeBytes = 100ULL * 1024 * 1024 * 1024;  // 100 GB
    dataDisk.blockSize = 4096;
    config.addStorageDevice(dataDisk);
    
    // Configure network
    NetworkConfiguration eth0;
    eth0.interfaceType = "bridge";
    eth0.macAddress = "52:54:00:12:34:56";
    eth0.enableDHCP = false;
    eth0.ipAddress = "192.168.1.100";
    eth0.gateway = "192.168.1.1";
    config.addNetworkInterface(eth0);
    
    // Configure boot
    config.boot.bootDevice = "disk0";
    config.boot.directBoot = true;
    config.boot.kernelPath = "/boot/vmlinuz";
    config.boot.bootArgs = "root=/dev/sda1";
    
    // Validate
    std::string error;
    if (!config.validate(&error)) {
        std::cerr << "Invalid configuration: " << error << "\n";
        return 1;
    }
    
    // Display summary
    std::cout << config.getResourceSummary();
    
    // Save to file
    if (!config.saveToFile("web-server.json", &error)) {
        std::cerr << "Failed to save: " << error << "\n";
        return 1;
    }
    
    std::cout << "Configuration saved successfully!\n";
    return 0;
}
```

## See Also

- `docs/VM_CONFIGURATION_SCHEMA.md` - Complete schema documentation
- `examples/config_example.cpp` - Extended examples
- `tests/test_vmconfig_serialization.cpp` - Test suite
