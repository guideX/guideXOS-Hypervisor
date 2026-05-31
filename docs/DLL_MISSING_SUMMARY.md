# Why the DLL is Missing - Summary

## Problem

The C# GUI code (`VMManagerWrapper.cs`) is trying to load `guideXOS_Hypervisor.dll`, but this DLL doesn't exist because:

1. **The C++ project is configured as an Application (EXE)**
   - Location: `guideXOS Hypervisor.vcxproj`
   - Current setting: `<ConfigurationType>Application</ConfigurationType>`
   - This produces: `x64\Debug\guideXOS Hypervisor.exe`
   - GUI expects: `guideXOS_Hypervisor.dll`

## What I Created

1. **`include\VMManager_DLL.h`**
   - DLL export declarations for P/Invoke
   - C-style API wrapper around the C++ VMManager class
   - Functions: Create/Destroy VM, Start/Stop, GetFramebuffer, etc.

2. **`src\api\VMManager_DLL.cpp`**
   - Implementation of DLL exports
   - Converts between C++ objects and C-style handles
   - Exception handling to prevent crashes across DLL boundary

3. **`BUILD_DLL_INSTRUCTIONS.md`**
   - Step-by-step guide to convert the project to a DLL
   - Configuration changes needed
   - Post-build event to copy DLL to GUI directory

## How to Fix (Manual Steps Required)

### Quick Fix - Edit the .vcxproj file:

1. **Close Visual Studio** (important!)

2. **Open `guideXOS Hypervisor.vcxproj` in Notepad or VS Code**

3. **Find and replace** all 4 instances of:
   ```xml
   <ConfigurationType>Application</ConfigurationType>
   ```
   
   With:
   ```xml
   <ConfigurationType>DynamicLibrary</ConfigurationType>
   ```

4. **Add `GUIDEXOS_HYPERVISOR_EXPORTS` to preprocessor definitions**:
   
   Find each `<PreprocessorDefinitions>` line (there are 4):
   ```xml
   <PreprocessorDefinitions>WIN32;_DEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
   ```
   
   Change to (add `GUIDEXOS_HYPERVISOR_EXPORTS;` at the beginning):
   ```xml
   <PreprocessorDefinitions>GUIDEXOS_HYPERVISOR_EXPORTS;WIN32;_DEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
   ```

5. **Add the new source file** to the project:
   
   Find the `<ItemGroup>` section with `<ClCompile Include="...">` items and add:
   ```xml
   <ClCompile Include="src\api\VMManager_DLL.cpp" />
   ```

6. **Save and reopen** Visual Studio

7. **Build the project** (Ctrl+Shift+B)
   - Should now produce: `x64\Debug\guideXOS Hypervisor.dll`

8. **Copy the DLL**:
   ```
   From: x64\Debug\guideXOS Hypervisor.dll
   To: guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\guideXOS_Hypervisor.dll
   ```
   (Note: rename with underscore)

## Current P/Invoke Status

The C# code in `VMManagerWrapper.cs` has:
- ? Method declarations (GetFramebuffer, GetFramebufferDimensions)
- ?? P/Invoke imports commented out (lines ~330-340)
- ?? Currently returns placeholder values

Once DLL is built, uncomment the `[DllImport]` attributes and it will call the real C++ code.

## Files to Review

1. `guideXOS Hypervisor.vcxproj` - Project configuration (needs editing)
2. `include\VMManager_DLL.h` - DLL API declarations (new file I created)
3. `src\api\VMManager_DLL.cpp` - DLL implementations (new file I created)
4. `BUILD_DLL_INSTRUCTIONS.md` - Detailed instructions (guide I created)

## Why This Matters

Without the DLL:
- ? VM screen shows only test pattern (gradient)
- ? Cannot create/start/stop VMs from GUI
- ? P/Invoke calls fail silently

With the DLL:
- ? Real framebuffer rendering from VM memory
- ? Full VM control from GUI
- ? IA-64 ISO boot output visible on screen
