# IA-64 ISO Boot Analysis and Fixes

## Problem Summary (Updated After Testing)

Based on the `output.log` analysis with enhanced debug logging, the **ROOT CAUSE** has been identified:

**The Debian 7.11.0 IA-64 ISO is NOT an EFI-bootable ISO.**

### Evidence:
1. ? **ISO parsed successfully** - ISO9660 filesystem is valid
2. ? **El Torito boot catalog found** - Boot catalog exists at LBA 782
3. ? **Boot catalog is for x86 BIOS only** - Platform ID: 0x0 (not 0xEF for EFI)
4. ? **No EFI directory exists** - Extensive search of entire ISO found no `/EFI` directory
5. ? **No EFI bootloader** - `BOOTIA64.EFI` does not exist anywhere in the ISO

### Root Directory Contents:
```
BOOT        [DIR]   - Contains BOOT.CAT, BOOT.IMG (x86 BIOS boot files)
CSS         [DIR]   - Stylesheets
DEBIAN      [FILE]  - Metadata
DISTS       [DIR]   - Debian package distribution
DOC         [DIR]   - Documentation
FIRMWARE    [DIR]   - Firmware packages
INSTALL     [DIR]   - Debian installer (likely contains kernel)
MD5SUM.TXT  [FILE]  - Checksums
PICS        [DIR]   - Images
POOL        [DIR]   - Package pool
README.HTM  [FILE]  - Documentation
README.TXT  [FILE]  - Documentation
_DISK       [DIR]   - Disk metadata
```

**NO EFI DIRECTORY FOUND**

## Why This Matters

**IA-64 (Itanium) architecture REQUIRES EFI boot.** It does not support BIOS/legacy boot. This ISO was created for x86 BIOS systems and cannot boot on IA-64 without an EFI bootloader.

## Changes Made (Latest Version)

### 1. Enhanced Error Detection (`src/storage/ISO9660Parser.cpp`)

**Added clear warning for architecture mismatch:**
```cpp
LOG_WARN("  ISO boot catalog is for platform 0x" + 
        std::to_string(validation->platformID) + 
        (validation->platformID == 0 ? " (x86/BIOS)" : ""));
LOG_WARN("  This ISO is NOT designed for EFI boot (IA-64 requires EFI)");
```

### 2. Added Kernel Search Fallback (`src/vm/VMManager.cpp`)

**Attempts to find and directly boot IA-64 kernel:**
```cpp
const char* kernelPaths[] = {
    "/install/vmlinuz",
    "/install/vmlinux",
    "/boot/vmlinuz",
    "/boot/vmlinux",
    "install/vmlinuz",
    "install/vmlinux",
    "boot/vmlinuz",
    "boot/vmlinux"
};
```

**Provides clear warnings:**
```cpp
LOG_ERROR("Could not find EFI bootloader in ISO filesystem");
LOG_INFO("This ISO does not appear to have EFI boot support.");
LOG_INFO("Attempting to find IA-64 kernel for direct boot...");
```

**If kernel found:**
- Loads kernel to memory at 0x100000
- Sets entry point
- Warns that it's experimental and may need initrd/boot params

**If kernel NOT found:**
```cpp
LOG_WARN("WARNING: Loading x86 BIOS boot code on IA-64 architecture");
LOG_WARN("This will NOT work. Please use an EFI-bootable IA-64 ISO or direct kernel boot.");
```

### 3. Previous Changes (From Earlier Fix)

- ? ISO9660 filename normalization (removes `;1` version suffixes)
- ? Root directory listing for debugging
- ? Enhanced file search logging
- ? Multiple EFI path search attempts

## Solutions

### Solution 1: Use an EFI-Bootable IA-64 ISO ? RECOMMENDED

Find or create an ISO that has:
- EFI boot support (El Torito platform ID 0xEF)
- `/EFI/BOOT/BOOTIA64.EFI` bootloader
- Proper IA-64 EFI environment

**Where to find:**
- HP Integrity Server firmware CD/DVD (contains elilo)
- Modern Linux distributions with IA-64 EFI support
- FreeBSD IA-64 installation media
- Custom-built ISO with elilo or GRUB for IA-64 EFI

### Solution 2: Direct Kernel Boot (Experimental)

**If the hypervisor finds a kernel in `/install/` directory:**
1. The kernel will be loaded to memory
2. Entry point will be set
3. Execution will begin

**Requirements:**
- Kernel must be in ELF format for IA-64
- May need to manually specify:
  - Initrd location and load address
  - Kernel command line parameters
  - Boot protocol setup

**Limitations:**
- No bootloader environment
- No automatic initrd loading
- No boot parameter parsing
- May fail without proper kernel setup

### Solution 3: Extract and Configure Boot Files

1. **Mount the ISO** and extract kernel/initrd from `/install/` directory
2. **Configure direct boot** in the hypervisor:
   ```json
   {
     "boot": {
       "directBoot": true,
       "kernelPath": "path/to/vmlinux",
       "initrdPath": "path/to/initrd.gz",
       "bootArgs": "console=ttyS0",
       "entryPoint": 0x100000
     }
   }
   ```

## What Will Happen Now

When you run the application with the current Debian 7.11 ISO, you'll see:

```
[WARN] ISO boot catalog is for platform 0x0 (x86/BIOS)
[WARN] This ISO is NOT designed for EFI boot (IA-64 requires EFI)
[ERROR] Could not find EFI bootloader in ISO filesystem
[INFO] This ISO does not appear to have EFI boot support.
[INFO] Attempting to find IA-64 kernel for direct boot...
[INFO] Searching for kernel: /install/vmlinuz
...
```

**Possible Outcomes:**

1. **If kernel found in /install/:**
   - Kernel loads to 0x100000
   - Execution begins (experimental, may work partially)
   - You'll see actual IA-64 instructions instead of "unknown (0x0)"

2. **If kernel NOT found:**
   - Clear warning about x86 BIOS code on IA-64
   - Loads raw boot sector (will fail)
   - Shows "unknown (0x0)" instructions
   - **This confirms the ISO is incompatible**

## Files Modified (Latest)

1. ? `src/storage/ISO9660Parser.cpp` - Platform mismatch warning
2. ? `src/vm/VMManager.cpp` - Kernel search fallback and clear error messages
3. ? `include/ISO9660Parser.h` - listRootDirectory() declaration

## Summary

? **Problem Identified**: Debian 7.11.0 IA-64 ISO is x86 BIOS-only, not EFI-bootable

? **Root Cause**: No EFI bootloader exists in the ISO

? **Warnings Added**: Clear messages about architecture mismatch

? **Fallback Implemented**: Attempts to find and directly boot IA-64 kernel

? **Next Step**: Use an EFI-bootable IA-64 ISO or configure direct kernel boot

## Recommendation

**Stop using this ISO for IA-64 boot testing.** Either:

1. Find a proper EFI-bootable IA-64 ISO
2. Extract the kernel and use direct boot mode
3. Build a custom EFI-bootable ISO with elilo

The hypervisor is working correctly - the ISO is simply incompatible with IA-64 architecture.
