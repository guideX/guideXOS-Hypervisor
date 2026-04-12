#include "mmu.h"
#include "mmu.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

MMU::MMU(size_t pageSize, bool enabled)
    : pageSize_(pageSize)
    , enabled_(enabled)
    , nextHookId_(0)
    , nextWatchpointId_(0)
    , instructionTraceDepth_(100)
    , cpuStateRef_(nullptr) {
    
    if (!IsPowerOfTwo(pageSize)) {
        throw std::invalid_argument("Page size must be a power of 2");
    }
    
    if (pageSize < 256 || pageSize > 256 * 1024 * 1024) {
        throw std::invalid_argument("Page size must be between 256 bytes and 256MB");
    }
}

// ============================================================================
// Address Translation
// ============================================================================

uint64_t MMU::TranslateAddress(uint64_t virtualAddr) const {
    // If MMU is disabled, use identity mapping
    if (!enabled_) {
        return virtualAddr;
    }
    
    // Get page-aligned address
    uint64_t pageAddr = AlignToPage(virtualAddr);
    uint64_t offset = GetPageOffset(virtualAddr);
    
    // Look up page in table
    auto it = pageTable_.find(pageAddr);
    if (it == pageTable_.end()) {
        // Page not mapped - throw detailed PageFault exception
        throw PageFault(PageFaultType::NOT_MAPPED, virtualAddr, 
                       MemoryAccessType::READ, pageSize_, nullptr);
    }
    
    const PageEntry& entry = it->second;
    
    // Check if page is present
    if (!entry.present) {
        // Page exists but not present - throw detailed PageFault exception
        throw PageFault(PageFaultType::NOT_PRESENT, virtualAddr, 
                       MemoryAccessType::READ, pageSize_, &entry);
    }
    
    // Return physical address
    return entry.physicalAddress + offset;
}

bool MMU::CanTranslate(uint64_t virtualAddr) const {
    // If MMU disabled, all addresses are translatable (identity)
    if (!enabled_) {
        return true;
    }
    
    uint64_t pageAddr = AlignToPage(virtualAddr);
    auto it = pageTable_.find(pageAddr);
    
    return it != pageTable_.end() && it->second.present;
}

// ============================================================================
// Page Table Management
// ============================================================================

void MMU::MapPage(uint64_t virtualAddr, uint64_t physicalAddr, PermissionFlags permissions) {
    // Align addresses to page boundaries
    uint64_t virtualPage = AlignToPage(virtualAddr);
    uint64_t physicalPage = AlignToPage(physicalAddr);
    
    // Create page entry
    PageEntry entry(physicalPage, permissions);
    
    // Insert or update in page table
    pageTable_[virtualPage] = entry;
}

void MMU::UnmapPage(uint64_t virtualAddr) {
    uint64_t pageAddr = AlignToPage(virtualAddr);
    pageTable_.erase(pageAddr);
}

const PageEntry* MMU::GetPageEntry(uint64_t virtualAddr) const {
    uint64_t pageAddr = AlignToPage(virtualAddr);
    auto it = pageTable_.find(pageAddr);
    
    if (it == pageTable_.end()) {
        return nullptr;
    }
    
    return &(it->second);
}

void MMU::SetPagePermissions(uint64_t virtualAddr, PermissionFlags permissions) {
    uint64_t pageAddr = AlignToPage(virtualAddr);
    auto it = pageTable_.find(pageAddr);
    
    if (it == pageTable_.end()) {
        std::ostringstream oss;
        oss << "Cannot set permissions: Virtual address 0x" << std::hex << virtualAddr 
            << " (page 0x" << pageAddr << ") not mapped";
        throw std::runtime_error(oss.str());
    }
    
    it->second.permissions = permissions;
}

// ============================================================================
// Permission Checking
// ============================================================================

bool MMU::CheckPermission(uint64_t virtualAddr, MemoryAccessType accessType) const {
    // If MMU disabled, all accesses are allowed
    if (!enabled_) {
        return true;
    }
    
    // Get page entry
    const PageEntry* entry = GetPageEntry(virtualAddr);
    if (!entry || !entry->present) {
        return false;  // Page not mapped or not present
    }
    
    // Check permission based on access type
    switch (accessType) {
        case MemoryAccessType::READ:
            return HasPermission(entry->permissions, PermissionFlags::READ);
        
        case MemoryAccessType::WRITE:
            return HasPermission(entry->permissions, PermissionFlags::WRITE);
        
        case MemoryAccessType::EXECUTE:
            return HasPermission(entry->permissions, PermissionFlags::EXECUTE);
        
        default:
            return false;
    }
}

