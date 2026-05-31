# Local Control API Documentation

## Overview

The IA-64 Hypervisor provides a comprehensive local control API for managing virtual machines through three interfaces:
- **REST API**: HTTP-based RESTful API (port 8080 by default)
- **IPC API**: Windows Named Pipes for local inter-process communication
- **WebSocket**: Real-time status streaming and event notifications (port 8081 by default)

All APIs are thread-safe and support concurrent operations.

## Architecture

```
???????????????????????????????????????????????????
?            Client Applications                  ?
?  (CLI, GUI, External Tools, Monitoring)        ?
???????????????????????????????????????????????????
           ?              ?              ?
           ?              ?              ?
    ????????????   ????????????   ????????????
    ?   REST   ?   ?   IPC    ?   ?WebSocket ?
    ?   API    ?   ?   API    ?   ? Streaming?
    ? :8080    ?   ?  NamedPipe?  ?  :8081   ?
    ????????????   ????????????   ????????????
           ?              ?              ?
           ???????????????????????????????
                          ?
              ???????????????????????????
              ?  VM Control API Handler ?
              ?  (Thread-Safe)          ?
              ???????????????????????????
                          ?
                          ?
              ???????????????????????????
              ?      VMManager          ?
              ?   (Manages All VMs)     ?
              ???????????????????????????
                          ?
            ?????????????????????????????
            ?             ?             ?
       ??????????   ??????????   ??????????
       ?  VM #1 ?   ?  VM #2 ?   ?  VM #n ?
       ??????????   ??????????   ??????????
```

## REST API

### Base URL
```
http://localhost:8080/api/v1
```

### Endpoints

#### 1. Health Check
```http
GET /api/v1/health
```

**Response:**
```json
{
  "status": "healthy",
  "version": "1.0.0"
}
```

#### 2. Create VM
```http
POST /api/v1/vms
Content-Type: application/json

{
  "name": "my-vm",
  "config": {
    "memory": { "size_mb": 512 },
    "cpu": { "count": 2 },
    "storage": []
  },
  "auto_start": false
}
```

**Response:**
```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "name": "my-vm",
  "state": "stopped"
}
```

#### 3. List VMs
```http
GET /api/v1/vms
```

**Response:**
```json
{
  "vms": [
    {
      "vm_id": "550e8400-e29b-41d4-a716-446655440000",
      "name": "my-vm",
      "state": "running",
      "memory_mb": 512,
      "cpu_count": 2,
      "created_time": 1234567890,
      "started_time": 1234567900,
      "cycles_executed": 1000000
    }
  ],
  "total_count": 1
}
```

#### 4. Get VM Info
```http
GET /api/v1/vms/{vm_id}
```

**Response:**
```json
{
  "basic": {
    "vm_id": "550e8400-e29b-41d4-a716-446655440000",
    "name": "my-vm",
    "state": "running",
    "memory_mb": 512,
    "cpu_count": 2
  },
  "cpu_stats": {
    "cycles_executed": 1000000,
    "instructions_executed": 750000
  },
  "memory_stats": {
    "used_bytes": 268435456,
    "total_bytes": 536870912,
    "page_faults": 100
  }
}
```

#### 5. Start VM
```http
POST /api/v1/vms/{vm_id}/start
```

**Response:**
```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "state": "running",
  "success": true,
  "message": "VM started successfully"
}
```

#### 6. Stop VM
```http
POST /api/v1/vms/{vm_id}/stop
```

#### 7. Pause VM
```http
POST /api/v1/vms/{vm_id}/pause
```

#### 8. Resume VM
```http
POST /api/v1/vms/{vm_id}/resume
```

#### 9. Delete VM
```http
DELETE /api/v1/vms/{vm_id}
```

#### 10. Create Snapshot
```http
POST /api/v1/vms/{vm_id}/snapshot
Content-Type: application/json

{
  "name": "checkpoint-1",
  "description": "Before major update"
}
```

**Response:**
```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "snapshot_id": "snap-12345",
  "snapshot_name": "checkpoint-1",
  "timestamp": 1234567890
}
```

