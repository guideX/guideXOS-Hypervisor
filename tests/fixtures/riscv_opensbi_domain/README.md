# RISCV-16 OpenSBI restricted-domain fixture

This opt-in platform fixture boots the accepted, unmodified OpenSBI v1.8.1
FW_JUMP image with a supervisor domain declared through OpenSBI's FDT domain
configuration mechanism. The domain grants S-mode the guest RAM tail, UART0,
UART1, and the complete 4 MiB PLIC window. It denies S-mode access to both
8-byte ACLINT registers, OpenSBI's inherited firmware windows, and all
unmatched physical addresses. The 16 KiB page-table window is S-readable and
S-read-only. PMP cannot distinguish individual PLIC registers in its 4 MiB
authorized window.

The independently assembled RV64IMA_Zicsr guest runs Sv39, checks authorized
and denied translations, programs the S-context PLIC, services UART1 receive,
requests SBI Base and TIME services, and recovers from expected traps in its
own S-mode handler. Its page tables are preloaded into the read-only S-mode
window so page-table reads succeed while A-bit writes fail through PMP.

The CPU node uses the standard `mmu-type = "riscv,sv39"` property consumed by
OpenSBI v1.8.1's CPU fixup. The guest-visible FDT therefore retains an enabled
hart 0 node, and OpenSBI's domain fixup removes the private `opensbi-domain`
property. OpenSBI adds exact protected firmware text and data reservations;
the input FDT separately reserves the unused tail of the 2 MiB firmware slot
so S-mode memory allocators cannot select a PMP-unmatched hole.

Build the fixture from the repository root with:

```powershell
& .\tests\fixtures\riscv_opensbi_domain\build.ps1
```

The script verifies the pinned firmware hashes before compiling the new DTB
and guest. It writes compiler intermediates to the relocated artifact directory
and does not rebuild or modify the accepted OpenSBI firmware.
