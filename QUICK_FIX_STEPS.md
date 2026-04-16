# Quick Fix Steps - Execute These Commands

## Step 1: Fix Core Library Project

Run this PowerShell script to remove ExcludedFromBuild from all source files:

```powershell
.\fix_core_project.ps1
```

**What it does:** Removes all `ExcludedFromBuild="true"` attributes from the Core project so all source files will be compiled.

## Step 2: Rebuild Core Library

In Visual Studio:
1. Right-click **"guideXOS Hypervisor Core"** project
2. Click **"Clean"**
3. Click **"Rebuild"**

**Verify:** Check that `x64\Debug\guideXOS Hypervisor Core.lib` is now **10-20 MB** (was ~400 KB before)

## Step 3: Rebuild DLL

In Visual Studio:
1. Right-click **"guideXOS Hypervisor DLL"** project
2. Click **"Rebuild"**

**Expected result:** Build should complete successfully with no linker errors!

## Step 4: Verify DLL Was Created

Check that these files exist:
```
x64\Debug\guideXOS_Hypervisor.dll
x64\Debug\guideXOS_Hypervisor.lib
x64\Debug\guideXOS_Hypervisor.exp
```

## Step 5: Verify DLL Was Copied to GUI

Check that the DLL was automatically copied:
```
guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll
```

If not, manually copy it:
```powershell
Copy-Item "x64\Debug\guideXOS_Hypervisor.dll" "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\"
```

## Step 6: Test the GUI

Run the GUI:
```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

**What to look for:**
- Console should NOT show "DLL not found" message
- When you create and start a VM, it should use **real C++ backend**
- Screen will initially show black (no ISO loaded yet), not animated pattern

## If Build Still Fails

### Check Preprocessor Definition

Make sure DLL project has `GUIDEXOS_HYPERVISOR_EXPORTS` defined:

1. Right-click DLL project ? Properties
2. Configuration: All Configurations
3. Platform: x64
4. C/C++ ? Preprocessor ? Preprocessor Definitions
5. Should contain: `GUIDEXOS_HYPERVISOR_EXPORTS;_DEBUG;_CONSOLE`

### Check Library Path

Make sure DLL project can find Core.lib:

1. Right-click DLL project ? Properties
2. Configuration: All Configurations
3. Platform: x64
4. Linker ? General ? Additional Library Directories
5. Should contain: `$(SolutionDir)x64\$(Configuration)`

### Check Dependencies

Make sure DLL project links against Core.lib:

1. Right-click DLL project ? Properties
2. Configuration: All Configurations
3. Platform: x64
4. Linker ? Input ? Additional Dependencies
5. Should contain: `guideXOS Hypervisor Core.lib;%(AdditionalDependencies)`

## Verify Exports (Optional)

To check that the DLL exports the correct functions:

```powershell
dumpbin /exports "x64\Debug\guideXOS_Hypervisor.dll"
```

Should show:
- VMManager_Create
- VMManager_Destroy
- VMManager_CreateVM
- VMManager_StartVM
- VMManager_StopVM
- VMManager_RunVM
- VMManager_GetFramebuffer
- VMManager_GetFramebufferDimensions
- etc.

## Success Criteria

? Core.lib is 10-20 MB
? DLL builds without linker errors
? DLL is copied to GUI output
? GUI starts without "DLL not found" message
? Creating and starting VM works

## Troubleshooting

### "Still getting linker errors"
- Clean entire solution (Build ? Clean Solution)
- Close Visual Studio
- Delete `x64` folder
- Reopen Visual Studio
- Build Core ? Build DLL

### "DLL not found at runtime"
- Make sure DLL is in same folder as GUI exe
- Check file name is exactly `guideXOS_Hypervisor.dll` (underscore, not space)

### "Entry point not found"
- Run `dumpbin /exports` to verify function names
- Check that `GUIDEXOS_HYPERVISOR_EXPORTS` is defined
- Rebuild with clean solution

## Files Modified

I've already modified these files for you:
- ? `src\api\VMManager_DLL.cpp` - Added VMManager_RunVM function
- ? `include\VMManager_DLL.h` - Added VMManager_RunVM declaration
- ? `fix_core_project.ps1` - Script to fix Core project

**Just run the steps above and it should work!**
