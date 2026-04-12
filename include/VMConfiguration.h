#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <sstream>
#include <fstream>
#include "JsonUtils.h"

namespace ia64 {

// Forward declarations
class IStorageDevice;

/**
 * CPUConfiguration - CPU settings for a virtual machine
 */
struct CPUConfiguration {
    std::string isaType;        // ISA type (e.g., "IA-64", "x86-64", "ARM64")
    size_t cpuCount;            // Number of virtual CPUs
    uint64_t clockFrequency;    // CPU clock frequency in Hz (0 = unlimited)
    bool enableProfiling;       // Enable performance profiling
    
    CPUConfiguration()
        : isaType("IA-64")
        , cpuCount(1)
        , clockFrequency(0)
        , enableProfiling(false) {}
    
    CPUConfiguration(const std::string& isa, size_t count = 1)
        : isaType(isa)
        , cpuCount(count)
        , clockFrequency(0)
        , enableProfiling(false) {}
    
    // Serialization
    void toJson(json::JsonBuilder& builder) const {
        builder.beginObject();
        builder.writeString("isaType", isaType);
        builder.writeNumber("cpuCount", cpuCount);
        builder.writeNumber("clockFrequency", clockFrequency);
        builder.writeBool("enableProfiling", enableProfiling);
        builder.endObject();
    }
    
    static CPUConfiguration fromJson(json::JsonParser& parser) {
        CPUConfiguration config;
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            if (key == "isaType") {
                config.isaType = parser.readString();
            } else if (key == "cpuCount") {
                config.cpuCount = static_cast<size_t>(parser.readNumber());
            } else if (key == "clockFrequency") {
                config.clockFrequency = parser.readNumber();
            } else if (key == "enableProfiling") {
                config.enableProfiling = parser.readBool();
            }
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    static CPUConfiguration fromJson(const std::string& jsonStr) {
        json::JsonParser parser(jsonStr);
        return fromJson(parser);
    }
};

/**
 * MemoryConfiguration - Memory settings for a virtual machine
 */
struct MemoryConfiguration {
    size_t memorySize;          // Total memory size in bytes
    bool enableMMU;             // Enable Memory Management Unit
    bool enablePaging;          // Enable virtual memory paging
    size_t pageSize;            // Page size (typically 4KB, 16KB, or 64KB)
    
    MemoryConfiguration()
        : memorySize(16 * 1024 * 1024)  // 16 MB default
        , enableMMU(true)
        , enablePaging(false)
        , pageSize(4096) {}
    
    explicit MemoryConfiguration(size_t size)
        : memorySize(size)
        , enableMMU(true)
        , enablePaging(false)
        , pageSize(4096) {}
    
    // Serialization
    void toJson(json::JsonBuilder& builder) const {
        builder.beginObject();
        builder.writeNumber("memorySize", memorySize);
        builder.writeBool("enableMMU", enableMMU);
        builder.writeBool("enablePaging", enablePaging);
        builder.writeNumber("pageSize", pageSize);
        builder.endObject();
    }
    
    static MemoryConfiguration fromJson(json::JsonParser& parser) {
        MemoryConfiguration config;
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            if (key == "memorySize") {
                config.memorySize = static_cast<size_t>(parser.readNumber());
            } else if (key == "enableMMU") {
                config.enableMMU = parser.readBool();
            } else if (key == "enablePaging") {
                config.enablePaging = parser.readBool();
            } else if (key == "pageSize") {
                config.pageSize = static_cast<size_t>(parser.readNumber());
            }
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    static MemoryConfiguration fromJson(const std::string& jsonStr) {
        json::JsonParser parser(jsonStr);
        return fromJson(parser);
    }
};

/**
 * StorageConfiguration - Storage device configuration
 */
struct StorageConfiguration {
    std::string deviceId;       // Unique device identifier
    std::string deviceType;     // Device type ("raw", "qcow2", "vdi", etc.)
    std::string imagePath;      // Path to disk image file
    bool readOnly;              // Read-only access
    uint64_t sizeBytes;         // Device size in bytes (0 = auto-detect)
    uint32_t blockSize;         // Block size in bytes (default 512)
    
