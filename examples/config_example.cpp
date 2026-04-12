#include "VMConfiguration.h"
#include <iostream>
#include <cstdlib>

using namespace ia64;

/**
 * Example: VM Configuration Management
 * 
 * Demonstrates how to:
 * - Create VM configurations programmatically
 * - Save configurations to JSON files
 * - Load configurations from JSON files
 * - Validate and modify configurations
 */

void printSeparator() {
    std::cout << "\n" << std::string(70, '=') << "\n\n";
}

void example1_CreateAndSaveMinimalConfig() {
    std::cout << "Example 1: Create and Save Minimal Configuration\n";
    std::cout << std::string(50, '-') << "\n";
    
    // Create a minimal VM configuration
    VMConfiguration config = VMConfiguration::createMinimal("test-vm-1");
    config.description = "A minimal test VM";
    
    // Display configuration
    std::cout << "Created configuration:\n";
    std::cout << config.getResourceSummary() << "\n";
    
    // Save to file
    std::string filepath = "test-minimal-config.json";
    std::string error;
    if (config.saveToFile(filepath, &error)) {
        std::cout << "? Configuration saved to: " << filepath << "\n";
    } else {
        std::cout << "? Failed to save: " << error << "\n";
    }
}

void example2_CreateAndSaveStandardConfig() {
    std::cout << "Example 2: Create and Save Standard Configuration with Storage\n";
    std::cout << std::string(50, '-') << "\n";
    
    // Create standard VM configuration
    VMConfiguration config = VMConfiguration::createStandard("web-server", 512);
    config.description = "Web server VM with storage";
    
    // Add storage devices
    StorageConfiguration disk0("disk0", "webserver-system.img");
    disk0.sizeBytes = 10ULL * 1024 * 1024 * 1024;  // 10 GB
    disk0.blockSize = 4096;
    config.addStorageDevice(disk0);
    
    StorageConfiguration disk1("disk1", "webserver-data.img");
    disk1.sizeBytes = 50ULL * 1024 * 1024 * 1024;  // 50 GB
    disk1.blockSize = 4096;
    config.addStorageDevice(disk1);
    
    // Add network interface
    NetworkConfiguration eth0;
    eth0.interfaceId = "eth0";
    eth0.interfaceType = "bridge";
    eth0.enableDHCP = false;
    eth0.ipAddress = "192.168.1.100";
    eth0.gateway = "192.168.1.1";
    config.addNetworkInterface(eth0);
    
    // Configure boot
    config.boot.bootDevice = "disk0";
    config.boot.directBoot = true;
    config.boot.kernelPath = "/boot/vmlinuz";
    config.boot.bootArgs = "root=/dev/sda1";
    
    // Display configuration
    std::cout << "Created configuration:\n";
    std::cout << config.getResourceSummary() << "\n";
    std::cout << "  Boot Device: " << config.boot.bootDevice << "\n";
    std::cout << "  Direct Boot: " << (config.boot.directBoot ? "Yes" : "No") << "\n";
    
    // Save to file
    std::string filepath = "test-webserver-config.json";
    std::string error;
    if (config.saveToFile(filepath, &error)) {
        std::cout << "? Configuration saved to: " << filepath << "\n";
    } else {
        std::cout << "? Failed to save: " << error << "\n";
    }
}

void example3_LoadAndModifyConfig() {
    std::cout << "Example 3: Load and Modify Configuration\n";
    std::cout << std::string(50, '-') << "\n";
    
    // Load configuration from file
    std::string filepath = "test-minimal-config.json";
    std::string error;
    
    VMConfiguration config = VMConfiguration::loadFromFile(filepath, &error);
    if (config.name.empty()) {
        std::cout << "? Failed to load configuration: " << error << "\n";
        std::cout << "  (This is expected if example 1 hasn't run yet)\n";
        return;
    }
    
    std::cout << "? Loaded configuration from: " << filepath << "\n";
    std::cout << "Original configuration:\n";
    std::cout << config.getResourceSummary() << "\n";
    
    // Modify configuration
    config.name = "test-vm-modified";
    config.description = "Modified minimal VM with more resources";
    config.cpu.cpuCount = 2;
    config.memory.memorySize = 64 * 1024 * 1024;  // 64 MB
    config.enableDebugger = true;
    
    // Add a storage device
    StorageConfiguration disk("disk0", "test-disk.img");
    disk.sizeBytes = 1024 * 1024 * 1024;  // 1 GB
    config.addStorageDevice(disk);
    
    std::cout << "\nModified configuration:\n";
    std::cout << config.getResourceSummary() << "\n";
    
    // Save modified configuration
    std::string newFilepath = "test-modified-config.json";
    if (config.saveToFile(newFilepath, &error)) {
        std::cout << "? Modified configuration saved to: " << newFilepath << "\n";
    } else {
        std::cout << "? Failed to save: " << error << "\n";
    }
}

