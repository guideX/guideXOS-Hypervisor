#include "VMConfiguration.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

// Test CPU configuration serialization
void testCPUConfigSerialization() {
    std::cout << "Testing CPU configuration serialization...\n";
    
    CPUConfiguration original;
    original.isaType = "IA-64";
    original.cpuCount = 4;
    original.clockFrequency = 2000000000;
    original.enableProfiling = true;
    
    // Serialize
    json::JsonBuilder builder;
    original.toJson(builder);
    std::string json = builder.toString();
    
    assert(!json.empty());
    assert(json.find("\"isaType\"") != std::string::npos);
    assert(json.find("\"IA-64\"") != std::string::npos);
    assert(json.find("\"cpuCount\"") != std::string::npos);
    
    // Deserialize
    CPUConfiguration restored = CPUConfiguration::fromJson(json);
    
    assert(restored.isaType == original.isaType);
    assert(restored.cpuCount == original.cpuCount);
    assert(restored.clockFrequency == original.clockFrequency);
    assert(restored.enableProfiling == original.enableProfiling);
    
    std::cout << "  ? CPU configuration round-trip passed\n";
}

// Test memory configuration serialization
void testMemoryConfigSerialization() {
    std::cout << "Testing memory configuration serialization...\n";
    
    MemoryConfiguration original;
    original.memorySize = 256 * 1024 * 1024;
    original.enableMMU = true;
    original.enablePaging = true;
    original.pageSize = 4096;
    
    // Serialize
    json::JsonBuilder builder;
    original.toJson(builder);
    std::string json = builder.toString();
    
    assert(!json.empty());
    
    // Deserialize
    MemoryConfiguration restored = MemoryConfiguration::fromJson(json);
    
    assert(restored.memorySize == original.memorySize);
    assert(restored.enableMMU == original.enableMMU);
    assert(restored.enablePaging == original.enablePaging);
    assert(restored.pageSize == original.pageSize);
    
    std::cout << "  ? Memory configuration round-trip passed\n";
}

// Test storage configuration serialization
void testStorageConfigSerialization() {
    std::cout << "Testing storage configuration serialization...\n";
    
    StorageConfiguration original;
    original.deviceId = "disk0";
    original.deviceType = "raw";
    original.imagePath = "/path/to/disk.img";
    original.readOnly = false;
    original.sizeBytes = 1024 * 1024 * 1024;
    original.blockSize = 512;
    
    // Serialize
    json::JsonBuilder builder;
    original.toJson(builder);
    std::string json = builder.toString();
    
    assert(!json.empty());
    
    // Deserialize
    StorageConfiguration restored = StorageConfiguration::fromJson(json);
    
    assert(restored.deviceId == original.deviceId);
    assert(restored.deviceType == original.deviceType);
    assert(restored.imagePath == original.imagePath);
    assert(restored.readOnly == original.readOnly);
    assert(restored.sizeBytes == original.sizeBytes);
    assert(restored.blockSize == original.blockSize);
    
    std::cout << "  ? Storage configuration round-trip passed\n";
}

// Test boot configuration serialization
void testBootConfigSerialization() {
    std::cout << "Testing boot configuration serialization...\n";
    
    BootConfiguration original;
    original.bootDevice = "disk0";
    original.kernelPath = "/boot/vmlinuz";
    original.initrdPath = "/boot/initrd.img";
    original.bootArgs = "root=/dev/sda1 console=ttyS0";
    original.entryPoint = 0x1000;
    original.directBoot = true;
    
    // Serialize
    json::JsonBuilder builder;
    original.toJson(builder);
    std::string json = builder.toString();
    
    assert(!json.empty());
    
    // Deserialize
    BootConfiguration restored = BootConfiguration::fromJson(json);
    
    assert(restored.bootDevice == original.bootDevice);
    assert(restored.kernelPath == original.kernelPath);
    assert(restored.initrdPath == original.initrdPath);
    assert(restored.bootArgs == original.bootArgs);
    assert(restored.entryPoint == original.entryPoint);
    assert(restored.directBoot == original.directBoot);
    
    std::cout << "  ? Boot configuration round-trip passed\n";
}

// Test network configuration serialization
void testNetworkConfigSerialization() {
    std::cout << "Testing network configuration serialization...\n";
    
    NetworkConfiguration original;
    original.interfaceId = "eth0";
    original.interfaceType = "bridge";
    original.macAddress = "52:54:00:12:34:56";
    original.enableDHCP = false;
    original.ipAddress = "192.168.1.100";
    original.gateway = "192.168.1.1";
    
    // Serialize
    json::JsonBuilder builder;
    original.toJson(builder);
    std::string json = builder.toString();
    
    assert(!json.empty());
    
    // Deserialize
    NetworkConfiguration restored = NetworkConfiguration::fromJson(json);
    
    assert(restored.interfaceId == original.interfaceId);
    assert(restored.interfaceType == original.interfaceType);
    assert(restored.macAddress == original.macAddress);
    assert(restored.enableDHCP == original.enableDHCP);
    assert(restored.ipAddress == original.ipAddress);
    assert(restored.gateway == original.gateway);
    
    std::cout << "  ? Network configuration round-trip passed\n";
}

