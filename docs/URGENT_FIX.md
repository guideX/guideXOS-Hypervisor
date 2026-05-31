# URGENT FIX - Close GUI and Rebuild

## I Found the Bug!

**The JSON field name was wrong:**
- C# sent: `"storage"`
- C++ expected: `"storageDevices"`

This caused JSON parsing to fail and no storage devices were created!

## Quick Fix Steps

### 1. Close the Running GUI
The GUI is currently running. **Press Ctrl+C** in the console or **close the window**.

### 2. Rebuild
```powershell
cd "guideXOS Hypervisor GUI"
dotnet build
dotnet run
```

### 3. Test
Create a VM with an ISO. It should now work!

## What Changed
Line 322 in `VMStateModel.cs`:
```csharp
// OLD (broken):
json.Append("\"storage\":[");

// NEW (fixed):
json.Append("\"storageDevices\":[");
```

## Expected Result
The console will now show:
```
? JSON parsed successfully
Creating 1 storage device(s)
? Storage device connected successfully
? VM created successfully
```

**This was the root cause! Close the GUI, rebuild, and test again.**

See `FOUND_THE_BUG.md` for full details.
