#pragma once

#include "VMConfiguration.h"
#include <string>
#include <cstdint>
#include <chrono>
#include <sstream>

namespace ia64 {

/**
 * VMState - Virtual machine lifecycle states
 */
enum class VMState {
    // Creation states
    CREATED,            // VM created but not initialized
    CONFIGURING,        // VM being configured
    
    // Runtime states
    INITIALIZING,       // VM initializing (loading config, devices)
    STOPPED,            // VM stopped (can be started)
    STARTING,           // VM starting up
    RUNNING,            // VM running normally
    PAUSING,            // VM being paused
    PAUSED,             // VM paused (can be resumed)
    RESUMING,           // VM resuming from pause
    
    // Termination states
    STOPPING,           // VM shutting down
    TERMINATING,        // VM being forcefully terminated
    TERMINATED,         // VM terminated
    
    // Error states
    ERROR,              // VM encountered an error
    CRASHED,            // VM crashed
    
    // Snapshot states
    SNAPSHOTTING,       // VM being snapshotted
    RESTORING           // VM restoring from snapshot
};

/**
 * Convert VMState to string
 */
inline std::string vmStateToString(VMState state) {
    switch (state) {
        case VMState::CREATED: return "CREATED";
        case VMState::CONFIGURING: return "CONFIGURING";
        case VMState::INITIALIZING: return "INITIALIZING";
        case VMState::STOPPED: return "STOPPED";
        case VMState::STARTING: return "STARTING";
        case VMState::RUNNING: return "RUNNING";
        case VMState::PAUSING: return "PAUSING";
        case VMState::PAUSED: return "PAUSED";
        case VMState::RESUMING: return "RESUMING";
        case VMState::STOPPING: return "STOPPING";
        case VMState::TERMINATING: return "TERMINATING";
        case VMState::TERMINATED: return "TERMINATED";
        case VMState::ERROR: return "ERROR";
        case VMState::CRASHED: return "CRASHED";
        case VMState::SNAPSHOTTING: return "SNAPSHOTTING";
        case VMState::RESTORING: return "RESTORING";
        default: return "UNKNOWN";
    }
}

/**
 * VMResourceUsage - Runtime resource usage statistics
 */
struct VMResourceUsage {
    uint64_t cpuTimeMs;             // Total CPU time in milliseconds
    uint64_t cyclesExecuted;        // Total CPU cycles executed
    uint64_t instructionsExecuted;  // Total instructions executed
    uint64_t memoryUsedBytes;       // Current memory usage
    uint64_t diskReadBytes;         // Total disk read bytes
    uint64_t diskWriteBytes;        // Total disk write bytes
    uint64_t networkRxBytes;        // Network received bytes
    uint64_t networkTxBytes;        // Network transmitted bytes
    
    VMResourceUsage()
        : cpuTimeMs(0)
        , cyclesExecuted(0)
        , instructionsExecuted(0)
        , memoryUsedBytes(0)
        , diskReadBytes(0)
        , diskWriteBytes(0)
        , networkRxBytes(0)
        , networkTxBytes(0) {}
    
    void reset() {
        cpuTimeMs = 0;
        cyclesExecuted = 0;
        instructionsExecuted = 0;
        memoryUsedBytes = 0;
        diskReadBytes = 0;
        diskWriteBytes = 0;
        networkRxBytes = 0;
        networkTxBytes = 0;
    }
    
    std::string toString() const {
        std::ostringstream oss;
        oss << "Resource Usage:\n";
        oss << "  CPU Time: " << cpuTimeMs << " ms\n";
        oss << "  Cycles: " << cyclesExecuted << "\n";
        oss << "  Instructions: " << instructionsExecuted << "\n";
        oss << "  Memory: " << (memoryUsedBytes / 1024) << " KB\n";
        oss << "  Disk Read: " << (diskReadBytes / 1024) << " KB\n";
        oss << "  Disk Write: " << (diskWriteBytes / 1024) << " KB\n";
        oss << "  Network RX: " << (networkRxBytes / 1024) << " KB\n";
        oss << "  Network TX: " << (networkTxBytes / 1024) << " KB\n";
        return oss.str();
    }
};

/**
 * VMMetadata - Virtual machine metadata and state information
 * 
 * This structure contains runtime state, statistics, and metadata for a VM.
 * It tracks lifecycle state, resource usage, timestamps, and configuration.
 */
struct VMMetadata {
    // Identification
    std::string vmId;               // Unique VM identifier
    std::string name;               // VM name
    std::string uuid;               // UUID
    
    // State tracking
    VMState currentState;           // Current lifecycle state
    VMState previousState;          // Previous state (for transitions)
    std::string stateMessage;       // Human-readable state message
    
    // Timestamps (using system_clock for serialization)
    std::chrono::system_clock::time_point createdTime;      // VM creation time
    std::chrono::system_clock::time_point startedTime;      // Last start time
    std::chrono::system_clock::time_point stoppedTime;      // Last stop time
    std::chrono::system_clock::time_point modifiedTime;     // Last modification time
    
    // Resource usage
    VMResourceUsage resourceUsage;  // Current resource usage
    