// Test complete VM configuration serialization
void testVMConfigSerialization() {
    std::cout << "Testing complete VM configuration serialization...\n";
    
    // Create a complex configuration
    VMConfiguration original;
    original.name = "test-vm";
    original.description = "Test VM for serialization";
    original.uuid = "12345678-1234-1234-1234-123456789abc";
    
    original.cpu.isaType = "IA-64";
    original.cpu.cpuCount = 4;
    original.cpu.clockFrequency = 2000000000;
    original.cpu.enableProfiling = true;
    
    original.memory.memorySize = 512 * 1024 * 1024;
    original.memory.enableMMU = true;
    original.memory.enablePaging = true;
    
    StorageConfiguration disk0("disk0", "/path/to/disk0.img");
    disk0.sizeBytes = 10ULL * 1024 * 1024 * 1024;
    original.addStorageDevice(disk0);
    
    StorageConfiguration disk1("disk1", "/path/to/disk1.img");
    disk1.readOnly = true;
    original.addStorageDevice(disk1);
    
    original.boot.bootDevice = "disk0";
    original.boot.directBoot = true;
    original.boot.kernelPath = "/boot/vmlinuz";
    
    NetworkConfiguration eth0;
    eth0.interfaceType = "bridge";
    eth0.macAddress = "52:54:00:12:34:56";
    original.addNetworkInterface(eth0);
    
    original.enableDebugger = true;
    original.enableSnapshots = true;
    original.autoStart = false;
    original.maxCycles = 1000000;
    
    // Serialize
    std::string json = original.toJson();
    assert(!json.empty());
    assert(json.find("\"name\"") != std::string::npos);
    assert(json.find("\"test-vm\"") != std::string::npos);
    
    std::cout << "  JSON size: " << json.length() << " bytes\n";
    
    // Deserialize
    VMConfiguration restored = VMConfiguration::fromJson(json);
    
    // Verify all fields
    assert(restored.name == original.name);
    assert(restored.description == original.description);
    assert(restored.uuid == original.uuid);
    
    assert(restored.cpu.isaType == original.cpu.isaType);
    assert(restored.cpu.cpuCount == original.cpu.cpuCount);
    assert(restored.cpu.clockFrequency == original.cpu.clockFrequency);
    assert(restored.cpu.enableProfiling == original.cpu.enableProfiling);
    
    assert(restored.memory.memorySize == original.memory.memorySize);
    assert(restored.memory.enableMMU == original.memory.enableMMU);
    assert(restored.memory.enablePaging == original.memory.enablePaging);
    
    assert(restored.storageDevices.size() == 2);
    assert(restored.storageDevices[0].deviceId == "disk0");
    assert(restored.storageDevices[1].deviceId == "disk1");
    assert(restored.storageDevices[1].readOnly == true);
    
    assert(restored.boot.bootDevice == original.boot.bootDevice);
    assert(restored.boot.directBoot == original.boot.directBoot);
    
    assert(restored.networkInterfaces.size() == 1);
    assert(restored.networkInterfaces[0].macAddress == "52:54:00:12:34:56");
    
    assert(restored.enableDebugger == original.enableDebugger);
    assert(restored.enableSnapshots == original.enableSnapshots);
    assert(restored.autoStart == original.autoStart);
    assert(restored.maxCycles == original.maxCycles);
    
    std::cout << "  ? Complete VM configuration round-trip passed\n";
}

// Test file save and load
void testFileSaveLoad() {
    std::cout << "Testing file save and load...\n";
    
    VMConfiguration original = VMConfiguration::createStandard("file-test-vm", 128);
    original.description = "Testing file I/O";
    
    StorageConfiguration disk("disk0", "test.img");
    original.addStorageDevice(disk);
    
    // Save to file
    std::string filepath = "test-config-temp.json";
    std::string error;
    
    bool saved = original.saveToFile(filepath, &error);
    assert(saved);
    std::cout << "  ? Configuration saved to file\n";
    
    // Load from file
    VMConfiguration restored = VMConfiguration::loadFromFile(filepath, &error);
    assert(!restored.name.empty());
    assert(restored.name == original.name);
    assert(restored.description == original.description);
    assert(restored.cpu.cpuCount == original.cpu.cpuCount);
    assert(restored.memory.memorySize == original.memory.memorySize);
    assert(restored.storageDevices.size() == 1);
    
    std::cout << "  ? Configuration loaded from file\n";
    
    // Clean up
    remove(filepath.c_str());
}

