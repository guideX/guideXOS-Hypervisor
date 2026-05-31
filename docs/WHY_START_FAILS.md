# Why "Failed to start machine" Happens

## The Problem You're Seeing

**Error:** "Failed to start machine: Virtual machine 1"

This means `VMManagerWrapper.StartVM()` returned `false`, which happens when:
1. The C++ VMManager_StartVM returns false
2. The VM doesn't exist in the C++ backend
3. The VM ID doesn't match between C# and C++

## Root Cause

Based on the code, here's what's likely happening:

### Scenario 1: VM Creation Fails Silently (Most Likely)

```
1. User clicks "New VM"
2. C# calls VMManagerWrapper.CreateVM(config)
3. C++ VMManager_CreateVM is called with JSON
4. C++ creation FAILS (returns null)
5. C# silently falls back to mock GUID
6. C# stores the mock GUID
7. User clicks "Start"
8. C# calls StartVM(mock-guid)
9. C++ doesn't have that VM ? returns false
10. Error: "Failed to start machine"
```

### Why C++ Creation Fails

The C++ `VMManager_CreateVM` could fail for several reasons:

1. **JSON Parsing Error**
   - Config format doesn't match C++ expectations
   - Missing required fields

2. **Storage Device Error**
   - ISO file doesn't exist
   - File path has wrong format (backslashes not escaped)
   - Can't open file (permissions, locked, etc.)

3. **Configuration Validation**
   - Invalid CPU count, memory size, etc.

4. **C++ Exception**
   - Out of memory
   - Internal error

## What I Fixed

### 1. Enhanced Error Logging

**VMManagerWrapper.CreateVM:**
- Now throws exception if native creation fails
- Doesn't fall back to mock in native mode
- Logs detailed error information

**MainViewModel.OnStartVM:**
- Shows which step failed
- Provides troubleshooting hints

### 2. Better Error Messages

Now when creation fails, you'll see:
```
? VMManager_CreateVM returned null
ERROR: Native VM creation failed!
This could be due to:
  - Invalid configuration
  - Storage device file not found
  - Memory allocation failure
  - C++ exception during creation
```

## How to Diagnose

### Run the GUI Again

```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

### Create a VM and Watch Console

When you create a VM, you should now see:

**Success:**
```
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON: {...}
? VM created with ID: vm-12345
```

**Failure (what you're probably seeing):**
```
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON: {...}
? VMManager_CreateVM returned null
ERROR: Native VM creation failed!
[Error dialog pops up]
```

## Most Likely Issues

### Issue 1: Storage Device Path

The JSON includes the ISO path with backslashes:
```json
"imagePath": "C:\path\to\file.iso"
```

But JSON requires escaped backslashes:
```json
"imagePath": "C:\\path\\to\\file.iso"
```

**Check:** Look at the JSON printed in console. Are backslashes escaped?

### Issue 2: ISO File Doesn't Exist

C++ tries to open the file and fails.

**Check:** 
```powershell
Test-Path "C:\path\to\your.iso"
```

### Issue 3: RawDiskDevice::connect() Fails

The C++ code creates a RawDiskDevice and calls `connect()`. If this fails, VM creation fails.

**Check C++ Code:**
```cpp
// In VMManager::createStorageDevices
device = std::make_unique<RawDiskDevice>(...);
if (!device->connect()) {  // <-- This might be failing
    LOG_ERROR("Failed to connect storage device");
    return false;
}
```

## Quick Fixes to Try

### Fix 1: Check JSON Output

Run the GUI, create a VM, and copy the JSON from console. Check:
- Are paths escaped properly?
- Does the ISO file actually exist?

### Fix 2: Test with No Storage

Try creating a VM without selecting an ISO (just cancel the file dialog). If this works, the problem is storage-related.

### Fix 3: Add C++ Logging

Add logging to `src\vm\VMManager.cpp` in the `createStorageDevices` function:

```cpp
bool VMManager::createStorageDevices(...) {
    LOG_INFO("Creating storage devices...");
    for (const auto& config : configs) {
        LOG_INFO("  Device: " + config.deviceId + " Path: " + config.imagePath);
        
        auto device = std::make_unique<RawDiskDevice>(...);
        
        if (!device->connect()) {
            LOG_ERROR("Failed to connect device: " + config.imagePath);
            // ADD MORE DETAILS HERE
            return false;
        }
    }
    return true;
}
```

## Expected Console Output (Working)

```
Attempting to load native VMManager DLL...
? Native VMManager DLL loaded successfully!

[User creates VM]
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON:
{
  "name": "Virtual Machine 1",
  "cpu": {"cpuCount": 1, "isaType": "IA-64"},
  "storage": [{
    "deviceId": "disk0",
    "imagePath": "C:\\Users\\...\\test.iso",
    "readOnly": true
  }]
}
? VM created with ID: vm-abc123

[User starts VM]
OnStartVM: Attempting to start VM vm-abc123
StartVM called for vm-abc123. Mock mode: False
Calling native VMManager_StartVM...
Native StartVM returned: True
OnStartVM: StartVM returned True
```

## Next Steps

1. **Run the GUI** - `dotnet run`
2. **Try to create a VM**
3. **Copy the entire console output** (especially the JSON and any error messages)
4. **Share it** - This will show exactly where it's failing

The enhanced logging will pinpoint the exact failure point!
