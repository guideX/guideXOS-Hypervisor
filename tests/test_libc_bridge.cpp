#include "libc_bridge.h"
#include "abi.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

void test_malloc_free() {
    std::cout << "Testing malloc/free..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);  // 256MB
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    // Test malloc
    uint64_t addr1 = bridge.Malloc(memory, 1024);
    assert(addr1 != 0);
    std::cout << "  malloc(1024) = 0x" << std::hex << addr1 << std::dec << std::endl;
    
    uint64_t addr2 = bridge.Malloc(memory, 2048);
    assert(addr2 != 0);
    assert(addr2 != addr1);
    std::cout << "  malloc(2048) = 0x" << std::hex << addr2 << std::dec << std::endl;
    
    // Test free
    bridge.Free(memory, addr1);
    std::cout << "  free(0x" << std::hex << addr1 << std::dec << ")" << std::endl;
    
    // Allocate again - should reuse freed block
    uint64_t addr3 = bridge.Malloc(memory, 512);
    assert(addr3 != 0);
    std::cout << "  malloc(512) = 0x" << std::hex << addr3 << std::dec << std::endl;
    
    bridge.Free(memory, addr2);
    bridge.Free(memory, addr3);
    
    std::cout << "  PASSED\n" << std::endl;
}

void test_calloc() {
    std::cout << "Testing calloc..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    uint64_t addr = bridge.Calloc(memory, 10, 100);
    assert(addr != 0);
    std::cout << "  calloc(10, 100) = 0x" << std::hex << addr << std::dec << std::endl;
    
    // Verify zero initialization
    bool allZero = true;
    for (size_t i = 0; i < 1000; ++i) {
        if (memory.read<uint8_t>(addr + i) != 0) {
            allZero = false;
            break;
        }
    }
    assert(allZero);
    std::cout << "  Memory is zero-initialized" << std::endl;
    
    bridge.Free(memory, addr);
    
    std::cout << "  PASSED\n" << std::endl;
}

void test_string_functions() {
    std::cout << "Testing string functions..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    // Setup test strings in memory
    uint64_t str1Addr = 0x10000;
    uint64_t str2Addr = 0x10100;
    uint64_t destAddr = 0x10200;
    
    const char* testStr = "Hello, World!";
    for (size_t i = 0; i <= strlen(testStr); ++i) {
        memory.write<uint8_t>(str1Addr + i, testStr[i]);
    }
    
    // Test strlen
    size_t len = bridge.Strlen(memory, str1Addr);
    assert(len == strlen(testStr));
    std::cout << "  strlen(\"" << testStr << "\") = " << len << std::endl;
    
    // Test strcpy
    bridge.Strcpy(memory, destAddr, str1Addr);
    char copied[100];
    for (size_t i = 0; i <= len; ++i) {
        copied[i] = memory.read<uint8_t>(destAddr + i);
    }
    assert(strcmp(copied, testStr) == 0);
    std::cout << "  strcpy works correctly" << std::endl;
    
    // Test strcmp
    bridge.Strcpy(memory, str2Addr, str1Addr);
    int cmp = bridge.Strcmp(memory, str1Addr, str2Addr);
    assert(cmp == 0);
    std::cout << "  strcmp(identical strings) = " << cmp << std::endl;
    
    memory.write<uint8_t>(str2Addr, 'A');
    cmp = bridge.Strcmp(memory, str1Addr, str2Addr);
    assert(cmp != 0);
    std::cout << "  strcmp(different strings) = " << cmp << std::endl;
    
    std::cout << "  PASSED\n" << std::endl;
}