void example4_ValidateConfiguration() {
    std::cout << "Example 4: Configuration Validation\n";
    std::cout << std::string(50, '-') << "\n";
    
    // Test valid configuration
    VMConfiguration validConfig = VMConfiguration::createStandard("valid-vm");
    std::string error;
    
    if (validConfig.validate(&error)) {
        std::cout << "? Valid configuration passed validation\n";
    } else {
        std::cout << "? Unexpected validation failure: " << error << "\n";
    }
    
    // Test invalid configurations
    std::cout << "\nTesting invalid configurations:\n";
    
    // Empty name
    VMConfiguration invalidConfig1;
    invalidConfig1.name = "";
    if (!invalidConfig1.validate(&error)) {
        std::cout << "? Correctly rejected config with empty name: " << error << "\n";
    }
    
    // Zero CPUs
    VMConfiguration invalidConfig2 = VMConfiguration::createMinimal("test");
    invalidConfig2.cpu.cpuCount = 0;
    if (!invalidConfig2.validate(&error)) {
        std::cout << "? Correctly rejected config with 0 CPUs: " << error << "\n";
    }
    
    // Too many CPUs
    VMConfiguration invalidConfig3 = VMConfiguration::createMinimal("test");
    invalidConfig3.cpu.cpuCount = 300;
    if (!invalidConfig3.validate(&error)) {
        std::cout << "? Correctly rejected config with too many CPUs: " << error << "\n";
    }
    
    // Duplicate storage device IDs
    VMConfiguration invalidConfig4 = VMConfiguration::createMinimal("test");
    StorageConfiguration disk1("disk0", "disk1.img");
    StorageConfiguration disk2("disk0", "disk2.img");  // Same ID!
    invalidConfig4.addStorageDevice(disk1);
    invalidConfig4.addStorageDevice(disk2);
    if (!invalidConfig4.validate(&error)) {
        std::cout << "? Correctly rejected config with duplicate device IDs: " << error << "\n";
    }
}

void example5_JSONRoundTrip() {
    std::cout << "Example 5: JSON Serialization Round-Trip\n";
    std::cout << std::string(50, '-') << "\n";
    
    // Create a complex configuration
    VMConfiguration original = VMConfiguration::createStandard("roundtrip-test", 256);
    original.description = "Testing JSON round-trip";
    original.uuid = "12345678-1234-1234-1234-123456789abc";
    
    StorageConfiguration disk("disk0", "test.img");
    disk.sizeBytes = 2ULL * 1024 * 1024 * 1024;
    original.addStorageDevice(disk);
    
    NetworkConfiguration net;
    net.macAddress = "52:54:00:12:34:56";
    original.addNetworkInterface(net);
    
    // Serialize to JSON
    std::string json = original.toJson();
    std::cout << "Original configuration serialized to JSON (" 
              << json.length() << " bytes)\n";
    
    // Deserialize from JSON
    VMConfiguration restored = VMConfiguration::fromJson(json);
    
    // Verify round-trip
    bool match = true;
    if (restored.name != original.name) {
        std::cout << "? Name mismatch\n";
        match = false;
    }
    if (restored.cpu.cpuCount != original.cpu.cpuCount) {
        std::cout << "? CPU count mismatch\n";
        match = false;
    }
    if (restored.memory.memorySize != original.memory.memorySize) {
        std::cout << "? Memory size mismatch\n";
        match = false;
    }
    if (restored.storageDevices.size() != original.storageDevices.size()) {
        std::cout << "? Storage device count mismatch\n";
        match = false;
    }
    if (restored.networkInterfaces.size() != original.networkInterfaces.size()) {
        std::cout << "? Network interface count mismatch\n";
        match = false;
    }
    
    if (match) {
        std::cout << "? JSON round-trip successful - all data preserved\n";
    }
}

void example6_LoadExampleConfigs() {
    std::cout << "Example 6: Load Predefined Example Configurations\n";
    std::cout << std::string(50, '-') << "\n";
    
    const char* exampleFiles[] = {
        "examples/configs/minimal-vm.json",
        "examples/configs/standard-vm.json",
        "examples/configs/server-vm.json"
    };
    
    for (const char* filepath : exampleFiles) {
        std::string error;
        VMConfiguration config = VMConfiguration::loadFromFile(filepath, &error);
        
        if (config.name.empty()) {
            std::cout << "? Failed to load " << filepath << ": " << error << "\n";
        } else {
            std::cout << "\n? Loaded: " << filepath << "\n";
            std::cout << config.getResourceSummary();
            
            // Validate
            if (config.validate(&error)) {
                std::cout << "  Validation: PASSED\n";
            } else {
                std::cout << "  Validation: FAILED - " << error << "\n";
            }
        }
    }
}

int main() {
    std::cout << "\n";
    std::cout << "???????????????????????????????????????????????????????????????????\n";
    std::cout << "  VM Configuration Management Examples\n";
    std::cout << "???????????????????????????????????????????????????????????????????\n";
    
    try {
        example1_CreateAndSaveMinimalConfig();
        printSeparator();
        
        example2_CreateAndSaveStandardConfig();
        printSeparator();
        
        example3_LoadAndModifyConfig();
        printSeparator();
        
        example4_ValidateConfiguration();
        printSeparator();
        
        example5_JSONRoundTrip();
        printSeparator();
        
        example6_LoadExampleConfigs();
        printSeparator();
        
        std::cout << "All examples completed!\n\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
