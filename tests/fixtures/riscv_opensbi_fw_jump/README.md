# RISCV-9 OpenSBI FW_JUMP fixture

This fixture contains an unmodified OpenSBI v1.8.1 generic `FW_JUMP` firmware
build, a separately linked RV64IMA/Zicsr supervisor payload, and the compiled
platform FDT. `binary-manifest.txt` records exact source revision, build
configuration, artifact sizes, and SHA-256 digests. `opensbi-COPYING.BSD`
preserves the upstream BSD-2-Clause license for the redistributed firmware.

Rebuild from the repository root with PowerShell:

```powershell
& .\tests\fixtures\riscv_opensbi_fw_jump\rebuild.ps1
```

The script clones the pinned OpenSBI tag into the ignored `.build` directory
when `-OpenSbiSource` is omitted. To reuse an existing checkout at the pinned
commit:

```powershell
& .\tests\fixtures\riscv_opensbi_fw_jump\rebuild.ps1 `
    -OpenSbiSource C:\path\to\opensbi-v1.8.1
```

The default tool locations are MSYS2 at `C:\msys64` and LLVM at
`C:\Program Files\LLVM\bin`; override them with `-MsysRoot` and `-LlvmBin`.
The build needs MSYS2 Bash/Make/DTC, Python 3, and LLVM Clang/LLD/objcopy/
readobj/objdump. The script rejects any OpenSBI source revision other than
`74434f255873d74e56cc50aa762d1caf24c099f8`.

Generated intermediates and host wrappers live under `.build/`, which is
ignored. The script refreshes the checked-in firmware, payload, FDT, metadata,
disassembly, and manifest files. No OpenSBI source changes are applied.
