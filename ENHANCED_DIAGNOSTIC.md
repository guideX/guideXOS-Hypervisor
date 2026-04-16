# Enhanced Diagnostic - What to Check

## I Added Enhanced Diagnostics

The GUI now:
1. ? Prints the native manager handle
2. ? Saves JSON to `last_vm_config.json` file
3. ? Creates `vm_creation_log.txt` with diagnostic info
4. ? Tells you if C++ messages should appear

## What to Do Now

### 1. Run the GUI
```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

### 2. Try to Create a VM

When you click "New" and select an ISO, watch for these outputs:

#### In Console
```
CreateVM called. Mock mode: False
Native manager handle: 12345678
Calling native VMManager_CreateVM with JSON:
{...full JSON...}
(JSON length: 567 characters)
JSON saved to: last_vm_config.json
```

#### Files Created
- `last_vm_config.json` - The exact JSON sent to C++
- `vm_creation_log.txt` - Diagnostic information

### 3. Check the Files

After getting the error, check:

**last_vm_config.json:**
```powershell
Get-Content "guideXOS Hypervisor GUI\last_vm_config.json"
```

This shows the exact JSON that was sent to C++.

**vm_creation_log.txt:**
```powershell
Get-Content "guideXOS Hypervisor GUI\vm_creation_log.txt"
```

This shows diagnostic information about each creation attempt.

### 4. Check for C++ Output

**Important:** The C++ DLL uses `std::cout` which might not show in the GUI console window.

Try running from PowerShell/CMD directly:
```powershell
cd "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows"
.\guideXOS` Hypervisor` GUI.exe
```

The C++ output should appear in the same window.

## What to Look For

### Scenario 1: Mock Mode = True
```
CreateVM called. Mock mode: True
Using mock VM creation (DLL not loaded)
```

**Problem:** DLL didn't load
**Check:** Did you see "? Native VMManager DLL loaded successfully!" when GUI started?

### Scenario 2: Mock Mode = False, No C++ Output
```
CreateVM called. Mock mode: False
Native manager handle: 12345678
Calling native VMManager_CreateVM with JSON:
{...}
JSON saved to: last_vm_config.json
? VMManager_CreateVM returned null
```

**Problem:** DLL loaded, but C++ function returned null without logging
**Possible causes:**
- C++ output going to different console
- C++ crashing before it can log
- DLL version mismatch (old DLL still being used)

**Check:**
```powershell
# Verify DLL is recent
Get-Item "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll" | Select LastWriteTime

# Should match
Get-Item "x64\Debug\guideXOS_Hypervisor.dll" | Select LastWriteTime
```

### Scenario 3: Mock Mode = False, See C++ Output
```
CreateVM called. Mock mode: False
...
VMManager_CreateVM called
Config JSON: {...}
Parsing JSON configuration...
ERROR: <specific error>
```

**Good!** Now we can see what's wrong in C++.

## Common Issues

### Issue 1: Old DLL Being Used

**Check timestamps:**
```powershell
ls "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll"
ls "x64\Debug\guideXOS_Hypervisor.dll"
```

If timestamps don't match, manually copy:
```powershell
Copy-Item "x64\Debug\guideXOS_Hypervisor.dll" "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\" -Force
```

### Issue 2: C++ Console Not Showing

The `std::cout` from C++ might not appear in the WPF console.

**Solution:** Run from command line:
```powershell
cd "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows"
.\guideXOS` Hypervisor` GUI.exe
```

### Issue 3: DLL Has Debug Symbols Issue

Try rebuilding DLL in Release mode:
```
In VS: Configuration ? Release
Rebuild DLL
Copy to GUI folder
```

## After Running

Please share:
1. **The console output** (all of it)
2. **Contents of last_vm_config.json**
3. **Contents of vm_creation_log.txt**
4. **Whether you saw C++ output** (VMManager_CreateVM called, etc.)

This will tell us exactly what's happening!

## Quick Diagnostic Commands

```powershell
# Check if files were created
Test-Path "guideXOS Hypervisor GUI\last_vm_config.json"
Test-Path "guideXOS Hypervisor GUI\vm_creation_log.txt"

# View them
Get-Content "guideXOS Hypervisor GUI\last_vm_config.json" | Out-Host
Get-Content "guideXOS Hypervisor GUI\vm_creation_log.txt" | Out-Host

# Check DLL timestamps
Get-ChildItem "x64\Debug\guideXOS_Hypervisor.dll", "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll" | Select Name, LastWriteTime
```
