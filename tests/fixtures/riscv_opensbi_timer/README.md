# RISCV-10 authentic OpenSBI timer fixture

This fixture reuses the byte-identical, pinned OpenSBI v1.8.1 FW_JUMP image
from `../riscv_opensbi_fw_jump`. It adds an ACLINT MTIMER FDT node and an
independently linked RV64IMA_Zicsr supervisor payload that probes SBI TIME,
arms three deadlines through real SBI calls, handles delegated STIP traps, and
writes its completion record last.

Build the payload and DTB from the repository root with:

```powershell
& .\tests\fixtures\riscv_opensbi_timer\build.ps1
```

The build does not clone or rebuild OpenSBI. It uses the configured MSYS2 DTC
and LLVM cross tools, and records the source firmware hash, payload hash, DTB
hash, and tool versions in `binary-manifest.txt`.
