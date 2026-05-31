# Fixing DLL Linker Errors - Complete Solution

## The Root Cause

The linker errors you're seeing are because:

1. **Core library has files excluded from build** - Most `src\*.cpp` files have `<ExcludedFromBuild>true</ExcludedFromBuild>`
2. **DLL expects symbols that aren't in Core.lib** - VMManager methods aren't compiled into the library
3. **`GUIDEXOS_HYPERVISOR_EXPORTS` macro issue** - Functions are being treated as imports instead of exports

## The Errors Explained

```
LNK2019: unresolved external symbol "public: bool __cdecl ia64::VMManager::stopVM(...)"
LNK2019: unresolved external symbol "public: __cdecl ia64::VMManager::VMManager(void)"
```

This means: **The DLL is looking for VMManager functions in Core.lib, but they don't exist because those files weren't compiled.**

## Solution: Two Approaches

### ?? IMPORTANT DECISION POINT

You have two choices:

**Option A: Fix Core Library** (Recommended but requires rebuild)
- Remove `ExcludedFromBuild` from all Core project files
- Rebuild Core.lib with all source files
- Rebuild DLL (will link successfully)

**Option B: Link DLL Directly to Source** (Quick fix)
- Don't use Core.lib for DLL
- Add source files directly to DLL project
- Build just the DLL

---

## Option A: Fix Core Library (Recommended)

This is the "proper" solution that matches the design we planned.

### Step 1: Remove ExcludedFromBuild Attributes

Create a PowerShell script to fix the Core project:

```powershell
# fix_core_project.ps1
$projectFile = "guideXOS Hypervisor Core.vcxproj"

if (-not (Test-Path $projectFile)) {
    Write-Error "Core project not found"
    exit 1
}

Write-Host "Fixing Core project..." -ForegroundColor Cyan
$xml = [xml](Get-Content $projectFile)
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace("ns", "http://schemas.microsoft.com/developer/msbuild/2003")

# Find all ClCompile elements with ExcludedFromBuild
$excludedFiles = $xml.SelectNodes("//ns:ClCompile[@ExcludedFromBuild='true' or ns:ExcludedFromBuild]", $ns)

Write-Host "Found $($excludedFiles.Count) excluded files" -ForegroundColor Yellow

foreach ($file in $excludedFiles) {
    $fileName = $file.GetAttribute("Include")
    
    # Remove ExcludedFromBuild attribute
    $file.RemoveAttribute("ExcludedFromBuild")
    
    # Remove child ExcludedFromBuild elements
    $childNodes = $file.SelectNodes("ns:ExcludedFromBuild", $ns)
    foreach ($child in $childNodes) {
        $file.RemoveChild($child) | Out-Null
    }
    
    Write-Host "  ? Included: $fileName" -ForegroundColor Green
}

$xml.Save((Resolve-Path $projectFile).Path)
Write-Host "`n? Core project fixed!" -ForegroundColor Green
Write-Host "Next: Rebuild Core library in Visual Studio" -ForegroundColor Cyan
```

**Run this script:**
```powershell
.\fix_core_project.ps1
```

### Step 2: Rebuild Core Library

1. Open Visual Studio
2. Right-click "guideXOS Hypervisor Core" project
3. Clean
4. Rebuild
5. Verify `x64\Debug\guideXOS Hypervisor Core.lib` is much larger (~10-20 MB)

### Step 3: Add Missing Export to VMManager_DLL.cpp

The `VMManager_RunVM` function is missing. Add it:

```cpp
// Add to src\api\VMManager_DLL.cpp after VMManager_DeleteVM

GUIDEXOS_API uint64_t VMManager_RunVM(
    VMManagerHandle manager,
    const char* vmId,
    uint64_t maxCycles) {
    
    if (!manager || !vmId) {
        return 0;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->runVM(vmId, maxCycles);
    } catch (...) {
        return 0;
    }
}
```

### Step 4: Update VMManager_DLL.h

Add the declaration:

```cpp
// Add after VMManager_DeleteVM declaration