void MMU::CheckPermissionOrThrow(uint64_t virtualAddr, MemoryAccessType accessType) const {
    // If MMU disabled, all accesses are allowed
    if (!enabled_) {
        return;
    }
    
    uint64_t pageAddr = AlignToPage(virtualAddr);
    auto it = pageTable_.find(pageAddr);
    
    // Check if page is mapped
    if (it == pageTable_.end()) {
        throw PageFault(PageFaultType::NOT_MAPPED, virtualAddr, 
                       accessType, pageSize_, nullptr);
    }
    
    const PageEntry& entry = it->second;
    
    // Check if page is present
    if (!entry.present) {
        throw PageFault(PageFaultType::NOT_PRESENT, virtualAddr, 
                       accessType, pageSize_, &entry);
    }
    
    // Check permission based on access type
    bool hasPermission = false;
    PageFaultType faultType;
    
    switch (accessType) {
        case MemoryAccessType::READ:
            hasPermission = HasPermission(entry.permissions, PermissionFlags::READ);
            faultType = PageFaultType::PERMISSION_READ;
            break;
        
        case MemoryAccessType::WRITE:
            hasPermission = HasPermission(entry.permissions, PermissionFlags::WRITE);
            faultType = PageFaultType::PERMISSION_WRITE;
            break;
        
        case MemoryAccessType::EXECUTE:
            hasPermission = HasPermission(entry.permissions, PermissionFlags::EXECUTE);
            faultType = PageFaultType::PERMISSION_EXEC;
            break;
        
        default:
            hasPermission = false;
            faultType = PageFaultType::PERMISSION_READ;
            break;
    }
    
    if (!hasPermission) {
        throw PageFault(faultType, virtualAddr, accessType, pageSize_, &entry);
    }
}

// ============================================================================
// Memory Access Hooks
// ============================================================================

size_t MMU::RegisterReadHook(MemoryHook hook) {
    size_t id = nextHookId_++;
    readHooks_.push_back({id, hook});
    return id;
}

size_t MMU::RegisterWriteHook(MemoryHook hook) {
    size_t id = nextHookId_++;
    writeHooks_.push_back({id, hook});
    return id;
}

void MMU::UnregisterReadHook(size_t hookId) {
    readHooks_.erase(
        std::remove_if(readHooks_.begin(), readHooks_.end(),
            [hookId](const Hook& h) { return h.id == hookId; }),
        readHooks_.end()
    );
}

void MMU::UnregisterWriteHook(size_t hookId) {
    writeHooks_.erase(
        std::remove_if(writeHooks_.begin(), writeHooks_.end(),
            [hookId](const Hook& h) { return h.id == hookId; }),
        writeHooks_.end()
    );
}

void MMU::ClearHooks() {
    readHooks_.clear();
    writeHooks_.clear();
}

bool MMU::InvokeReadHooks(uint64_t virtualAddr, uint64_t physicalAddr, size_t size, uint8_t* data) const {
    bool allowAccess = true;
    
    HookContext context(virtualAddr, physicalAddr, size, MemoryAccessType::READ);
    context.allowAccess = &allowAccess;
    
    for (const auto& hook : readHooks_) {
        hook.callback(context, data);
        if (!allowAccess) {
            return false;  // Hook denied access
        }
    }
    
    // Check watchpoints after regular hooks
    if (!CheckWatchpoints(virtualAddr, size, false, data)) {
        return false;  // Watchpoint blocked access
    }
    
    return true;
}

bool MMU::InvokeWriteHooks(uint64_t virtualAddr, uint64_t physicalAddr, size_t size, uint8_t* data) const {
    bool allowAccess = true;
    
    HookContext context(virtualAddr, physicalAddr, size, MemoryAccessType::WRITE);
    context.allowAccess = &allowAccess;
    
    for (const auto& hook : writeHooks_) {
        hook.callback(context, data);
        if (!allowAccess) {
            return false;  // Hook denied access
        }
    }
    
    // Check watchpoints after regular hooks
    if (!CheckWatchpoints(virtualAddr, size, true, data)) {
        return false;  // Watchpoint blocked access
    }
    
    return true;
}

// ============================================================================
// Extended API
// ============================================================================

