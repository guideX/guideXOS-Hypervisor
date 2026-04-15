#include "RestAPIServer.h"
#include "logger.h"
#include <sstream>
#include <algorithm>
#include <chrono>

#ifdef DELETE
#undef DELETE
#endif

namespace ia64 {
namespace api {

// ============================================================================
// RestAPIServer Implementation
// ============================================================================

RestAPIServer::RestAPIServer(VMManager& vmManager, uint16_t port)
    : vmManager_(vmManager)
    , apiHandler_(std::make_unique<VMControlAPIHandler>(vmManager))
    , port_(port)
    , listenSocket_(INVALID_SOCKET)
    , running_(false)
    , serverThread_()
    , workerThreads_()
    , corsEnabled_(false)
    , allowedOrigins_()
    , originsMutex_() {
}

RestAPIServer::~RestAPIServer() {
    stop();
}

bool RestAPIServer::start() {
    if (running_) {
        LOG_WARN("REST API server already running");
        return false;
    }
    
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LOG_ERROR("WSAStartup failed: " + std::to_string(result));
        return false;
    }
    
    // Create socket
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        LOG_ERROR("Socket creation failed: " + std::to_string(WSAGetLastError()));
        WSACleanup();
        return false;
    }
    
    // Bind
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port_);
    
    if (bind(listenSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LOG_ERROR("Bind failed: " + std::to_string(WSAGetLastError()));
        closesocket(listenSocket_);
        WSACleanup();
        return false;
    }
    
    // Listen
    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("Listen failed: " + std::to_string(WSAGetLastError()));
        closesocket(listenSocket_);
        WSACleanup();
        return false;
    }
    
    running_ = true;
    serverThread_ = std::thread(&RestAPIServer::serverLoop, this);
    
    LOG_INFO("REST API server started on port " + std::to_string(port_));
    return true;
}

void RestAPIServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Close listen socket to unblock accept()
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
    
    // Wait for server thread
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
    // Wait for worker threads
    for (auto& thread : workerThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workerThreads_.clear();
    
    WSACleanup();
    
    LOG_INFO("REST API server stopped");
}

std::string RestAPIServer::getEndpoint() const {
    return "http://localhost:" + std::to_string(port_);
}

void RestAPIServer::enableCORS(bool enable) {
    corsEnabled_ = enable;
}

void RestAPIServer::addAllowedOrigin(const std::string& origin) {
    std::lock_guard<std::mutex> lock(originsMutex_);
    allowedOrigins_.push_back(origin);
}

void RestAPIServer::serverLoop() {
    while (running_) {
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        
        SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientSocket == INVALID_SOCKET) {
            if (running_) {
                LOG_ERROR("Accept failed: " + std::to_string(WSAGetLastError()));
            }
            break;
        }
        
        // Handle client in worker thread
        workerThreads_.emplace_back(&RestAPIServer::handleClient, this, clientSocket);
    }
}

void RestAPIServer::handleClient(SOCKET clientSocket) {
    const size_t BUFFER_SIZE = 8192;
    char buffer[BUFFER_SIZE];
    
    // Receive request
    int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }
    
    buffer[bytesReceived] = '\0';
    std::string requestData(buffer, bytesReceived);
    
    // Parse HTTP request
    HTTPRequest httpRequest = parseHTTPRequest(requestData);
    
    // Route and handle request
    HTTPResponse httpResponse = routeRequest(httpRequest);
    
    // Add CORS headers if enabled
    if (corsEnabled_) {
        addCORSHeaders(httpResponse);
    }
    
    // Send response
    std::string responseStr = httpResponse.toString();
    send(clientSocket, responseStr.c_str(), static_cast<int>(responseStr.length()), 0);
    
    closesocket(clientSocket);
}

HTTPRequest RestAPIServer::parseHTTPRequest(const std::string& requestData) {
    HTTPRequest request;
    std::istringstream iss(requestData);
    std::string line;
    
    // Parse request line
    if (std::getline(iss, line)) {
        std::istringstream lineStream(line);
        lineStream >> request.method >> request.path >> request.version;
    }
    
    // Parse headers
    while (std::getline(iss, line) && line != "\r") {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r"));
            value.erase(value.find_last_not_of(" \t\r") + 1);
            
            request.headers[key] = value;
        }
    }
    
    // Parse body
    std::ostringstream bodyStream;
    bodyStream << iss.rdbuf();
    request.body = bodyStream.str();
    
    return request;
}

