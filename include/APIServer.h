#pragma once

#include "APITypes.h"
#include "VMManager.h"
#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>

namespace ia64 {
namespace api {

// ============================================================================
// API Server Base Interface
// ============================================================================

class IAPIServer {
public:
    virtual ~IAPIServer() = default;
    
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    
    virtual std::string getEndpoint() const = 0;
};

// ============================================================================
// API Handler Interface
// ============================================================================

class IAPIHandler {
public:
    virtual ~IAPIHandler() = default;
    
    virtual APIResponse handleRequest(const APIRequest& request) = 0;
};

// ============================================================================
// VM Control API Handler
// ============================================================================

class VMControlAPIHandler : public IAPIHandler {
public:
    explicit VMControlAPIHandler(VMManager& vmManager);
    ~VMControlAPIHandler() override;
    
    APIResponse handleRequest(const APIRequest& request) override;
    
    // Specific operation handlers
    APIResponse handleCreateVM(const APIRequest& request);
    APIResponse handleDeleteVM(const APIRequest& request);
    APIResponse handleStartVM(const APIRequest& request);
    APIResponse handleStopVM(const APIRequest& request);
    APIResponse handlePauseVM(const APIRequest& request);
    APIResponse handleResumeVM(const APIRequest& request);
    APIResponse handleResetVM(const APIRequest& request);
    APIResponse handleSnapshotVM(const APIRequest& request);
    APIResponse handleRestoreVM(const APIRequest& request);
    APIResponse handleListVMs(const APIRequest& request);
    APIResponse handleGetVMInfo(const APIRequest& request);
    APIResponse handleGetVMLogs(const APIRequest& request);
    APIResponse handleExecuteVM(const APIRequest& request);
    
private:
    VMManager& vmManager_;
    mutable std::mutex mutex_;
    
    APIResponse createErrorResponse(const std::string& requestId, 
                                    const std::string& message,
                                    APIStatus status = APIStatus::ERROR);
    
    APIResponse createSuccessResponse(const std::string& requestId,
                                      const std::string& body);
};

// ============================================================================
// Status Streaming Server
// ============================================================================

class StatusStreamingServer {
public:
    using StatusUpdateCallback = std::function<void(const StatusUpdate&)>;
    
    explicit StatusStreamingServer(VMManager& vmManager);
    ~StatusStreamingServer();
    
    bool start(uint16_t port);
    void stop();
    bool isRunning() const { return running_; }
    
    // Client management
    void addClient(const std::string& clientId, StatusUpdateCallback callback);
    void removeClient(const std::string& clientId);
    
    // Configuration
    void setUpdateInterval(uint32_t intervalMs);
    void setStreamingConfig(const StreamingConfig& config);
    
    // Manual status updates
    void pushUpdate(const StatusUpdate& update);
    
private:
    VMManager& vmManager_;
    std::atomic<bool> running_;
    std::thread metricsThread_;
    std::mutex clientsMutex_;
    std::map<std::string, StatusUpdateCallback> clients_;
    
    StreamingConfig config_;
    uint32_t updateIntervalMs_;
    
    void metricsCollectionLoop();
    void collectAndBroadcastMetrics();
    VMMetrics collectVMMetrics(const std::string& vmId);
    void broadcastUpdate(const StatusUpdate& update);
};

// ============================================================================
// Log Streaming
// ============================================================================

class LogStreamer {
public:
    using LogCallback = std::function<void(const LogEntry&)>;
    
    LogStreamer();
    ~LogStreamer();
    
    void start();
    void stop();
    
    void addSubscriber(const std::string& vmId, LogCallback callback);
    void removeSubscriber(const std::string& vmId);
    
    void pushLog(const std::string& vmId, const LogEntry& entry);
    
private:
    std::mutex subscribersMutex_;
    std::map<std::string, std::vector<LogCallback>> subscribers_;
    std::atomic<bool> running_;
};

// ============================================================================
// Metrics Collector
// ============================================================================

class MetricsCollector {
public:
    explicit MetricsCollector(VMManager& vmManager);
    ~MetricsCollector();
    
    void startCollection(uint32_t intervalMs = 1000);
    void stopCollection();
    
    VMMetrics getLatestMetrics(const std::string& vmId) const;
    std::vector<VMMetrics> getMetricsHistory(const std::string& vmId, 
                                            size_t count) const;
    
private:
    VMManager& vmManager_;
    std::atomic<bool> running_;
    std::thread collectionThread_;
    
    mutable std::mutex metricsMutex_;
    std::map<std::string, std::deque<VMMetrics>> metricsHistory_;
    static constexpr size_t MAX_HISTORY_SIZE = 1000;
    
    void collectionLoop(uint32_t intervalMs);
    VMMetrics collectMetrics(const std::string& vmId);
};

} // namespace api
} // namespace ia64