void MMU::MapIdentityRange(uint64_t startAddr, size_t size, PermissionFlags permissions) {
    // Align start to page boundary
    uint64_t pageStart = AlignToPage(startAddr);
    
    // Calculate number of pages to map
    uint64_t endAddr = startAddr + size;
    uint64_t pageEnd = AlignToPage(endAddr);
    if (GetPageOffset(endAddr) != 0) {
        pageEnd += pageSize_;  // Round up if not page-aligned
    }
    
    // Map each page
    for (uint64_t addr = pageStart; addr < pageEnd; addr += pageSize_) {
        MapPage(addr, addr, permissions);  // Identity: virtual == physical
    }
}

void MMU::ClearPageTable() {
    pageTable_.clear();
}

void MMU::RestorePageTable(const std::map<uint64_t, PageEntry>& pageTable) {
    pageTable_ = pageTable;
}

std::string MMU::DumpPageTable(uint64_t startAddr, size_t count) const {
    std::ostringstream oss;
    
    oss << "========== PAGE TABLE DUMP ==========\n";
    oss << "Page Size: " << pageSize_ << " bytes (" << (pageSize_ / 1024) << " KB)\n";
    oss << "Total Mapped Pages: " << pageTable_.size() << "\n";
    oss << "MMU Enabled: " << (enabled_ ? "YES" : "NO") << "\n";
    oss << "\n";
    
    if (pageTable_.empty()) {
        oss << "No pages mapped.\n";
    } else {
        oss << std::setfill('-') << std::setw(90) << "-" << "\n";
        oss << std::setfill(' ');
        oss << std::left << std::setw(18) << "Virtual Addr"
            << std::setw(18) << "Physical Addr"
            << std::setw(12) << "Permissions"
            << std::setw(8) << "Present"
            << std::setw(8) << "Accessed"
            << std::setw(8) << "Dirty"
            << "\n";
        oss << std::setfill('-') << std::setw(90) << "-" << "\n";
        oss << std::setfill(' ');
        
        size_t displayed = 0;
        for (const auto& pair : pageTable_) {
            // Skip if before start address
            if (startAddr > 0 && pair.first < startAddr) {
                continue;
            }
            
            // Stop if we've displayed enough
            if (count > 0 && displayed >= count) {
                break;
            }
            
            const uint64_t virtAddr = pair.first;
            const PageEntry& entry = pair.second;
            
            oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << virtAddr << "  ";
            oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << entry.physicalAddress << "  ";
            oss << std::setfill(' ');
            
            // Permission flags
            std::string perms;
            if (HasPermission(entry.permissions, PermissionFlags::READ)) perms += "R";
            else perms += "-";
            if (HasPermission(entry.permissions, PermissionFlags::WRITE)) perms += "W";
            else perms += "-";
            if (HasPermission(entry.permissions, PermissionFlags::EXECUTE)) perms += "X";
            else perms += "-";
            
            oss << std::left << std::setw(12) << perms;
            oss << std::setw(8) << (entry.present ? "YES" : "NO");
            oss << std::setw(8) << (entry.accessed ? "YES" : "NO");
            oss << std::setw(8) << (entry.dirty ? "YES" : "NO");
            oss << "\n";
            
            displayed++;
        }
        
        oss << std::setfill('-') << std::setw(90) << "-" << "\n";
        
        if (count > 0 && pageTable_.size() > displayed) {
            oss << "(Showing " << displayed << " of " << pageTable_.size() << " total pages)\n";
        }
    }
    
    oss << "=====================================\n";
    
    return oss.str();
}

