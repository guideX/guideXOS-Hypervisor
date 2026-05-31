# VM Configuration Schema

## Overview

The IA-64 Hypervisor uses a comprehensive JSON-based configuration schema to define virtual machine settings. This document describes the complete configuration structure, all available options, and provides examples.

## Table of Contents

- [Configuration Structure](#configuration-structure)
- [CPU Configuration](#cpu-configuration)
- [Memory Configuration](#memory-configuration)
- [Storage Configuration](#storage-configuration)
- [Boot Configuration](#boot-configuration)
- [Network Configuration](#network-configuration)
- [VM Behavior Settings](#vm-behavior-settings)
- [File Operations](#file-operations)
- [Validation](#validation)
- [Examples](#examples)

## Configuration Structure

The top-level VM configuration contains the following sections:

```json
{
  "name": "string",                    // VM name (required, unique)
  "description": "string",             // Human-readable description
  "uuid": "string",                    // UUID (auto-generated if empty)
  "cpu": { ... },                      // CPU configuration
  "memory": { ... },                   // Memory configuration
  "storageDevices": [ ... ],           // Array of storage devices
  "boot": { ... },                     // Boot configuration
  "networkInterfaces": [ ... ],        // Array of network interfaces
  "enableDebugger": boolean,           // Enable debugging features
  "enableSnapshots": boolean,          // Enable snapshot support
  "autoStart": boolean,                // Auto-start VM on manager startup
  "maxCycles": number                  // Maximum execution cycles (0 = unlimited)
}
```

## CPU Configuration

Defines the virtual CPU(s) for the VM.

### Schema

```json
{
  "cpu": {
    "isaType": "string",               // ISA architecture
    "cpuCount": number,                // Number of virtual CPUs
    "clockFrequency": number,          // Clock frequency in Hz (0 = unlimited)
    "enableProfiling": boolean         // Enable performance profiling
  }
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `isaType` | string | Yes | "IA-64" | Instruction set architecture (e.g., "IA-64", "x86-64", "ARM64") |
| `cpuCount` | number | Yes | 1 | Number of virtual CPUs (1-256) |
| `clockFrequency` | number | No | 0 | CPU clock frequency in Hz (0 = unlimited) |
| `enableProfiling` | boolean | No | false | Enable CPU performance profiling |

### Example

```json
{
  "cpu": {
    "isaType": "IA-64",
    "cpuCount": 4,
    "clockFrequency": 2000000000,
    "enableProfiling": true
  }
}
```

## Memory Configuration

Defines RAM and memory management settings.

### Schema

```json
{
  "memory": {
    "memorySize": number,              // Total RAM in bytes
    "enableMMU": boolean,              // Enable Memory Management Unit
    "enablePaging": boolean,           // Enable virtual memory paging
    "pageSize": number                 // Page size in bytes
  }
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `memorySize` | number | Yes | 16777216 | Total RAM in bytes (minimum 1 MB) |
| `enableMMU` | boolean | No | true | Enable Memory Management Unit |
| `enablePaging` | boolean | No | false | Enable virtual memory paging |
| `pageSize` | number | No | 4096 | Page size in bytes (typically 4096, 16384, or 65536) |

### Example

```json
{
  "memory": {
    "memorySize": 268435456,
    "enableMMU": true,
    "enablePaging": true,
    "pageSize": 4096
  }
}
```

### Memory Size Reference

- 16 MB: `16777216`
- 64 MB: `67108864`
- 256 MB: `268435456`
- 512 MB: `536870912`
- 1 GB: `1073741824`
- 2 GB: `2147483648`
- 4 GB: `4294967296`

## Storage Configuration

Defines virtual storage devices (disks, CD-ROMs, etc.).

### Schema

```json
{
  "storageDevices": [
    {
      "deviceId": "string",            // Unique device identifier
      "deviceType": "string",          // Device type
      "imagePath": "string",           // Path to disk image
      "readOnly": boolean,             // Read-only access
      "sizeBytes": number,             // Device size in bytes
      "blockSize": number              // Block size in bytes
    }
  ]
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `deviceId` | string | Yes | "disk0" | Unique device identifier |
| `deviceType` | string | No | "raw" | Device type ("raw", "qcow2", "vdi", etc.) |
| `imagePath` | string | Yes | "" | Path to disk image file |
| `readOnly` | boolean | No | false | Mount device as read-only |
| `sizeBytes` | number | No | 0 | Device size in bytes (0 = auto-detect from image) |
| `blockSize` | number | No | 512 | Block size in bytes (512, 4096, etc.) |

### Example

```json
{
  "storageDevices": [
    {
      "deviceId": "disk0",
      "deviceType": "raw",
      "imagePath": "/path/to/system.img",
      "readOnly": false,
      "sizeBytes": 10737418240,
      "blockSize": 4096
    },
    {
      "deviceId": "cdrom0",
      "deviceType": "raw",
      "imagePath": "/path/to/install.iso",
      "readOnly": true,
      "sizeBytes": 0,
      "blockSize": 2048
    }
  ]
}
```

## Boot Configuration

Defines boot device and kernel loading options.

### Schema

```json
{
  "boot": {
    "bootDevice": "string",            // Boot device ID
    "kernelPath": "string",            // Kernel image path (direct boot)
    "initrdPath": "string",            // Initial ramdisk path
    "bootArgs": "string",              // Kernel boot arguments
    "entryPoint": number,              // Entry point address
    "directBoot": boolean              // Skip bootloader
  }
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `bootDevice` | string | Yes | "disk0" | Boot device ID (must match a storage device) |
| `kernelPath` | string | No | "" | Path to kernel image (for direct boot) |
| `initrdPath` | string | No | "" | Path to initial ramdisk |
| `bootArgs` | string | No | "" | Kernel command-line arguments |
| `entryPoint` | number | No | 0 | Entry point address (0 = auto-detect) |
| `directBoot` | boolean | No | false | Boot kernel directly (skip bootloader) |

### Example - Bootloader Boot

```json
{
  "boot": {
    "bootDevice": "disk0",
    "kernelPath": "",
    "initrdPath": "",
    "bootArgs": "",
    "entryPoint": 0,
    "directBoot": false
  }
}
```

### Example - Direct Kernel Boot

```json
{
  "boot": {
    "bootDevice": "disk0",
    "kernelPath": "/boot/vmlinuz",
    "initrdPath": "/boot/initrd.img",
    "bootArgs": "root=/dev/sda1 console=ttyS0 debug",
    "entryPoint": 0,
    "directBoot": true
  }
}
```

## Network Configuration

Defines virtual network interfaces.

### Schema

```json
{
  "networkInterfaces": [
    {
      "interfaceId": "string",         // Interface identifier
      "interfaceType": "string",       // Interface type
      "macAddress": "string",          // MAC address
      "enableDHCP": boolean,           // Enable DHCP
      "ipAddress": "string",           // Static IP address
      "gateway": "string"              // Gateway address
    }
  ]
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `interfaceId` | string | Yes | "eth0" | Network interface identifier |
| `interfaceType` | string | No | "user" | Interface type ("user", "tap", "bridge") |
| `macAddress` | string | No | "" | MAC address (empty = auto-generate) |
| `enableDHCP` | boolean | No | true | Enable DHCP for address assignment |
| `ipAddress` | string | No | "" | Static IP address (if DHCP disabled) |
| `gateway` | string | No | "" | Default gateway address |

### Example - DHCP Configuration

```json
{
  "networkInterfaces": [
    {
      "interfaceId": "eth0",
      "interfaceType": "user",
      "macAddress": "",
      "enableDHCP": true,
      "ipAddress": "",
      "gateway": ""
    }
  ]
}
```

### Example - Static IP Configuration

```json
{
  "networkInterfaces": [
    {
      "interfaceId": "eth0",
      "interfaceType": "bridge",
      "macAddress": "52:54:00:12:34:56",
      "enableDHCP": false,
      "ipAddress": "192.168.1.100",
      "gateway": "192.168.1.1"
    }
  ]
}
```

## VM Behavior Settings

Top-level settings that control VM behavior.

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `enableDebugger` | boolean | No | false | Enable debugging features (breakpoints, watchpoints, etc.) |
| `enableSnapshots` | boolean | No | true | Enable snapshot functionality |
| `autoStart` | boolean | No | false | Automatically start VM when manager starts |
| `maxCycles` | number | No | 0 | Maximum execution cycles (0 = unlimited) |

### Example

```json
{
  "enableDebugger": true,
  "enableSnapshots": true,
  "autoStart": false,
  "maxCycles": 0
}
```

## File Operations

### Saving Configuration

```cpp
VMConfiguration config = VMConfiguration::createStandard("my-vm", 256);

std::string error;
if (config.saveToFile("my-vm.json", &error)) {
    std::cout << "Configuration saved successfully\n";
} else {
    std::cerr << "Failed to save: " << error << "\n";
}
```

### Loading Configuration

```cpp
std::string error;
VMConfiguration config = VMConfiguration::loadFromFile("my-vm.json", &error);

if (config.name.empty()) {
    std::cerr << "Failed to load: " << error << "\n";
} else {
    std::cout << "Configuration loaded: " << config.name << "\n";
}
```

### JSON String Operations

```cpp
// Serialize to JSON string
std::string json = config.toJson();

// Deserialize from JSON string
VMConfiguration config = VMConfiguration::fromJson(json);
```

## Validation

All configurations are validated before use. Validation checks:

- VM name is not empty
- CPU count is between 1 and 256
- Memory size is at least 1 MB
- No duplicate storage device IDs
- All referenced boot devices exist

```cpp
std::string error;
if (config.validate(&error)) {
    std::cout << "Configuration is valid\n";
} else {
    std::cerr << "Validation failed: " << error << "\n";
}
```

## Examples

### Minimal VM

```json
{
  "name": "minimal-vm",
  "description": "Minimal IA-64 VM",
  "uuid": "",
  "cpu": {
    "isaType": "IA-64",
    "cpuCount": 1,
    "clockFrequency": 0,
    "enableProfiling": false
  },
  "memory": {
    "memorySize": 16777216,
    "enableMMU": true,
    "enablePaging": false,
    "pageSize": 4096
  },
  "storageDevices": [],
  "boot": {
    "bootDevice": "disk0",
    "kernelPath": "",
    "initrdPath": "",
    "bootArgs": "",
    "entryPoint": 0,
    "directBoot": false
  },
  "networkInterfaces": [],
  "enableDebugger": false,
  "enableSnapshots": true,
  "autoStart": false,
  "maxCycles": 0
}
```

### Development VM

```json
{
  "name": "dev-vm",
  "description": "Development VM with debugging enabled",
  "uuid": "",
  "cpu": {
    "isaType": "IA-64",
    "cpuCount": 2,
    "clockFrequency": 1000000000,
    "enableProfiling": true
  },
  "memory": {
    "memorySize": 268435456,
    "enableMMU": true,
    "enablePaging": true,
    "pageSize": 4096
  },
  "storageDevices": [
    {
      "deviceId": "disk0",
      "deviceType": "raw",
      "imagePath": "/vm/dev-disk.img",
      "readOnly": false,
      "sizeBytes": 10737418240,
      "blockSize": 4096
    }
  ],
  "boot": {
    "bootDevice": "disk0",
    "kernelPath": "",
    "initrdPath": "",
    "bootArgs": "",
    "entryPoint": 0,
    "directBoot": false
  },
  "networkInterfaces": [
    {
      "interfaceId": "eth0",
      "interfaceType": "user",
      "macAddress": "",
      "enableDHCP": true,
      "ipAddress": "",
      "gateway": ""
    }
  ],
  "enableDebugger": true,
  "enableSnapshots": true,
  "autoStart": false,
  "maxCycles": 0
}
```

### Production Server VM

```json
{
  "name": "prod-server",
  "description": "Production server VM with multiple disks",
  "uuid": "12345678-1234-1234-1234-123456789abc",
  "cpu": {
    "isaType": "IA-64",
    "cpuCount": 8,
    "clockFrequency": 2500000000,
    "enableProfiling": false
  },
  "memory": {
    "memorySize": 4294967296,
    "enableMMU": true,
    "enablePaging": true,
    "pageSize": 4096
  },
  "storageDevices": [
    {
      "deviceId": "disk0",
      "deviceType": "raw",
      "imagePath": "/vm/prod-system.img",
      "readOnly": false,
      "sizeBytes": 107374182400,
      "blockSize": 4096
    },
    {
      "deviceId": "disk1",
      "deviceType": "raw",
      "imagePath": "/vm/prod-data.img",
      "readOnly": false,
      "sizeBytes": 1099511627776,
      "blockSize": 4096
    }
  ],
  "boot": {
    "bootDevice": "disk0",
    "kernelPath": "/boot/vmlinuz-production",
    "initrdPath": "/boot/initrd-production.img",
    "bootArgs": "root=/dev/sda1 console=ttyS0",
    "entryPoint": 0,
    "directBoot": true
  },
  "networkInterfaces": [
    {
      "interfaceId": "eth0",
      "interfaceType": "bridge",
      "macAddress": "52:54:00:12:34:56",
      "enableDHCP": false,
      "ipAddress": "192.168.1.100",
      "gateway": "192.168.1.1"
    },
    {
      "interfaceId": "eth1",
      "interfaceType": "bridge",
      "macAddress": "52:54:00:12:34:57",
      "enableDHCP": false,
      "ipAddress": "10.0.0.100",
      "gateway": "10.0.0.1"
    }
  ],
  "enableDebugger": false,
  "enableSnapshots": true,
  "autoStart": true,
  "maxCycles": 0
}
```

## See Also

- `VMConfiguration.h` - C++ configuration structures
- `VMManager.h` - VM lifecycle management
- `examples/config_example.cpp` - Usage examples
- `tests/test_vmconfig_serialization.cpp` - Test suite
