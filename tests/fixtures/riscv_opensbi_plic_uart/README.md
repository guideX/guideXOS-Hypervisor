# RISCV-11 authentic OpenSBI PLIC and UART receive fixture

This fixture reuses the byte-identical OpenSBI v1.8.1 FW_JUMP firmware from
`../riscv_opensbi_fw_jump`. Its DTB adds the pinned OpenSBI PLIC-compatible
node and connects the existing NS16550 UART to PLIC source 10. The independent
RV64IMA_Zicsr supervisor payload configures a non-FIFO UART receive interrupt,
enables source 10 in S-context 1, runs under Sv39, and services three host
injected bytes in a real S-mode external interrupt handler.

Build the payload and DTB from the repository root with:

```powershell
& .\tests\fixtures\riscv_opensbi_plic_uart\build.ps1
```

The build reuses the accepted firmware and does not download, patch, or rebuild
OpenSBI. The script records the firmware provenance, independent payload and
DTB hashes, and toolchain versions in `binary-manifest.txt`.
