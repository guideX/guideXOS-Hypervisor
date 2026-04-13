#pragma once

#include "APIServer.h"
#include "APISerializer.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>

#pragma comment(lib, "Ws2_32.lib")

namespace ia64 {
namespace api {

// ============================================================================
// HTTP Request
// ============================================================================

struct HTTPRequest {
    std::string method;       // GET, POST, PUT, DELETE
    std::string path;
    std::string version;      // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;
    
    HTTPRequest()
        : method()
        , path()
        , version()
        , headers()
        , body() {}
};

// ============================================================================
// HTTP Response
// ============================================================================

struct HTTPResponse {
    int statusCode;
    std::string statusMessage;
    std::map<std::string, std::string> headers;
    std::string body;
    
    HTTPResponse()
        : statusCode(200)
        , statusMessage("OK")
        , headers()
        , body() {
        headers["Content-Type"] = "application/json";
        headers["Server"] = "IA64-Hypervisor-API/1.0";
    }
    
    std::string toString() const;
};

// ============================================================================
// REST API Server
// ============================================================================

class RestAPIServer : public IAPIServer {
public:
    explicit RestAPIServer(VMManager& vmManager, uint16_t port = 8080);
    ~RestAPIServer() override;
    
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_; }
    
    std::string getEndpoint() const override;
    
    // CORS configuration
    void enableCORS(bool enable);
    void addAllowedOrigin(const std::string& origin);
    
private:
    VMManager& vmManager_;
    std::unique_ptr<VMControlAPIHandler> apiHandler_;
    
    uint16_t port_;
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::thread serverThread_;
    std::vector<std::thread> workerThreads_;
    
    bool corsEnabled_;
    std::vector<std::string> allowedOrigins_;
    mutable std::mutex originsMutex_;
    
    void serverLoop();
    void handleClient(SOCKET clientSocket);
    
    HTTPRequest parseHTTPRequest(const std::string& requestData);
    HTTPResponse routeRequest(const HTTPRequest& httpRequest);
    
    // Route handlers
    HTTPResponse handlePOST(const HTTPRequest& request);
    HTTPResponse handleGET(const HTTPRequest& request);
    HTTPResponse handlePUT(const HTTPRequest& request);
    HTTPResponse handleDELETE(const HTTPRequest& request);
    HTTPResponse handleOPTIONS(const HTTPRequest& request);
    
    // API endpoints
    HTTPResponse handleCreateVM(const HTTPRequest& request);
    HTTPResponse handleDeleteVM(const std::string& vmId);
    HTTPResponse handleStartVM(const std::string& vmId);
    HTTPResponse handleStopVM(const std::string& vmId);
    HTTPResponse handlePauseVM(const std::string& vmId);
    HTTPResponse handleResumeVM(const std::string& vmId);
    HTTPResponse handleListVMs();
    HTTPResponse handleGetVMInfo(const std::string& vmId);
    HTTPResponse handleGetVMLogs(const std::string& vmId, const std::map<std::string, std::string>& params);
    HTTPResponse handleGetConsoleOutput(const std::string& vmId, const std::map<std::string, std::string>& params);
    HTTPResponse handleSnapshot(const std::string& vmId, const HTTPRequest& request);
    HTTPResponse handleRestore(const std::string& vmId, const HTTPRequest& request);
    
    HTTPResponse createJSONResponse(int code, const std::string& body);
    HTTPResponse createErrorResponse(int code, const std::string& message);
    
    void addCORSHeaders(HTTPResponse& response);
    
    std::string extractPathParameter(const std::string& path, const std::string& pattern);
    std::map<std::string, std::string> parseQueryString(const std::string& query);
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline std::string HTTPResponse::toString() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << statusMessage << "\r\n";
    
    for (const auto& header : headers) {
        oss << header.first << ": " << header.second << "\r\n";
    }
    
    oss << "Content-Length: " << body.length() << "\r\n";
    oss << "\r\n";
    oss << body;
    
    return oss.str();
}

} // namespace api
} // namespace ia64
