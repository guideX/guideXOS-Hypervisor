# Web-Based VM Management UI

## Overview

The IA-64 Hypervisor includes a modern web-based management interface for controlling and monitoring virtual machines. The Web UI provides an intuitive dashboard for managing VM lifecycle, viewing configurations, and monitoring status in real-time.

## Features

- **VM Dashboard**: List all virtual machines with status indicators
- **VM Lifecycle Control**: Start, stop, pause, and resume VMs with a single click
- **VM Creation**: Create new VMs with configurable memory and CPU settings
- **VM Details**: View detailed information, configurations, and logs
- **Real-time Updates**: Auto-refresh to show current VM status
- **Configuration Export**: Export VM configurations as JSON
- **Responsive Design**: Works on desktop and mobile devices

## Quick Start

### 1. Build the Project

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### 2. Run the Web UI Server

```bash
cd bin
./webui_example.exe
```

**Default ports:**
- REST API Server: `http://localhost:8080`
- Web UI: `http://localhost:8090`

### 3. Open the Web Interface

Open your web browser and navigate to:
```
http://localhost:8090
```

### 4. Custom Ports

You can specify custom ports:

```bash
./webui_example.exe 9000 9001
```

This will start:
- API server on port 9000
- Web UI on port 9001

## User Interface Guide

### Main Dashboard

The main dashboard displays all virtual machines in a table format:

| Column | Description |
|--------|-------------|
| **Name** | VM display name |
| **VM ID** | Unique identifier (abbreviated) |
| **Status** | Current state with color-coded badge |
| **CPUs** | Number of virtual CPUs |
| **Memory** | Allocated memory size |
| **Uptime** | Time since VM started |
| **Actions** | Control buttons |

### Status Indicators

VMs can be in one of the following states:

- ?? **Running** - VM is actively executing
- ? **Stopped** - VM is not running
- ?? **Paused** - VM execution is suspended
- ?? **Error** - VM encountered an error

### VM Actions

#### Quick Actions (from table)

- **?? Start**: Start a stopped VM
- **?? Pause**: Pause a running VM
- **?? Resume**: Resume a paused VM
- **?? Stop**: Stop a running or paused VM
- **?? Info**: View detailed VM information
- **??? Delete**: Remove VM (with confirmation)

#### Create New VM

Click the **Create VM** button to open the creation dialog:

1. Enter a unique VM name
2. Configure memory (MB) and CPU count
3. Optionally select a template:
   - **Minimal**: 128MB RAM, 1 CPU
   - **Standard**: 512MB RAM, 2 CPUs
   - **Server**: 2048MB RAM, 4 CPUs
4. Choose whether to auto-start the VM
5. Click **Create VM**

### VM Details Modal

Click on any VM row or the **??** button to open detailed information:

#### Overview Tab

Displays key VM metrics:
- VM ID (full UUID)
- Current state
- Memory allocation
- CPU count
- Total instructions executed
- Uptime

#### Configuration Tab

Shows the complete VM configuration in JSON format. You can:
- View all configuration settings
- Export configuration to a file

#### Logs Tab

Displays VM execution logs and events. You can:
- View recent log entries
- Refresh logs manually
- Clear the log view

## Features in Detail

### Auto-Refresh

The dashboard automatically refreshes VM status every 3 seconds. You can:
- Toggle auto-refresh on/off with the checkbox
- Manually refresh using the **Refresh** button

### Toast Notifications

The UI displays temporary notifications for:
- ? Successful operations (green)
- ? Errors (red)
- ?? Information messages (blue)
- ?? Warnings (yellow)

### Keyboard Shortcuts

- **Escape**: Close any open modal
- Click outside modal to close

### Responsive Design

The UI adapts to different screen sizes:
- Desktop: Full table layout
- Tablet: Adjusted columns
- Mobile: Stacked layout

## REST API Integration

The Web UI communicates with the REST API server. All operations are performed via HTTP requests:

| Operation | Method | Endpoint |
|-----------|--------|----------|
| List VMs | GET | `/api/v1/vms` |
| Create VM | POST | `/api/v1/vms` |
| Get VM Details | GET | `/api/v1/vms/{id}` |
| Start VM | POST | `/api/v1/vms/{id}/start` |
| Stop VM | POST | `/api/v1/vms/{id}/stop` |
| Pause VM | POST | `/api/v1/vms/{id}/pause` |
| Resume VM | POST | `/api/v1/vms/{id}/resume` |
| Delete VM | DELETE | `/api/v1/vms/{id}` |
| Get Logs | GET | `/api/v1/vms/{id}/logs` |

## Configuration

### Custom Web Root

To use a different web files directory:

```cpp
WebUIServer webServer(8090, "/path/to/web/files");
```

