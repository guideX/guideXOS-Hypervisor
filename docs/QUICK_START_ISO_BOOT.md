# Quick Start - Boot VM from ISO

## Test It Right Now

### 1. Build and Run
```powershell
# Build C# GUI
cd "guideXOS Hypervisor GUI"
dotnet build
dotnet run
```

### 2. Create VM with ISO
1. Click **"New"** button
2. File dialog opens - Select your ISO/IMG file
3. VM appears in list

### 3. Start VM
1. Select the VM
2. Click **"Start"**
3. Click **"Show Screen"**

### 4. Watch Console Output
You should see:
```
Starting VM: <vm-id>
Loading bootloader from device: disk0
Loading boot sector to address: 0x100000
Bootloader loaded successfully, entry point: 0x100000
VM started: <vm-id>
```

### 5. Check Framebuffer
- **If working:** Boot messages appear (even if garbled, it's reading from ISO!)
- **If not working yet:** Still shows gradient (DLL not built or boot code issue)

## Build C++ DLL (For Real Boot)

```
1. Open Visual Studio
2. Right-click "guideXOS Hypervisor DLL" ? Rebuild
3. Check: x64\Debug\guideXOS_Hypervisor.dll created
4. Restart GUI
```

## What Was Implemented

### C# (GUI)
? ISO file picker on VM creation
? VMConfiguration with storage support
? JSON serialization
? P/Invoke to native backend

### C++ (Backend)  
? JSON configuration parsing
? Storage device attachment
? Bootloader loading from ISO
? Entry point setting

## Files Changed
- `Models\VMStateModel.cs` - Storage config, ToJson()
- `ViewModels\MainViewModel.cs` - ISO picker
- `Services\VMManagerWrapper.cs` - Native calls
- `src\api\VMManager_DLL.cpp` - JSON parsing
- `src\vm\VMManager.cpp` - Boot loading
- `include\VMManager_DLL.h` - Updated signature

## Success = Gradient Disappears!

When you see the framebuffer change from the rainbow gradient to **anything else** (even black screen or garbage), the ISO is being read!

## Troubleshooting

**Still shows gradient:**
- DLL not built or not found
- Check console for "DLL not found" message

**Black screen:**
- Boot code is loading but not executing
- Check IP is set to 0x100000

**Garbage/random pixels:**
- Memory being accessed but wrong area
- This is progress! Boot code is executing

**Boot messages:**
- ?? **SUCCESS!** VM is booting from ISO!

## Next: Test with Real ISO

Try an IA-64 Linux installer or any IA-64 bootable ISO.