std::string MMU::DiagnoseAddress(uint64_t virtualAddr) const {
    std::ostringstream oss;
    
    uint64_t pageAddr = AlignToPage(virtualAddr);
    uint64_t offset = GetPageOffset(virtualAddr);
    
    oss << "========== ADDRESS DIAGNOSIS ==========\n";
    oss << "Virtual Address: 0x" << std::hex << std::setw(16) << std::setfill('0') 
        << virtualAddr << std::dec << "\n";
    oss << "Page Address:    0x" << std::hex << std::setw(16) << std::setfill('0') 
        << pageAddr << std::dec << "\n";
    oss << "Page Offset:     0x" << std::hex << std::setw(4) << std::setfill('0') 
        << offset << std::dec << " (" << offset << " bytes)\n";
    oss << "Page Size:       " << pageSize_ << " bytes\n";
    oss << "MMU Enabled:     " << (enabled_ ? "YES" : "NO") << "\n";
    oss << "\n";
    
    auto it = pageTable_.find(pageAddr);
    
    if (it == pageTable_.end()) {
        oss << "Status:          NOT MAPPED\n";
        oss << "\n";
        oss << "This virtual address is not mapped in the page table.\n";
        oss << "Any access will result in a PAGE FAULT (NOT_MAPPED).\n";
        oss << "\n";
        oss << "To fix:\n";
        oss << "  - Call MapPage(0x" << std::hex << pageAddr 
            << ", physicalAddr, permissions)\n";
        oss << "  - Or use MapIdentityRange() for a range of addresses\n";
    } else {
        const PageEntry& entry = it->second;
        
        oss << "Status:          MAPPED\n";
        oss << "\n";
        oss << "Page Entry:\n";
        oss << "  Physical Addr: 0x" << std::hex << std::setw(16) << std::setfill('0') 
            << entry.physicalAddress << std::dec << "\n";
        oss << "  Translated:    0x" << std::hex << std::setw(16) << std::setfill('0') 
            << (entry.physicalAddress + offset) << std::dec << "\n";
        
        std::string perms;
        if (HasPermission(entry.permissions, PermissionFlags::READ)) perms += "R";
        else perms += "-";
        if (HasPermission(entry.permissions, PermissionFlags::WRITE)) perms += "W";
        else perms += "-";
        if (HasPermission(entry.permissions, PermissionFlags::EXECUTE)) perms += "X";
        else perms += "-";
        
        oss << "  Permissions:   " << perms << "\n";
        oss << "  Present:       " << (entry.present ? "YES" : "NO") << "\n";
        oss << "  Accessed:      " << (entry.accessed ? "YES" : "NO") << "\n";
        oss << "  Dirty:         " << (entry.dirty ? "YES" : "NO") << "\n";
        oss << "\n";
        
        if (!entry.present) {
            oss << "WARNING: Page not present - will cause PAGE FAULT (NOT_PRESENT)\n";
        }
        
        oss << "Access Rights:\n";
        oss << "  READ:    " << (HasPermission(entry.permissions, PermissionFlags::READ) ? "ALLOWED" : "DENIED") << "\n";
        oss << "  WRITE:   " << (HasPermission(entry.permissions, PermissionFlags::WRITE) ? "ALLOWED" : "DENIED") << "\n";
        oss << "  EXECUTE: " << (HasPermission(entry.permissions, PermissionFlags::EXECUTE) ? "ALLOWED" : "DENIED") << "\n";
    }
    
    oss << "=======================================\n";
    
    return oss.str();
}

void MMU::LogPageFault(const PageFault& fault) {
    Logger& logger = Logger::getInstance();
    
    // Log the basic error message
    logger.logError(fault.what());
    
    // If in DEBUG or higher level, log full diagnostics
    if (logger.getLogLevel() >= LogLevel::DEBUG) {
        std::string diagnostics = fault.getDiagnostics();
        logger.logError(diagnostics);
    }
}

// ============================================================================
// Watchpoint Management
// ============================================================================

size_t MMU::RegisterWatchpoint(const Watchpoint& watchpoint) {
    Watchpoint wp = watchpoint;
    wp.id = nextWatchpointId_++;
    watchpoints_.push_back(wp);
    
    Logger::getInstance().logDebug("Registered watchpoint " + std::to_string(wp.id) + 
                                   " at 0x" + std::to_string(wp.addressStart) + 
                                   " [" + WatchpointTypeToString(wp.type) + "]");
    
    return wp.id;
}

bool MMU::UnregisterWatchpoint(size_t watchpointId) {
    auto it = std::find_if(watchpoints_.begin(), watchpoints_.end(),
        [watchpointId](const Watchpoint& wp) { return wp.id == watchpointId; });
    
    if (it != watchpoints_.end()) {
        watchpoints_.erase(it);
        Logger::getInstance().logDebug("Unregistered watchpoint " + std::to_string(watchpointId));
        return true;
    }
    
    return false;
}

bool MMU::SetWatchpointEnabled(size_t watchpointId, bool enabled) {
    auto it = std::find_if(watchpoints_.begin(), watchpoints_.end(),
        [watchpointId](const Watchpoint& wp) { return wp.id == watchpointId; });
    
    if (it != watchpoints_.end()) {
        it->enabled = enabled;
        return true;
    }
    
    return false;
}

const Watchpoint* MMU::GetWatchpoint(size_t watchpointId) const {
    auto it = std::find_if(watchpoints_.begin(), watchpoints_.end(),
        [watchpointId](const Watchpoint& wp) { return wp.id == watchpointId; });
    
    if (it != watchpoints_.end()) {
        return &(*it);
    }
    
    return nullptr;
}