    StorageConfiguration()
        : deviceId("disk0")
        , deviceType("raw")
        , imagePath()
        , readOnly(false)
        , sizeBytes(0)
        , blockSize(512) {}
    
    StorageConfiguration(const std::string& id, const std::string& path)
        : deviceId(id)
        , deviceType("raw")
        , imagePath(path)
        , readOnly(false)
        , sizeBytes(0)
        , blockSize(512) {}
    
    // Serialization
    void toJson(json::JsonBuilder& builder) const {
        builder.beginObject();
        builder.writeString("deviceId", deviceId);
        builder.writeString("deviceType", deviceType);
        builder.writeString("imagePath", imagePath);
        builder.writeBool("readOnly", readOnly);
        builder.writeNumber("sizeBytes", sizeBytes);
        builder.writeNumber("blockSize", blockSize);
        builder.endObject();
    }
    
    static StorageConfiguration fromJson(json::JsonParser& parser) {
        StorageConfiguration config;
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            if (key == "deviceId") {
                config.deviceId = parser.readString();
            } else if (key == "deviceType") {
                config.deviceType = parser.readString();
            } else if (key == "imagePath") {
                config.imagePath = parser.readString();
            } else if (key == "readOnly") {
                config.readOnly = parser.readBool();
            } else if (key == "sizeBytes") {
                config.sizeBytes = parser.readNumber();
            } else if (key == "blockSize") {
                config.blockSize = static_cast<uint32_t>(parser.readNumber());
            }
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    static StorageConfiguration fromJson(const std::string& jsonStr) {
        json::JsonParser parser(jsonStr);
        return fromJson(parser);
    }
};

/**
 * BootConfiguration - Boot settings for a virtual machine
 */
struct BootConfiguration {
    std::string bootDevice;     // Boot device ID (e.g., "disk0", "network")
    std::string kernelPath;     // Path to kernel image (if direct boot)
    std::string initrdPath;     // Path to initial ramdisk
    std::string bootArgs;       // Kernel boot arguments
    uint64_t entryPoint;        // Entry point address (0 = auto-detect)
    bool directBoot;            // Direct kernel boot (skip bootloader)
    
    BootConfiguration()
        : bootDevice("disk0")
        , kernelPath()
        , initrdPath()
        , bootArgs()
        , entryPoint(0)
        , directBoot(false) {}
    
    // Serialization
    void toJson(json::JsonBuilder& builder) const {
        builder.beginObject();
        builder.writeString("bootDevice", bootDevice);
        builder.writeString("kernelPath", kernelPath);
        builder.writeString("initrdPath", initrdPath);
        builder.writeString("bootArgs", bootArgs);
        builder.writeNumber("entryPoint", entryPoint);
        builder.writeBool("directBoot", directBoot);
        builder.endObject();
    }
    
    static BootConfiguration fromJson(json::JsonParser& parser) {
        BootConfiguration config;
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            if (key == "bootDevice") {
                config.bootDevice = parser.readString();
            } else if (key == "kernelPath") {
                config.kernelPath = parser.readString();
            } else if (key == "initrdPath") {
                config.initrdPath = parser.readString();
            } else if (key == "bootArgs") {
                config.bootArgs = parser.readString();
            } else if (key == "entryPoint") {
                config.entryPoint = parser.readNumber();
            } else if (key == "directBoot") {
                config.directBoot = parser.readBool();
            }
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    static BootConfiguration fromJson(const std::string& jsonStr) {
        json::JsonParser parser(jsonStr);
        return fromJson(parser);
    }
};

/**
 * NetworkConfiguration - Network settings for a virtual machine
 */
struct NetworkConfiguration {
    std::string interfaceId;    // Network interface ID
    std::string interfaceType;  // Interface type ("user", "tap", "bridge")
    std::string macAddress;     // MAC address (empty = auto-generate)
    bool enableDHCP;            // Enable DHCP
    std::string ipAddress;      // Static IP address (if DHCP disabled)
    std::string gateway;        // Gateway address
    
