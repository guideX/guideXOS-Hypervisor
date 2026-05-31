# Diagnosing Why ISO Doesn't Boot - With Logging

## What I Fixed

Added extensive console logging to `VMManagerWrapper.cs` so you can see:
1. Whether the DLL loads successfully
2. Whether CreateVM uses native or mock
3. Whether StartVM calls the native backend
4. Any errors that occur

## How to Test

### 1. Run the GUI
```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

### 2. Watch the Console Output

**When GUI starts, you should see:**
```
Attempting to load native VMManager DLL...
? Native VMManager DLL loaded successfully!
```

**OR if DLL has issues:**
```
? DLL not found: <error message>
Using mock execution mode
```

### 3. Create a VM with ISO

Click "New" ? Select ISO file

**Console should show:**
```
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON:
{
  "name": "Virtual Machine 1",
  "cpu": {...},
  "storage": [{
    "deviceId": "disk0",
    "imagePath": "C:\\path\\to\\your.iso",
    ...
  }]
}
? VM created with ID: <vm-id>
```

### 4. Start the VM

Click "Start"

**Console should show:**
```
StartVM called for <vm-id>. Mock mode: False
Calling native VMManager_StartVM...
Native StartVM returned: True
```

## Interpreting the Output

### Scenario 1: DLL Not Loading
```
? DLL not found: Unable to load DLL 'guideXOS_Hypervisor.dll'
Using mock execution mode
```

**Problem:** DLL isn't found or can't be loaded
**Causes:**
- DLL not in the same folder as .exe
- Missing dependencies (VCRUNTIME, MSVCP)
- DLL compiled for wrong architecture (x86 vs x64)

**Solution:**
```powershell
# Check DLL exists
ls "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"

# Check dependencies
dumpbin /dependents "x64\Debug\guideXOS_Hypervisor.dll"

# Manually copy DLL
Copy-Item "x64\Debug\guideXOS_Hypervisor.dll" "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\"
```

### Scenario 2: DLL Loads but VMManager_Create Fails
```
? VMManager_Create returned null - using mock execution
```

**Problem:** DLL loaded but couldn't create VMManager instance
**Causes:**
- Exception in C++ constructor
- Memory allocation failure
- Missing Core.lib symbols

**Solution:** Check C++ VMManager constructor for errors

### Scenario 3: DLL Loads, CreateVM Fails
```
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON:
{...}
? VMManager_CreateVM returned null
Returning mock VM ID
```

**Problem:** VM creation in C++ failed
**Causes:**
- JSON parsing error
- Configuration validation failed
- Storage device creation failed

**Solution:** Add logging to C++ `VMManager_CreateVM`

### Scenario 4: Everything Works but Still Shows Gradient
```
? Native VMManager DLL loaded successfully!
? VM created with ID: <id>
StartVM called for <id>. Mock mode: False
Calling native VMManager_StartVM...
Native StartVM returned: True
```

**Problem:** Native backend works but doesn't produce output
**Causes:**
- Bootloader not loaded (C++ startVM bug)
- Framebuffer not initialized
- runVM not executing instructions
- Entry point not set correctly

**Solution:** Check C++ boot loading logic

## Expected Full Output (Success)

```
Attempting to load native VMManager DLL...
? Native VMManager DLL loaded successfully!

[User creates VM]
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON:
{"name":"Virtual Machine 1","cpu":{"cpuCount":1},"storage":[{"deviceId":"disk0","imagePath":"C:\\test.iso"}]}
? VM created with ID: vm-12345

[User starts VM]
StartVM called for vm-12345. Mock mode: False
Calling native VMManager_StartVM...
Native StartVM returned: True

[C++ backend logs - if working]
Loading bootloader from device: disk0
Loading boot sector to address: 0x100000
Bootloader loaded successfully, entry point: 0x100000
```

## Quick Diagnostic Commands

```powershell
# 1. Check DLL exists in GUI output
Test-Path "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"

# 2. Check DLL timestamp (should be recent)
(Get-Item "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll").LastWriteTime

# 3. Check DLL exports
dumpbin /exports "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"

# 4. Check for missing dependencies
dumpbin /dependents "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"
```

## Most Likely Issue

If you're still seeing the gradient, the most likely cause is:

**The DLL loads successfully BUT the C++ VMManager::startVM() isn't actually loading the bootloader.**

This could be because:
1. The boot device isn't being found
2. The readBytes() call is failing
3. The loadProgram() call is failing
4. The entry point isn't being set

**To verify:** Run the GUI and look for C++ log messages. If you don't see "Loading bootloader from device: disk0", then the boot loading code isn't executing.

## Next Steps

1. **Run the GUI** with `dotnet run`
2. **Create VM** with ISO
3. **Copy the console output** and share it
4. This will tell us exactly where the process is failing

The logging I added will pinpoint the exact issue!