HTTPResponse RestAPIServer::routeRequest(const HTTPRequest& httpRequest) {
    if (httpRequest.method == "POST") {
        return handlePOST(httpRequest);
    } else if (httpRequest.method == "GET") {
        return handleGET(httpRequest);
    } else if (httpRequest.method == "PUT") {
        return handlePUT(httpRequest);
    } else if (httpRequest.method == "DELETE") {
        return handleDELETE(httpRequest);
    } else if (httpRequest.method == "OPTIONS") {
        return handleOPTIONS(httpRequest);
    } else {
        return createErrorResponse(405, "Method Not Allowed");
    }
}

HTTPResponse RestAPIServer::handlePOST(const HTTPRequest& request) {
    if (request.path == "/api/v1/vms") {
        return handleCreateVM(request);
    } else if (request.path.find("/api/v1/vms/") == 0 && request.path.find("/snapshot") != std::string::npos) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/(.*)/snapshot");
        return handleSnapshot(vmId, request);
    } else if (request.path.find("/api/v1/vms/") == 0 && request.path.find("/start") != std::string::npos) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/(.*)/start");
        return handleStartVM(vmId);
    } else if (request.path.find("/api/v1/vms/") == 0 && request.path.find("/stop") != std::string::npos) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/(.*)/stop");
        return handleStopVM(vmId);
    } else if (request.path.find("/api/v1/vms/") == 0 && request.path.find("/pause") != std::string::npos) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/(.*)/pause");
        return handlePauseVM(vmId);
    } else if (request.path.find("/api/v1/vms/") == 0 && request.path.find("/resume") != std::string::npos) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/(.*)/resume");
        return handleResumeVM(vmId);
    }
    
    return createErrorResponse(404, "Not Found");
}

HTTPResponse RestAPIServer::handleGET(const HTTPRequest& request) {
    if (request.path == "/api/v1/vms") {
        return handleListVMs();
    } else if (request.path.find("/api/v1/vms/") == 0) {
        // Extract VM ID and optional subpath
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/([^/]+)");
        
        if (request.path.find("/console") != std::string::npos) {
            return handleGetConsoleOutput(vmId, parseQueryString(request.path));
        } else if (request.path.find("/logs") != std::string::npos) {
            return handleGetVMLogs(vmId, parseQueryString(request.path));
        } else {
            return handleGetVMInfo(vmId);
        }
    } else if (request.path == "/api/v1/health") {
        return createJSONResponse(200, "{\"status\":\"healthy\",\"version\":\"" + std::string(API_VERSION) + "\"}");
    }
    
    return createErrorResponse(404, "Not Found");
}

HTTPResponse RestAPIServer::handleDELETE(const HTTPRequest& request) {
    if (request.path.find("/api/v1/vms/") == 0) {
        std::string vmId = extractPathParameter(request.path, "/api/v1/vms/([^/]+)");
        return handleDeleteVM(vmId);
    }
    
    return createErrorResponse(404, "Not Found");
}

HTTPResponse RestAPIServer::handleOPTIONS(const HTTPRequest& request) {
    HTTPResponse response;
    response.statusCode = 204;
    response.statusMessage = "No Content";
    response.headers["Allow"] = "GET, POST, PUT, DELETE, OPTIONS";
    return response;
}

HTTPResponse RestAPIServer::handleGetConsoleOutput(const std::string& vmId, const std::map<std::string, std::string>& params) {
    VMConsoleOutputResponse consoleResponse;
    consoleResponse.vmId = vmId;
    
    // Parse query parameters
    size_t startLine = 0;
    size_t count = 100;  // Default to last 100 lines
    size_t maxBytes = 0;
    
    auto startIt = params.find("start");
    if (startIt != params.end()) {
        startLine = static_cast<size_t>(std::stoull(startIt->second));
    }
    
    auto countIt = params.find("count");
    if (countIt != params.end()) {
        count = static_cast<size_t>(std::stoull(countIt->second));
    }
    
    auto bytesIt = params.find("bytes");
    if (bytesIt != params.end()) {
        maxBytes = static_cast<size_t>(std::stoull(bytesIt->second));
    }
    
    // Get console output from VMManager
    if (maxBytes > 0) {
        // Return recent output as single string
        std::string recentOutput = vmManager_.getRecentConsoleOutput(vmId, maxBytes);
        if (!recentOutput.empty()) {
            // Split into lines
            std::istringstream iss(recentOutput);
            std::string line;
            while (std::getline(iss, line)) {
                consoleResponse.lines.push_back(line);
            }
        }
        consoleResponse.startLine = 0;
    } else {
        // Return line range
        consoleResponse.lines = vmManager_.getConsoleOutput(vmId, startLine, count);
        consoleResponse.startLine = startLine;
    }
    
    consoleResponse.totalLines = vmManager_.getConsoleLineCount(vmId);
    consoleResponse.totalBytes = vmManager_.getConsoleTotalBytes(vmId);
    
    std::string jsonResponse = JSONSerializer::serialize(consoleResponse);
    return createJSONResponse(200, jsonResponse);
}

