# Quick Fix - "Failed to start machine"

## What I Changed

**VMManagerWrapper.CreateVM** now:
- ? Throws exception if native creation fails
- ? Doesn't return mock GUID when in native mode
- ? Shows detailed error messages

**MainViewModel.OnStartVM** now:
- ? Logs every step
- ? Shows helpful troubleshooting hints

## What to Do Now

### 1. Run the GUI
```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

### 2. Watch for TWO Things

**When GUI starts:**
```
? Native VMManager DLL loaded successfully!
```
If you see this ? DLL is loaded ?

**When you create a VM:**
Either:
```
? VM created with ID: vm-12345
```
Success! VM exists in C++ ?

OR:
```
? VMManager_CreateVM returned null
ERROR: Native VM creation failed!
```
This is the problem! ?

### 3. If Creation Fails

The error dialog will now pop up immediately when you click "New" (not when you click "Start").

**Look at the console for:**
- The JSON that was sent
- Error messages about what failed

**Common causes:**
- ISO file path is wrong
- ISO file doesn't exist
- Storage device can't be opened
- C++ has an internal error

## What Changed vs Before

**Before:**
- CreateVM fails silently
- Returns mock GUID
- Start fails with "Failed to start machine"

**After:**
- CreateVM fails immediately
- Shows error dialog right away
- Console shows detailed error

## Expected Flow (Working)

```
[Start GUI]
? Native VMManager DLL loaded successfully!

[Click New ? Select ISO]
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON: {...}
? VM created with ID: vm-abc123
[VM appears in list]

[Click Start]
OnStartVM: Attempting to start VM vm-abc123
StartVM called for vm-abc123. Mock mode: False
Calling native VMManager_StartVM...
Native StartVM returned: True
[VM runs, screen shows output]
```

## Expected Flow (Failing)

```
[Start GUI]
? Native VMManager DLL loaded successfully!

[Click New ? Select ISO]
CreateVM called. Mock mode: False
Calling native VMManager_CreateVM with JSON: {...}
? VMManager_CreateVM returned null
ERROR: Native VM creation failed!
[Error dialog pops up NOW - not when you click Start]
```

## Test It

Run the GUI and try to create a VM. If the creation fails, you'll see the error immediately with details about why.

**Copy the console output** and share it - this will tell us exactly what's wrong in the C++ code.