### CORS Configuration

Enable/disable cross-origin requests:

```cpp
webServer.setCORSEnabled(true);  // Enable CORS
webServer.setAPIUrl("http://localhost:8080");  // Set API URL
```

### Sample VMs

The example launcher creates three sample VMs by default:
- minimal-vm (128MB, 1 CPU)
- standard-vm (512MB, 2 CPUs)
- server-vm (2048MB, 4 CPUs)

To start without sample VMs:

```bash
./webui_example.exe 8080 8090 --no-samples
```

## Troubleshooting

### Web UI Won't Load

**Problem**: Browser shows "Cannot connect" or "Page not found"

**Solutions**:
1. Verify the Web UI server is running
2. Check the port is correct (default: 8090)
3. Ensure no firewall is blocking the connection
4. Check server logs for errors

### API Requests Fail

**Problem**: Web UI shows "Failed to connect to server"

**Solutions**:
1. Verify the REST API server is running
2. Check the API port is correct (default: 8080)
3. Verify CORS is enabled if using different origins
4. Check browser console for detailed error messages

### VMs Don't Appear

**Problem**: Dashboard shows "No virtual machines found"

**Solutions**:
1. Click the **Refresh** button
2. Check auto-refresh is enabled
3. Verify VMs exist via API: `curl http://localhost:8080/api/v1/vms`
4. Check API server logs

### Port Already in Use

**Problem**: Server fails to start with "Bind failed" error

**Solutions**:
1. Choose different port numbers
2. Stop other applications using the ports
3. Use command: `netstat -ano | findstr :8090` to find process using port

### Web Files Not Found

**Problem**: Server returns "404 Not Found" for all pages

**Solutions**:
1. Ensure web files exist in the `web/` directory
2. Check the web root path is correct
3. Verify files were copied during build
4. Check file permissions

## Security Considerations

### Local Access Only

The default configuration binds to `localhost` only, preventing external access. For production use:

1. Implement authentication
2. Use HTTPS/TLS encryption
3. Configure firewall rules
4. Limit API access

### CORS Policy

CORS is enabled by default for development. In production:
- Restrict allowed origins
- Validate all API requests
- Implement rate limiting

### Input Validation

The Web UI validates all user inputs, but always validate on the server side as well.

## Browser Compatibility

The Web UI is compatible with:
- ? Chrome/Edge (recommended)
- ? Firefox
- ? Safari
- ? Opera

Minimum versions:
- Chrome 60+
- Firefox 55+
- Safari 11+
- Edge 79+

## Development

### File Structure

```
web/
??? index.html      # Main HTML page
??? app.js          # JavaScript application logic
??? style.css       # CSS styles
```

### Customization

You can customize the UI by modifying:
- `style.css`: Colors, fonts, layout
- `app.js`: Behavior, API endpoints, refresh intervals
- `index.html`: Structure, new features

### Adding Features

To add new features:
1. Update HTML structure in `index.html`
2. Add event handlers in `app.js`
3. Style new elements in `style.css`
4. Ensure API endpoints exist

## Performance

### Optimization Tips

1. **Reduce refresh interval** for many VMs
2. **Disable auto-refresh** when not actively monitoring
3. **Close detail modals** when not in use
4. **Limit log output** to recent entries

### Scalability

The Web UI is designed for:
- Up to 100 VMs (optimal)
- Up to 500 VMs (acceptable)
- Beyond 500: Consider pagination

## Examples

### Basic Usage

```cpp
#include "VMManager.h"
#include "RestAPIServer.h"
#include "WebUIServer.h"

int main() {
    VMManager vmManager;
    RestAPIServer apiServer(vmManager, 8080);
    WebUIServer webServer(8090, "./web");
    
    apiServer.start();
    webServer.start();
    
    // Run until interrupted...
    
    webServer.stop();
    apiServer.stop();
    return 0;
}
```

### Custom Configuration

```cpp
// Custom ports
RestAPIServer apiServer(vmManager, 9000);
WebUIServer webServer(9001, "./custom-web");

// Configure CORS
webServer.setCORSEnabled(true);
webServer.setAPIUrl("http://localhost:9000");

// Start servers
apiServer.start();
webServer.start();
```

## Additional Resources

- **API Documentation**: See `docs/API_DOCUMENTATION.md`
- **VM Manager Guide**: See `docs/VMMANAGER.md`
- **Configuration Schema**: See `docs/VM_CONFIGURATION_SCHEMA.md`
- **Example Configurations**: See `examples/configs/`

## Support

For issues or questions:
1. Check the troubleshooting section
2. Review the API documentation
3. Check server logs for error messages
4. Consult the project documentation

## License

Part of the IA-64 Hypervisor project.
