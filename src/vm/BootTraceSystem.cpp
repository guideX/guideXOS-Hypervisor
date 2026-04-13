#include "BootTraceSystem.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace ia64 {

BootTraceSystem::BootTraceSystem(size_t maxEvents)
    : enabled_(true)
    , verbosity_(2)
    , maxEvents_(maxEvents)
    , events_()
    , stats_() {
}

void BootTraceSystem::addEvent(const BootTraceEntry& entry) {
    if (!enabled_) {
        return;
    }
    
    // Check verbosity filtering
    int importance = getEventImportance(entry.type);
    if (importance > verbosity_) {
        return;
    }
    
    // Add to circular buffer
    events_.push_back(entry);
    if (events_.size() > maxEvents_) {
        events_.pop_front();
    }
    
    // Update statistics
    stats_.totalEvents++;
    
    switch (entry.type) {
        case BootTraceEventType::POWER_ON:
            stats_.powerOnTimestamp = entry.timestamp;
            break;
        case BootTraceEventType::KERNEL_ENTRY_POINT:
            stats_.kernelEntryTimestamp = entry.timestamp;
            break;
        case BootTraceEventType::USERSPACE_READY:
            stats_.userSpaceTimestamp = entry.timestamp;
            stats_.bootCompleted = true;
            stats_.totalBootCycles = entry.timestamp - stats_.powerOnTimestamp;
            break;
        case BootTraceEventType::SYSCALL_EXECUTED:
        case BootTraceEventType::SYSCALL_FIRST:
            stats_.syscallCount++;
            break;
        case BootTraceEventType::INTERRUPT_RAISED:
            stats_.interruptCount++;
            break;
        case BootTraceEventType::EXCEPTION_OCCURRED:
            stats_.exceptionCount++;
            break;
        case BootTraceEventType::KERNEL_PANIC:
            stats_.panicOccurred = true;
            break;
        default:
            break;
    }
}

int BootTraceSystem::getEventImportance(BootTraceEventType type) const {
    // Return importance level: 0 = critical, 1 = major, 2 = detailed
    switch (type) {
        case BootTraceEventType::POWER_ON:
        case BootTraceEventType::KERNEL_ENTRY_POINT:
        case BootTraceEventType::USERSPACE_READY:
        case BootTraceEventType::KERNEL_PANIC:
        case BootTraceEventType::EMERGENCY_HALT:
        case BootTraceEventType::BOOT_FAILURE:
            return 0;  // Critical
            
        case BootTraceEventType::BOOT_STATE_CHANGE:
        case BootTraceEventType::KERNEL_LOADED:
        case BootTraceEventType::SYSCALL_FIRST:
        case BootTraceEventType::MEMORY_ALLOCATED:
        case BootTraceEventType::PAGE_TABLE_INIT:
            return 1;  // Major milestone
            
        default:
            return 2;  // Detailed
    }
}

void BootTraceSystem::recordEvent(BootTraceEventType type, uint64_t timestamp, uint64_t ip,
                                  VMBootState bootState, const std::string& description,
                                  uint64_t data1, uint64_t data2, uint64_t data3) {
    BootTraceEntry entry(type, timestamp, ip, bootState, description, data1, data2, data3);
    addEvent(entry);
}

void BootTraceSystem::recordBootStateChange(VMBootState fromState, VMBootState toState,
                                           uint64_t timestamp, uint64_t ip, const std::string& reason) {
    std::ostringstream desc;
    desc << "Boot state: " << bootStateToString(fromState) << " -> " << bootStateToString(toState);
    if (!reason.empty()) {
        desc << " (" << reason << ")";
    }
    
    BootTraceEntry entry(BootTraceEventType::BOOT_STATE_CHANGE, timestamp, ip, toState, desc.str(),
                        static_cast<uint64_t>(fromState), static_cast<uint64_t>(toState), 0);
    addEvent(entry);
}