/**
 * Run VM for specified number of cycles
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @param maxCycles Maximum cycles to execute
 * @return Number of cycles actually executed
 */
GUIDEXOS_API uint64_t VMManager_RunVM(
    VMManagerHandle manager,
    const char* vmId,
    uint64_t maxCycles);
```

### Step 5: Rebuild DLL

1. Right-click "guideXOS Hypervisor DLL"
2. Rebuild
3. Should link successfully!

---

## Option B: Quick Fix (Link DLL Directly)

If Option A seems too complicated, here's a faster approach:

### Modify DLL Project to Include Source Files Directly

**Instead of linking Core.lib, compile the needed files directly in the DLL project.**

Edit `guideXOS Hypervisor DLL.vcxproj`, replace the ItemGroup with:

```xml
<ItemGroup>
  <ClCompile Include="src\api\dllmain.cpp" />
  <ClCompile Include="src\api\VMManager_DLL.cpp" />
  <!-- Add the source files DLL needs -->
  <ClCompile Include="src\vm\VMManager.cpp" />
  <ClCompile Include="src\vm\VirtualMachine.cpp" />
  <ClCompile Include="src\vm\CPUContext.cpp" />
  <ClCompile Include="src\vm\SimpleCPUScheduler.cpp" />
  <ClCompile Include="src\vm\VMBootStateMachine.cpp" />
  <ClCompile Include="src\vm\VMSnapshotManager.cpp" />
  <ClCompile Include="src\vm\BootTraceSystem.cpp" />
  <ClCompile Include="src\vm\KernelPanic.cpp" />
  <ClCompile Include="src\core\cpu.cpp" />
  <ClCompile Include="src\core\cpu_core.cpp" />
  <ClCompile Include="src\memory\memory.cpp" />
  <ClCompile Include="src\mmu\mmu.cpp" />
  <ClCompile Include="src\decoder\decoder.cpp" />
  <ClCompile Include="src\decoder\atype_decoder.cpp" />
  <ClCompile Include="src\decoder\btype_decoder.cpp" />
  <ClCompile Include="src\decoder\itype_decoder.cpp" />
  <ClCompile Include="src\decoder\lx_decoder.cpp" />
  <ClCompile Include="src\decoder\mtype_decoder.cpp" />
  <ClCompile Include="src\io\Console.cpp" />
  <ClCompile Include="src\io\ConsoleOutputBuffer.cpp" />
  <ClCompile Include="src\io\FramebufferDevice.cpp" />
  <ClCompile Include="src\io\InterruptController.cpp" />
  <ClCompile Include="src\io\Timer.cpp" />
  <ClCompile Include="src\storage\RawDiskDevice.cpp" />
  <ClCompile Include="src\storage\VirtualBlockDevice.cpp" />
  <ClCompile Include="src\storage\VirtualDiskManager.cpp" />
  <ClCompile Include="src\isa\IA64ISAPlugin.cpp" />
  <ClCompile Include="src\isa\ISAPluginRegistry.cpp" />
  <ClCompile Include="src\loader\loader.cpp" />
  <ClCompile Include="src\loader\bootstrap.cpp" />
  <ClCompile Include="src\profiler\Profiler.cpp" />
</ItemGroup>
```

Then remove the Core.lib dependency and rebuild the DLL.

---

## Recommended Actions

**I recommend Option A** because:
1. It follows the proper architecture (Core ? DLL)
2. Core.lib can be reused by EXE project
3. Cleaner separation of concerns

**Here's what to do:**

1. **Run the PowerShell script** I provided above
2. **Rebuild Core.lib** in Visual Studio
3. **Add VMManager_RunVM** to VMManager_DLL.cpp
4. **Rebuild DLL**
5. **Test!**

Would you like me to:
- Create the PowerShell script file for you?
- Add the VMManager_RunVM function to VMManager_DLL.cpp?
- Provide step-by-step commands you can copy/paste?

Let me know and I'll implement the fix!
