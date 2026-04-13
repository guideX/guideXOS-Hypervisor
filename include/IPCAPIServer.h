#pragma once

#include "APIServer.h"
#include "APISerializer.h"
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace ia64 {
namespace api {

// ============================================================================
// IPC Message
// ============================================================================

struct IPCMessage {
    uint32_t messageId;
    uint32_t messageType;
    uint32_t dataLength;
    std::vector<uint8_t> data;
    
    IPCMessage()
        : messageId(0)
        , messageType(0)
        , dataLength(0)
        , data() {}
    
    std::vector<uint8_t> serialize() const;
    static bool deserialize(const std::vector<uint8_t>& bytes, IPCMessage& message);
};

// ============================================================================
// IPC Server using Windows Named Pipes
// ============================================================================

class IPCAPIServer : public IAPIServer {
public:
    explicit IPCAPIServer(VMManager& vmManager, const std::string& pipeName = "\\\\.\\pipe\\ia64-hypervisor-api");
    ~IPCAPIServer() override;
    
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_; }
    
    std::string getEndpoint() const override { return pipeName_; }
    
private:
    VMManager& vmManager_;
    std::unique_ptr<VMControlAPIHandler> apiHandler_;
    
    std::string pipeName_;
    std::atomic<bool> running_;
    std::thread serverThread_;
    std::vector<std::thread> workerThreads_;
    std::vector<HANDLE> pipeHandles_;
    std::mutex handlesMutex_;
    
    void serverLoop();
    void handleClient(HANDLE pipeHandle);
    
    bool readMessage(HANDLE pipe, IPCMessage& message);
    bool writeMessage(HANDLE pipe, const IPCMessage& message);
    
    IPCMessage processMessage(const IPCMessage& request);
};

// ============================================================================
// IPC Client
// ============================================================================

class IPCAPIClient {
public:
    explicit IPCAPIClient(const std::string& pipeName = "\\\\.\\pipe\\ia64-hypervisor-api");
    ~IPCAPIClient();
    
    bool connect(uint32_t timeoutMs = 5000);
    void disconnect();
    bool isConnected() const { return pipeHandle_ != INVALID_HANDLE_VALUE; }
    
    // Synchronous request/response
    APIResponse sendRequest(const APIRequest& request);
    
    // Async request
    bool sendRequestAsync(const APIRequest& request, 
                         std::function<void(const APIResponse&)> callback);
    
private:
    std::string pipeName_;
    HANDLE pipeHandle_;
    std::mutex pipeMutex_;
    std::atomic<uint32_t> nextMessageId_;
    
    bool readResponse(IPCMessage& response);
    bool writeRequest(const IPCMessage& request);
};

} // namespace api
} // namespace ia64
