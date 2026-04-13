#pragma once

#include "VMManager.h"
#include "RestAPIServer.h"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

namespace ia64 {
namespace api {

/**
 * WebUIServer - Simple HTTP server for serving the web-based VM management UI
 * 
 * This server serves static files (HTML, CSS, JavaScript) for the web-based
 * management interface. It runs alongside the REST API server and provides
 * a user-friendly interface for managing VMs.
 * 
 * Features:
 * - Serves static HTML/CSS/JavaScript files
 * - MIME type detection
 * - Simple HTTP/1.1 server
 * - CORS support for API access
 * - Configurable web root directory
 * 
 * Usage Example:
 * ```
 * VMManager vmManager;
 * RestAPIServer apiServer(vmManager, 8080);
 * WebUIServer webServer(8090, "./web");
 * 
 * apiServer.start();
 * webServer.start();
 * 
 * // Web UI accessible at http://localhost:8090
 * ```
 */
class WebUIServer {
public:
    /**
     * Constructor
     * 
     * @param port Port to listen on (default: 8090)
     * @param webRoot Path to web files directory (default: "./web")
     */
    explicit WebUIServer(uint16_t port = 8090, const std::string& webRoot = "./web");
    
    /**
     * Destructor - stops the server if running
     */
    ~WebUIServer();
    
    /**
     * Start the web UI server
     * 
     * @return true if server started successfully, false otherwise
     */
    bool start();
    
    /**
     * Stop the web UI server
     */
    void stop();
    
    /**
     * Check if server is running
     * 
     * @return true if server is running, false otherwise
     */
    bool isRunning() const { return running_; }
    
    /**
     * Get the server endpoint URL
     * 
     * @return Server endpoint (e.g., "http://localhost:8090")
     */
    std::string getEndpoint() const;
    
    /**
     * Set the web root directory
     * 
     * @param webRoot Path to web files directory
     */
    void setWebRoot(const std::string& webRoot);
    
    /**
     * Get the web root directory
     * 
     * @return Path to web files directory
     */
    std::string getWebRoot() const { return webRoot_; }
    
    /**
     * Enable CORS for cross-origin requests
     * 
     * @param enabled Enable or disable CORS
     */
    void setCORSEnabled(bool enabled) { corsEnabled_ = enabled; }
    
    /**
     * Set the API server URL for CORS configuration
     * 
     * @param apiUrl API server URL (e.g., "http://localhost:8080")
     */
    void setAPIUrl(const std::string& apiUrl) { apiUrl_ = apiUrl; }

private:
    uint16_t port_;
    std::string webRoot_;
    std::string apiUrl_;
    bool corsEnabled_;
    
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::thread serverThread_;
    
    // Server thread function
    void serverLoop();
    
    // Handle client connection
    void handleClient(SOCKET clientSocket);
    
    // Parse HTTP request
    struct HttpRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
    };
    
    HttpRequest parseRequest(const std::string& requestData);
    
    // Build HTTP response
    std::string buildResponse(int statusCode, const std::string& statusText,
                             const std::string& contentType, const std::string& body);
    
    // Serve static file
    bool serveFile(SOCKET clientSocket, const std::string& path);
    
    // Get MIME type from file extension
    std::string getMimeType(const std::string& path);
    
    // Read file contents
    bool readFile(const std::string& path, std::string& content);
    
    // URL decode
    std::string urlDecode(const std::string& str);
    
    // Security: validate path (prevent directory traversal)
    bool isPathSafe(const std::string& path);
};

} // namespace api
} // namespace ia64