    NetworkConfiguration()
        : interfaceId("eth0")
        , interfaceType("user")
        , macAddress()
        , enableDHCP(true)
        , ipAddress()
        , gateway() {}
    
    // Serialization
    void toJson(json::JsonBuilder& builder) const {
        builder.beginObject();
        builder.writeString("interfaceId", interfaceId);
        builder.writeString("interfaceType", interfaceType);
        builder.writeString("macAddress", macAddress);
        builder.writeBool("enableDHCP", enableDHCP);
        builder.writeString("ipAddress", ipAddress);
        builder.writeString("gateway", gateway);
        builder.endObject();
    }
    
    static NetworkConfiguration fromJson(json::JsonParser& parser) {
        NetworkConfiguration config;
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            if (key == "interfaceId") {
                config.interfaceId = parser.readString();
            } else if (key == "interfaceType") {
                config.interfaceType = parser.readString();
            } else if (key == "macAddress") {
                config.macAddress = parser.readString();
            } else if (key == "enableDHCP") {
                config.enableDHCP = parser.readBool();
            } else if (key == "ipAddress") {
                config.ipAddress = parser.readString();
            } else if (key == "gateway") {
                config.gateway = parser.readString();
            }
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    static NetworkConfiguration fromJson(const std::string& jsonStr) {
        json::JsonParser parser(jsonStr);
        return fromJson(parser);
    }
};

/**
 * VMConfiguration - Complete virtual machine configuration
 * 
 * This structure contains all settings needed to create and configure a VM:
 * - CPU settings (ISA type, count, frequency)
 * - Memory settings (size, MMU, paging)
 * - Storage devices (disks, images)
 * - Boot configuration (device, kernel, args)
 * - Network configuration (interfaces, addressing)
 * 
 * Usage:
 * ```
 * VMConfiguration config;
 * config.name = "my-vm";
 * config.cpu.isaType = "IA-64";
 * config.cpu.cpuCount = 4;
 * config.memory.memorySize = 256 * 1024 * 1024;  // 256 MB
 * 
 * StorageConfiguration disk;
 * disk.deviceId = "disk0";
 * disk.imagePath = "/path/to/disk.img";
 * config.storageDevices.push_back(disk);
 * 
 * config.boot.bootDevice = "disk0";
 * ```
 */
struct VMConfiguration {
    // VM identification
    std::string name;                               // VM name (unique identifier)
    std::string description;                        // VM description
    std::string uuid;                               // UUID (auto-generated if empty)
    
    // Component configurations
    CPUConfiguration cpu;                           // CPU settings
    MemoryConfiguration memory;                     // Memory settings
    std::vector<StorageConfiguration> storageDevices;  // Storage devices
    BootConfiguration boot;                         // Boot settings
    std::vector<NetworkConfiguration> networkInterfaces;  // Network interfaces
    
    // VM behavior settings
    bool enableDebugger;                            // Enable debugging support
    bool enableSnapshots;                           // Enable snapshot functionality
    bool autoStart;                                 // Auto-start on manager startup
    uint64_t maxCycles;                             // Max execution cycles (0 = unlimited)
    
    VMConfiguration()
        : name("vm")
        , description()
        , uuid()
        , cpu()
        , memory()
        , storageDevices()
        , boot()
        , networkInterfaces()
        , enableDebugger(false)
        , enableSnapshots(true)
        , autoStart(false)
        , maxCycles(0) {}
    
