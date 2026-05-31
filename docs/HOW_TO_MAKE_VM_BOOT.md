# How to Make the VM Actually Boot

## The Problem

The C# GUI calls `StartVM()` which changes state to "Running", but **never calls the execution loop**. The C++ backend has a `runVM(vmId, maxCycles)` method that needs to be called continuously.

## Current Flow (Broken)

```
User clicks Start
? StartVM(vmId)  ? Changes state to Running
? (Nothing else happens)
? VM sits idle, executes no code
```

## Needed Flow (Working)

```
User clicks Start
? StartVM(vmId)  ? Changes state to Running
? Background thread starts
? Loop: runVM(vmId, 10000)  ? Executes 10,000 cycles
? Sleep 16ms (~60 FPS)
? Loop continues while Running
```

## Solution: Add Execution Loop to VMManagerWrapper

### Step 1: Add RunVM P/Invoke Declaration

Add to `VMManagerWrapper.cs`:

```csharp
[DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
private static extern ulong VMManager_RunVM(IntPtr manager, string vmId, ulong maxCycles);
```

### Step 2: Add VM Execution Thread

Add to `VMManagerWrapper.cs`:

```csharp
private readonly Dictionary<string, CancellationTokenSource> _vmExecutionThreads = new();

public bool StartVM(string vmId)
{
    lock (_lock)
    {
        // TODO: Call native VMManager_StartVM to change state
        // For now, just return true
        
        // Start execution thread
        var cts = new CancellationTokenSource();
        _vmExecutionThreads[vmId] = cts;
        
        Task.Run(() => VMExecutionLoop(vmId, cts.Token), cts.Token);
        
        return true;
    }
}

private void VMExecutionLoop(string vmId, CancellationToken cancellationToken)
{
    const ulong cyclesPerFrame = 10000; // Adjust based on performance
    
    while (!cancellationToken.IsCancellationRequested)
    {
        try
        {
            // TODO: Call VMManager_RunVM(manager, vmId, cyclesPerFrame)
            // For now, just sleep
            
            // Sleep for ~16ms (60 FPS)
            Thread.Sleep(16);
        }
        catch (Exception ex)
        {
            // Log error and stop execution
            Debug.WriteLine($"VM execution error: {ex.Message}");
            break;
        }
    }
}

public bool StopVM(string vmId)
{
    lock (_lock)
    {
        // Stop execution thread
        if (_vmExecutionThreads.TryGetValue(vmId, out var cts))
        {
            cts.Cancel();
            _vmExecutionThreads.Remove(vmId);
        }
        
        // TODO: Call native VMManager_StopVM
        return true;
    }
}
```

## Temporary Mock Solution (Test Without DLL)

To test the framebuffer rendering **before building the DLL**, you can create a mock that writes directly to the framebuffer:

```csharp
// In VMManagerWrapper.cs
private readonly Dictionary<string, byte[]> _mockFramebuffers = new();

private void VMExecutionLoop(string vmId, CancellationToken cancellationToken)
{
    // Create mock framebuffer (640x480, BGRA32)
    byte[] framebuffer = new byte[640 * 480 * 4];
    _mockFramebuffers[vmId] = framebuffer;
    
    int frame = 0;
    
    while (!cancellationToken.IsCancellationRequested)
    {
        // Simulate VM execution by drawing a moving pattern
        UpdateMockFramebuffer(framebuffer, frame++);
        
        Thread.Sleep(16); // 60 FPS
    }
}

private void UpdateMockFramebuffer(byte[] framebuffer, int frame)
{
    int width = 640;
    int height = 480;
    
    // Draw a simple animation (moving vertical bars)
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int offset = (y * width + x) * 4;
            
            // Create a moving pattern
            byte value = (byte)(((x + frame) % 256));
            
            framebuffer[offset + 0] = value;       // B
            framebuffer[offset + 1] = (byte)(y % 256); // G
            framebuffer[offset + 2] = (byte)(255 - value); // R
            framebuffer[offset + 3] = 255;         // A
        }
    }
}

public bool GetFramebuffer(string vmId, byte[] buffer, out int width, out int height)
{
    lock (_lock)
    {
        width = 640;
        height = 480;
        
        if (_mockFramebuffers.TryGetValue(vmId, out byte[] framebuffer))
        {
            Array.Copy(framebuffer, buffer, Math.Min(buffer.Length, framebuffer.Length));
            return true;
        }
        
        return false;
    }
}
```

## With This Mock, You Should See

Instead of static gradient:
- ? **Moving vertical bars** (proves execution loop works)
- ? **Animated pattern** (proves framebuffer updates)
- ? **60 FPS rendering** (proves timing is correct)

## ISO Loading (Still Missing)

Even with execution working, you still need:

1. **ISO File Selection**
   - Add to VM configuration
   - File picker in GUI

2. **Storage Device Creation**
   ```cpp
   // In VMManager.cpp
   auto isoDevice = std::make_unique<RawDiskDevice>(isoPath, true); // read-only
   instance->storageDevices.push_back(std::move(isoDevice));
   ```

3. **Boot Entry Point**
   ```cpp
   // Set IP to bootloader address (usually 0x100000 for IA-64)
   instance->vm->setEntryPoint(0x100000);
   ```

4. **Load ISO into Memory**
   - Read ISO boot sector
   - Load into VM memory
   - Configure BIOS/EFI emulation

## Full Solution Checklist

### Phase 1: Test Mock Execution (Can do now)
- [ ] Add execution loop to VMManagerWrapper
- [ ] Implement mock framebuffer
- [ ] Run GUI and verify animation appears

### Phase 2: Build Real DLL
- [ ] Follow MANUAL_PROJECT_SETUP.md
- [ ] Add projects to solution
- [ ] Build Core.lib
- [ ] Build DLL
- [ ] Uncomment P/Invoke declarations

### Phase 3: Add ISO Support
- [ ] Add ISO path to VMConfiguration
- [ ] Create file picker UI
- [ ] Attach storage device in VMManager
- [ ] Load bootloader into memory

### Phase 4: Boot Real ISO
- [ ] Set entry point to bootloader
- [ ] Execute real IA-64 code
- [ ] Display boot output in framebuffer

## Quick Test (Immediate)

Want me to create the mock execution loop code so you can see the framebuffer animation **right now** without building the DLL?

This will prove:
- ? Execution loop concept works
- ? Framebuffer rendering works
- ? Screen update mechanism works
- ? Ready for real DLL integration

Then once that's working, you can:
1. Build the actual DLL
2. Replace mock with real P/Invoke calls
3. Add ISO loading
4. Boot the real T2/Linux image
