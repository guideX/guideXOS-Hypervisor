#include "InitramfsDevice.h"
#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <fstream>

using namespace ia64;

// ============================================================================
// Test Helper Functions
// ============================================================================

/**
 * Create a simple test initramfs image in memory
 * Just a minimal cpio archive for testing
 */
std::vector<uint8_t> CreateTestInitramfs() {
    // Simple cpio newc format header (ASCII)
    // This is a minimal valid cpio archive with just a trailer
    std::vector<uint8_t> data;
    
    // CPIO newc magic: "070701"
    const char* magic = "070701";
    for (size_t i = 0; i < 6; ++i) {
        data.push_back(static_cast<uint8_t>(magic[i]));
    }
    
    // Add some padding to make it larger than just the header
    // A real initramfs would have file entries
    for (size_t i = 0; i < 1024; ++i) {
        data.push_back(0);
    }
    
    return data;
}

/**
 * Create a test initramfs file on disk
 */
bool CreateTestInitramfsFile(const std::string& path) {
    std::vector<uint8_t> data = CreateTestInitramfs();
    
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

// ============================================================================
// Test Cases
// ============================================================================

void TestInitramfsDeviceCreationFromMemory() {
    std::cout << "Testing InitramfsDevice creation from memory buffer..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    // Verify device properties
    assert(device.getDeviceId() == "initramfs0");
    assert(device.getSize() == testData.size());
    assert(device.getBlockSize() == 4096);
    assert(device.isReadOnly());
    assert(!device.isConnected());
    assert(!device.isLoadedInMemory());
    assert(device.getPhysicalAddress() == InitramfsDevice::DEFAULT_IA64_ADDRESS);
    
    std::cout << "  ? Device created successfully" << std::endl;
    std::cout << "  ? Size: " << device.getSize() << " bytes" << std::endl;
    std::cout << "  ? Physical address: 0x" << std::hex << device.getPhysicalAddress() << std::dec << std::endl;
}

void TestInitramfsDeviceCreationFromFile() {
    std::cout << "Testing InitramfsDevice creation from file..." << std::endl;
    
    const std::string testFile = "test_initramfs.img";
    
    // Create test file
    if (!CreateTestInitramfsFile(testFile)) {
        std::cerr << "  ? Failed to create test file" << std::endl;
        return;
    }
    
    try {
        InitramfsDevice device("initramfs0", testFile);
        
        assert(device.getDeviceId() == "initramfs0");
        assert(device.getSize() > 0);
        assert(device.getImagePath() == testFile);
        assert(device.isReadOnly());
        
        std::cout << "  ? Device created from file successfully" << std::endl;
        std::cout << "  ? Size: " << device.getSize() << " bytes" << std::endl;
        
        // Clean up test file
        std::remove(testFile.c_str());
    } catch (const std::exception& e) {
        std::cerr << "  ? Exception: " << e.what() << std::endl;
        std::remove(testFile.c_str());
    }
}

void TestInitramfsDeviceConnect() {
    std::cout << "Testing InitramfsDevice connect/disconnect..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    assert(!device.isConnected());
    
    // Connect device
    bool connected = device.connect();
    assert(connected);
    assert(device.isConnected());
    
    std::cout << "  ? Device connected" << std::endl;
    
    // Disconnect device
    device.disconnect();
    assert(!device.isConnected());
    assert(!device.isLoadedInMemory());
    
    std::cout << "  ? Device disconnected" << std::endl;
}

void TestInitramfsDeviceReadBlocks() {
    std::cout << "Testing InitramfsDevice block reads..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    device.connect();
    
    // Read first block
    std::vector<uint8_t> buffer(device.getBlockSize());
    int64_t blocksRead = device.readBlocks(0, 1, buffer.data());
    
    assert(blocksRead == 1);
    
    // Verify data matches
    size_t compareSize = std::min(buffer.size(), testData.size());
    assert(std::memcmp(buffer.data(), testData.data(), compareSize) == 0);
    
    std::cout << "  ? Block read successful" << std::endl;
    std::cout << "  ? Data integrity verified" << std::endl;
    
    // Verify statistics
    assert(device.getTotalReads() > 0);
    assert(device.getBytesRead() > 0);
    
    std::cout << "  ? Statistics: " << device.getTotalReads() << " reads, " 
              << device.getBytesRead() << " bytes" << std::endl;
}

void TestInitramfsDeviceWriteBlocks() {
    std::cout << "Testing InitramfsDevice write protection..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    device.connect();
    
    // Try to write (should fail - read-only)
    std::vector<uint8_t> buffer(device.getBlockSize(), 0xFF);
    int64_t blocksWritten = device.writeBlocks(0, 1, buffer.data());
    
    assert(blocksWritten == -1);  // Write should fail
    
    std::cout << "  ? Write protection verified (read-only device)" << std::endl;
}

void TestInitramfsLoadIntoMemory() {
    std::cout << "Testing InitramfsDevice load into VM memory..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    uint64_t physAddr = 0x2000000;  // 32 MB
    
    InitramfsDevice device("initramfs0", testData.data(), testData.size(), physAddr);
    Memory memory;
    
    // Load into memory
    bool loaded = device.loadIntoMemory(memory);
    assert(loaded);
    assert(device.isLoadedInMemory());
    
    std::cout << "  ? Initramfs loaded into memory" << std::endl;
    
    // Verify data in memory
    for (size_t i = 0; i < testData.size(); ++i) {
        uint8_t byte = memory.read<uint8_t>(physAddr + i);
        assert(byte == testData[i]);
    }
    
    std::cout << "  ? Memory data integrity verified" << std::endl;
    std::cout << "  ? Loaded at: 0x" << std::hex << physAddr << std::dec << std::endl;
}

void TestInitramfsFormatValidation() {
    std::cout << "Testing InitramfsDevice format validation..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    // Should validate as cpio
    bool valid = device.validateFormat();
    assert(valid);
    
    std::string format = device.getFormatType();
    std::cout << "  ? Format detected: " << format << std::endl;
    assert(format == "cpio-newc" || format == "cpio-old");
}

void TestInitramfsGzipFormat() {
    std::cout << "Testing gzip-compressed initramfs detection..." << std::endl;
    
    // Create a buffer with gzip magic
    std::vector<uint8_t> gzipData;
    gzipData.push_back(0x1f);  // gzip magic
    gzipData.push_back(0x8b);
    for (size_t i = 0; i < 100; ++i) {
        gzipData.push_back(0);
    }
    
    InitramfsDevice device("initramfs0", gzipData.data(), gzipData.size());
    
    bool valid = device.validateFormat();
    assert(valid);
    
    std::string format = device.getFormatType();
    assert(format == "cpio.gz");
    
    std::cout << "  ? Gzip format detected: " << format << std::endl;
}

void TestKernelBootstrapWithInitramfs() {
    std::cout << "Testing kernel bootstrap with initramfs..." << std::endl;
    
    Memory memory;
    CPUState cpu;
    
    // Create and load initramfs
    std::vector<uint8_t> testData = CreateTestInitramfs();
    uint64_t initramfsAddr = 0x2000000;  // 32 MB
    
    InitramfsDevice device("initramfs0", testData.data(), testData.size(), initramfsAddr);
    device.loadIntoMemory(memory);
    
    // Configure kernel bootstrap
    KernelBootstrapConfig config;
    config.entryPoint = 0x100000;           // 1 MB
    config.globalPointer = 0x600000;
    config.bootParamAddress = 0x10000;      // 64 KB
    config.initramfsAddress = initramfsAddr;
    config.initramfsSize = testData.size();
    config.enableVirtualAddressing = false;
    
    // Write boot parameters to memory
    uint64_t bytesWritten = WriteKernelBootParameters(memory, config.bootParamAddress, config);
    assert(bytesWritten > 0);
    
    std::cout << "  ? Boot parameters written: " << bytesWritten << " bytes" << std::endl;
    
    // Verify initramfs info in boot parameters
    uint64_t storedInitramfsAddr = memory.read<uint64_t>(config.bootParamAddress + 0x10);
    uint64_t storedInitramfsSize = memory.read<uint64_t>(config.bootParamAddress + 0x18);
    
    assert(storedInitramfsAddr == initramfsAddr);
    assert(storedInitramfsSize == testData.size());
    
    std::cout << "  ? Initramfs address in boot params: 0x" << std::hex << storedInitramfsAddr << std::dec << std::endl;
    std::cout << "  ? Initramfs size in boot params: " << storedInitramfsSize << " bytes" << std::endl;
    
    // Initialize kernel bootstrap state
    uint64_t stackPointer = InitializeKernelBootstrapState(cpu, memory, config);
    
    // Verify CPU state
    assert(cpu.GetIP() == config.entryPoint);
    assert(cpu.GetGR(1) == config.globalPointer);
    assert(cpu.GetGR(12) == stackPointer);
    assert(cpu.GetGR(28) == config.bootParamAddress);
    
    std::cout << "  ? Kernel bootstrap state initialized" << std::endl;
    std::cout << "  ? Entry point: 0x" << std::hex << cpu.GetIP() << std::dec << std::endl;
    std::cout << "  ? Boot param address (r28): 0x" << std::hex << cpu.GetGR(28) << std::dec << std::endl;
}

void TestInitramfsDeviceInfo() {
    std::cout << "Testing InitramfsDevice getInfo()..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    device.connect();
    
    StorageDeviceInfo info = device.getInfo();
    
    assert(info.deviceId == "initramfs0");
    assert(info.type == StorageDeviceType::MEMORY_BACKED);
    assert(info.sizeBytes == testData.size());
    assert(info.blockSize == 4096);
    assert(info.readOnly == true);
    assert(info.connected == true);
    
    std::cout << "  ? Device info verified" << std::endl;
}

void TestInitramfsStatistics() {
    std::cout << "Testing InitramfsDevice statistics..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    InitramfsDevice device("initramfs0", testData.data(), testData.size());
    
    device.connect();
    
    // Perform some reads
    std::vector<uint8_t> buffer(device.getBlockSize());
    device.readBlocks(0, 1, buffer.data());
    device.readBlocks(1, 1, buffer.data());
    
    std::string stats = device.getStatistics();
    assert(!stats.empty());
    
    std::cout << "  ? Statistics generated" << std::endl;
    std::cout << stats << std::endl;
}

void TestInitramfsCustomPhysicalAddress() {
    std::cout << "Testing custom physical address..." << std::endl;
    
    std::vector<uint8_t> testData = CreateTestInitramfs();
    
    // Test different physical addresses
    uint64_t customAddr1 = 0x4000000;  // 64 MB
    InitramfsDevice device1("initramfs0", testData.data(), testData.size(), customAddr1);
    assert(device1.getPhysicalAddress() == customAddr1);
    
    // Test setting address after creation
    InitramfsDevice device2("initramfs1", testData.data(), testData.size());
    device2.setPhysicalAddress(0x8000000);  // 128 MB
    assert(device2.getPhysicalAddress() == 0x8000000);
    
    std::cout << "  ? Custom physical addresses work correctly" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "InitramfsDevice Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    try {
        TestInitramfsDeviceCreationFromMemory();
        std::cout << std::endl;
        
        TestInitramfsDeviceCreationFromFile();
        std::cout << std::endl;
        
        TestInitramfsDeviceConnect();
        std::cout << std::endl;
        
        TestInitramfsDeviceReadBlocks();
        std::cout << std::endl;
        
        TestInitramfsDeviceWriteBlocks();
        std::cout << std::endl;
        
        TestInitramfsLoadIntoMemory();
        std::cout << std::endl;
        
        TestInitramfsFormatValidation();
        std::cout << std::endl;
        
        TestInitramfsGzipFormat();
        std::cout << std::endl;
        
        TestKernelBootstrapWithInitramfs();
        std::cout << std::endl;
        
        TestInitramfsDeviceInfo();
        std::cout << std::endl;
        
        TestInitramfsStatistics();
        std::cout << std::endl;
        
        TestInitramfsCustomPhysicalAddress();
        std::cout << std::endl;
        
        std::cout << "========================================" << std::endl;
        std::cout << "All tests passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << std::endl;
        std::cerr << "========================================" << std::endl;
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}
