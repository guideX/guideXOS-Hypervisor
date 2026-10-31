Static RISC-V ELF64 guest
========================

This freestanding RV64IMA/Zicsr assembly fixture is assembled by Clang's LLVM
integrated assembler and linked by LLD. It has no C runtime, libc, dynamic
linker, compressed instructions, floating point, or runtime relocations.

The checked-in guest.elf is the input used by test_riscv_elf64_boot. Tests do
not need LLVM or network access. To regenerate the ELF and its metadata with
LLVM installed at the default Windows path, run:

    powershell -ExecutionPolicy Bypass -File .\build.ps1

For a different LLVM installation, pass its bin directory:

    powershell -ExecutionPolicy Bypass -File .\build.ps1 -LlvmBin C:\LLVM\bin

The build writes temporary object and map files to .build/, then refreshes the
checked-in guest.elf, binary-manifest.txt, guest.elf-info.txt, and
guest.disassembly.txt. The manifest records the toolchain and SHA-256.

The linker places RX text at 0x80200000, read-only message bytes at 0x80400000,
and initialized data plus BSS at 0x80600000. These are identity-mapped guest
physical addresses inside the RISCV-8 16 MiB RAM profile. Guest output,
trap-record, and stack regions are separate reservations.
