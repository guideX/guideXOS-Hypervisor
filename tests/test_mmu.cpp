#include "mmu.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

// Helper to print test results
void printTest(const char* name, bool passed) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
}

// Test 1: Basic MMU construction and configuration
bool test_mmu_construction() {
    try {
        MMU mmu(4096, true);
        
        if (mmu.GetPageSize() != 4096) return false;
        if (!mmu.IsEnabled()) return false;
        
        mmu.SetEnabled(false);
        if (mmu.IsEnabled()) return false;
        
        mmu.SetEnabled(true);
        if (!mmu.IsEnabled()) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 2: Page mapping and unmapping
bool test_page_mapping() {
    try {
        MMU mmu(4096, true);
        
        // Map a page
        mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE);
        
        // Check mapping exists
        const PageEntry* entry = mmu.GetPageEntry(0x1000);
        if (!entry) return false;
        if (entry->physicalAddress != 0x5000) return false;
        if (!entry->present) return false;
        
        // Unmap the page
        mmu.UnmapPage(0x1000);
        
        // Check mapping is gone
        entry = mmu.GetPageEntry(0x1000);
        if (entry != nullptr) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 3: Address translation
bool test_address_translation() {
    try {
        MMU mmu(4096, true);
        
        // Map page at 0x1000 to physical 0x5000
        mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE_EXECUTE);
        
        // Translate address within page
        uint64_t physical = mmu.TranslateAddress(0x1000);
        if (physical != 0x5000) return false;
        
        // Translate address with offset
        physical = mmu.TranslateAddress(0x1234);
        if (physical != 0x5234) return false;
        
        // Disable MMU - should use identity mapping
        mmu.SetEnabled(false);
        physical = mmu.TranslateAddress(0x9999);
        if (physical != 0x9999) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 4: Permission checking
bool test_permissions() {
    MMU mmu(4096, true);
    
    // Map read-only page
    mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ);
    
    // Should allow read
    if (!mmu.CheckPermission(0x1000, MemoryAccessType::READ)) return false;
    
    // Should deny write
    if (mmu.CheckPermission(0x1000, MemoryAccessType::WRITE)) return false;
    
    // Should deny execute
    if (mmu.CheckPermission(0x1000, MemoryAccessType::EXECUTE)) return false;
    
    // Map read-write-execute page
    mmu.MapPage(0x2000, 0x6000, PermissionFlags::READ_WRITE_EXECUTE);
    
    // Should allow all
    if (!mmu.CheckPermission(0x2000, MemoryAccessType::READ)) return false;
    if (!mmu.CheckPermission(0x2000, MemoryAccessType::WRITE)) return false;
    if (!mmu.CheckPermission(0x2000, MemoryAccessType::EXECUTE)) return false;
    
    // Update permissions
    mmu.SetPagePermissions(0x2000, PermissionFlags::READ);
    
    // Should now deny write
    if (mmu.CheckPermission(0x2000, MemoryAccessType::WRITE)) return false;
    
    return true;
}

// Test 5: Identity mapping
bool test_identity_mapping() {
    try {
        MMU mmu(4096, true);
        
        // Map 16KB range with identity mapping
        mmu.MapIdentityRange(0x10000, 16384, PermissionFlags::READ_WRITE);
        
        // Should have 4 pages mapped
        if (mmu.GetMappedPageCount() != 4) return false;
        
        // Check each page
        for (uint64_t addr = 0x10000; addr < 0x14000; addr += 4096) {
            uint64_t physical = mmu.TranslateAddress(addr);
            if (physical != addr) return false;  // Identity mapping
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 6: Read hooks
bool test_read_hooks() {
    MMU mmu(4096, true);
    
    bool hookCalled = false;
    uint64_t hookAddress = 0;
    
    // Register read hook
    size_t hookId = mmu.RegisterReadHook([&](HookContext& ctx, uint8_t* data) {
        hookCalled = true;
        hookAddress = ctx.address;
    });
    
    // Map and invoke hook
    mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ);
    uint8_t buffer[8];
    mmu.InvokeReadHooks(0x1234, 0x5234, 8, buffer);
    
    if (!hookCalled) return false;
    if (hookAddress != 0x1234) return false;
    
    // Unregister hook
    mmu.UnregisterReadHook(hookId);
    
    hookCalled = false;
    mmu.InvokeReadHooks(0x1234, 0x5234, 8, buffer);
    
    // Hook should not be called
    if (hookCalled) return false;
    
    return true;
}

// Test 7: Write hooks
bool test_write_hooks() {
    MMU mmu(4096, true);
    
    bool hookCalled = false;
    size_t hookSize = 0;
    
    // Register write hook
    size_t hookId = mmu.RegisterWriteHook([&](HookContext& ctx, uint8_t* data) {
        hookCalled = true;
        hookSize = ctx.size;
    });
    
    // Map and invoke hook
    mmu.MapPage(0x2000, 0x6000, PermissionFlags::WRITE);
    uint8_t buffer[16];
    mmu.InvokeWriteHooks(0x2000, 0x6000, 16, buffer);
    
    if (!hookCalled) return false;
    if (hookSize != 16) return false;
    
    // Test hook denial
    size_t denyHookId = mmu.RegisterWriteHook([](HookContext& ctx, uint8_t* data) {
        *ctx.allowAccess = false;  // Deny access
    });
    
    bool allowed = mmu.InvokeWriteHooks(0x2000, 0x6000, 16, buffer);
    if (allowed) return false;  // Should be denied
    
    mmu.ClearHooks();
    return true;
}

// Test 8: Integration with Memory class
bool test_memory_integration() {
    try {
        // Create memory with MMU enabled
        Memory memory(64 * 1024, true, 4096);
        
        // Memory should be identity-mapped by default
        uint8_t testData[] = {0x12, 0x34, 0x56, 0x78};
        memory.Write(0x1000, testData, 4);
        
        uint8_t readData[4];
        memory.Read(0x1000, readData, 4);
        
        if (std::memcmp(testData, readData, 4) != 0) return false;
        
        // Test with typed access
        memory.write<uint64_t>(0x2000, 0xDEADBEEFCAFEBABE);
        uint64_t value = memory.read<uint64_t>(0x2000);
        
        if (value != 0xDEADBEEFCAFEBABE) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 9: Permission violations
bool test_permission_violations() {
    try {
        Memory memory(64 * 1024, true, 4096);
        
        // Clear default identity mapping and create restricted mapping
        memory.GetMMU().ClearPageTable();
        memory.GetMMU().MapPage(0x1000, 0x1000, PermissionFlags::READ);  // Read-only
        
        // Write should fail
        uint8_t testData[] = {0x11, 0x22};
        try {
            memory.Write(0x1000, testData, 2);
            return false;  // Should have thrown
        } catch (const std::runtime_error&) {
            // Expected
        }
        
        // Make it writable
        memory.GetMMU().SetPagePermissions(0x1000, PermissionFlags::READ_WRITE);
        
        // Write should now succeed
        memory.Write(0x1000, testData, 2);
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 10: MMU disabled mode
bool test_mmu_disabled() {
    try {
        // Create memory with MMU disabled
        Memory memory(64 * 1024, false, 4096);
        
        // Should work without any page mappings
        uint64_t testValue = 0x123456789ABCDEF0;
        memory.write<uint64_t>(0x8000, testValue);
        uint64_t readValue = memory.read<uint64_t>(0x8000);
        
        if (readValue != testValue) return false;
        
        // Enable MMU
        memory.SetMMUEnabled(true);
        
        // Should still work with default identity mapping
        memory.write<uint64_t>(0x9000, testValue);
        readValue = memory.read<uint64_t>(0x9000);
        
        if (readValue != testValue) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 11: Hook-based monitoring
bool test_hook_monitoring() {
    try {
        Memory memory(64 * 1024, true, 4096);
        
        std::vector<uint64_t> readAddresses;
        std::vector<uint64_t> writeAddresses;
        
        // Register monitoring hooks
        memory.GetMMU().RegisterReadHook([&](HookContext& ctx, uint8_t* data) {
            readAddresses.push_back(ctx.address);
        });
        
        memory.GetMMU().RegisterWriteHook([&](HookContext& ctx, uint8_t* data) {
            writeAddresses.push_back(ctx.address);
        });
        
        // Perform some memory operations
        memory.write<uint32_t>(0x1000, 0x12345678);
        memory.read<uint32_t>(0x1000);
        memory.write<uint64_t>(0x2000, 0xDEADBEEF);
        memory.read<uint64_t>(0x2000);
        
        // Check hooks were called
        if (writeAddresses.size() != 2) return false;
        if (readAddresses.size() != 2) return false;
        if (writeAddresses[0] != 0x1000) return false;
        if (readAddresses[0] != 0x1000) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

int main() {
    std::cout << "=== MMU Test Suite ===" << std::endl;
    std::cout << std::endl;
    
    printTest("MMU Construction", test_mmu_construction());
    printTest("Page Mapping", test_page_mapping());
    printTest("Address Translation", test_address_translation());
    printTest("Permission Checking", test_permissions());
    printTest("Identity Mapping", test_identity_mapping());
    printTest("Read Hooks", test_read_hooks());
    printTest("Write Hooks", test_write_hooks());
    printTest("Memory Integration", test_memory_integration());
    printTest("Permission Violations", test_permission_violations());
    printTest("MMU Disabled Mode", test_mmu_disabled());
    printTest("Hook-based Monitoring", test_hook_monitoring());
    
    std::cout << std::endl;
    std::cout << "=== All Tests Complete ===" << std::endl;
    
    return 0;
}
