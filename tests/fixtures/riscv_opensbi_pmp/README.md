# RISCV-15 OpenSBI PMP guest

This fixture reuses the unmodified, pinned OpenSBI v1.8.1 FW_JUMP image and
RISCV-9 platform FDT. Its independently assembled RV64IMA_Zicsr supervisor
guest first confirms bare S-mode read, write, and fetch faults against the
firmware. It then enables Sv39, checks the guest stack and data mappings,
probes two virtual aliases of the protected firmware, verifies a page-level
load fault, and calls SBI Base after recovering from the expected traps.

The S-mode trap handler checks each cause and `stval`, saves the fault PC, and
returns to a guest-selected resume label. Unexpected traps set the failure
signature and stop advancing. The guest writes the version-2 result record and
its completion marker last. Authorized UART0 output provides an additional
platform-MMIO path.

Build the independent guest from the repository root with:

```powershell
& .\tests\fixtures\riscv_opensbi_pmp\build.ps1
```

The build uses LLVM's `riscv64-unknown-elf` target with `rv64ima_zicsr`,
`lp64`, no relaxation, and freestanding/no-runtime flags. It reads but does
not rebuild or alter the accepted OpenSBI binary or platform FDT. SHA-256
values and compiler provenance are recorded in `binary-manifest.txt`.
