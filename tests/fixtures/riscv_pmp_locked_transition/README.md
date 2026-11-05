# RISCV-17 locked PMP and reset transition guest

This independently assembled RV64IMA_Zicsr M/S-mode guest programs a locked
8-byte NAPOT deny entry and a locked TOR deny entry with a protected predecessor.
Guest M-mode CSR instructions attempt to change both restrictions; S-mode then
faults on both target accesses and records the architectural causes and
addresses.

The host invokes the production `RV64CPU::reset()` path after the guest proves
the locked restrictions. Guest RAM is retained across reset. The same guest
restarts, configures the NAPOT range without a lock, changes its address and
permissions with actual CSR instructions, and proves the S-mode load succeeds.
The existing unlocked deny/grant/revoke guest remains a separate regression.

Build from the repository root with:

```powershell
& .\tests\fixtures\riscv_pmp_locked_transition\build.ps1
```

The build keeps compiler intermediates under the relocated artifact directory.
The checked-in binary and manifest are the deterministic CTest inputs.
