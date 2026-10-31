Independent RV64IMA/Zicsr guest
================================

This fixture is assembled from guest.S by LLVM's RISC-V integrated assembler
and linked with LLD. It is freestanding, uses no C runtime or library, and is
linked at 0x80000000 for the RISCV-7 single-hart test entry contract.

Build on Windows with LLVM 22 or a compatible release:

    powershell -ExecutionPolicy Bypass -File .\build.ps1

To select another LLVM installation:

    powershell -ExecutionPolicy Bypass -File .\build.ps1 -LlvmBin C:\LLVM\bin

The script uses --target=riscv64-unknown-elf, -march=rv64ima_zicsr,
-mabi=lp64, -mcmodel=medany, -fno-pic, and -mno-relax. LLD receives
--no-relax. The source selects .option norvc and .option norelax, and does not
enable F, D, or V. The build emits the raw binary, ELF metadata, disassembly,
instruction inventory, and SHA-256 provenance manifest alongside this file.

The guest initializes a page table in M-mode, enters S-mode through MRET,
checks RV64M arithmetic, performs AMOADD and successful/failed/retried LR/SC
operations, then updates the same counter through an Sv39 virtual alias. It
also verifies that a failed SC against a read-only mapping raises a store page
fault and leaves the mapped physical word unchanged. It writes all completion
fields itself and commits the completion flag last. The host harness only
loads the accepted image, calls RV64CPU::step(), and validates the signature.

The guest RAM is a zero-filled 64 KiB range at 0x80000000. Page tables occupy
0x8000A000-0x8000CFFF, the counter is at 0x80009000, the read-only test word
is at 0x8000D000, and the signature starts at 0x80008000.
