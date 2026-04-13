#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <map>

namespace ia64 {
namespace api {

// ============================================================================
// API Version
// ============================================================================

constexpr const char* API_VERSION = "1.0.0";

// ============================================================================
// Request/Response Status
// ============================================================================

enum class APIStatus {
    SUCCESS,
    ERROR,
    NOT_FOUND,
    INVALID_REQUEST,
    UNAUTHORIZED,
    CONFLICT,
    TIMEOUT
};

inline std::string apiStatusToString(APIStatus status) {
    switch (status) {
        case APIStatus::SUCCESS: return "success";
        case APIStatus::ERROR: return "error";
        case APIStatus::NOT_FOUND: return "not_found";
        case APIStatus::INVALID_REQUEST: return "invalid_request";
        case APIStatus::UNAUTHORIZED: return "unauthorized";
        case APIStatus::CONFLICT: return "conflict";
        case APIStatus::TIMEOUT: return "timeout";
        default: return "unknown";
    }
}

// ============================================================================
// VM Operations
// ============================================================================

enum class VMOperation {
    CREATE,
    DELETE,
    START,
    STOP,
    PAUSE,
    RESUME,
    RESET,
    SNAPSHOT,
    RESTORE,
    LIST,
    GET_INFO,
    GET_LOGS,
    EXECUTE
};

inline std::string vmOperationToString(VMOperation op) {
    switch (op) {
        case VMOperation::CREATE: return "create";
        case VMOperation::DELETE: return "delete";
        case VMOperation::START: return "start";
        case VMOperation::STOP: return "stop";
        case VMOperation::PAUSE: return "pause";
        case VMOperation::RESUME: return "resume";
        case VMOperation::RESET: return "reset";
        case VMOperation::SNAPSHOT: return "snapshot";
        case VMOperation::RESTORE: return "restore";
        case VMOperation::LIST: return "list";
        case VMOperation::GET_INFO: return "get_info";
        case VMOperation::GET_LOGS: return "get_logs";
        case VMOperation::EXECUTE: return "execute";
        default: return "unknown";
    }
}

// ============================================================================
// API Request Types
// ============================================================================

struct APIRequest {
    std::string requestId;
    VMOperation operation;
    std::map<std::string, std::string> parameters;
    std::string body;
    
    APIRequest()
        : requestId()
        , operation(VMOperation::LIST)
        , parameters()
        , body() {}
};

struct APIResponse {
    std::string requestId;
    APIStatus status;
    std::string message;
    std::string body;
    int64_t timestamp;
    
    APIResponse()
        : requestId()
        , status(APIStatus::SUCCESS)
        , message()
        , body()
        , timestamp(0) {}
};

// ============================================================================
// VM Creation Request
// ============================================================================

struct VMCreateRequest {
    std::string name;
    std::string configJson;
    bool autoStart;
    
    VMCreateRequest()
        : name()
        , configJson()
        , autoStart(false) {}
};

struct VMCreateResponse {
    std::string vmId;
    std::string name;
    std::string state;
    
    VMCreateResponse()
        : vmId()
        , name()
        , state() {}
};

// ============================================================================
// VM Operation Request
// ============================================================================

struct VMOperationRequest {
    std::string vmId;
    std::map<std::string, std::string> parameters;
    
    VMOperationRequest()
        : vmId()
        , parameters() {}
};

struct VMOperationResponse {
    std::string vmId;
    std::string state;
    bool success;
    std::string message;
    
    VMOperationResponse()
        : vmId()
        , state()
        , success(false)
        , message() {}
};

// ============================================================================
// VM Snapshot Request
// ============================================================================

struct VMSnapshotRequest {
    std::string vmId;
    std::string snapshotName;
    std::string description;
    
    VMSnapshotRequest()
        : vmId()
        , snapshotName()
        , description() {}
};

struct VMSnapshotResponse {
    std::string vmId;
    std::string snapshotId;
    std::string snapshotName;
    int64_t timestamp;
    