#### 11. Restore Snapshot
```http
POST /api/v1/vms/{vm_id}/restore
Content-Type: application/json

{
  "snapshot_id": "snap-12345"
}
```

#### 12. Get VM Logs
```http
GET /api/v1/vms/{vm_id}/logs?limit=100&level=info
```

**Response:**
```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "logs": [
    {
      "timestamp": 1234567890,
      "level": "info",
      "message": "VM started",
      "source": "VMManager"
    }
  ],
  "total_count": 100,
  "has_more": false
}
```

#### 13. Get VM Console Output
```http
GET /api/v1/vms/{vm_id}/console?start=0&count=100
GET /api/v1/vms/{vm_id}/console?bytes=4096
```

**Query Parameters:**
- `start` - Start line number (0-based, optional)
- `count` - Number of lines to retrieve (optional, default: 100)
- `bytes` - Get recent N bytes instead of lines (optional)

**Response:**
```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "start_line": 0,
  "total_lines": 1523,
  "total_bytes": 45678,
  "lines": [
    "Starting IA-64 VM...",
    "Loading kernel...",
    "Kernel initialized"
  ]
}
```

**Features:**
- Real-time console output capture
- Configurable scrollback buffer (default: 10,000 lines)
- Line-based or byte-based retrieval
- Thread-safe access
- Automatic FIFO overflow handling

### Error Responses

All API errors follow this format:
```json
{
  "request_id": "req-12345",
  "status": "error",
  "message": "VM not found",
  "timestamp": 1234567890,
  "body": null
}
```

**Status Codes:**
- `200 OK` - Success
- `201 Created` - Resource created
- `400 Bad Request` - Invalid request
- `404 Not Found` - Resource not found
- `409 Conflict` - Resource conflict (e.g., VM already running)
- `500 Internal Server Error` - Server error

### CORS Support

CORS is supported for cross-origin requests. Enable CORS and configure allowed origins:

```cpp
restServer.enableCORS(true);
restServer.addAllowedOrigin("https://console.example.com");
```

## IPC API

### Named Pipe

Default pipe name: `\\\\.\\pipe\\ia64-hypervisor-api`

### Message Format

Binary message format:
```
+-------------------+
| Message ID (4B)   |
+-------------------+
| Message Type (4B) |
+-------------------+
| Data Length (4B)  |
+-------------------+
| Data (variable)   |
+-------------------+
```

### Usage Example

```cpp
#include "IPCAPIClient.h"

// Connect
ia64::api::IPCAPIClient client;
if (!client.connect()) {
    std::cerr << "Failed to connect\n";
    return;
}

// Create request
ia64::api::APIRequest request;
request.requestId = "req-1";
request.operation = ia64::api::VMOperation::LIST;

// Send and get response
ia64::api::APIResponse response = client.sendRequest(request);

std::cout << "Status: " << apiStatusToString(response.status) << "\n";
std::cout << "Body: " << response.body << "\n";
```

### Async Request

```cpp
client.sendRequestAsync(request, [](const ia64::api::APIResponse& response) {
    std::cout << "Received response: " << response.body << "\n";
});
```

## WebSocket Streaming API

### Connection

```
ws://localhost:8081
```

### Handshake

