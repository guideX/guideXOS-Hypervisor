#pragma once

#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include "IMMU.h"

namespace ia64 {

/**
 * PageFaultType - Categorizes the type of page fault
 */
enum class PageFaultType {
    NOT_MAPPED,      // Virtual address not mapped in page table
    NOT_PRESENT,     // Page entry exists but not marked present
    PERMISSION_READ, // Read permission violation
    PERMISSION_WRITE,// Write permission violation
    PERMISSION_EXEC  // Execute permission violation
};

/**
 * PageFault - Detailed exception for MMU page faults
 * 
 * Provides comprehensive diagnostic information when a page fault occurs,
 * including the faulting address, page table state, and permission details.
 */
class PageFault : public std::runtime_error {
public:
    /**
     * Construct a PageFault exception
     * @param type Type of page fault
     * @param virtualAddr Virtual address that caused the fault
     * @param accessType Memory access type attempted
     * @param pageSize Size of pages in MMU
     * @param entry Page entry if it exists (nullptr if not mapped)
     */
    PageFault(PageFaultType type, 
              uint64_t virtualAddr, 
              MemoryAccessType accessType,
              size_t pageSize,
              const PageEntry* entry = nullptr)
        : std::runtime_error(buildMessage(type, virtualAddr, accessType, pageSize, entry))
        , type_(type)
        , virtualAddr_(virtualAddr)
        , accessType_(accessType)
        , pageSize_(pageSize)
        , pageEntry_(entry ? *entry : PageEntry())
        , hasEntry_(entry != nullptr)
    {
    }

    // Getters for diagnostic information
    PageFaultType getType() const { return type_; }
    uint64_t getVirtualAddress() const { return virtualAddr_; }
    uint64_t getPageAddress() const { return virtualAddr_ & ~(pageSize_ - 1); }
    uint64_t getPageOffset() const { return virtualAddr_ & (pageSize_ - 1); }
    MemoryAccessType getAccessType() const { return accessType_; }
    size_t getPageSize() const { return pageSize_; }
    bool hasPageEntry() const { return hasEntry_; }
    const PageEntry& getPageEntry() const { return pageEntry_; }

    /**
     * Get a detailed diagnostic string with full page fault information
     */
    std::string getDiagnostics() const {
        std::ostringstream oss;
        
        oss << "========== PAGE FAULT DIAGNOSTIC ==========\n";
        oss << "Type:            " << getTypeString(type_) << "\n";
        oss << "Virtual Address: 0x" << std::hex << std::setw(16) << std::setfill('0') 
            << virtualAddr_ << std::dec << "\n";
        oss << "Page Address:    0x" << std::hex << std::setw(16) << std::setfill('0') 
            << getPageAddress() << std::dec << "\n";
        oss << "Page Offset:     0x" << std::hex << std::setw(4) << std::setfill('0') 
            << getPageOffset() << std::dec << " (" << getPageOffset() << " bytes)\n";
        oss << "Page Size:       " << pageSize_ << " bytes (" << (pageSize_ / 1024) << " KB)\n";
        oss << "Access Type:     " << getAccessTypeString(accessType_) << "\n";
        
        if (hasEntry_) {
            oss << "\nPage Entry Details:\n";
            oss << "  Physical Addr: 0x" << std::hex << std::setw(16) << std::setfill('0') 
                << pageEntry_.physicalAddress << std::dec << "\n";
            oss << "  Permissions:   " << getPermissionString(pageEntry_.permissions) << "\n";
            oss << "  Present:       " << (pageEntry_.present ? "YES" : "NO") << "\n";
            oss << "  Accessed:      " << (pageEntry_.accessed ? "YES" : "NO") << "\n";
            oss << "  Dirty:         " << (pageEntry_.dirty ? "YES" : "NO") << "\n";
        } else {
            oss << "\nPage Entry:      NOT MAPPED\n";
        }
        
        oss << "\nPossible Causes:\n";
        oss << getCausesString(type_);
        
        oss << "\n===========================================\n";
        
        return oss.str();
    }

private:
    static std::string buildMessage(PageFaultType type, 
                                    uint64_t virtualAddr, 
                                    MemoryAccessType accessType,
                                    size_t pageSize,
                                    const PageEntry* entry) {
        std::ostringstream oss;
        uint64_t pageAddr = virtualAddr & ~(pageSize - 1);
        
        oss << "PAGE FAULT [" << getTypeString(type) << "]: ";
        oss << getAccessTypeString(accessType) << " at virtual address 0x" 
            << std::hex << std::setw(16) << std::setfill('0') << virtualAddr 
            << " (page 0x" << std::setw(16) << pageAddr << ")";
        
        if (entry) {
            oss << " - Physical 0x" << std::setw(16) << entry->physicalAddress 
                << ", Perms: " << getPermissionString(entry->permissions);
        }
        
        return oss.str();
    }

