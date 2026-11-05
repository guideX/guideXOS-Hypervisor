# RISCV-16 machine PMP permission transition guest

This small RV64IMA_Zicsr guest runs from M-mode and enters S-mode through
`mret`. It denies an exact 8-byte data range, observes an S-mode load access
fault, grants read permission by writing `pmpcfg0` in M-mode, executes
`SFENCE.VMA`, and verifies the S-mode load succeeds. It then revokes read
permission with a second M-mode `pmpcfg0` write and `SFENCE.VMA`; the next
S-mode load faults and the guest recovers in its S-mode trap handler.

Build from the repository root with:

```powershell
& .\tests\fixtures\riscv_pmp_transition\build.ps1
```

The checked-in binary and ELF are the inputs for a separate architectural
test. No host-side privilege or PMP mutation is used during the transition.