// Test validation
void testValidation() {
    std::cout << "Testing configuration validation...\n";
    
    std::string error;
    
    // Valid configuration
    VMConfiguration validConfig = VMConfiguration::createStandard("valid");
    assert(validConfig.validate(&error));
    std::cout << "  ? Valid configuration passes\n";
    
    // Empty name
    VMConfiguration invalidName;
    invalidName.name = "";
    assert(!invalidName.validate(&error));
    assert(error.find("name") != std::string::npos);
    std::cout << "  ? Empty name rejected\n";
    
    // Zero CPUs
    VMConfiguration zeroCPU = VMConfiguration::createMinimal("test");
    zeroCPU.cpu.cpuCount = 0;
    assert(!zeroCPU.validate(&error));
    std::cout << "  ? Zero CPUs rejected\n";
    
    // Too many CPUs
    VMConfiguration tooManyCPUs = VMConfiguration::createMinimal("test");
    tooManyCPUs.cpu.cpuCount = 300;
    assert(!tooManyCPUs.validate(&error));
    std::cout << "  ? Excessive CPUs rejected\n";
    
    // Zero memory
    VMConfiguration zeroMem = VMConfiguration::createMinimal("test");
    zeroMem.memory.memorySize = 0;
    assert(!zeroMem.validate(&error));
    std::cout << "  ? Zero memory rejected\n";
    
    // Duplicate storage IDs
    VMConfiguration dupStorage = VMConfiguration::createMinimal("test");
    StorageConfiguration disk1("disk0", "disk1.img");
    StorageConfiguration disk2("disk0", "disk2.img");
    dupStorage.addStorageDevice(disk1);
    dupStorage.addStorageDevice(disk2);
    assert(!dupStorage.validate(&error));
    assert(error.find("Duplicate") != std::string::npos);
    std::cout << "  ? Duplicate storage IDs rejected\n";
}

// Test special characters and escaping
void testSpecialCharacters() {
    std::cout << "Testing special character handling...\n";
    
    VMConfiguration config = VMConfiguration::createMinimal("test-vm");
    config.description = "Test \"quotes\" and\nnewlines\tand\ttabs";
    config.boot.bootArgs = "arg1=\"value\" arg2='value2'";
    
    // Serialize and deserialize
    std::string json = config.toJson();
    VMConfiguration restored = VMConfiguration::fromJson(json);
    
    assert(restored.description == config.description);
    assert(restored.boot.bootArgs == config.boot.bootArgs);
    
    std::cout << "  ? Special characters handled correctly\n";
}

// Test empty arrays
void testEmptyArrays() {
    std::cout << "Testing empty arrays...\n";
    
    VMConfiguration config = VMConfiguration::createMinimal("minimal");
    // No storage devices or network interfaces
    
    std::string json = config.toJson();
    VMConfiguration restored = VMConfiguration::fromJson(json);
    
    assert(restored.storageDevices.empty());
    assert(restored.networkInterfaces.empty());
    
    std::cout << "  ? Empty arrays handled correctly\n";
}

// Test large values
void testLargeValues() {
    std::cout << "Testing large values...\n";
    
    VMConfiguration config = VMConfiguration::createMinimal("large-vm");
    config.memory.memorySize = 16ULL * 1024 * 1024 * 1024;  // 16 GB
    config.maxCycles = 1000000000000ULL;  // 1 trillion
    
    StorageConfiguration largeDisk("disk0", "large.img");
    largeDisk.sizeBytes = 1024ULL * 1024 * 1024 * 1024;  // 1 TB
    config.addStorageDevice(largeDisk);
    
    std::string json = config.toJson();
    VMConfiguration restored = VMConfiguration::fromJson(json);
    
    assert(restored.memory.memorySize == config.memory.memorySize);
    assert(restored.maxCycles == config.maxCycles);
    assert(restored.storageDevices[0].sizeBytes == largeDisk.sizeBytes);
    
    std::cout << "  ? Large values handled correctly\n";
}

int main() {
    std::cout << "\n";
    std::cout << "???????????????????????????????????????????????????????????????\n";
    std::cout << "  VM Configuration Serialization Tests\n";
    std::cout << "???????????????????????????????????????????????????????????????\n\n";
    
    try {
        testCPUConfigSerialization();
        testMemoryConfigSerialization();
        testStorageConfigSerialization();
        testBootConfigSerialization();
        testNetworkConfigSerialization();
        testVMConfigSerialization();
        testFileSaveLoad();
        testValidation();
        testSpecialCharacters();
        testEmptyArrays();
        testLargeValues();
        
        std::cout << "\n? All tests passed!\n\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n? Test failed with exception: " << e.what() << "\n\n";
        return 1;
    } catch (...) {
        std::cerr << "\n? Test failed with unknown exception\n\n";
        return 1;
    }
}
