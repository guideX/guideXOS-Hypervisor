# Fix DLL Export Errors - Quick Solution

## The Error You're Seeing

```
C2491: 'VMManager_Create': definition of dllimport function not allowed
```

This means the compiler thinks these functions should be **imported** (dllimport) instead of **exported** (dllexport).

## Root Cause

The `GUIDEXOS_HYPERVISOR_EXPORTS` preprocessor definition is not being applied when compiling the DLL project.

## Solutions (Try These in Order)

### Solution 1: Verify You're Building the Correct Project

**Are you building the DLL project or the entire solution?**

The error messages suggest you might be building the **main EXE project** which includes VMManager_DLL.cpp but doesn't have the EXPORTS definition.

**Fix:**
1. In Visual Studio, right-click **"guideXOS Hypervisor DLL"** project (not the main project)
2. Select **"Build"** or **"Rebuild"**
3. Do NOT build the entire solution (the EXE will fail)

### Solution 2: Clean and Rebuild DLL Project

```
1. Right-click "guideXOS Hypervisor DLL" project
2. Select "Clean"
3. Wait for clean to complete
4. Right-click again ? "Rebuild"
```

### Solution 3: Verify Configuration

Make sure you're building **Debug | x64**:

1. Top toolbar in Visual Studio
2. Configuration dropdown should be: **Debug**
3. Platform dropdown should be: **x64**

### Solution 4: Force Preprocessor Definition in Code

If the above don't work, add this to the **top** of `VMManager_DLL.cpp`:

```cpp
// Force export when building DLL
#ifndef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_HYPERVISOR_EXPORTS
#endif

#include "VMManager_DLL.h"
// ... rest of code
```

### Solution 5: Check Project File Directly

Verify the DLL project has the definition:

1. Open `guideXOS Hypervisor DLL.vcxproj` in text editor
2. Find `<PreprocessorDefinitions>` for Debug|x64
3. Should contain: `GUIDEXOS_HYPERVISOR_EXPORTS;_DEBUG;_CONSOLE;%(PreprocessorDefinitions)`
4. If missing, add it manually and save

### Solution 6: Exclude VMManager_DLL.cpp from Main Project

The main EXE project might be trying to compile VMManager_DLL.cpp. Check if it's included:

1. In main "guideXOS Hypervisor" project
2. Expand src\api folder
3. Right-click `VMManager_DLL.cpp`
4. Properties ? Excluded From Build ? **Yes**

## Quick PowerShell Fix

Run this to force the build of just the DLL:

```powershell
# Build DLL only (not entire solution)
MSBuild "guideXOS Hypervisor DLL.vcxproj" /p:Configuration=Debug /p:Platform=x64 /t:Rebuild
```

## Understanding the Error

### What GUIDEXOS_API Does

In `VMManager_DLL.h`:
```cpp
#ifdef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_API __declspec(dllexport)  // Export from DLL
#else
#define GUIDEXOS_API __declspec(dllimport)  // Import in client code
#endif
```

When building the **DLL project**: `GUIDEXOS_HYPERVISOR_EXPORTS` is defined ? functions are exported
When building **client code** (C#): Not defined ? functions are imported

### What's Going Wrong

The compiler isn't seeing `GUIDEXOS_HYPERVISOR_EXPORTS` defined, so it treats all functions as imports, which is illegal in the implementation file.

## Verify Success

After fixing, you should see:
```
Build succeeded
Created: x64\Debug\guideXOS_Hypervisor.dll
```

Check the output window for this line to confirm DLL was built.

## If Still Failing

Run this diagnostic:

```powershell
# Check which project is being built
Get-Content "guideXOS Hypervisor DLL.vcxproj" | Select-String "GUIDEXOS_HYPERVISOR_EXPORTS"
```

Should show the line with the preprocessor definition.

Then try building from command line:
```cmd
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "guideXOS Hypervisor DLL.vcxproj" /p:Configuration=Debug /p:Platform=x64 /t:Rebuild /v:detailed > build.log
```

Check `build.log` for which preprocessor definitions are actually being used.

## Most Likely Solution

**Just build the DLL project directly, not the whole solution:**

1. In Solution Explorer
2. Right-click **"guideXOS Hypervisor DLL"** (the specific project)
3. Click **"Rebuild"**

That should work!