HTTPResponse RestAPIServer::handlePUT(const HTTPRequest& request) {
    return createErrorResponse(501, "Not Implemented");
}

HTTPResponse RestAPIServer::handleCreateVM(const HTTPRequest& request) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::CREATE;
    apiRequest.body = request.body;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 201 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleDeleteVM(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::DELETE;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 404;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleStartVM(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::START;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleStopVM(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::STOP;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handlePauseVM(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::PAUSE;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleResumeVM(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::RESUME;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleListVMs() {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::LIST;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 500;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleGetVMInfo(const std::string& vmId) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::GET_INFO;
    apiRequest.parameters["vmId"] = vmId;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 404;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleGetVMLogs(const std::string& vmId, const std::map<std::string, std::string>& params) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::GET_LOGS;
    apiRequest.parameters["vmId"] = vmId;
    
    for (const auto& param : params) {
        apiRequest.parameters[param.first] = param.second;
    }
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 200 : 404;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::handleSnapshot(const std::string& vmId, const HTTPRequest& request) {
    APIRequest apiRequest;
    apiRequest.requestId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    apiRequest.operation = VMOperation::SNAPSHOT;
    apiRequest.parameters["vmId"] = vmId;
    apiRequest.body = request.body;
    
    APIResponse apiResponse = apiHandler_->handleRequest(apiRequest);
    
    int statusCode = (apiResponse.status == APIStatus::SUCCESS) ? 201 : 400;
    std::string jsonResponse = JSONSerializer::serialize(apiResponse);
    return createJSONResponse(statusCode, jsonResponse);
}

HTTPResponse RestAPIServer::createJSONResponse(int code, const std::string& body) {
    HTTPResponse response;
    response.statusCode = code;
    
    switch (code) {
        case 200: response.statusMessage = "OK"; break;
        case 201: response.statusMessage = "Created"; break;
        case 400: response.statusMessage = "Bad Request"; break;
        case 404: response.statusMessage = "Not Found"; break;
        case 500: response.statusMessage = "Internal Server Error"; break;
        case 501: response.statusMessage = "Not Implemented"; break;
        default: response.statusMessage = "Unknown"; break;
    }
    
    response.body = body;
    response.headers["Content-Type"] = "application/json";
    return response;
}

HTTPResponse RestAPIServer::createErrorResponse(int code, const std::string& message) {
    std::string jsonBody = "{\"error\":\"" + message + "\",\"code\":" + std::to_string(code) + "}";
    return createJSONResponse(code, jsonBody);
}

void RestAPIServer::addCORSHeaders(HTTPResponse& response) {
    response.headers["Access-Control-Allow-Origin"] = "*";
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
    response.headers["Access-Control-Max-Age"] = "86400";
}

std::string RestAPIServer::extractPathParameter(const std::string& path, const std::string& pattern) {
    size_t patternStart = pattern.find("(");
    size_t patternEnd = pattern.find(")");
    
    if (patternStart == std::string::npos || patternEnd == std::string::npos) {
        return "";
    }
    
    std::string prefix = pattern.substr(0, patternStart);
    std::string suffix = pattern.substr(patternEnd + 1);
    
    if (path.find(prefix) != 0) {
        return "";
    }
    
    size_t paramStart = prefix.length();
    size_t paramEnd = path.length();
    
    if (!suffix.empty()) {
        size_t suffixPos = path.find(suffix, paramStart);
        if (suffixPos != std::string::npos) {
            paramEnd = suffixPos;
        }
    }
    
    return path.substr(paramStart, paramEnd - paramStart);
}

std::map<std::string, std::string> RestAPIServer::parseQueryString(const std::string& query) {
    std::map<std::string, std::string> params;
    
    size_t queryStart = query.find('?');
    if (queryStart == std::string::npos) {
        return params;
    }
    
    std::string queryString = query.substr(queryStart + 1);
    std::istringstream iss(queryString);
    std::string pair;
    
    while (std::getline(iss, pair, '&')) {
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = pair.substr(0, eqPos);
            std::string value = pair.substr(eqPos + 1);
            params[key] = value;
        }
    }
    
    return params;
}

} // namespace api
} // namespace ia64