Standard WebSocket handshake (RFC 6455):
```http
GET / HTTP/1.1
Host: localhost:8081
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

### Message Format

All messages are JSON text frames:

```json
{
  "type": "vm_state_changed",
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "timestamp": 1234567890,
  "data": {
    "state": "running",
    "previous_state": "stopped"
  }
}
```

### Event Types

#### 1. VM State Changed
```json
{
  "type": "vm_state_changed",
  "vm_id": "...",
  "timestamp": 1234567890,
  "data": {
    "state": "running"
  }
}
```

#### 2. CPU Usage Update
```json
{
  "type": "cpu_usage",
  "vm_id": "...",
  "timestamp": 1234567890,
  "data": {
    "cpu0": "25.5",
    "cpu1": "50.2",
    "average": "37.85"
  }
}
```

#### 3. Memory Usage Update
```json
{
  "type": "memory_usage",
  "vm_id": "...",
  "timestamp": 1234567890,
  "data": {
    "used_bytes": "268435456",
    "total_bytes": "536870912",
    "percent": "50.0"
  }
}
```

#### 4. Execution Stats
```json
{
  "type": "execution_stats",
  "vm_id": "...",
  "timestamp": 1234567890,
  "data": {
    "cycles_executed": "1000000",
    "instructions_executed": "750000",
    "bundles_executed": "250000"
  }
}
```

#### 5. Log Message
```json
{
  "type": "log_message",
  "vm_id": "...",
  "timestamp": 1234567890,
  "data": {
    "level": "info",
    "message": "Checkpoint created",
    "source": "VMManager"
  }
}
```

### Metrics Streaming

Complete VM metrics are broadcast periodically:

```json
{
  "vm_id": "550e8400-e29b-41d4-a716-446655440000",
  "timestamp": 1234567890,
  "cpu_usage": [25.5, 50.2],
  "cycles_executed": 1000000,
  "instructions_executed": 750000,
  "memory_used_bytes": 268435456,
  "memory_total_bytes": 536870912,
  "page_faults": 100,
  "bundles_executed": 250000,
  "interrupts_delivered": 50,
  "state": "running"
}
```

### Configuration

Configure streaming behavior:
```cpp
ia64::api::StreamingConfig config;
config.enableCPUMetrics = true;
config.enableMemoryMetrics = true;
config.enableExecutionMetrics = true;
config.enableLogStreaming = true;
config.updateIntervalMs = 1000;  // 1 second
config.vmIds = {"vm-1", "vm-2"}; // Empty = all VMs

webSocketServer.setStreamingConfig(config);
```

### Client Example (JavaScript)

```javascript
const ws = new WebSocket('ws://localhost:8081');

ws.onopen = () => {
    console.log('Connected to hypervisor status stream');
};

ws.onmessage = (event) => {
    const update = JSON.parse(event.data);
    console.log(`Received ${update.type} for VM ${update.vm_id}`);
    
    // Handle different update types
    switch (update.type) {
        case 'vm_state_changed':
            console.log(`State: ${update.data.state}`);
            break;
        case 'cpu_usage':
            console.log(`CPU: ${update.data.average}%`);
            break;
        case 'execution_stats':
            console.log(`Cycles: ${update.data.cycles_executed}`);
            break;
    }
};

ws.onclose = () => {
    console.log('Disconnected from hypervisor');
};
```

### Client Example (C++)

```cpp
#include "WebSocketClient.h"

ia64::api::WebSocketClient client("ws://localhost:8081");

client.setMessageCallback([](const std::string& message) {
    std::cout << "Received: " << message << "\n";
});

if (client.connect()) {
    std::cout << "Connected to streaming server\n";
    
    // Keep connection alive
    std::this_thread::sleep_for(std::chrono::seconds(60));
    
    client.disconnect();
}
```

## Thread Safety

All API operations are thread-safe:

1. **VMManager**: Protected by internal mutexes for VM lifecycle operations
2. **VMControlAPIHandler**: Uses mutex for request handling
3. **StatusStreamingServer**: Thread-safe client management and broadcasting
4. **MetricsCollector**: Separate collection thread with synchronized data access

Multiple concurrent API requests are safe and will not cause race conditions or data corruption.

## Performance Considerations

- **REST API**: ~1000 requests/second on modern hardware
- **IPC API**: ~5000 requests/second (lower overhead)
- **WebSocket**: ~10000 messages/second broadcast capability
- **Metrics Collection**: Configurable interval (default 1000ms)
- **Connection Limit**: Up to 1000 concurrent WebSocket connections

## Security Notes

**Current Implementation:**
- Local-only access (localhost binding)
- No authentication/authorization
- No encryption (HTTP/WS, not HTTPS/WSS)

**For Production Use, Add:**
- API key or token-based authentication
- TLS/SSL encryption (HTTPS/WSS)
- Rate limiting
- IP whitelisting
- Audit logging

## Example Integration

Complete example:

```cpp
#include "RestAPIServer.h"
#include "IPCAPIServer.h"
#include "WebSocketServer.h"
#include "VMManager.h"

