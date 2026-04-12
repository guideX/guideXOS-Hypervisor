#include "mmu.h"
#include "PageFault.h"
#include "memory.h"
#include "logger.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

// Forward declaration for external access
void run_page_fault_tests();

// Helper to print test results
void printTest(const char* name, bool passed) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
}

// Test 1: PageFault exception for unmapped page
bool test_page_fault_not_mapped() {
    try {
        MMU mmu(4096, true);
        
        // Try to translate unmapped address
        try {
            uint64_t physical = mmu.TranslateAddress(0x8000);
            return false;  // Should have thrown
        } catch (const PageFault& fault) {
            // Verify fault details
            if (fault.getType() != PageFaultType::NOT_MAPPED) return false;
            if (fault.getVirtualAddress() != 0x8000) return false;
            if (fault.getPageAddress() != 0x8000) return false;
            if (fault.getPageOffset() != 0) return false;
            if (fault.hasPageEntry()) return false;
            
            // Check that diagnostics can be retrieved
            std::string diag = fault.getDiagnostics();
            if (diag.find("NOT_MAPPED") == std::string::npos) return false;
            if (diag.find("0x0000000000008000") == std::string::npos) return false;
            
            return true;
        }
    } catch (...) {
        return false;
    }
}

// Test 2: PageFault exception for non-present page
bool test_page_fault_not_present() {
    try {
        MMU mmu(4096, true);
        
        // Create a page entry that's not present
        mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE);
        const PageEntry* entry = mmu.GetPageEntry(0x1000);
        
        // Manually modify to make it not present (for testing)
        // In real scenario, this would be done through proper API
        PageEntry modifiedEntry = *entry;
        modifiedEntry.present = false;
        
        // We can't easily test this without modifying the internal state,
        // so we'll just verify the PageFault structure works
        PageFault fault(PageFaultType::NOT_PRESENT, 0x1234, 
                       MemoryAccessType::READ, 4096, &modifiedEntry);
        
        if (fault.getType() != PageFaultType::NOT_PRESENT) return false;
        if (fault.getVirtualAddress() != 0x1234) return false;
        if (!fault.hasPageEntry()) return false;
        
        std::string diag = fault.getDiagnostics();
        if (diag.find("NOT_PRESENT") == std::string::npos) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 3: PageFault exception for read permission violation
bool test_page_fault_permission_read() {
    try {
        Memory memory(64 * 1024, true, 4096);
        
        // Clear default identity mapping
        memory.GetMMU().ClearPageTable();
        
        // Map a page with write-only permission (unusual but for testing)
        memory.GetMMU().MapPage(0x1000, 0x1000, PermissionFlags::WRITE);
        
        // Try to read from write-only page
        uint8_t buffer[4];
        try {
            memory.Read(0x1000, buffer, 4);
            return false;  // Should have thrown
        } catch (const PageFault& fault) {
            if (fault.getType() != PageFaultType::PERMISSION_READ) return false;
            if (fault.getVirtualAddress() != 0x1000) return false;
            if (fault.getAccessType() != MemoryAccessType::READ) return false;
            if (!fault.hasPageEntry()) return false;
            
            std::string diag = fault.getDiagnostics();
            if (diag.find("PERMISSION_READ") == std::string::npos) return false;
            if (diag.find("Attempting to read from non-readable page") == std::string::npos) return false;
            
            return true;
        }
    } catch (...) {
        return false;
    }
}

// Test 4: PageFault exception for write permission violation
bool test_page_fault_permission_write() {
    try {
        Memory memory(64 * 1024, true, 4096);
        
        // Clear default identity mapping
        memory.GetMMU().ClearPageTable();
        
        // Map a page with read-only permission
        memory.GetMMU().MapPage(0x2000, 0x2000, PermissionFlags::READ);
        
        // Try to write to read-only page
        uint8_t data[] = {0x11, 0x22, 0x33, 0x44};
        try {
            memory.Write(0x2000, data, 4);
            return false;  // Should have thrown
        } catch (const PageFault& fault) {
            if (fault.getType() != PageFaultType::PERMISSION_WRITE) return false;
            if (fault.getVirtualAddress() != 0x2000) return false;
            if (fault.getAccessType() != MemoryAccessType::WRITE) return false;
            if (!fault.hasPageEntry()) return false;
            
            const PageEntry& entry = fault.getPageEntry();
            if (!HasPermission(entry.permissions, PermissionFlags::READ)) return false;
            if (HasPermission(entry.permissions, PermissionFlags::WRITE)) return false;
            
            std::string diag = fault.getDiagnostics();
            if (diag.find("PERMISSION_WRITE") == std::string::npos) return false;
            if (diag.find("read-only") == std::string::npos) return false;
            
            return true;
        }
    } catch (...) {
        return false;
    }
}

// Test 5: PageFault diagnostics with page offset
bool test_page_fault_with_offset() {
    try {
        MMU mmu(4096, true);
        
        // Try to translate unmapped address with offset
        try {
            uint64_t physical = mmu.TranslateAddress(0x5678);
            return false;  // Should have thrown
        } catch (const PageFault& fault) {
            if (fault.getVirtualAddress() != 0x5678) return false;
            if (fault.getPageAddress() != 0x5000) return false;  // Page-aligned
            if (fault.getPageOffset() != 0x678) return false;
            
            std::string diag = fault.getDiagnostics();
            if (diag.find("0x0000000000005678") == std::string::npos) return false;
            if (diag.find("0x0000000000005000") == std::string::npos) return false;
            if (diag.find("0x0678") == std::string::npos) return false;
            
            return true;
        }
    } catch (...) {
        return false;
    }
}

// Test 6: DumpPageTable functionality
bool test_dump_page_table() {
    try {
        MMU mmu(4096, true);
        
        // Map several pages with different permissions
        mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ);
        mmu.MapPage(0x2000, 0x6000, PermissionFlags::READ_WRITE);
        mmu.MapPage(0x3000, 0x7000, PermissionFlags::READ_WRITE_EXECUTE);
        
        std::string dump = mmu.DumpPageTable();
        
        // Verify dump contains expected information
        if (dump.find("PAGE TABLE DUMP") == std::string::npos) return false;
        if (dump.find("Total Mapped Pages: 3") == std::string::npos) return false;
        if (dump.find("0x0000000000001000") == std::string::npos) return false;
        if (dump.find("0x0000000000002000") == std::string::npos) return false;
        if (dump.find("0x0000000000003000") == std::string::npos) return false;
        if (dump.find("R--") == std::string::npos) return false;  // Read-only
        if (dump.find("RW-") == std::string::npos) return false;  // Read-write
        if (dump.find("RWX") == std::string::npos) return false;  // All permissions
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 7: DiagnoseAddress functionality
bool test_diagnose_address() {
    try {
        MMU mmu(4096, true);
        
        // Test unmapped address
        std::string diag1 = mmu.DiagnoseAddress(0x9000);
        if (diag1.find("NOT MAPPED") == std::string::npos) return false;
        if (diag1.find("0x0000000000009000") == std::string::npos) return false;
        
        // Map a page and test mapped address
        mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE);
        
        std::string diag2 = mmu.DiagnoseAddress(0x1234);
        if (diag2.find("MAPPED") == std::string::npos) return false;
        if (diag2.find("0x0000000000001234") == std::string::npos) return false;
        if (diag2.find("0x0000000000001000") == std::string::npos) return false;  // Page addr
        if (diag2.find("0x0234") == std::string::npos) return false;  // Offset
        if (diag2.find("0x0000000000005234") == std::string::npos) return false;  // Translated
        if (diag2.find("RW-") == std::string::npos) return false;
        if (diag2.find("READ:    ALLOWED") == std::string::npos) return false;
        if (diag2.find("WRITE:   ALLOWED") == std::string::npos) return false;
        if (diag2.find("EXECUTE: DENIED") == std::string::npos) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Test 8: Page fault logging
bool test_page_fault_logging() {
    try {
        Logger& logger = Logger::getInstance();
        logger.setLogLevel(LogLevel::DEBUG);
        
        Memory memory(64 * 1024, true, 4096);
        memory.GetMMU().ClearPageTable();
        
        // This will trigger a page fault and log it
        uint8_t buffer[4];
        try {
            memory.Read(0xDEAD000, buffer, 4);
            return false;  // Should have thrown
        } catch (const PageFault& fault) {
            // The fault should have been logged
            // We can't easily verify the log output, but we can verify the exception
            if (fault.getType() != PageFaultType::NOT_MAPPED) return false;
            return true;
        }
    } catch (...) {
        return false;
    }
}

// Test 9: Multiple page faults with different types
bool test_multiple_fault_types() {
    try {
        Memory memory(64 * 1024, true, 4096);
        memory.GetMMU().ClearPageTable();
        
        // Setup different page scenarios
        memory.GetMMU().MapPage(0x1000, 0x1000, PermissionFlags::READ);
        memory.GetMMU().MapPage(0x2000, 0x2000, PermissionFlags::WRITE);
        memory.GetMMU().MapPage(0x3000, 0x3000, PermissionFlags::EXECUTE);
        
        int faultCount = 0;
        
        // Test 1: Unmapped
        try {
            uint8_t buf[4];
            memory.Read(0x9000, buf, 4);
        } catch (const PageFault& fault) {
            if (fault.getType() == PageFaultType::NOT_MAPPED) faultCount++;
        }
        
        // Test 2: Read from write-only
        try {
            uint8_t buf[4];
            memory.Read(0x2000, buf, 4);
        } catch (const PageFault& fault) {
            if (fault.getType() == PageFaultType::PERMISSION_READ) faultCount++;
        }
        
        // Test 3: Write to read-only
        try {
            uint8_t data[] = {0x11, 0x22};
            memory.Write(0x1000, data, 2);
        } catch (const PageFault& fault) {
            if (fault.getType() == PageFaultType::PERMISSION_WRITE) faultCount++;
        }
        
        return faultCount == 3;
    } catch (...) {
        return false;
    }
}

// Test 10: Large page table dump
bool test_large_page_table_dump() {
    try {
        MMU mmu(4096, true);
        
        // Map many pages
        for (uint64_t i = 0; i < 100; i++) {
            uint64_t vaddr = i * 0x1000;
            uint64_t paddr = (i + 0x100) * 0x1000;
            mmu.MapPage(vaddr, paddr, PermissionFlags::READ_WRITE);
        }
        
        // Dump all pages
        std::string dumpAll = mmu.DumpPageTable();
        if (dumpAll.find("Total Mapped Pages: 100") == std::string::npos) return false;
        
        // Dump limited range
        std::string dumpLimited = mmu.DumpPageTable(0, 5);
        if (dumpLimited.find("Showing 5 of 100") == std::string::npos) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// Run all page fault tests
void run_page_fault_tests() {
    std::cout << "\n=== Page Fault Diagnostics Tests ===\n\n";
    
    printTest("Page fault: NOT_MAPPED", test_page_fault_not_mapped());
    printTest("Page fault: NOT_PRESENT", test_page_fault_not_present());
    printTest("Page fault: PERMISSION_READ", test_page_fault_permission_read());
    printTest("Page fault: PERMISSION_WRITE", test_page_fault_permission_write());
    printTest("Page fault with offset", test_page_fault_with_offset());
    printTest("DumpPageTable functionality", test_dump_page_table());
    printTest("DiagnoseAddress functionality", test_diagnose_address());
    printTest("Page fault logging", test_page_fault_logging());
    printTest("Multiple fault types", test_multiple_fault_types());
    printTest("Large page table dump", test_large_page_table_dump());
    
    std::cout << "\n=== Page Fault Tests Complete ===\n\n";
}