    // Configuration reference
    VMConfiguration configuration;  // VM configuration
    
    // Snapshot information
    size_t snapshotCount;           // Number of snapshots
    std::string activeSnapshot;     // Currently active snapshot (if any)
    
    // Error tracking
    bool hasError;                  // Error flag
    std::string lastError;          // Last error message
    
    VMMetadata()
        : vmId()
        , name()
        , uuid()
        , currentState(VMState::CREATED)
        , previousState(VMState::CREATED)
        , stateMessage("Created")
        , createdTime(std::chrono::system_clock::now())
        , startedTime()
        , stoppedTime()
        , modifiedTime(std::chrono::system_clock::now())
        , resourceUsage()
        , configuration()
        , snapshotCount(0)
        , activeSnapshot()
        , hasError(false)
        , lastError() {}
    
    /**
     * Update state with transition tracking
     */
    void setState(VMState newState, const std::string& message = "") {
        previousState = currentState;
        currentState = newState;
        stateMessage = message.empty() ? vmStateToString(newState) : message;
        modifiedTime = std::chrono::system_clock::now();
        
        // Update timestamps based on state
        if (newState == VMState::RUNNING && previousState != VMState::RESUMING) {
            startedTime = std::chrono::system_clock::now();
        }
        if (newState == VMState::STOPPED || newState == VMState::TERMINATED) {
            stoppedTime = std::chrono::system_clock::now();
        }
    }
    
    /**
     * Set error state
     */
    void setError(const std::string& errorMessage) {
        hasError = true;
        lastError = errorMessage;
        setState(VMState::ERROR, errorMessage);
    }
    
    /**
     * Clear error state
     */
    void clearError() {
        hasError = false;
        lastError.clear();
    }
    
    /**
     * Check if VM can be started
     */
    bool canStart() const {
        return currentState == VMState::STOPPED || 
               currentState == VMState::CREATED ||
               currentState == VMState::TERMINATED;
    }
    
    /**
     * Check if VM can be stopped
     */
    bool canStop() const {
        return currentState == VMState::RUNNING || 
               currentState == VMState::PAUSED;
    }
    
    /**
     * Check if VM can be paused
     */
    bool canPause() const {
        return currentState == VMState::RUNNING;
    }
    
    /**
     * Check if VM can be resumed
     */
    bool canResume() const {
        return currentState == VMState::PAUSED;
    }
    
    /**
     * Check if VM can be snapshotted
     */
    bool canSnapshot() const {
        return currentState == VMState::RUNNING || 
               currentState == VMState::PAUSED ||
               currentState == VMState::STOPPED;
    }
    
    /**
     * Get uptime in seconds
     */
    uint64_t getUptimeSeconds() const {
        if (currentState != VMState::RUNNING && currentState != VMState::PAUSED) {
            return 0;
        }
        
        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - startedTime);
        return duration.count();
    }
    
    /**
     * Get summary string
     */
    std::string getSummary() const {
        std::ostringstream oss;
        oss << "VM: " << name << " (" << vmId << ")\n";
        oss << "  State: " << vmStateToString(currentState);
        if (!stateMessage.empty() && stateMessage != vmStateToString(currentState)) {
            oss << " - " << stateMessage;
        }
        oss << "\n";
        
        if (currentState == VMState::RUNNING || currentState == VMState::PAUSED) {
            oss << "  Uptime: " << getUptimeSeconds() << " seconds\n";
        }
        
        oss << "  CPUs: " << configuration.cpu.cpuCount 
            << " x " << configuration.cpu.isaType << "\n";
        oss << "  Memory: " << (configuration.memory.memorySize / (1024 * 1024)) 
            << " MB\n";
        oss << "  Storage: " << configuration.storageDevices.size() 
            << " device(s)\n";
        
        if (hasError) {
            oss << "  Error: " << lastError << "\n";
        }
        
        return oss.str();
    }
};

/**
 * VMSnapshot - Virtual machine snapshot metadata
 */
struct VMSnapshot {
    std::string snapshotId;         // Unique snapshot identifier
    std::string name;               // Snapshot name
    std::string description;        // Description
    std::chrono::system_clock::time_point createdTime;  // Creation time
    VMState vmState;                // VM state at snapshot time
    uint64_t memorySize;            // Memory snapshot size
    std::string dataPath;           // Path to snapshot data
    VMMetadata vmMetadata;          // VM metadata at snapshot time
    
    VMSnapshot()
        : snapshotId()
        , name()
        , description()
        , createdTime(std::chrono::system_clock::now())
        , vmState(VMState::STOPPED)
        , memorySize(0)
        , dataPath()
        , vmMetadata() {}
    
    std::string getSummary() const {
        std::ostringstream oss;
        oss << "Snapshot: " << name << " (" << snapshotId << ")\n";
        oss << "  Created: " << std::chrono::system_clock::to_time_t(createdTime) << "\n";
        oss << "  VM State: " << vmStateToString(vmState) << "\n";
        oss << "  Memory: " << (memorySize / (1024 * 1024)) << " MB\n";
        if (!description.empty()) {
            oss << "  Description: " << description << "\n";
        }
        return oss.str();
    }
};

} // namespace ia64
