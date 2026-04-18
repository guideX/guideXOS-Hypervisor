# SIMPLE FIX - Use Visual Studio Directly

## The Issue

The build system is getting confused with macro definitions and cached builds. The simplest solution is to use Visual Studio's UI directly.

## Step-by-Step Fix

### 1. Open Visual Studio
Double-click: `guideXOS Hypervisor.sln`

### 2. Clean Everything
- Menu: **Build ? Clean Solution**
- Wait for it to finish

### 3. Close Visual Studio Completely
-File ? Exit (or Alt+F4)

### 4. Delete Build Artifacts
Run in PowerShell:
```powershell
Remove-Item "x64" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item ".vs" -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -Recurse -Filter "*.obj" | Remove-Item -Force
Get-ChildItem -Recurse -Filter "*.pdb" | Remove-Item -Force
```

### 5. Reopen Visual Studio
Double-click: `guideXOS Hypervisor.sln`

### 6. Set Startup Project
- Right-click **"guideXOS Hypervisor DLL"** in Solution Explorer
- **Set as Startup Project**

### 7. Rebuild DLL Only
- Right-click **"guideXOS Hypervisor DLL"**
- **Rebuild**

### 8. Check Output
Look for:
```
Build started...
1>------ Rebuild All started: Project: guideXOS Hypervisor Core, Configuration: Debug x64 ------
1>  ISO9660Parser.cpp
1>  ...
1>  guideXOS Hypervisor Core.lib created
2>------ Rebuild All started: Project: guideXOS Hypervisor DLL, Configuration: Debug x64 ------
2>  VMManager_DLL.cpp
2>  ...
2>  guideXOS_Hypervisor.dll created
========== Rebuild All: 2 succeeded, 0 failed, 0 skipped ==========
```

## If Still Failing

The macro might not be getting defined from the project settings. You can force it in the code.

**Edit `src\api\VMManager_DLL.cpp` line 2-4:**

Change:
```cpp
#ifndef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_HYPERVISOR_EXPORTS
#endif
```

To:
```cpp
// Force definition for DLL builds
#undef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_HYPERVISOR_EXPORTS
```

This FORCES the macro to be defined regardless of project settings.

## Alternative: Build Just What We Need

If the full solution won't build, we only need:
1. **Core library** (with ISO parser)
2. **DLL project** (with exports)

You don't need the tests or other projects.

In Visual Studio:
- Right-click **"guideXOS Hypervisor Core"** ? **Build**
- Right-click **"guideXOS Hypervisor DLL"** ? **Build**

## Then Test

Once the DLL builds successfully:
```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

Create VM ? Start ? See ISO parser output!

## Why This Is So Difficult

The GUIDEXOS_HYPERVISOR_EXPORTS macro issue is a classic Windows DLL export problem:
- When BUILDING the DLL ? Need `__declspec(dllexport)`
- When USING the DLL ? Need `__declspec(dllimport)`

The macro switches between them. But Visual Studio's build cache can get confused about which state files are in.

**Clean rebuild is the only reliable fix.**
