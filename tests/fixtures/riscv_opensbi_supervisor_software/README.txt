RISCV-14 guest-controlled supervisor software interrupt fixture
================================================================

supervisor.S is a freestanding RV64IMA_Zicsr S-mode guest. It is assembled
independently, receives the normal OpenSBI v1.8.1 FW_JUMP handoff, enables Sv39,
and runs on the RISCV-12/13 single-hart UART/PLIC/ACLINT platform. The guest
sets and clears SSIP with CSRRS/CSRRC instructions. The host injects UART input
and advances bounded virtual time; it does not set SSIP or create guest records.

Regenerate the accepted payload and DTB from this directory with:

  .\build.ps1

The script reuses the accepted OpenSBI binaries and validates their SHA-256
hashes before assembling the guest. It uses RV64IMA_Zicsr, LP64, no relaxation,
no C runtime, and DTC. Its external build products and execution traces are
kept under:

  E:\CodexArtifacts\guideXOS_Hypervisor\phase-riscv14-build

binary-manifest.txt records the compiler, flags, firmware provenance and
hashes. supervisor-disassembly.txt and instruction-inventory.txt record the
assembled image. This local SSIP operation demonstrates a single-hart
supervisor software interrupt; it does not implement or establish SBI IPI,
remote-hart signaling, or SMP support.
