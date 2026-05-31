# Web UI Quick Reference

## Starting the Web UI

```bash
cd build/bin
./webui_example.exe
```

**Access**: http://localhost:8090

## Quick Actions

| Action | How To |
|--------|--------|
| Create VM | Click **Create VM** button |
| Start VM | Click **??** button |
| Stop VM | Click **??** button |
| Pause VM | Click **??** button |
| Resume VM | Click **??** button (paused VMs) |
| View Details | Click on VM row or **??** button |
| Delete VM | Click **???** button, then confirm |
| Refresh List | Click **Refresh** button |
| Export Config | Open VM details ? Configuration tab ? **Export Config** |

## VM Status Indicators

| Badge | Meaning |
|-------|---------|
| ?? Running | VM is executing |
| ? Stopped | VM is not running |
| ?? Paused | VM is suspended |
| ?? Error | VM has an error |

## VM Templates

| Template | Memory | CPUs |
|----------|--------|------|
| Minimal | 128 MB | 1 |
| Standard | 512 MB | 2 |
| Server | 2048 MB | 4 |

## Keyboard Shortcuts

- **ESC**: Close current modal

## Configuration

### Custom Ports
```bash
./webui_example.exe [api_port] [webui_port]

# Example
./webui_example.exe 9000 9001
```

### Disable Sample VMs
```bash
./webui_example.exe 8080 8090 --no-samples
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Page won't load | Check port 8090 is not in use |
| No VMs shown | Click **Refresh** button |
| API errors | Verify API server on port 8080 |
| Port in use | Use different ports |

## API Endpoints

| Operation | Endpoint |
|-----------|----------|
| Health | `GET /api/v1/health` |
| List VMs | `GET /api/v1/vms` |
| Create VM | `POST /api/v1/vms` |
| VM Details | `GET /api/v1/vms/{id}` |
| Start VM | `POST /api/v1/vms/{id}/start` |
| Stop VM | `POST /api/v1/vms/{id}/stop` |
| Delete VM | `DELETE /api/v1/vms/{id}` |

## Browser Requirements

- Chrome 60+
- Firefox 55+
- Safari 11+
- Edge 79+

## Default Ports

- REST API: 8080
- Web UI: 8090

## File Structure

```
web/
??? index.html    # Main page
??? app.js        # Application logic
??? style.css     # Styling
```

## Features

? VM lifecycle management  
? Real-time status updates  
? Configuration viewing  
? Log viewing  
? Responsive design  
? Toast notifications  
? Auto-refresh (3s interval)  
? CORS support  

## See Also

- Full Documentation: `docs/WEB_UI.md`
- API Documentation: `docs/API_DOCUMENTATION.md`
- VM Manager Guide: `docs/VMMANAGER.md`
