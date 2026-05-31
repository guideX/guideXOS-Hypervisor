# Manual Configuration Steps Required

The project files have been created, but you need to manually configure the EXE project to link against the Core library.

## Files Created

? `guideXOS Hypervisor Core.vcxproj` - Static library with all shared code
? `guideXOS Hypervisor DLL.vcxproj` - DLL for GUI integration  
? `src/api/dllmain.cpp` - DLL entry point

## Step 1: Add Projects to Solution

1. **Open your solution** in Visual Studio
2. **Right-click Solution** ? Add ? Existing Project
3. **Add**: `guideXOS Hypervisor Core.vcxproj`
4. **Add**: `guideXOS Hypervisor DLL.vcxproj`

## Step 2: Configure EXE Project Dependencies

1. **Right-click** `guideXOS Hypervisor` project ? Properties
2. **Configuration**: All Configurations
3. **Platform**: x64

### Linker Settings:

**General ? Additional Library Directories:**
```
$(SolutionDir)x64\$(Configuration);%(AdditionalLibraryDirectories)
```

**Input ? Additional Dependencies:**
```
guideXOS Hypervisor Core.lib;%(AdditionalDependencies)
```

### Exclude Source Files (Already in Core):

All `src\*.cpp` files should be **excluded from build** in the EXE project:
- Right-click each `src\*.cpp` file ? Properties
- Set: Excluded From Build = Yes

**KEEP these files in EXE project** (not excluded):
- ? `debug_harness.cpp`
- ? `examples\*.cpp`
- ? `tests\*.cpp`
- ? `kernels\minimal_kernel.c`

## Step 3: Set Build Order

1. Right-click Solution ? Project Dependencies
2. **guideXOS Hypervisor** depends on: **guideXOS Hypervisor Core**
3. **guideXOS Hypervisor DLL** depends on: **guideXOS Hypervisor Core**

## Step 4: Verify Post-Build Event (DLL)

The DLL project already has a post-build event configured:
```cmd
copy "$(TargetPath)" "$(SolutionDir)guideXOS Hypervisor GUI\bin\$(Configuration)\net9.0-windows\guideXOS_Hypervisor.dll"
```

This automatically copies the DLL to the C# GUI output directory.

## Step 5: Build Order

1. **Build Core** (Static Library)
   - Configuration: Debug or Release
   - Platform: x64
   - Output: `x64\Debug\guideXOS Hypervisor Core.lib`

2. **Build EXE** (Development Tools)
   - Links against Core.lib
   - Output: `x64\Debug\guideXOS Hypervisor.exe`

3. **Build DLL** (GUI Integration)
   - Links against Core.lib
   - Output: `x64\Debug\guideXOS_Hypervisor.dll`
   - Auto-copied to GUI

4. **Build GUI** (C# WPF)
   - Loads: `guideXOS_Hypervisor.dll`

## Step 6: Test Each Project

### Test Core Library:
```
Build ? Should succeed with no errors
Output: x64\Debug\guideXOS Hypervisor Core.lib (should exist)
```

### Test EXE (Console Tools):
```cmd
cd x64\Debug
"guideXOS Hypervisor.exe" --debug --cycles 10
```
Should run without errors.

### Test DLL:
```
Build ? Should succeed
Check: guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll exists
Use dumpbin to verify exports:
  dumpbin /exports "guideXOS_Hypervisor.dll"
Should see: VMManager_Create, VMManager_GetFramebuffer, etc.
```

### Test GUI:
```
Run: guideXOS Hypervisor GUI.exe
Should start without DLL load errors
```

## Alternative: Automated Exclusion Script

If you want to exclude all `src\*.cpp` files from the EXE project automatically, you can use this PowerShell script:

```powershell
# exclude_src_from_exe.ps1
$projectFile = "guideXOS Hypervisor.vcxproj"
$xml = [xml](Get-Content $projectFile)

$namespace = @{ns="http://schemas.microsoft.com/developer/msbuild/2003"}
$srcFiles = $xml.SelectNodes("//ns:ClCompile[starts-with(@Include, 'src\')]", $namespace)

foreach ($file in $srcFiles) {
    if ($file.GetAttribute("ExcludedFromBuild") -eq "") {
        $file.SetAttribute("ExcludedFromBuild", "true")
    }
}

$xml.Save((Resolve-Path $projectFile))
Write-Host "Excluded $($srcFiles.Count) source files from EXE project"
```

## Project Structure After Setup

```
Solution:
??? guideXOS Hypervisor Core.lib       (Static Library)
?   ??? All src\*.cpp files
?   ??? All include\*.h files
?
??? guideXOS Hypervisor.exe            (Console Application)
?   ??? debug_harness.cpp
?   ??? examples\*.cpp
?   ??? tests\*.cpp
?   ??? kernels\minimal_kernel.c
?   ??? Links to: Core.lib
?
??? guideXOS_Hypervisor.dll            (DLL for GUI)
?   ??? src\api\dllmain.cpp
?   ??? src\api\VMManager_DLL.cpp
?   ??? Links to: Core.lib
?   ??? Auto-copies to GUI output
?
??? guideXOS Hypervisor GUI.exe        (WPF Application)
    ??? Loads: guideXOS_Hypervisor.dll
```

## Troubleshooting

### "unresolved external symbol" errors in EXE:
- Make sure Core.lib is in Additional Dependencies
- Make sure Library Directories includes `$(SolutionDir)x64\$(Configuration)`
- Verify Core.lib was built successfully

### "multiply defined symbols" errors:
- Source files are in both EXE and Core projects
- Exclude them from EXE project (keep only in Core)

### DLL not found when running GUI:
- Check post-build event ran successfully
- Manually copy DLL if needed
- Verify DLL is in same directory as GUI exe

### Entry point not found in DLL:
- Check exports with `dumpbin /exports`
- Make sure `GUIDEXOS_HYPERVISOR_EXPORTS` is defined in DLL project
- Rebuild DLL with clean solution

## Next Steps After Configuration

Once all projects build successfully:

1. **Uncomment P/Invoke declarations** in `VMManagerWrapper.cs`
2. **Implement actual calls** to DLL functions (replace placeholder returns)
3. **Test framebuffer** - Should show real VM output instead of gradient
4. **Test VM creation** - Should create actual VMs in backend

## Ready to Build!

After following these steps, you should have:
- ? Working console tools (EXE)
- ? Working GUI integration (DLL)
- ? Shared core code (no duplication)
- ? Clean separation of concerns