void BootTraceSystem::recordMemoryAllocation(uint64_t size, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Memory allocated: " << size << " bytes (" << (size / 1024 / 1024) << " MB)";
    
    BootTraceEntry entry(BootTraceEventType::MEMORY_ALLOCATED, timestamp, 0, 
                        VMBootState::POWER_ON, desc.str(), size, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordMemoryRegionMap(uint64_t startAddr, uint64_t endAddr, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Memory region mapped: 0x" << std::hex << std::setw(16) << std::setfill('0') << startAddr
         << " - 0x" << std::hex << std::setw(16) << std::setfill('0') << endAddr
         << " (" << std::dec << (endAddr - startAddr) << " bytes)";
    
    BootTraceEntry entry(BootTraceEventType::MEMORY_REGION_MAPPED, timestamp, 0,
                        VMBootState::FIRMWARE_INIT, desc.str(), startAddr, endAddr, 0);
    addEvent(entry);
}

void BootTraceSystem::recordPageTableInit(uint64_t pageTableBase, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Page table initialized at 0x" << std::hex << std::setw(16) << std::setfill('0') << pageTableBase;
    
    BootTraceEntry entry(BootTraceEventType::PAGE_TABLE_INIT, timestamp, 0,
                        VMBootState::KERNEL_INIT, desc.str(), pageTableBase, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordKernelLoad(uint64_t loadAddress, uint64_t size, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Kernel loaded at 0x" << std::hex << std::setw(16) << std::setfill('0') << loadAddress
         << " (size: " << std::dec << size << " bytes)";
    
    BootTraceEntry entry(BootTraceEventType::KERNEL_LOADED, timestamp, loadAddress,
                        VMBootState::KERNEL_LOAD, desc.str(), loadAddress, size, 0);
    addEvent(entry);
}

void BootTraceSystem::recordKernelEntry(uint64_t entryPoint, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Kernel entry point reached at 0x" << std::hex << std::setw(16) << std::setfill('0') << entryPoint;
    
    BootTraceEntry entry(BootTraceEventType::KERNEL_ENTRY_POINT, timestamp, entryPoint,
                        VMBootState::KERNEL_ENTRY, desc.str(), entryPoint, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordSyscall(uint64_t syscallNumber, const std::string& syscallName,
                                   uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "Syscall: " << syscallName << " (#" << syscallNumber << ")";
    
    BootTraceEntry entry(BootTraceEventType::SYSCALL_EXECUTED, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), syscallNumber, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordFirstSyscall(uint64_t syscallNumber, const std::string& syscallName,
                                        uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "First syscall: " << syscallName << " (#" << syscallNumber << ")";
    
    BootTraceEntry entry(BootTraceEventType::SYSCALL_FIRST, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), syscallNumber, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordBundleExecution(uint64_t ip, const Bundle& bundle, uint64_t timestamp) {
    std::ostringstream desc;
    desc << "Bundle executed at 0x" << std::hex << std::setw(16) << std::setfill('0') << ip
         << " (template: 0x" << std::hex << static_cast<int>(bundle.templateType) << ")";
    
    BootTraceEntry entry(BootTraceEventType::CPU_BUNDLE_EXECUTED, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), 
                        static_cast<uint64_t>(bundle.templateType), bundle.instructions.size(), 0);
    addEvent(entry);
}

void BootTraceSystem::recordInstructionMilestone(uint64_t count, uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "Instruction milestone: " << count << " instructions executed";
    
    BootTraceEntry entry(BootTraceEventType::CPU_INSTRUCTION_EXECUTED, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), count, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordInterrupt(uint8_t vector, uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "Interrupt raised: vector 0x" << std::hex << std::setw(2) << std::setfill('0') 
         << static_cast<int>(vector);
    
    BootTraceEntry entry(BootTraceEventType::INTERRUPT_RAISED, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), vector, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordException(const std::string& exceptionType, uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "Exception: " << exceptionType;
    
    BootTraceEntry entry(BootTraceEventType::EXCEPTION_OCCURRED, timestamp, ip,
                        VMBootState::USERSPACE_RUNNING, desc.str(), 0, 0, 0);
    addEvent(entry);
}

void BootTraceSystem::recordKernelPanic(const std::string& reason, uint64_t timestamp, uint64_t ip) {
    std::ostringstream desc;
    desc << "KERNEL PANIC: " << reason;
    
    BootTraceEntry entry(BootTraceEventType::KERNEL_PANIC, timestamp, ip,
                        VMBootState::KERNEL_PANIC, desc.str(), 0, 0, 0);
    addEvent(entry);
}

std::vector<BootTraceEntry> BootTraceSystem::getAllEvents() const {
    return std::vector<BootTraceEntry>(events_.begin(), events_.end());
}

std::vector<BootTraceEntry> BootTraceSystem::getEventsByType(BootTraceEventType type) const {
    std::vector<BootTraceEntry> result;
    for (const auto& entry : events_) {
        if (entry.type == type) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<BootTraceEntry> BootTraceSystem::getEventsByBootState(VMBootState state) const {
    std::vector<BootTraceEntry> result;
    for (const auto& entry : events_) {
        if (entry.bootState == state) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<BootTraceEntry> BootTraceSystem::getEventsInRange(uint64_t startTime, uint64_t endTime) const {
    std::vector<BootTraceEntry> result;
    for (const auto& entry : events_) {
        if (entry.timestamp >= startTime && entry.timestamp <= endTime) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<BootTraceEntry> BootTraceSystem::getLastEvents(size_t count) const {
    std::vector<BootTraceEntry> result;
    size_t start = events_.size() > count ? events_.size() - count : 0;
    for (size_t i = start; i < events_.size(); i++) {
        result.push_back(events_[i]);
    }
    return result;
}

BootTraceStatistics BootTraceSystem::getStatistics() const {
    return stats_;
}

std::string BootTraceSystem::generateTraceReport() const {
    std::ostringstream report;
    report << "=== BOOT TRACE REPORT ===\n\n";
    
    report << "Statistics:\n";
    report << "  Total Events: " << stats_.totalEvents << "\n";
    report << "  Boot Completed: " << (stats_.bootCompleted ? "YES" : "NO") << "\n";
    report << "  Panic Occurred: " << (stats_.panicOccurred ? "YES" : "NO") << "\n";
    
    if (stats_.bootCompleted) {
        report << "  Total Boot Cycles: " << stats_.totalBootCycles << "\n";
        if (stats_.kernelEntryTimestamp > 0) {
            report << "  Firmware Boot Cycles: " << (stats_.kernelEntryTimestamp - stats_.powerOnTimestamp) << "\n";
            report << "  Kernel Init Cycles: " << (stats_.userSpaceTimestamp - stats_.kernelEntryTimestamp) << "\n";
        }
    }
    
    report << "  Syscalls Executed: " << stats_.syscallCount << "\n";
    report << "  Interrupts Raised: " << stats_.interruptCount << "\n";
    report << "  Exceptions: " << stats_.exceptionCount << "\n\n";
    
    report << "Event Timeline:\n";
    report << std::left;
    report << std::setw(12) << "Timestamp" << " | "
           << std::setw(18) << "IP" << " | "
           << std::setw(25) << "Boot State" << " | "
           << std::setw(30) << "Event Type" << " | "
           << "Description\n";
    report << std::string(120, '-') << "\n";
    
    for (const auto& entry : events_) {
        report << std::dec << std::setw(12) << entry.timestamp << " | ";
        report << "0x" << std::hex << std::setw(16) << std::setfill('0') << entry.instructionPointer << " | ";
        report << std::setw(25) << bootStateToString(entry.bootState) << " | ";
        report << std::setw(30) << bootTraceEventTypeToString(entry.type) << " | ";
        report << entry.description << "\n";
    }
    
    return report.str();
}

std::string BootTraceSystem::generateBootTimeline() const {
    std::ostringstream timeline;
    timeline << "=== BOOT TIMELINE ===\n\n";
    
    // Find critical milestones
    std::vector<const BootTraceEntry*> milestones;
    for (const auto& entry : events_) {
        if (getEventImportance(entry.type) <= 1) {
            milestones.push_back(&entry);
        }
    }
    
    if (milestones.empty()) {
        timeline << "No milestone events recorded.\n";
        return timeline.str();
    }
    
    uint64_t startTime = milestones.front()->timestamp;
    
    for (const auto* milestone : milestones) {
        uint64_t elapsed = milestone->timestamp - startTime;
        timeline << "[+" << std::dec << std::setw(10) << elapsed << " cycles] ";
        timeline << std::setw(25) << std::left << bootStateToString(milestone->bootState) << " | ";
        timeline << milestone->description << "\n";
    }
    
    return timeline.str();
}

std::string BootTraceSystem::generateSyscallSummary() const {
    std::ostringstream summary;
    summary << "=== SYSCALL SUMMARY ===\n\n";
    
    auto syscallEvents = getEventsByType(BootTraceEventType::SYSCALL_EXECUTED);
    auto firstSyscall = getEventsByType(BootTraceEventType::SYSCALL_FIRST);
    
    syscallEvents.insert(syscallEvents.end(), firstSyscall.begin(), firstSyscall.end());
    
    if (syscallEvents.empty()) {
        summary << "No syscalls recorded.\n";
        return summary.str();
    }
    
    summary << "Total Syscalls: " << syscallEvents.size() << "\n\n";
    summary << std::left;
    summary << std::setw(12) << "Timestamp" << " | "
            << std::setw(18) << "IP" << " | "
            << std::setw(10) << "Number" << " | "
            << "Description\n";
    summary << std::string(80, '-') << "\n";
    
    for (const auto& entry : syscallEvents) {
        summary << std::dec << std::setw(12) << entry.timestamp << " | ";
        summary << "0x" << std::hex << std::setw(16) << std::setfill('0') << entry.instructionPointer << " | ";
        summary << std::dec << std::setw(10) << entry.data1 << " | ";
        summary << entry.description << "\n";
    }
    
    return summary.str();
}

void BootTraceSystem::clear() {
    events_.clear();
    stats_ = BootTraceStatistics();
}

} // namespace ia64
