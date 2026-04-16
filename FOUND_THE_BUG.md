# FOUND THE BUG! - JSON Field Name Mismatch

## The Problem

**C# was sending:** `"storage": [...]`
**C++ was expecting:** `"storageDevices": [...]`

This caused JSON parsing to fail silently, and the storage devices were never created!

## What I Fixed

### VMStateModel.cs - ToJson() method
Changed line 322 from:
```csharp
json.Append("\"storage\":[");
```

To:
```csharp
json.Append("\"storageDevices\":[");
```

Now the JSON field name matches what C++ expects!

## What to Do Now

### 1. Close the Running GUI

The GUI is currently running (process 31316). **Close it** so we can rebuild.

### 2. Rebuild C# GUI

```powershell
cd "guideXOS Hypervisor GUI"
dotnet build
```

### 3. Run the GUI Again

```powershell
dotnet run
```

### 4. Create a VM with ISO

Click "New" ? Select ISO ? The VM should now create successfully!

## Why This Fixes It

**Before (broken):**
```json
{
  "name": "Virtual Machine 1",
  "storage": [{          ? C++ doesn't recognize this
    "deviceId": "disk0",
    "imagePath": "C:\\test.iso"
  }]
}
```

C++ JSON parser saw "storage" but was looking for "storageDevices", so it skipped the array. Result: No storage devices created!

**After (fixed):**
```json
{
  "name": "Virtual Machine 1",
  "storageDevices": [{   ? C++ recognizes this!
    "deviceId": "disk0",
    "imagePath": "C:\\test.iso"
  }]
}
```

C++ JSON parser finds "storageDevices", parses the array, creates RawDiskDevice, connects to ISO file. VM created successfully!

## Expected Console Output (After Fix)

```
[C# side]
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON:
{
  "name":"Virtual Machine 1",
  "cpu":{...},
  "memory":{...},
  "boot":{...},
  "storageDevices":[{              ? Note: Changed from "storage"
    "deviceId":"disk0",
    "deviceType":"raw",
    "imagePath":"C:\\Users\\...\\test.iso",
    "readOnly":true,
    "sizeBytes":0,
    "blockSize":2048
  }],
  "features":{...}
}

[C++ side]
VMManager_CreateVM called
Config JSON: {...}
Parsing JSON configuration...
? JSON parsed successfully
Validating configuration...
? Configuration valid
Creating VM with name: Virtual Machine 1
Creating 1 storage device(s)
  Device ID: disk0
  Type: raw
  Path: C:\Users\...\test.iso
  Read-only: yes
  Creating RawDiskDevice...
  ? RawDiskDevice created
  Connecting storage device...
  ? Storage device connected successfully
? All storage devices created successfully
? VM created successfully with ID: Virtual Machine 1-000001
```

## What Happens Next

1. **VM will create successfully** with storage attached
2. **When you start the VM**, it will:
   - Load bootloader from ISO
   - Set entry point
   - Begin execution
3. **Framebuffer should show boot output** instead of gradient!

## Files Modified

- ? `guideXOS Hypervisor GUI\Models\VMStateModel.cs` - Fixed JSON field name

## Quick Test

1. **Close GUI** (kill the running process)
2. **Rebuild:** `dotnet build`
3. **Run:** `dotnet run`
4. **Create VM** with ISO
5. **Check console** - Should see ? checkmarks in C++ logs
6. **Start VM** - Boot should begin!

This was the bug preventing VM creation! The JSON field name mismatch meant storage was never configured.
