#include "WebUIServer.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace ia64 {
namespace api {

// ============================================================================
// WebUIServer Implementation
// ============================================================================

WebUIServer::WebUIServer(uint16_t port, const std::string& webRoot)
    : port_(port)
    , webRoot_(webRoot)
    , apiUrl_("http://localhost:8080")
    , corsEnabled_(true)
    , listenSocket_(INVALID_SOCKET)
    , running_(false)
    , serverThread_() {
}

WebUIServer::~WebUIServer() {
    stop();
}

bool WebUIServer::start() {
    if (running_) {
        LOG_WARN("Web UI server already running");
        return false;
    }
    
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LOG_ERROR("WSAStartup failed: " + std::to_string(result));
        return false;
    }
#endif
    
    // Create socket
    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
#ifdef _WIN32
        LOG_ERROR("Socket creation failed: " + std::to_string(WSAGetLastError()));
        WSACleanup();
#else
        LOG_ERROR("Socket creation failed");
#endif
        return false;
    }
    
    // Set socket options
    int opt = 1;
#ifdef _WIN32
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    
    // Bind socket
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port_);
    
    if (bind(listenSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
#ifdef _WIN32
        LOG_ERROR("Bind failed: " + std::to_string(WSAGetLastError()));
        closesocket(listenSocket_);
        WSACleanup();
#else
        LOG_ERROR("Bind failed");
        close(listenSocket_);
#endif
        return false;
    }
    
    // Listen
    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
#ifdef _WIN32
        LOG_ERROR("Listen failed: " + std::to_string(WSAGetLastError()));
        closesocket(listenSocket_);
        WSACleanup();
#else
        LOG_ERROR("Listen failed");
        close(listenSocket_);
#endif
        return false;
    }
    
    running_ = true;
    serverThread_ = std::thread(&WebUIServer::serverLoop, this);
    
    LOG_INFO("Web UI server started on port " + std::to_string(port_));
    LOG_INFO("Web root: " + webRoot_);
    LOG_INFO("Access UI at: " + getEndpoint());
    
    return true;
}

void WebUIServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Close listen socket
    if (listenSocket_ != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(listenSocket_);
#else
        close(listenSocket_);
#endif
        listenSocket_ = INVALID_SOCKET;
    }
    
    // Wait for server thread
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    LOG_INFO("Web UI server stopped");
}

std::string WebUIServer::getEndpoint() const {
    return "http://localhost:" + std::to_string(port_);
}

void WebUIServer::setWebRoot(const std::string& webRoot) {
    webRoot_ = webRoot;
}

void WebUIServer::serverLoop() {
    while (running_) {
        sockaddr_in clientAddr = {};
        int clientAddrSize = sizeof(clientAddr);
        
        SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &clientAddrSize);
        
        if (clientSocket == INVALID_SOCKET) {
            if (running_) {
#ifdef _WIN32
                LOG_ERROR("Accept failed: " + std::to_string(WSAGetLastError()));
#else
                LOG_ERROR("Accept failed");
#endif
            }
            continue;
        }
        
        // Handle client in a separate thread
        std::thread(&WebUIServer::handleClient, this, clientSocket).detach();
    }
}

void WebUIServer::handleClient(SOCKET clientSocket) {
    char buffer[4096];
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        std::string requestData(buffer);
        
        HttpRequest request = parseRequest(requestData);
        
        // Serve file or return 404
        if (!serveFile(clientSocket, request.path)) {
            std::string response = buildResponse(404, "Not Found", "text/html",
                "<html><body><h1>404 Not Found</h1></body></html>");
            send(clientSocket, response.c_str(), (int)response.length(), 0);
        }
    }
    
#ifdef _WIN32
    closesocket(clientSocket);
#else
    close(clientSocket);
#endif
}

WebUIServer::HttpRequest WebUIServer::parseRequest(const std::string& requestData) {
    HttpRequest request;
    std::istringstream iss(requestData);
    std::string line;
    
    // Parse request line
    if (std::getline(iss, line)) {
        std::istringstream lineStream(line);
        lineStream >> request.method >> request.path;
        
        // Remove query string
        size_t queryPos = request.path.find('?');
        if (queryPos != std::string::npos) {
            request.path = request.path.substr(0, queryPos);
        }
        
        // Decode URL
        request.path = urlDecode(request.path);
    }
    
    // Parse headers
    while (std::getline(iss, line) && line != "\r" && !line.empty()) {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            
            request.headers[key] = value;
        }
    }
    
    return request;
}

std::string WebUIServer::buildResponse(int statusCode, const std::string& statusText,
                                       const std::string& contentType, const std::string& body) {
    std::ostringstream response;
    
    response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    
    if (corsEnabled_) {
        response << "Access-Control-Allow-Origin: *\r\n";
        response << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
        response << "Access-Control-Allow-Headers: Content-Type\r\n";
    }
    
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    
    return response.str();
}

bool WebUIServer::serveFile(SOCKET clientSocket, const std::string& path) {
    // Default to index.html
    std::string filePath = path;
    if (filePath == "/" || filePath.empty()) {
        filePath = "/index.html";
    }
    
    // Security check
    if (!isPathSafe(filePath)) {
        LOG_WARN("Rejected unsafe path: " + filePath);
        return false;
    }
    
    // Build full file path
    std::string fullPath = webRoot_ + filePath;
    
    // Read file
    std::string content;
    if (!readFile(fullPath, content)) {
        LOG_DEBUG("File not found: " + fullPath);
        return false;
    }
    
    // Get MIME type
    std::string mimeType = getMimeType(filePath);
    
    // Build and send response
    std::string response = buildResponse(200, "OK", mimeType, content);
    send(clientSocket, response.c_str(), (int)response.length(), 0);
    
    LOG_DEBUG("Served file: " + filePath);
    return true;
}

std::string WebUIServer::getMimeType(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) {
        return "application/octet-stream";
    }
    
    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".txt") return "text/plain";
    
    return "application/octet-stream";
}

bool WebUIServer::readFile(const std::string& path, std::string& content) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    std::ostringstream ss;
    ss << file.rdbuf();
    content = ss.str();
    
    return true;
}

std::string WebUIServer::urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.length());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream iss(str.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    
    return result;
}

bool WebUIServer::isPathSafe(const std::string& path) {
    // Prevent directory traversal attacks
    if (path.find("..") != std::string::npos) {
        return false;
    }
    
    // Must start with /
    if (path.empty() || path[0] != '/') {
        return false;
    }
    
    return true;
}

} // namespace api
} // namespace ia64
