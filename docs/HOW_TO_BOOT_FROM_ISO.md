# How to Make VMs Boot from ISO/IMG Files

## The Problem

Your VM shows the gradient pattern instead of booting because:
1. ? VM is created and started
2. ? Screen renders (shows gradient test pattern)
3. ? **No storage device (ISO/IMG) is attached to the VM**
4. ? **VM has nothing to boot from**

## What I Fixed

### 1. Created VMConfiguration Model (`Models\VMConfiguration.cs`)

**New classes:**
- `VMConfiguration` - Complete configuration matching C++ backend
- `StorageConfiguration` - Represents a disk/ISO/IMG file
- `BootConfiguration` - Boot settings

**Key feature - CreateWithISO() method:**
```csharp
var config = VMConfiguration.CreateWithISO(
    name: "My VM",
    isoPath: @"C:\path\to\your.iso",
    memoryMB: 512,
    cpuCount: 1
);
```

This creates a VM with the ISO attached as a bootable storage device.

### 2. Updated VMManagerWrapper.CreateVM()

Now actually calls the native C++ function and passes the JSON configuration:
```csharp
string configJson = config.ToJson();
IntPtr result = VMManager_CreateVM(_nativeManager, configJson);
```

### 3. Added VMManager_FreeString P/Invoke

To properly clean up strings returned from C++.

## How to Attach an ISO/IMG to Your VM

### Option A: Modify MainViewModel.OnNewVM() (Quick Fix)

Change the VM creation code to include an ISO:

```csharp
private void OnNewVM()
{
    StatusMessage = "Creating new virtual machine...";
    
    try
    {
        // Let user select ISO file
        var openDialog = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "Disk Images (*.iso;*.img)|*.iso;*.img|All Files (*.*)|*.*",
            Title = "Select Boot Image"
        };
        
        string isoPath = string.Empty;
        if (openDialog.ShowDialog() == true)
        {
            isoPath = openDialog.FileName;
        }
        
        // Create VM with ISO attached
        var config = VMConfiguration.CreateWithISO(
            name: $"Virtual Machine {VirtualMachines.Count + 1}",
            isoPath: isoPath,
            memoryMB: 512,
            cpuCount: 1
        );

        // Call VMManager.CreateVM
        var vmId = VMManagerWrapper.Instance.CreateVM(config);
        
        // ... rest of the code
    }
    catch (Exception ex)
    {
        StatusMessage = $"Error creating VM: {ex.Message}";
        MessageBox.Show($"Failed to create virtual machine:\n{ex.Message}", 
            "Error", MessageBoxButton.OK, MessageBoxImage.Error);
    }
}
```

### Option B: Use the Settings Dialog (Proper Way)

The VMSettingsView already has fields for:
- `BootFromISO` - Path to ISO file
- `DiskImages` - Collection of disk images

You need to wire these up to actually attach storage when creating/updating a VM.

## What the C++ Backend Needs to Do

When the C++ VMManager receives the configuration JSON, it needs to:

1. **Parse the storage devices**
```cpp
for (auto& storage : config.storage) {
    auto device = std::make_shared<RawDiskDevice>(
        storage.imagePath,
        storage.readOnly
    );
    vm->attachStorageDevice(storage.deviceId, device);
}
```

2. **Load bootloader from the ISO/IMG**
```cpp
// Read boot sector from storage device
auto bootDevice = vm->getStorageDevice(config.boot.bootDevice);
std::vector<uint8_t> bootSector(512);
bootDevice->read(0, bootSector.data(), 512);

// Load into VM memory
vm->writeMemory(0x7C00, bootSector.data(), 512); // BIOS loads boot sector here

// Set entry point
vm->setIP(0x7C00);
```

3. **Start execution**
```cpp
vm->run(maxCycles);
```

## Current Status After My Changes

### ? What's Working
- VMConfiguration class created
- VMManagerWrapper.CreateVM() updated to call native
- P/Invoke declarations added
- JSON serialization implemented

### ?? What Still Needs Work

**C# Side:**
1. Update MainViewModel.OnNewVM() to let user select ISO
2. Or wire up VMSettingsView to handle storage configuration
3. Pass storage info when creating VM

**C++ Side:**
1. VMManager_CreateVM needs to parse JSON config
2. Attach storage devices from configuration
3. Load boot sector from storage into memory
4. Set entry point to bootloader address
5. Start execution from bootloader

## Testing the Fix

### Step 1: Build the Updated C# Code

```powershell
cd "guideXOS Hypervisor GUI"
dotnet build
```

### Step 2: Test with Hardcoded ISO Path (Quick Test)

Temporarily modify MainViewModel.OnNewVM():

```csharp
// Hardcode ISO path for testing
var config = VMConfiguration.CreateWithISO(
    name: "Test VM",
    isoPath: @"D:\path\to\your\test.iso",  // CHANGE THIS!
    memoryMB: 512,
    cpuCount: 1
);
```

### Step 3: Run and Check

```powershell
dotnet run
```

Create a new VM. Check the console output - you should see the JSON configuration being passed.

### Step 4: Verify C++ Backend Receives Config

Add logging to VMManager_DLL.cpp:
```cpp
GUIDEXOS_API const char* VMManager_CreateVM(
    VMManagerHandle manager,
    const char* configJson) {
    
    // Log the received config
    std::cout << "Received config JSON: " << configJson << std::endl;
    
    // ... rest of code
}
```

## Next Steps to Full Boot

1. **Implement C++ storage attachment** (critical!)
2. **Load bootloader from ISO** into VM memory
3. **Set IP to bootloader entry point**
4. **Initialize devices** (keyboard, video, disk controller)
5. **Run execution loop**

## File Picker Dialog Example

For a proper UI, add this to MainViewModel:

```csharp
private string? SelectISOFile()
{
    var dialog = new Microsoft.Win32.OpenFileDialog
    {
        Filter = "Disk Images|*.iso;*.img;*.ima|ISO Images (*.iso)|*.iso|IMG Files (*.img)|*.img|All Files|*.*",
        Title = "Select Bootable Disk Image",
        CheckFileExists = true
    };
    
    return dialog.ShowDialog() == true ? dialog.FileName : null;
}
```

Then in OnNewVM():
```csharp
string? isoPath = SelectISOFile();
if (isoPath != null)
{
    var config = VMConfiguration.CreateWithISO(name, isoPath, 512, 1);
    // ... create VM
}
```

## Summary

**I've created the infrastructure for ISO/IMG boot:**
- ? VMConfiguration model
- ? Storage configuration support
- ? JSON serialization
- ? P/Invoke integration

**You need to:**
1. Add ISO file selection to UI
2. Implement C++ storage attachment
3. Load bootloader into memory
4. Set entry point and run

**Once done, VMs will boot from ISO instead of showing the gradient!**
