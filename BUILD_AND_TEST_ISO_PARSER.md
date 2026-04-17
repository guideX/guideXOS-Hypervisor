# Quick Build & Test - ISO Parser

## Build Steps

### 1. Rebuild C++ DLL

**In Visual Studio:**
```
Right-click "guideXOS Hypervisor DLL" ? Rebuild
```

The new files will be automatically included:
- `include\ISO9660Parser.h`
- `src\storage\ISO9660Parser.cpp`

**Or via command line:**
```powershell
msbuild "guideXOS Hypervisor DLL.vcxproj" /t:Rebuild /p:Configuration=Debug /p:Platform=x64
```

### 2. Run the GUI

```powershell
cd "guideXOS Hypervisor GUI"
dotnet run
```

## Test the ISO Parser

### Step 1: Create VM
1. Click **"New"** button
2. Select your IA-64 ISO file (e.g., `t2-26.3-ia64-desktop.iso`)
3. VM appears in list

### Step 2: Start VM
1. Select the VM
2. Click **"Start"** button
3. **Watch the console window!**

## What to Look For

### Success Indicators:

**In Console:**
```
[INFO ] Attempting to parse ISO 9660 filesystem...
[INFO ] Primary Volume Descriptor found:
[INFO ]   Volume: <ISO name>
[INFO ] El Torito Boot Record found:
[INFO ]   Boot catalog at LBA: <number>
[INFO ] ? ISO filesystem parsed successfully
[INFO ] ? El Torito boot catalog found
[INFO ] ? EFI boot entry found
[INFO ]   Loading from LBA: <number>
[INFO ]   Size: <number> sectors
[INFO ] ? EFI bootloader read: <bytes> bytes
[INFO ] ? EFI bootloader loaded successfully
```

**In Execution:**
```
[IP=0x100000, Slot=0] <real instruction, not nop>
[IP=0x100000, Slot=1] <real instruction, not nop>
[IP=0x100000, Slot=2] <real instruction, not nop>
```

### Failure Indicators:

**ISO Not Bootable:**
```
[WARN ] El Torito boot catalog not found
[INFO ] This may not be a bootable ISO
[INFO ] Falling back to raw boot sector loading...
```

**Parse Failed:**
```
[WARN ] Failed to parse ISO filesystem, trying raw boot sector...
[INFO ] Loading raw boot data to address: 0x100000
```

**Still NOPs:**
```
[IP=0x100000, Slot=0] nop
[IP=0x100000, Slot=1] nop
```
This means either:
- ISO structure is non-standard
- Boot entry LBA is incorrect
- ISO is not actually bootable

## Quick Diagnostic

### Check Console Output

**Look for these specific lines:**

1. **"Primary Volume Descriptor found"** ? ISO is readable ?
2. **"El Torito Boot Record found"** ? ISO has boot catalog ?
3. **"EFI boot entry found"** ? EFI bootloader detected ?
4. **"EFI bootloader read: X bytes"** ? Bootloader extracted ?
5. **"EFI bootloader loaded successfully"** ? In memory ?

If all 5 show up, the parser worked perfectly!

### Check Instruction Output

**After "VM started", look at the first few instructions:**

**Good (real code):**
```
[IP=0x100000, Slot=0] mov r14=r0
[IP=0x100000, Slot=1] adds r15=16,r12
[IP=0x100000, Slot=2] br.call.sptk b0=0x100200
```

**Bad (still NOPs):**
```
[IP=0x100000, Slot=0] nop
[IP=0x100000, Slot=1] nop
[IP=0x100000, Slot=2] nop
```

## Troubleshooting

### Issue 1: Build Errors

**Error:** `ISO9660Parser.h not found`

**Solution:**
- Make sure files are in correct directories
- Rebuild entire solution
- Check project includes

### Issue 2: DLL Not Updated

**Check DLL timestamp:**
```powershell
Get-Item "x64\Debug\guideXOS_Hypervisor.dll" | Select LastWriteTime
```

Should be recent (after rebuild).

**Manually copy if needed:**
```powershell
Copy-Item "x64\Debug\guideXOS_Hypervisor.dll" `
          "guideXOS Hypervisor GUI\bin\Debug\net9.0-windows\" -Force
```

### Issue 3: Console Not Showing

Make sure console window is visible (we enabled it earlier with `OutputType=Exe`).

### Issue 4: ISO Not Bootable

Some ISOs might not have El Torito boot records. Try a known-bootable IA-64 ISO:
- Official IA-64 Linux distributions
- IA-64 Windows installation discs
- IA-64 EFI test images

## Success Criteria

**You'll know it worked when:**

1. ? Console shows "? EFI bootloader loaded successfully"
2. ? Bootloader size is > 4096 bytes (bigger than before)
3. ? Instructions at 0x100000 are NOT all NOPs
4. ? Execution shows varied instructions

**If you see this, the ISO parser is working and real EFI code is executing!**

## What Happens Next

**If EFI bootloader loads successfully:**
- The EFI code will start executing
- It may crash (we don't have full EFI environment yet)
- But seeing non-NOP instructions means it's reading the real bootloader!

**Next step would be:**
- Implement basic EFI boot services
- Provide minimal EFI runtime
- Allow bootloader to load kernel

**But this is already a major milestone!**

## Run It Now!

```powershell
# Rebuild DLL in Visual Studio, then:
cd "guideXOS Hypervisor GUI"
dotnet run
```

Create VM ? Start VM ? Check console for ISO parser output!