    static std::string getTypeString(PageFaultType type) {
        switch (type) {
            case PageFaultType::NOT_MAPPED:      return "NOT_MAPPED";
            case PageFaultType::NOT_PRESENT:     return "NOT_PRESENT";
            case PageFaultType::PERMISSION_READ:  return "PERMISSION_READ";
            case PageFaultType::PERMISSION_WRITE: return "PERMISSION_WRITE";
            case PageFaultType::PERMISSION_EXEC:  return "PERMISSION_EXEC";
            default: return "UNKNOWN";
        }
    }

    static std::string getAccessTypeString(MemoryAccessType type) {
        switch (type) {
            case MemoryAccessType::READ:    return "READ";
            case MemoryAccessType::WRITE:   return "WRITE";
            case MemoryAccessType::EXECUTE: return "EXECUTE";
            default: return "UNKNOWN";
        }
    }

    static std::string getPermissionString(PermissionFlags perms) {
        std::ostringstream oss;
        bool hasAny = false;
        
        if (HasPermission(perms, PermissionFlags::READ)) {
            oss << "READ";
            hasAny = true;
        }
        if (HasPermission(perms, PermissionFlags::WRITE)) {
            if (hasAny) oss << " | ";
            oss << "WRITE";
            hasAny = true;
        }
        if (HasPermission(perms, PermissionFlags::EXECUTE)) {
            if (hasAny) oss << " | ";
            oss << "EXECUTE";
            hasAny = true;
        }
        
        if (!hasAny) {
            oss << "NONE";
        }
        
        return oss.str();
    }

    static std::string getCausesString(PageFaultType type) {
        switch (type) {
            case PageFaultType::NOT_MAPPED:
                return "  - Virtual address has not been mapped to physical memory\n"
                       "  - Check that MapPage() was called for this address range\n"
                       "  - Verify the loader properly mapped all program sections\n";
            
            case PageFaultType::NOT_PRESENT:
                return "  - Page entry exists but marked as not present\n"
                       "  - Page may have been swapped out (not implemented)\n"
                       "  - Page entry may be invalid\n";
            
            case PageFaultType::PERMISSION_READ:
                return "  - Attempting to read from non-readable page\n"
                       "  - Check page permissions include READ flag\n"
                       "  - Verify memory region should be readable\n";
            
            case PageFaultType::PERMISSION_WRITE:
                return "  - Attempting to write to non-writable page\n"
                       "  - Page may be in read-only section (e.g., .text, .rodata)\n"
                       "  - Check page permissions include WRITE flag\n"
                       "  - Consider if Copy-on-Write should be implemented\n";
            
            case PageFaultType::PERMISSION_EXEC:
                return "  - Attempting to execute from non-executable page\n"
                       "  - Page may be in data section (e.g., .data, .bss)\n"
                       "  - Check page permissions include EXECUTE flag\n"
                       "  - Verify instruction pointer is in valid code region\n";
            
            default:
                return "  - Unknown page fault type\n";
        }
    }

    PageFaultType type_;
    uint64_t virtualAddr_;
    MemoryAccessType accessType_;
    size_t pageSize_;
    PageEntry pageEntry_;
    bool hasEntry_;
};

} // namespace ia64