std::vector<Watchpoint> MMU::GetAllWatchpoints() const {
    return watchpoints_;
}

void MMU::ClearWatchpoints() {
    watchpoints_.clear();
    Logger::getInstance().logDebug("Cleared all watchpoints");
}

size_t MMU::GetWatchpointCount() const {
    return watchpoints_.size();
}

void MMU::SetCPUStateReference(const CPUState* cpuState) {
    cpuStateRef_ = cpuState;
}

void MMU::AddInstructionTrace(const InstructionTrace& trace) {
    // Add to buffer
    instructionTraceBuffer_.push_back(trace);
    
    // Trim buffer if it exceeds depth
    if (instructionTraceBuffer_.size() > instructionTraceDepth_) {
        instructionTraceBuffer_.erase(instructionTraceBuffer_.begin());
    }
}

void MMU::ClearInstructionTrace() {
    instructionTraceBuffer_.clear();
}

void MMU::SetInstructionTraceDepth(size_t depth) {
    instructionTraceDepth_ = depth;
    
    // Trim existing buffer if needed
    while (instructionTraceBuffer_.size() > instructionTraceDepth_) {
        instructionTraceBuffer_.erase(instructionTraceBuffer_.begin());
    }
}

bool MMU::CheckWatchpoints(uint64_t address, size_t size, bool isWrite, uint8_t* data) const {
    for (auto& wp : watchpoints_) {
        // Note: We need to cast away const to update trigger counts
        // This is a design tradeoff for const correctness
        Watchpoint& watchpoint = const_cast<Watchpoint&>(wp);
        
        // Check if watchpoint should trigger
        if (!watchpoint.ShouldTrigger(address, size, isWrite)) {
            continue;
        }
        
        // Extract value for condition checking
        uint64_t value = ExtractValue(data, size);
        
        // Check condition
        if (!watchpoint.CheckCondition(value)) {
            continue;
        }
        
        // Watchpoint triggered - prepare context
        WatchpointContext context;
        context.address = address;
        context.watchpointStart = watchpoint.addressStart;
        context.watchpointEnd = watchpoint.addressEnd;
        context.accessSize = size;
        context.triggerType = watchpoint.type;
        context.data = data;
        context.oldValue = watchpoint.lastValue;
        context.newValue = value;
        context.isWrite = isWrite;
        context.cpuState = cpuStateRef_;
        
        bool continueExecution = true;
        bool skipAccess = false;
        context.continueExecution = &continueExecution;
        context.skipAccess = &skipAccess;
        
        // Capture instruction trace if enabled
        if (watchpoint.captureInstructions && watchpoint.instructionTraceDepth > 0) {
            size_t traceCount = std::min(watchpoint.instructionTraceDepth, 
                                        instructionTraceBuffer_.size());
            
            // Copy the most recent instructions
            if (traceCount > 0) {
                size_t startIdx = instructionTraceBuffer_.size() - traceCount;
                context.instructionTrace.assign(
                    instructionTraceBuffer_.begin() + startIdx,
                    instructionTraceBuffer_.end()
                );
            }
        }
        
        // Increment trigger count
        watchpoint.triggerCount++;
        
        // Log the trigger
        std::ostringstream oss;
        oss << "Watchpoint " << watchpoint.id << " triggered at 0x" 
            << std::hex << address << std::dec
            << " (" << (isWrite ? "WRITE" : "READ") << ")";
        if (!watchpoint.description.empty()) {
            oss << " - " << watchpoint.description;
        }
        Logger::getInstance().logDebug(oss.str());
        
        // Invoke callback if set
        if (watchpoint.callback) {
            watchpoint.callback(context);
        }
        
        // Check if watchpoint should auto-disable
        if (watchpoint.maxTriggers > 0 && watchpoint.triggerCount >= watchpoint.maxTriggers) {
            watchpoint.enabled = false;
            Logger::getInstance().logDebug("Watchpoint " + std::to_string(watchpoint.id) + 
                                          " auto-disabled after " + 
                                          std::to_string(watchpoint.triggerCount) + " triggers");
        }
        
        // Check if execution should be halted or access skipped
        if (!continueExecution || skipAccess) {
            return false;
        }
    }
    
    return true;
}

uint64_t MMU::ExtractValue(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }
    
    uint64_t value = 0;
    size_t bytesToCopy = std::min(size, sizeof(uint64_t));
    
    std::memcpy(&value, data, bytesToCopy);
    
    return value;
}

} // namespace ia64

