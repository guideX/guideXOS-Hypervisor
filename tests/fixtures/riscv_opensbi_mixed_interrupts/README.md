# RISCV-13 mixed timer and external interrupt fixture

This fixture boots the pinned, unmodified OpenSBI v1.8.1 FW_JUMP image used by
earlier phases. Its independently assembled RV64IMA_Zicsr supervisor guest
enables Sv39, discovers the SBI TIME extension through the real OpenSBI SBI
Base interface, and uses two independent 16550-compatible UART receive paths
connected to the real 96-source PLIC model.

The guest and host harness exercise seven configurations: simultaneous STIP
and SEIP with SEI priority, STIE masking, SEIE masking, global SIE masking
across WFI, PLIC threshold masking, UART IER masking, and a three-way case with
both PLIC sources and STIP pending. UART input enters only through the host's
UART receive API. The S-mode handler claims and completes PLIC sources, reads
the matching UART RBR, records architectural interrupt state, and returns with
SRET. Host traces include the interrupt boundary PC, privilege, mtime, pending
and enable CSRs, and PLIC state.

Build using the pinned firmware and local LLVM/DTC tools:

```powershell
& .\tests\fixtures\riscv_opensbi_mixed_interrupts\build.ps1 -BuildRoot E:\CodexArtifacts\guideXOS_Hypervisor\phase-riscv13-build
```

The build script checks both accepted OpenSBI firmware hashes, builds only the
guest and DTB, and records the compiler, flags, firmware provenance, and
artifact hashes in `binary-manifest.txt`.