    VMSnapshotResponse()
        : vmId()
        , snapshotId()
        , snapshotName()
        , timestamp(0) {}
};

// ============================================================================
// VM List Response
// ============================================================================

struct VMInfo {
    std::string vmId;
    std::string name;
    std::string state;
    uint64_t memorySizeMB;
    uint32_t cpuCount;
    int64_t createdTime;
    int64_t startedTime;
    uint64_t cyclesExecuted;
    
    VMInfo()
        : vmId()
        , name()
        , state()
        , memorySizeMB(0)
        , cpuCount(0)
        , createdTime(0)
        , startedTime(0)
        , cyclesExecuted(0) {}
};

struct VMListResponse {
    std::vector<VMInfo> vms;
    size_t totalCount;
    
    VMListResponse()
        : vms()
        , totalCount(0) {}
};

// ============================================================================
// VM Detailed Info
// ============================================================================

struct VMDetailedInfo {
    VMInfo basic;
    std::map<std::string, uint64_t> cpuStats;
    std::map<std::string, uint64_t> memoryStats;
    std::vector<std::string> activeSnapshots;
    std::string lastError;
    
    VMDetailedInfo()
        : basic()
        , cpuStats()
        , memoryStats()
        , activeSnapshots()
        , lastError() {}
};

// ============================================================================
// VM Logs
// ============================================================================

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

struct LogEntry {
    int64_t timestamp;
    LogLevel level;
    std::string message;
    std::string source;
    
    LogEntry()
        : timestamp(0)
        , level(LogLevel::INFO)
        , message()
        , source() {}
};

struct VMLogsResponse {
    std::string vmId;
    std::vector<LogEntry> logs;
    size_t totalCount;
    bool hasMore;
    
    VMLogsResponse()
        : vmId()
        , logs()
        , totalCount(0)
        , hasMore(false) {}
};

// ============================================================================
// VM Console Output
// ============================================================================

struct VMConsoleOutputResponse {
    std::string vmId;
    std::vector<std::string> lines;
    size_t startLine;
    size_t totalLines;
    uint64_t totalBytes;
    
    VMConsoleOutputResponse()
        : vmId()
        , lines()
        , startLine(0)
        , totalLines(0)
        , totalBytes(0) {}
};

// ============================================================================
// Real-time Status Updates
// ============================================================================

enum class StatusUpdateType {
    VM_STATE_CHANGED,
    CPU_USAGE,
    MEMORY_USAGE,
    EXECUTION_STATS,
    LOG_MESSAGE,
    ERROR
};

struct StatusUpdate {
    StatusUpdateType type;
    std::string vmId;
    int64_t timestamp;
    std::map<std::string, std::string> data;
    
    StatusUpdate()
        : type(StatusUpdateType::VM_STATE_CHANGED)
        , vmId()
        , timestamp(0)
        , data() {}
};

struct VMMetrics {
    std::string vmId;
    int64_t timestamp;
    
    // CPU metrics
    std::vector<double> cpuUsagePercent;  // Per CPU
    uint64_t cyclesExecuted;
    uint64_t instructionsExecuted;
    
    // Memory metrics
    uint64_t memoryUsedBytes;
    uint64_t memoryTotalBytes;
    uint64_t pageFaults;
    
    // Execution metrics
    uint64_t bundlesExecuted;
    uint64_t interruptsDelivered;
    std::string currentState;
    
    VMMetrics()
        : vmId()
        , timestamp(0)
        , cpuUsagePercent()
        , cyclesExecuted(0)
        , instructionsExecuted(0)
        , memoryUsedBytes(0)
        , memoryTotalBytes(0)
        , pageFaults(0)
        , bundlesExecuted(0)
        , interruptsDelivered(0)
        , currentState() {}
};

// ============================================================================
// Streaming Configuration
// ============================================================================

struct StreamingConfig {
    bool enableCPUMetrics;
    bool enableMemoryMetrics;
    bool enableExecutionMetrics;
    bool enableLogStreaming;
    uint32_t updateIntervalMs;
    std::vector<std::string> vmIds;  // Empty = all VMs
    
    StreamingConfig()
        : enableCPUMetrics(true)
        , enableMemoryMetrics(true)
        , enableExecutionMetrics(true)
        , enableLogStreaming(false)
        , updateIntervalMs(1000)
        , vmIds() {}
};

} // namespace api
} // namespace ia64
