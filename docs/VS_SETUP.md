# Visual Studio Configuration Guide

If you want to use Visual Studio directly instead of CMake, follow these steps:

## Step 1: Open Solution
Open `guideXOS Hypervisor.sln` in Visual Studio 2022

## Step 2: Configure Project Properties

1. Right-click on the `guideXOS Hypervisor` project in Solution Explorer
2. Select **Properties**

### For All Configurations and Platforms:

#### C/C++ ? General
- **Additional Include Directories**: Add `$(ProjectDir)include`
  ```
  $(ProjectDir)include;%(AdditionalIncludeDirectories)
  ```

#### C/C++ ? Language
- **C++ Language Standard**: Select `ISO C++20 Standard (/std:c++20)`

#### Apply Settings
- Configuration: `All Configurations`
- Platform: `All Platforms`
- Click **OK**

## Step 3: Verify Source Files

Ensure these files are included in the project (should already be there):

### Source Files (*.cpp)
- ? main.cpp
- ? src\core\cpu.cpp
- ? src\decoder\decoder.cpp
- ? src\memory\memory.cpp
- ? src\loader\loader.cpp
- ? src\abi\abi.cpp

### Header Files (*.h)
- ? include\cpu_state.h
- ? include\decoder.h
- ? include\memory.h
- ? include\loader.h
- ? include\abi.h

## Step 4: Build

- Select configuration: **Debug** or **Release**
- Select platform: **x64** (recommended) or **Win32**
- Build ? Build Solution (Ctrl+Shift+B)

## Step 5: Run

- Debug ? Start Without Debugging (Ctrl+F5)

---

## Troubleshooting

### Error: Cannot open include file 'cpu_state.h'
**Solution**: Make sure you added `$(ProjectDir)include` to Additional Include Directories

### Error: C++ standard not recognized
**Solution**: Ensure C++ Language Standard is set to C++20 (`/std:c++20`)

### Build succeeds but files not in project
**Solution**: Right-click project ? Add ? Existing Item ? Select missing files

---

## Alternative: Use CMake (Recommended)

CMake handles all configuration automatically:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
.\build\bin\Debug\ia64emu.exe
```

This is the recommended approach as it requires no manual configuration.
