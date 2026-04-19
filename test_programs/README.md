# Test Programs for guideXOS IA-64 Hypervisor

This directory contains simple test programs that can be used to verify the hypervisor is working without needing a full IA-64 operating system or cross-compiler.

## Quick Start

### Option 1: Use the C# Test Kernel Generator (Easiest)

1. Build and run the test kernel generator:
   ```bash
   cd TestKernelGenerator
   dotnet run
   ```

2. This will create `test_kernel.bin` in the current directory

3. In the guideXOS Hypervisor GUI:
   - Create a new VM
   - Enable "Direct Boot" in VM settings
   - Set kernel path to the `test_kernel.bin` file
   - Set entry point to: `0x100000`
   - Start the VM

4. You should see a blue screen with:
   ```
   ====================================================================
   |                                                                  |
   |              guideXOS IA-64 Hypervisor                           |
   |                  Test Kernel v1.0                                |
   |                                                                  |
   |  Architecture: IA-64 (Itanium)                                   |
   |  Status: Running                                                 |
   |  Entry Point: 0x100000                                           |
   |                                                                  |
   |  Testing hypervisor components:                                  |
   |    [OK] CPU execution                                            |
   |    [OK] Memory subsystem                                         |
   |    [OK] Framebuffer output                                       |
   |    [OK] Binary loader                                            |
   |                                                                  |
   |            All systems operational!                              |
   |                                                                  |
   |        Press any key to continue...                              |
   ====================================================================
   ```

### Option 2: Create a Test Kernel in C (Advanced)

If you have an IA-64 cross-compiler installed (`ia64-linux-gnu-gcc`), you can compile the minimal test program:

```bash
ia64-linux-gnu-gcc -nostdlib -nodefaultlibs -static -Ttext=0x100000 \
  -o minimal_test.elf minimal_test.c -lgcc
```

Then use `minimal_test.elf` as the kernel file in the GUI.

### Option 3: Create a Raw Binary Generator (C++)

Compile and run the binary generator:

```bash
g++ -o create_test_binary create_test_binary.c
./create_test_binary
```

This creates `test_kernel.elf` which can be loaded via direct boot.

## Test Kernel Format

The test kernels use a simple command-based format that the hypervisor recognizes:

### Magic Header
```
Bytes 0-7:  "GUIDEXOS" (magic identifier)
Bytes 8-11: Version (uint32, little-endian)
Bytes 12-15: Entry point (uint32, little-endian)
```

### Command Format
```
Command opcodes:
  0x01: Clear screen
        [color_byte]
  
  0x02: Write string
        [x] [y] [color] [length] [text_bytes...]
  
  0x03: Draw box
        [x1] [y1] [x2] [y2] [color]
  
  0xFF: End of kernel
```

### VGA Text Mode Colors
```
Background (high nibble) | Foreground (low nibble)
0x0 = Black              | 0x8 = Dark Gray
0x1 = Blue               | 0x9 = Light Blue
0x2 = Green              | 0xA = Light Green
0x3 = Cyan               | 0xB = Light Cyan
0x4 = Red                | 0xC = Light Red
0x5 = Magenta            | 0xD = Light Magenta
0x6 = Brown              | 0xE = Yellow
0x7 = Light Gray         | 0xF = White
```

Example: `0x1F` = White text on blue background

## Troubleshooting

### "Test kernel execution failed"
- Make sure the kernel file exists and is readable
- Check that the file starts with "GUIDEXOS" magic bytes
- Verify the command format is correct

### "Framebuffer not available"
- The VM needs to be initialized before starting
- Make sure framebuffer device is created during VM initialization

### No output visible
- Check that you opened the "Screen" window for the VM
- The screen updates at 60 FPS when the VM is running
- Try stopping and restarting the VM

## Next Steps

Once you verify the test kernel works:

1. Create your own test patterns by modifying `TestKernelGenerator/Program.cs`
2. Test the CPU execution with real IA-64 instructions (requires IA-64 cross-compiler)
3. Load a real IA-64 EFI bootable ISO (requires EFI-capable IA-64 operating system)

## Files

- `TestKernelGenerator/Program.cs` - C# program to generate test kernels
- `minimal_test.c` - Minimal C test program (requires IA-64 cross-compiler)
- `create_test_binary.c` - C++ program to create test binaries
- `README.md` - This file

## License

These test programs are provided as-is for testing the guideXOS Hypervisor.
