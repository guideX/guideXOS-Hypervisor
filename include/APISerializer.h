#pragma once

#include "APITypes.h"
#include <string>
#include <sstream>
#include <iomanip>

namespace ia64 {
namespace api {

// ============================================================================
// JSON Serialization Helpers
// ============================================================================

class JSONSerializer {
public:
    // Serialize API types to JSON
    static std::string serialize(const APIResponse& response);
    static std::string serialize(const VMCreateResponse& response);
    static std::string serialize(const VMOperationResponse& response);
    static std::string serialize(const VMSnapshotResponse& response);
    static std::string serialize(const VMListResponse& response);
    static std::string serialize(const VMDetailedInfo& info);
    static std::string serialize(const VMLogsResponse& logs);
    static std::string serialize(const VMConsoleOutputResponse& console);
    static std::string serialize(const StatusUpdate& update);
    static std::string serialize(const VMMetrics& metrics);
    
    // Deserialize API types from JSON
    static bool deserialize(const std::string& json, APIRequest& request);
    static bool deserialize(const std::string& json, VMCreateRequest& request);
    static bool deserialize(const std::string& json, VMOperationRequest& request);
    static bool deserialize(const std::string& json, VMSnapshotRequest& request);
    static bool deserialize(const std::string& json, StreamingConfig& config);
    
private:
    static std::string escapeJSON(const std::string& str);
    static std::string toJSON(const VMInfo& info);
    static std::string toJSON(const LogEntry& entry);
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline std::string JSONSerializer::escapeJSON(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c >= 0 && c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

inline std::string JSONSerializer::serialize(const APIResponse& response) {
    std::ostringstream oss;
    oss << "{"
        << "\"request_id\":\"" << escapeJSON(response.requestId) << "\","
        << "\"status\":\"" << apiStatusToString(response.status) << "\","
        << "\"message\":\"" << escapeJSON(response.message) << "\","
        << "\"timestamp\":" << response.timestamp << ","
        << "\"body\":" << (response.body.empty() ? "null" : response.body)
        << "}";
    return oss.str();
}

inline std::string JSONSerializer::serialize(const VMCreateResponse& response) {
    std::ostringstream oss;
    oss << "{"
        << "\"vm_id\":\"" << escapeJSON(response.vmId) << "\","
        << "\"name\":\"" << escapeJSON(response.name) << "\","
        << "\"state\":\"" << escapeJSON(response.state) << "\""
        << "}";
    return oss.str();
}

inline std::string JSONSerializer::serialize(const VMOperationResponse& response) {
    std::ostringstream oss;
    oss << "{"
        << "\"vm_id\":\"" << escapeJSON(response.vmId) << "\","
        << "\"state\":\"" << escapeJSON(response.state) << "\","
        << "\"success\":" << (response.success ? "true" : "false") << ","
        << "\"message\":\"" << escapeJSON(response.message) << "\""
        << "}";
    return oss.str();
}

inline std::string JSONSerializer::toJSON(const VMInfo& info) {
    std::ostringstream oss;
    oss << "{"
        << "\"vm_id\":\"" << escapeJSON(info.vmId) << "\","
        << "\"name\":\"" << escapeJSON(info.name) << "\","
        << "\"state\":\"" << escapeJSON(info.state) << "\","
        << "\"memory_mb\":" << info.memorySizeMB << ","
        << "\"cpu_count\":" << info.cpuCount << ","
        << "\"created_time\":" << info.createdTime << ","
        << "\"started_time\":" << info.startedTime << ","
        << "\"cycles_executed\":" << info.cyclesExecuted
        << "}";
    return oss.str();
}

inline std::string JSONSerializer::serialize(const VMListResponse& response) {
    std::ostringstream oss;
    oss << "{\"vms\":[";
    for (size_t i = 0; i < response.vms.size(); ++i) {
        if (i > 0) oss << ",";
        oss << toJSON(response.vms[i]);
    }
    oss << "],\"total_count\":" << response.totalCount << "}";
    return oss.str();
}

inline std::string JSONSerializer::serialize(const VMMetrics& metrics) {
    std::ostringstream oss;
    oss << "{"
        << "\"vm_id\":\"" << escapeJSON(metrics.vmId) << "\","
        << "\"timestamp\":" << metrics.timestamp << ","
        << "\"cpu_usage\":[";
    
    for (size_t i = 0; i < metrics.cpuUsagePercent.size(); ++i) {
        if (i > 0) oss << ",";
        oss << metrics.cpuUsagePercent[i];
    }
    
    oss << "],"
        << "\"cycles_executed\":" << metrics.cyclesExecuted << ","
        << "\"instructions_executed\":" << metrics.instructionsExecuted << ","
        << "\"memory_used_bytes\":" << metrics.memoryUsedBytes << ","
        << "\"memory_total_bytes\":" << metrics.memoryTotalBytes << ","
        << "\"page_faults\":" << metrics.pageFaults << ","
        << "\"bundles_executed\":" << metrics.bundlesExecuted << ","
        << "\"interrupts_delivered\":" << metrics.interruptsDelivered << ","
        << "\"state\":\"" << escapeJSON(metrics.currentState) << "\""
        << "}";
    
    return oss.str();
}

inline std::string JSONSerializer::serialize(const VMConsoleOutputResponse& console) {
    std::ostringstream oss;
    oss << "{"
        << "\"vm_id\":\"" << escapeJSON(console.vmId) << "\","
        << "\"start_line\":" << console.startLine << ","
        << "\"total_lines\":" << console.totalLines << ","
        << "\"total_bytes\":" << console.totalBytes << ","
        << "\"lines\":[";
    
    for (size_t i = 0; i < console.lines.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << escapeJSON(console.lines[i]) << "\"";
    }
    
    oss << "]}";
    return oss.str();
}

inline std::string JSONSerializer::serialize(const StatusUpdate& update) {
    std::ostringstream oss;
    oss << "{"
        << "\"type\":\"";
    
    switch (update.type) {
        case StatusUpdateType::VM_STATE_CHANGED: oss << "vm_state_changed"; break;
        case StatusUpdateType::CPU_USAGE: oss << "cpu_usage"; break;
        case StatusUpdateType::MEMORY_USAGE: oss << "memory_usage"; break;
        case StatusUpdateType::EXECUTION_STATS: oss << "execution_stats"; break;
        case StatusUpdateType::LOG_MESSAGE: oss << "log_message"; break;
        case StatusUpdateType::ERROR: oss << "error"; break;
    }
    
    oss << "\","
        << "\"vm_id\":\"" << escapeJSON(update.vmId) << "\","
        << "\"timestamp\":" << update.timestamp << ","
        << "\"data\":{";
    
    bool first = true;
    for (const auto& pair : update.data) {
        if (!first) oss << ",";
        oss << "\"" << escapeJSON(pair.first) << "\":\""
            << escapeJSON(pair.second) << "\"";
        first = false;
    }
    
    oss << "}}";
    return oss.str();
}

} // namespace api
} // namespace ia64