    /**
     * Create a minimal VM configuration
     */
    static VMConfiguration createMinimal(const std::string& vmName) {
        VMConfiguration config;
        config.name = vmName;
        config.cpu.cpuCount = 1;
        config.memory.memorySize = 16 * 1024 * 1024;  // 16 MB
        return config;
    }
    
    /**
     * Create a standard VM configuration
     */
    static VMConfiguration createStandard(const std::string& vmName, 
                                         size_t memoryMB = 256) {
        VMConfiguration config;
        config.name = vmName;
        config.cpu.cpuCount = 2;
        config.memory.memorySize = memoryMB * 1024 * 1024;
        config.enableDebugger = true;
        config.enableSnapshots = true;
        return config;
    }
    
    /**
     * Validate configuration
     * @return true if configuration is valid
     */
    bool validate(std::string* errorMessage = nullptr) const {
        if (name.empty()) {
            if (errorMessage) *errorMessage = "VM name cannot be empty";
            return false;
        }
        
        if (cpu.cpuCount == 0) {
            if (errorMessage) *errorMessage = "CPU count must be at least 1";
            return false;
        }
        
        if (cpu.cpuCount > 256) {
            if (errorMessage) *errorMessage = "CPU count exceeds maximum (256)";
            return false;
        }
        
        if (memory.memorySize == 0) {
            if (errorMessage) *errorMessage = "Memory size must be greater than 0";
            return false;
        }
        
        // Minimum 1 MB memory
        if (memory.memorySize < 1024 * 1024) {
            if (errorMessage) *errorMessage = "Memory size must be at least 1 MB";
            return false;
        }
        
        // Check for duplicate storage device IDs
        for (size_t i = 0; i < storageDevices.size(); ++i) {
            for (size_t j = i + 1; j < storageDevices.size(); ++j) {
                if (storageDevices[i].deviceId == storageDevices[j].deviceId) {
                    if (errorMessage) {
                        *errorMessage = "Duplicate storage device ID: " + 
                                      storageDevices[i].deviceId;
                    }
                    return false;
                }
            }
        }
        
        return true;
    }
    
    /**
     * Add a storage device
     */
    void addStorageDevice(const StorageConfiguration& device) {
        storageDevices.push_back(device);
    }
    
    /**
     * Add a network interface
     */
    void addNetworkInterface(const NetworkConfiguration& iface) {
        networkInterfaces.push_back(iface);
    }
    
    /**
     * Get total configured resources summary
     */
    std::string getResourceSummary() const {
        std::ostringstream oss;
        oss << "VM: " << name << "\n";
        oss << "  CPUs: " << cpu.cpuCount << " x " << cpu.isaType << "\n";
        oss << "  Memory: " << (memory.memorySize / (1024 * 1024)) << " MB\n";
        oss << "  Storage: " << storageDevices.size() << " device(s)\n";
        oss << "  Network: " << networkInterfaces.size() << " interface(s)\n";
        return oss.str();
    }
    
    // ========================================================================
    // JSON Serialization
    // ========================================================================
    
    /**
     * Serialize configuration to JSON string
     */
    std::string toJson() const {
        json::JsonBuilder builder;
        builder.beginObject();
        
        // VM identification
        builder.writeString("name", name);
        builder.writeString("description", description);
        builder.writeString("uuid", uuid);
        
        // CPU configuration
        builder.writeObjectValue("cpu");
        cpu.toJson(builder);
        
        // Memory configuration
        builder.writeObjectValue("memory");
        memory.toJson(builder);
        
        // Storage devices
        builder.writeArrayValue("storageDevices");
        builder.beginArray();
        for (const auto& device : storageDevices) {
            builder.writeArrayElement();
            device.toJson(builder);
        }
        builder.endArray();
        
        // Boot configuration
        builder.writeObjectValue("boot");
        boot.toJson(builder);
        
        // Network interfaces
        builder.writeArrayValue("networkInterfaces");
        builder.beginArray();
        for (const auto& iface : networkInterfaces) {
            builder.writeArrayElement();
            iface.toJson(builder);
        }
        builder.endArray();
        
        // VM behavior settings
        builder.writeBool("enableDebugger", enableDebugger);
        builder.writeBool("enableSnapshots", enableSnapshots);
        builder.writeBool("autoStart", autoStart);
        builder.writeNumber("maxCycles", maxCycles);
        
        builder.endObject();
        return builder.toString();
    }
    
