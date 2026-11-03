# RISCV-12 authentic OpenSBI dual UART fixture

This fixture reuses the accepted OpenSBI v1.8.1 FW_JUMP binary from
`../riscv_opensbi_fw_jump`. The DTB preserves UART0 as `/chosen/stdout-path`
and adds UART1 at `0x1000A000`, connected to PLIC source 12. UART0 remains at
`0x10000000` on source 10. Both use the bounded non-FIFO, one-byte receive
profile.

The independently assembled RV64IMA_Zicsr S-mode guest enables Sv39, configures
both UARTs and PLIC inputs, and exercises simultaneous pending interrupts under
five priority and masking configurations. Its trap handler claims a source,
reads only the corresponding UART RBR, records source and byte in guest RAM,
completes that source, and returns with SRET. Four scenarios enter WFI before
the first host input; all five snapshots prove both sources pending before the
first claim. A threshold scenario releases an already pending UART0 request,
and a local SEIE mask is tested before delivery.

Build from the repository root with:

```powershell
& .\tests\fixtures\riscv_opensbi_dual_uart\build.ps1
```

The script checks and reuses the pinned firmware hash, then records compiler,
DTC, build flags, DTB, payload, and ELF hashes in `binary-manifest.txt`.
