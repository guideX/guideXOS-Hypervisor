#include "InitramfsDevice.h"
#include "bootstrap.h"
#include "memory.h"
#include "cpu_state.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

using namespace ia64;

/**
 * Example: Loading and booting with an initramfs
 * 
 * This example demonstrates how to:
 * 1. Create an initramfs device from a file
 * 2. Load it into VM memory
 * 3. Configure kernel bootstrap with initramfs
 * 4. Initialize CPU state for kernel boot
 */

// Helper function to create a minimal test initramfs
std::vector<uint8_t> CreateMinimalInitramfs() {
    std::vector<uint8_t> data;
    
    // CPIO newc format magic
    const char* magic = "070701";
    for (size_t i = 0; i < 6; ++i) {
        data.push_back(static_cast<uint8_t>(magic[i]));
    }
    
    // Add some data to make it realistic
    for (size_t i = 0; i < 4096; ++i) {
        data.push_back(0);
    }
    
    return data;
}

// Helper to write a test initramfs file
bool CreateTestInitramfsFile(const std::string& path) {
    std::vector<uint8_t> data = CreateMinimalInitramfs();
    
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

void Example1_BasicInitramfsLoad() {
    std::cout << "========================================" << std::endl;
    std::cout << "Example 1: Basic Initramfs Loading" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Create test initramfs file
    const std::string testFile = "test_initramfs.cpio";
    if (!CreateTestInitramfsFile(testFile)) {
        std::cerr << "Failed to create test initramfs file" << std::endl;
        return;
    }
    
    try {
        // Create initramfs device from file
        auto initramfs = std::make_unique<InitramfsDevice>(
            "initramfs0",
            testFile,
            0x2000000  // Load at 32 MB (default IA-64 address)
        );
        
        std::cout << "Initramfs device created:" << std::endl;
        std::cout << "  Device ID: " << initramfs->getDeviceId() << std::endl;
        std::cout << "  Size: " << initramfs->getSize() << " bytes" << std::endl;
        std::cout << "  Physical Address: 0x" << std::hex << initramfs->getPhysicalAddress() << std::dec << std::endl;
        std::cout << "  Format: " << initramfs->getFormatType() << std::endl;
        
        // Validate format
        if (initramfs->validateFormat()) {
            std::cout << "  Format validation: PASSED" << std::endl;
        } else {
            std::cout << "  Format validation: FAILED" << std::endl;
        }
        
        // Connect device
        initramfs->connect();
        std::cout << "  Device connected: " << (initramfs->isConnected() ? "YES" : "NO") << std::endl;
        
        // Load into memory
        Memory memory;
        if (initramfs->loadIntoMemory(memory)) {
            std::cout << "  Loaded into memory: SUCCESS" << std::endl;
        } else {
            std::cout << "  Loaded into memory: FAILED" << std::endl;
        }
        
        std::cout << std::endl;
        
        // Clean up
        std::remove(testFile.c_str());
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::remove(testFile.c_str());
    }
}

void Example2_KernelBootWithInitramfs() {
    std::cout << "========================================" << std::endl;
    std::cout << "Example 2: Kernel Boot with Initramfs" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Create memory and CPU
    Memory memory;
    CPUState cpu;
    
    // Create initramfs from memory buffer
    std::vector<uint8_t> initramfsData = CreateMinimalInitramfs();
    
    auto initramfs = std::make_unique<InitramfsDevice>(
        "initramfs0",
        initramfsData.data(),
        initramfsData.size(),
        0x2000000  // 32 MB
    );
    
    std::cout << "Created initramfs device from memory buffer" << std::endl;
    std::cout << "  Size: " << initramfs->getSize() << " bytes" << std::endl;
    
    // Load initramfs into VM memory
    if (!initramfs->loadIntoMemory(memory)) {
        std::cerr << "Failed to load initramfs into memory" << std::endl;
        return;
    }
    
    std::cout << "Loaded initramfs at: 0x" << std::hex << initramfs->getPhysicalAddress() << std::dec << std::endl;
    
    // Configure kernel bootstrap
    KernelBootstrapConfig config;
    config.entryPoint = 0x100000;           // Kernel at 1 MB
    config.globalPointer = 0x600000;        // GP from ELF
    config.bootParamAddress = 0x10000;      // Boot params at 64 KB
    config.initramfsAddress = initramfs->getPhysicalAddress();
    config.initramfsSize = initramfs->getSize();
    config.commandLineAddress = 0x11000;    // Command line at 68 KB
    config.enableVirtualAddressing = false; // Physical mode initially
    
    std::cout << std::endl << "Kernel Bootstrap Configuration:" << std::endl;
    std::cout << "  Entry Point: 0x" << std::hex << config.entryPoint << std::dec << std::endl;
    std::cout << "  Boot Params: 0x" << std::hex << config.bootParamAddress << std::dec << std::endl;
    std::cout << "  Initramfs Address: 0x" << std::hex << config.initramfsAddress << std::dec << std::endl;
    std::cout << "  Initramfs Size: " << config.initramfsSize << " bytes" << std::endl;
    
    // Write kernel command line
    std::string cmdline = "console=ttyS0 root=/dev/ram0 rw";
    uint64_t cmdAddr = config.commandLineAddress;
    for (char c : cmdline) {
        memory.write<uint8_t>(cmdAddr++, static_cast<uint8_t>(c));
    }
    memory.write<uint8_t>(cmdAddr, 0);  // Null terminator
    
    std::cout << "  Command Line: \"" << cmdline << "\"" << std::endl;
    
    // Write boot parameters to memory
    uint64_t bytesWritten = WriteKernelBootParameters(memory, config.bootParamAddress, config);
    std::cout << std::endl << "Boot parameters written: " << bytesWritten << " bytes" << std::endl;
    
    // Verify boot parameters
    uint64_t storedInitramfsAddr = memory.read<uint64_t>(config.bootParamAddress + 0x10);
    uint64_t storedInitramfsSize = memory.read<uint64_t>(config.bootParamAddress + 0x18);
    
    std::cout << "Verified boot parameters:" << std::endl;
    std::cout << "  Initramfs Address: 0x" << std::hex << storedInitramfsAddr << std::dec << std::endl;
    std::cout << "  Initramfs Size: " << storedInitramfsSize << " bytes" << std::endl;
    
    // Initialize kernel bootstrap state
    uint64_t stackPointer = InitializeKernelBootstrapState(cpu, memory, config);
    
    std::cout << std::endl << "Kernel CPU state initialized:" << std::endl;
    std::cout << "  IP (Entry Point): 0x" << std::hex << cpu.GetIP() << std::dec << std::endl;
    std::cout << "  r1 (GP): 0x" << std::hex << cpu.GetGR(1) << std::dec << std::endl;
    std::cout << "  r12 (SP): 0x" << std::hex << cpu.GetGR(12) << std::dec << std::endl;
    std::cout << "  r28 (Boot Params): 0x" << std::hex << cpu.GetGR(28) << std::dec << std::endl;
    std::cout << "  Stack Pointer: 0x" << std::hex << stackPointer << std::dec << std::endl;
    
    std::cout << std::endl << "Ready to start kernel execution!" << std::endl;
    std::cout << std::endl;
}

void Example3_MultipleAddresses() {
    std::cout << "========================================" << std::endl;
    std::cout << "Example 3: Custom Physical Addresses" << std::endl;
    std::cout << "========================================" << std::endl;
    
    std::vector<uint8_t> data = CreateMinimalInitramfs();
    
    // Create devices with different physical addresses
    struct TestCase {
        std::string name;
        uint64_t address;
    };
    
    TestCase testCases[] = {
        {"IA-64 Default", InitramfsDevice::DEFAULT_IA64_ADDRESS},
        {"x86-64 Default", InitramfsDevice::DEFAULT_X86_64_ADDRESS},
        {"ARM64 Default", InitramfsDevice::DEFAULT_ARM64_ADDRESS},
        {"Custom 128 MB", 0x8000000},
    };
    
    for (const auto& tc : testCases) {
        auto device = std::make_unique<InitramfsDevice>(
            "initramfs",
            data.data(),
            data.size(),
            tc.address
        );
        
        std::cout << tc.name << ":" << std::endl;
        std::cout << "  Physical Address: 0x" << std::hex << device->getPhysicalAddress() << std::dec;
        std::cout << " (" << (device->getPhysicalAddress() / (1024 * 1024)) << " MB)" << std::endl;
    }
    
    std::cout << std::endl;
}

void Example4_FormatDetection() {
    std::cout << "========================================" << std::endl;
    std::cout << "Example 4: Format Detection" << std::endl;
    std::cout << "========================================" << std::endl;
    
    struct FormatTest {
        std::string name;
        std::vector<uint8_t> magic;
        std::string expectedFormat;
    };
    
    FormatTest formats[] = {
        {"CPIO newc", {'0','7','0','7','0','1'}, "cpio-newc"},
        {"CPIO old", {'0','7','0','7','0','7'}, "cpio-old"},
        {"Gzip", {0x1f, 0x8b}, "cpio.gz"},
        {"XZ", {0xfd, '7', 'z', 'X', 'Z', 0x00}, "cpio.xz"},
    };
    
    for (const auto& fmt : formats) {
        std::vector<uint8_t> data = fmt.magic;
        data.resize(1024, 0);  // Pad with zeros
        
        auto device = std::make_unique<InitramfsDevice>(
            "test",
            data.data(),
            data.size()
        );
        
        std::cout << fmt.name << ":" << std::endl;
        std::cout << "  Valid: " << (device->validateFormat() ? "YES" : "NO") << std::endl;
        std::cout << "  Detected Format: " << device->getFormatType() << std::endl;
        std::cout << "  Expected Format: " << fmt.expectedFormat << std::endl;
        std::cout << std::endl;
    }
}

void Example5_DeviceStatistics() {
    std::cout << "========================================" << std::endl;
    std::cout << "Example 5: Device Statistics" << std::endl;
    std::cout << "========================================" << std::endl;
    
    std::vector<uint8_t> data = CreateMinimalInitramfs();
    auto device = std::make_unique<InitramfsDevice>(
        "initramfs0",
        data.data(),
        data.size(),
        0x2000000
    );
    
    device->connect();
    
    // Perform some reads
    std::vector<uint8_t> buffer(device->getBlockSize());
    device->readBlocks(0, 1, buffer.data());
    device->readBlocks(1, 1, buffer.data());
    device->readBlocks(2, 1, buffer.data());
    
    // Display statistics
    std::cout << device->getStatistics() << std::endl;
}

int main() {
    std::cout << std::endl;
    std::cout << "??????????????????????????????????????????????????????????" << std::endl;
    std::cout << "?      Virtual Initramfs - Usage Examples               ?" << std::endl;
    std::cout << "??????????????????????????????????????????????????????????" << std::endl;
    std::cout << std::endl;
    
    try {
        Example1_BasicInitramfsLoad();
        Example2_KernelBootWithInitramfs();
        Example3_MultipleAddresses();
        Example4_FormatDetection();
        Example5_DeviceStatistics();
        
        std::cout << "========================================" << std::endl;
        std::cout << "All examples completed successfully!" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << std::endl;
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