    /**
     * Deserialize configuration from JSON string
     */
    static VMConfiguration fromJson(const std::string& jsonStr) {
        VMConfiguration config;
        json::JsonParser parser(jsonStr);
        
        parser.expectObjectStart();
        
        while (!parser.isObjectEnd()) {
            std::string key = parser.parseKey();
            
            if (key == "name") {
                config.name = parser.readString();
            } else if (key == "description") {
                config.description = parser.readString();
            } else if (key == "uuid") {
                config.uuid = parser.readString();
            } else if (key == "cpu") {
                config.cpu = CPUConfiguration::fromJson(parser);
            } else if (key == "memory") {
                config.memory = MemoryConfiguration::fromJson(parser);
            } else if (key == "storageDevices") {
                parser.expectArrayStart();
                while (!parser.isArrayEnd()) {
                    config.storageDevices.push_back(StorageConfiguration::fromJson(parser));
                    parser.tryConsumeComma();
                }
                parser.expectArrayEnd();
            } else if (key == "boot") {
                config.boot = BootConfiguration::fromJson(parser);
            } else if (key == "networkInterfaces") {
                parser.expectArrayStart();
                while (!parser.isArrayEnd()) {
                    config.networkInterfaces.push_back(NetworkConfiguration::fromJson(parser));
                    parser.tryConsumeComma();
                }
                parser.expectArrayEnd();
            } else if (key == "enableDebugger") {
                config.enableDebugger = parser.readBool();
            } else if (key == "enableSnapshots") {
                config.enableSnapshots = parser.readBool();
            } else if (key == "autoStart") {
                config.autoStart = parser.readBool();
            } else if (key == "maxCycles") {
                config.maxCycles = parser.readNumber();
            }
            
            parser.tryConsumeComma();
        }
        
        parser.expectObjectEnd();
        return config;
    }
    
    /**
     * Save configuration to a JSON file
     */
    bool saveToFile(const std::string& filepath, std::string* errorMessage = nullptr) const {
        try {
            std::ofstream file(filepath);
            if (!file.is_open()) {
                if (errorMessage) {
                    *errorMessage = "Failed to open file for writing: " + filepath;
                }
                return false;
            }
            
            std::string json = toJson();
            file << json;
            file.close();
            
            return true;
        } catch (const std::exception& e) {
            if (errorMessage) {
                *errorMessage = "Exception while saving configuration: " + std::string(e.what());
            }
            return false;
        }
    }
    
    /**
     * Load configuration from a JSON file
     */
    static VMConfiguration loadFromFile(const std::string& filepath, std::string* errorMessage = nullptr) {
        try {
            std::ifstream file(filepath);
            if (!file.is_open()) {
                if (errorMessage) {
                    *errorMessage = "Failed to open file for reading: " + filepath;
                }
                return VMConfiguration();
            }
            
            std::ostringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            std::string jsonStr = buffer.str();
            VMConfiguration config = fromJson(jsonStr);
            
            // Validate loaded configuration
            if (!config.validate(errorMessage)) {
                if (errorMessage) {
                    *errorMessage = "Invalid configuration: " + *errorMessage;
                }
                return VMConfiguration();
            }
            
            return config;
        } catch (const std::exception& e) {
            if (errorMessage) {
                *errorMessage = "Exception while loading configuration: " + std::string(e.what());
            }
            return VMConfiguration();
        }
    }
};

} // namespace ia64
