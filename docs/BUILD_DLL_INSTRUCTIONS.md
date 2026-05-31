# Building guideXOS Hypervisor as a DLL

The C++ hypervisor project is currently configured as a console application (`.exe`). To use it from the C# GUI via P/Invoke, you need to build it as a DLL.

## Current Status

- **Project Type**: Application (builds `guideXOS Hypervisor.exe`)
- **C# GUI Expects**: `guideXOS_Hypervisor.dll`
- **DLL Export Code**: Created in `src/api/VMManager_DLL.cpp`

## Option 1: Convert Existing Project to DLL (Quick Solution)

### Steps:

1. **Open `guideXOS Hypervisor.vcxproj` in a text editor**

2. **Change `ConfigurationType` for all configurations**:

   Find these lines (around line 31, 37, 45, 51):
   ```xml
   <ConfigurationType>Application</ConfigurationType>
   ```
   
   Change to:
   ```xml
   <ConfigurationType>DynamicLibrary</ConfigurationType>
   ```

3. **Add preprocessor definition**:

   Find the `<PreprocessorDefinitions>` sections for each configuration and add `GUIDEXOS_HYPERVISOR_EXPORTS;`:
   
   Example for Debug|x64:
   ```xml
   <PreprocessorDefinitions>GUIDEXOS_HYPERVISOR_EXPORTS;_DEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
   ```

4. **Exclude `main.cpp` and example files** (they're already excluded, but verify):
   - `main.cpp` should have `ExcludedFromBuild="true"`
   - `debug_harness.cpp` should remain included or be replaced with a DLL entry point

5. **Add the new DLL export files to the project**:
   - Right-click project in Solution Explorer ? Add ? Existing Item
   - Add `src\api\VMManager_DLL.cpp`
   - Add `include\VMManager_DLL.h`

6. **Build the project**:
   ```
   Configuration: Debug or Release
   Platform: x64
   ```

7. **Output will be**:
   - `x64\Debug\guideXOS Hypervisor.dll`
   - `x64\Release\guideXOS Hypervisor.dll`

8. **Copy the DLL to the C# GUI output directory**:
   ```
   Copy to: guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\
   Rename to: guideXOS_Hypervisor.dll (underscore instead of space)
   ```

## Option 2: Create Separate DLL Project (Recommended for Production)

Create a new C++ DLL project in the solution:

1. File ? New ? Project ? "Dynamic-Link Library (DLL)"
2. Name: `guideXOS Hypervisor DLL`
3. Add reference to existing hypervisor code
4. Include only `VMManager_DLL.cpp` and necessary source files
5. Link against a static library built from the main project

## Post-Build Event (Automatic Copy)

Add this to the project's post-build event to automatically copy the DLL:

1. Right-click project ? Properties
2. Build Events ? Post-Build Event ? Command Line:

```cmd
copy "$(TargetDir)$(TargetFileName)" "$(SolutionDir)guideXOS Hypervisor GUI\bin\$(Configuration)\net9.0-windows\guideXOS_Hypervisor.dll"
```

## Updating C# P/Invoke Declarations

Once the DLL is built, update `VMManagerWrapper.cs`:

```csharp
// Uncomment and update these declarations:

[DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern IntPtr VMManager_Create();

[DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern void VMManager_Destroy(IntPtr manager);

[DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern IntPtr VMManager_CreateVM(
    IntPtr manager, 
    string name, 
    ulong memorySize, 
    int cpuCount);

[DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern bool VMManager_GetFramebuffer(
    IntPtr manager, 
    string vmId, 
    byte[] buffer, 
    int bufferSize,
    out int width,
    out int height);

// ... add other functions as needed
```

## Testing the DLL

1. Build the C++ project as DLL
2. Verify `guideXOS_Hypervisor.dll` exists in the C# output directory
3. Run the C# GUI
4. The framebuffer should now work if you write to memory address `0xB8000000`

## Troubleshooting

**DLL not found error**:
- Check that `guideXOS_Hypervisor.dll` is in the same directory as the GUI exe
- Use [Dependencies](https://github.com/lucasg/Dependencies) tool to check DLL dependencies

**Entry point not found**:
- Verify exports with `dumpbin /exports guideXOS_Hypervisor.dll`
- Ensure `GUIDEXOS_HYPERVISOR_EXPORTS` is defined when building

**Access violation**:
- Check that buffer sizes match between C# and C++
- Ensure proper marshaling of strings and arrays

## Files Created

- `include\VMManager_DLL.h` - DLL export declarations
- `src\api\VMManager_DLL.cpp` - DLL export implementations
- This guide: `BUILD_DLL_INSTRUCTIONS.md`