int main() {
    // Create VM manager
    ia64::VMManager vmManager;
    
    // Start REST API server
    ia64::api::RestAPIServer restServer(vmManager, 8080);
    restServer.enableCORS(true);
    restServer.start();
    
    // Start IPC API server
    ia64::api::IPCAPIServer ipcServer(vmManager);
    ipcServer.start();
    
    // Start WebSocket streaming server
    ia64::api::WebSocketServer wsServer(vmManager, 8081);
    wsServer.start();
    
    std::cout << "API servers running:\n";
    std::cout << "  REST API: " << restServer.getEndpoint() << "\n";
    std::cout << "  IPC API: " << ipcServer.getEndpoint() << "\n";
    std::cout << "  WebSocket: " << wsServer.getEndpoint() << "\n";
    
    // Keep running
    std::cin.get();
    
    // Cleanup
    wsServer.stop();
    ipcServer.stop();
    restServer.stop();
    
    return 0;
}
```

## Testing

Test the API using curl:

```bash
# Health check
curl http://localhost:8080/api/v1/health

# Create VM
curl -X POST http://localhost:8080/api/v1/vms \
  -H "Content-Type: application/json" \
  -d '{
    "name": "test-vm",
    "config": {
      "memory": {"size_mb": 256},
      "cpu": {"count": 1}
    }
  }'

# List VMs
curl http://localhost:8080/api/v1/vms

# Start VM
curl -X POST http://localhost:8080/api/v1/vms/{vm-id}/start

# Get VM info
curl http://localhost:8080/api/v1/vms/{vm-id}

# Stop VM
curl -X POST http://localhost:8080/api/v1/vms/{vm-id}/stop

# Delete VM
curl -X DELETE http://localhost:8080/api/v1/vms/{vm-id}
```

Test WebSocket with wscat:
```bash
# Install wscat
npm install -g wscat

# Connect
wscat -c ws://localhost:8081

# You'll start receiving real-time updates
```

## Monitoring Dashboard Example

Build a simple monitoring dashboard:

```html
<!DOCTYPE html>
<html>
<head>
    <title>VM Monitor</title>
</head>
<body>
    <h1>VM Status Dashboard</h1>
    <div id="vms"></div>
    
    <script>
        const ws = new WebSocket('ws://localhost:8081');
        const vms = {};
        
        ws.onmessage = (event) => {
            const update = JSON.parse(event.data);
            
            if (!vms[update.vm_id]) {
                vms[update.vm_id] = {};
            }
            
            vms[update.vm_id][update.type] = update.data;
            updateDisplay();
        };
        
        function updateDisplay() {
            const div = document.getElementById('vms');
            div.innerHTML = Object.keys(vms).map(vmId => {
                const vm = vms[vmId];
                return `
                    <div>
                        <h3>VM: ${vmId}</h3>
                        <p>State: ${vm.vm_state_changed?.state || 'unknown'}</p>
                        <p>CPU: ${vm.cpu_usage?.average || '0'}%</p>
                        <p>Cycles: ${vm.execution_stats?.cycles_executed || '0'}</p>
                    </div>
                `;
            }).join('');
        }
    </script>
</body>
</html>
```

## Troubleshooting

**Connection Refused:**
- Check if server is running
- Verify port is not blocked by firewall
- Ensure correct endpoint URL

**Timeout:**
- Check VM operation is not blocking
- Increase timeout in client
- Verify VM is responsive

**WebSocket Disconnects:**
- Check network stability
- Implement reconnection logic
- Verify server is not overloaded

**Missing Metrics:**
- Ensure streaming is enabled
- Check StreamingConfig settings
- Verify VM is running

## References

- REST API: [include/RestAPIServer.h](../include/RestAPIServer.h)
- IPC API: [include/IPCAPIServer.h](../include/IPCAPIServer.h)
- WebSocket: [include/WebSocketServer.h](../include/WebSocketServer.h)
- API Types: [include/APITypes.h](../include/APITypes.h)
- VMManager: [include/VMManager.h](../include/VMManager.h)
