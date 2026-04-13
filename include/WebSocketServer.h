#pragma once

#include "APIServer.h"
#include "APISerializer.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <map>
#include <queue>

#pragma comment(lib, "Ws2_32.lib")

namespace ia64 {
namespace api {

// ============================================================================
// WebSocket Frame
// ============================================================================

enum class WebSocketOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};

struct WebSocketFrame {
    WebSocketOpcode opcode;
    bool fin;
    bool masked;
    std::vector<uint8_t> payload;
    
    WebSocketFrame()
        : opcode(WebSocketOpcode::TEXT)
        , fin(true)
        , masked(false)
        , payload() {}
    
    std::vector<uint8_t> serialize() const;
    static bool deserialize(const uint8_t* data, size_t length, WebSocketFrame& frame);
};

// ============================================================================
// WebSocket Connection
// ============================================================================

class WebSocketConnection {
public:
    explicit WebSocketConnection(SOCKET socket);
    ~WebSocketConnection();
    
    bool handshake(const std::string& key);
    bool sendFrame(const WebSocketFrame& frame);
    bool receiveFrame(WebSocketFrame& frame);
    
    void close();
    bool isOpen() const { return socket_ != INVALID_SOCKET; }
    
    std::string getClientId() const { return clientId_; }
    
private:
    SOCKET socket_;
    std::string clientId_;
    std::mutex sendMutex_;
    
    std::string generateAcceptKey(const std::string& key);
};

// ============================================================================
// WebSocket Server for Status Streaming
// ============================================================================

class WebSocketServer : public IAPIServer {
public:
    explicit WebSocketServer(VMManager& vmManager, uint16_t port = 8081);
    ~WebSocketServer() override;
    
    bool start() override;
    void stop() override;
    bool isRunning() const override { return running_; }
    
    std::string getEndpoint() const override;
    
    // Broadcast to all clients
    void broadcast(const std::string& message);
    
    // Send to specific client
    void sendToClient(const std::string& clientId, const std::string& message);
    
    // Client management
    size_t getClientCount() const;
    std::vector<std::string> getConnectedClients() const;
    
private:
    VMManager& vmManager_;
    std::unique_ptr<StatusStreamingServer> streamingServer_;
    
    uint16_t port_;
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::thread serverThread_;
    std::thread metricsThread_;
    
    std::mutex connectionsMutex_;
    std::map<std::string, std::unique_ptr<WebSocketConnection>> connections_;
    
    void serverLoop();
    void metricsLoop();
    void handleClient(SOCKET clientSocket);
    void handleWebSocketConnection(std::unique_ptr<WebSocketConnection> connection);
    
    bool performWebSocketHandshake(SOCKET socket, std::string& clientId);
    void removeConnection(const std::string& clientId);
    
    void broadcastMetrics();
};

// ============================================================================
// WebSocket Client for Testing
// ============================================================================

class WebSocketClient {
public:
    explicit WebSocketClient(const std::string& url);
    ~WebSocketClient();
    
    using MessageCallback = std::function<void(const std::string&)>;
    
    bool connect();
    void disconnect();
    bool isConnected() const { return connected_; }
    
    bool send(const std::string& message);
    
    void setMessageCallback(MessageCallback callback);
    
private:
    std::string url_;
    std::string host_;
    uint16_t port_;
    std::string path_;
    
    SOCKET socket_;
    std::atomic<bool> connected_;
    std::thread receiveThread_;
    
    MessageCallback messageCallback_;
    std::mutex callbackMutex_;
    
    void receiveLoop();
    bool parseURL(const std::string& url);
    bool performHandshake();
};

} // namespace api
} // namespace ia64