void test_memory_functions() {
    std::cout << "Testing memory functions..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    uint64_t srcAddr = 0x20000;
    uint64_t destAddr = 0x21000;
    
    // Fill source with pattern
    for (size_t i = 0; i < 256; ++i) {
        memory.write<uint8_t>(srcAddr + i, i & 0xFF);
    }
    
    // Test memcpy
    bridge.Memcpy(memory, destAddr, srcAddr, 256);
    bool copyOk = true;
    for (size_t i = 0; i < 256; ++i) {
        if (memory.read<uint8_t>(destAddr + i) != (i & 0xFF)) {
            copyOk = false;
            break;
        }
    }
    assert(copyOk);
    std::cout << "  memcpy works correctly" << std::endl;
    
    // Test memset
    bridge.Memset(memory, destAddr, 0xAA, 128);
    bool setOk = true;
    for (size_t i = 0; i < 128; ++i) {
        if (memory.read<uint8_t>(destAddr + i) != 0xAA) {
            setOk = false;
            break;
        }
    }
    assert(setOk);
    std::cout << "  memset works correctly" << std::endl;
    
    // Test memcmp
    bridge.Memcpy(memory, destAddr, srcAddr, 256);
    int cmp = bridge.Memcmp(memory, srcAddr, destAddr, 256);
    assert(cmp == 0);
    std::cout << "  memcmp(identical) = " << cmp << std::endl;
    
    memory.write<uint8_t>(destAddr + 10, 0xFF);
    cmp = bridge.Memcmp(memory, srcAddr, destAddr, 256);
    assert(cmp != 0);
    std::cout << "  memcmp(different) = " << cmp << std::endl;
    
    std::cout << "  PASSED\n" << std::endl;
}

void test_sprintf() {
    std::cout << "Testing sprintf..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);
    CPUState cpu;
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    uint64_t bufAddr = 0x30000;
    uint64_t fmtAddr = 0x30100;
    
    // Write format string
    const char* format = "Value: %d, String: %s";
    for (size_t i = 0; i <= strlen(format); ++i) {
        memory.write<uint8_t>(fmtAddr + i, format[i]);
    }
    
    // Write test string argument
    uint64_t strArgAddr = 0x30200;
    const char* strArg = "test";
    for (size_t i = 0; i <= strlen(strArg); ++i) {
        memory.write<uint8_t>(strArgAddr + i, strArg[i]);
    }
    
    // Setup CPU registers with arguments
    cpu.SetGR(34, 42);           // Integer argument
    cpu.SetGR(35, strArgAddr);   // String argument
    
    // Call sprintf
    int64_t written = bridge.Sprintf(cpu, memory, bufAddr, fmtAddr);
    assert(written > 0);
    
    // Read result
    char result[256];
    for (size_t i = 0; i < written; ++i) {
        result[i] = memory.read<uint8_t>(bufAddr + i);
    }
    result[written] = '\0';
    
    std::cout << "  sprintf result: \"" << result << "\"" << std::endl;
    std::cout << "  PASSED\n" << std::endl;
}

void test_heap_stats() {
    std::cout << "Testing heap statistics..." << std::endl;
    
    Memory memory(1024 * 1024 * 256);
    LibCBridge bridge;
    LinuxABI abi;
    bridge.Initialize(&abi);
    
    // Allocate some memory
    std::vector<uint64_t> addrs;
    for (int i = 0; i < 5; ++i) {
        uint64_t addr = bridge.Malloc(memory, 1024 * (i + 1));
        addrs.push_back(addr);
    }
    
    size_t allocated, free, numAllocs;
    bridge.GetHeapStats(allocated, free, numAllocs);
    
    std::cout << "  Allocated: " << allocated << " bytes" << std::endl;
    std::cout << "  Free: " << free << " bytes" << std::endl;
    std::cout << "  Number of allocations: " << numAllocs << std::endl;
    
    assert(numAllocs == 5);
    
    // Free some
    bridge.Free(memory, addrs[0]);
    bridge.Free(memory, addrs[2]);
    bridge.Free(memory, addrs[4]);
    
    bridge.GetHeapStats(allocated, free, numAllocs);
    std::cout << "  After freeing 3 blocks:" << std::endl;
    std::cout << "  Allocated: " << allocated << " bytes" << std::endl;
    std::cout << "  Free: " << free << " bytes" << std::endl;
    std::cout << "  Number of allocations: " << numAllocs << std::endl;
    
    assert(numAllocs == 2);
    
    // Cleanup
    for (auto addr : addrs) {
        bridge.Free(memory, addr);
    }
    
    std::cout << "  PASSED\n" << std::endl;
}

int main() {
    std::cout << "=== LibC Bridge Tests ===" << std::endl << std::endl;
    
    try {
        test_malloc_free();
        test_calloc();
        test_string_functions();
        test_memory_functions();
        test_sprintf();
        test_heap_stats();
        
        std::cout << "=== ALL TESTS PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
