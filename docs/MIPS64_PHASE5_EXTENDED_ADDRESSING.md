# MIPS64-5 — Extended 64-bit Addressing

## Scope and architecture source

MIPS64-5 adds MIPS64 Release 2 extended virtual address regions, XKPHYS direct mapping, UX/SX/KX checks, wider TLB tags, and XTLB refill routing to the accepted MIPS64-4R1 core. The architectural reference is [MIPS64 Architecture for Programmers, Volume III: Privileged Resource Architecture, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf), especially §§3.5.1, 4.2–4.11, 5.2 and Chapter 8.

No Malta board, firmware, generic page-table walker, cache-coherence model, or production VM snapshot format was added.

## Address widths and region matrix

The fixed virtual processor configuration is **SEGBITS=40** and **PABITS=36**. The existing EntryLo PFN carries 24 bits; with the 12-bit 4 KiB page offset, it represents all 36 physical address bits. These compile-time values are exposed as `kMips64SegBits`, `kMips64PaBits`, and `kPhysicalAddressMask` in `include/mips64/Mips64Cpu.h`.

| Region | Supported virtual range | Translation | Access and enable |
| --- | --- | --- | --- |
| Compatibility kuseg | `0x0000000000000000–0x000000007FFFFFFF` | TLB; with ERL set, uncached direct mapping with PA=VA | User, supervisor, kernel; no UX gate for compatibility addresses |
| XUSEG | `0x0000000080000000–0x000000FFFFFFFFFF` | TLB | User, supervisor, kernel; UX required |
| XSSEG | `0x4000000000000000–0x400000FF7FFFFFFF` | TLB | Supervisor and kernel; SX required |
| XKPHYS | `0x8000000000000000–0xBFFFFFFFFFFFFFFF` | Direct map, no TLB | Kernel only; KX required; CCA and PABITS checks apply |
| XKSEG | `0xC000000000000000–0xC00000FF7FFFFFFF` | TLB | Kernel only; KX required |
| CKSEG0 | `0xFFFFFFFF80000000–0xFFFFFFFF9FFFFFFF` | Direct, `PA = VA & 0x1FFFFFFF` | Kernel only |
| CKSEG1 | `0xFFFFFFFFA0000000–0xFFFFFFFFBFFFFFFF` | Direct, `PA = VA & 0x1FFFFFFF` | Kernel only |
| CKSSEG | `0xFFFFFFFFC0000000–0xFFFFFFFFDFFFFFFF` | TLB | Supervisor and kernel |
| CKSEG3 | `0xFFFFFFFFE0000000–0xFFFFFFFFFFFFFFFF` | TLB | Kernel only |
| Holes | All other addresses | None | Address Error; never narrowed or masked into a supported region |

The XUSEG/XSSEG/XKSEG endpoints follow the implemented 40-bit segment width. Addresses above those ranges remain holes even when the top two region bits resemble a supported segment.

## Effective privilege and address enable

`KSU=User` and `KSU=Supervisor` are enforced. `EXL` or `ERL` forces effective kernel privilege; `KSU=3` is reserved and remains kernel-equivalent, matching the prior implementation choice. The status enable bits apply to their extended segments:

| Effective mode | USEG/XUSEG | SSEG/XSSEG/CKSSEG | KSEG/XKSEG/XKPHYS/CKSEG0/1/3 |
| --- | --- | --- | --- |
| User | Allowed; UX required for XUSEG | Address Error | Address Error |
| Supervisor | Allowed; UX required for XUSEG | Allowed; SX required for XSSEG | Address Error |
| Kernel, including EXL/ERL | Allowed; UX required for XUSEG | Allowed; SX required for XSSEG | Allowed; KX required for XKSEG/XKPHYS |

Compatibility segments retain their prior enable behavior. Refill-vector selection also follows the PRA compatibility rules: compatibility and extended USEG use UX; XSSEG uses SX; XKSEG uses KX; compatibility CKSSEG and CKSEG3 use KX for normal versus XTLB refill selection. That vector selection is separate from whether the compatibility segment itself is enabled.

With UX=0 in user mode, load/store effective addresses are sign-extended from bit 31 after 32-bit address arithmetic and then validated. Instruction fetches keep the original full PC and receive no normalization. Fault entry records the address actually validated. With ERL set, the low compatibility kuseg window bypasses the TLB, uses PA=VA, and reports uncached CCA 2.

## XKPHYS translation and CCA

XKPHYS uses VA[61:59] as CCA and VA[35:0] as PA. Bits VA[58:36] must be zero. The implementation recognizes required CCA values **2 (uncached)** and **3 (cacheable)**; other encodings return AdEL/AdES as an explicit unsupported-configuration choice. CCA is retained in `Mips64Probe::cacheAttribute` for direct mappings and selected TLB halves. The emulator does not simulate cacheability or coherence.

XKPHYS translation validates reserved bits before producing a PA. It does not mask invalid high address bits into the implemented range. EntryLo PFN width likewise caps mapped translations at 36 physical bits. The final physical memory access remains subject to the configured `IMemory` bounds and reports IBE/DBE when backing memory rejects it.

## TLB, EntryHi and XContext

The 16-entry paired-page TLB is retained, including existing PageMask sizes, ASID/global behavior, even/odd selection, Valid/Dirty checks, and deterministic Wired/Random behavior. Its tag preserves all implemented VPN bits:

- EntryHi: `R=VA[63:62]`, `VPN2=VA[39:13]`, `ASID=bits[7:0]`; bits `[61:40]` and `[12:8]` read as zero.
- The TLB entry's existing 64-bit `vpn2` field stores packed R plus VPN2, so distinct supported high VPNs cannot alias through discarded bits.
- XContext: `PTEBase=bits[63:33]`, `R=VA[63:62]` in bits `[32:31]`, and `BadVPN2=VA[39:13]` in bits `[30:4]`; bits `[3:0]` are zero.
- Context's compatibility BadVPN2 field is still updated alongside XContext on a TLB exception.

TLBP, TLBR, TLBWI and TLBWR use the extended EntryHi representation. The extended address suite exercises guest-encoded TLBWI/TLBWR and TLBP/TLBR, a high-VPN non-alias case, ASID mismatch, and a global mapping across ASIDs. Existing suites continue to qualify all supported PageMask sizes and invalid/modified permissions.

## Exception vectors and retry

Only a TLB miss with EXL clear uses a refill vector. XTLB is selected with offset `0x080`; the normal refill offset is `0x000`. TLB Invalid, TLB Modified, address errors and other exceptions use the general vector.

- BEV=1 XTLB: `0xFFFFFFFFBFC00280` (boot base `0xFFFFFFFFBFC00200` + `0x080`).
- BEV=0 XTLB with reset EBase: `0xFFFFFFFF80000080`.
- Branch-delay refill faults set Cause.BD and EPC to the branch address while BadVAddr, EntryHi, Context and XContext describe the faulting effective address.

Guest Demo C runs an encoded load at an unmapped XUSEG address, executes the XTLB handler, installs the mapping with guest CP0 writes and TLBWI, executes ERET, retries the exact load, and publishes the loaded result. No host-side TLB insertion participates in the demo. Separate standalone tests exercise BEV=0 and BEV=1 XTLB routing and BD/EPC state.

## Encoded guest and production VM evidence

The standalone extended-address suite executes a real user-mode program at XUSEG VA `0x0000001000000000`, loads a 64-bit value from its mapped odd page, stores it, and reads it back with UX enabled. It then clears UX and observes an AdEL on the original extended instruction fetch.

The XKPHYS suite uses bounded sparse backing memory, executes guest instructions fetched above 4 GiB at PA `0x100000000`, then stores and reloads through XKPHYS at PA `0x100001000`. It verifies CCA 3 for instruction fetch and CCA 2 for data access, tests the highest representable PA, rejects reserved address bits and unsupported CCA values, and repeats execution in little- and big-endian modes. User and supervisor denial, plus KX=0 denial, are covered.

The production VM suite runs through `VirtualMachine(..., "MIPS64")` and the normal scheduler. One guest enables KX, then the test sets an XKPHYS entry point outside the compatibility address range through `setEntryPoint` and executes a store/load program. A second VM guest takes a BEV=0 XTLB miss at XUSEG VA `0x0000001200002000`, runs its guest handler, installs PFN 5 with TLBWI, executes ERET, retries the load, and writes the expected result to CKSEG0 memory. Existing VM integration coverage retains two-CPU round-robin isolation and Count/Compare interrupt handling.

## State format

MIPS64-5 increments the CPU state format from v4 to **v5** because EntryHi, packed TLB VPN tags, and XContext now have different architectural meanings. The layout remains 957 bytes; no additional mutable state was introduced. Current v5 state round-trips the extended VPN and translation state. Canonical v3/v4 compatibility state migrates to v5; noncanonical old fill fields and malformed current fields are rejected. Restore remains staged and atomic. Version tests cover v3, v4, v5 and rejection rollback.

## Regression results

On the MIPS64-5 working tree:

- All 14 registered MIPS64 CTest suites pass (14/14).
- The 10 inherited MIPS64 suites pass with 2,139 current assertion checks. The supplied MIPS64-4R1 baseline was 2,092 checks; the difference is 47 additional MIPS64-5 ERL setup, extended-address migration cases, and revised field assertions.
- Four new suites pass with 251 assertion checks: extended addresses 113, XKPHYS 78, address modes 38, production VM 22.
- Total across the 14 MIPS64 suites: **2,390 assertion checks passed, zero failed**.
- Relevant IA-64 suites passed: BasicTests, NewInstructionTests, EfiMemoryMapTests, IA64SalTests, and VMCheckpointTests (5/5).
- RISC-V decoder smoke passed (1/1).

The full repository CTest set reports 20/26 passing; its six pre-existing unavailable executables are SyscallTests, DebuggerTests, ProfilerTests, FuzzerTests, VMIsolationTests, and ELFValidationTests. The separately built, unregistered `test_isa_plugin.exe` also reproduces its known baseline NaT assertion at `tests/test_isa_plugin.cpp:1551`. This IA-64 failure is independent of the five selected IA-64 suites, which all pass. No IA-64 production source or IA-64 `.phase` file was changed.

## Known limitations and MIPS64-6 recommendations

- SEGBITS=40 and PABITS=36 are fixed implementation parameters; runtime width configuration and wider physical widths are unsupported.
- XKPHYS accepts only CCA 2 and 3. Cache behavior is recorded but not simulated.
- The TLB is the existing fixed 16-entry implementation. There is no generic guest page-table walker, larger TLB, or VMID.
- Compatibility KSEG direct maps remain 29-bit as before; this phase does not add platform-specific physical devices or memory maps.
- Full cache/coherence behavior, Malta, firmware, SMP/IPI, FPU, LL/SC, unaligned partial-word instructions, and full VM snapshots remain out of scope.

Recommended MIPS64-6 scope: add a guest-visible memory map and platform-independent page-table bootstrap helpers; qualify a configurable larger TLB/VMID only if a target platform requires it; add optional CCA policies behind an explicit CPU configuration; and decide whether full VM snapshot integration is required before extending serialized state again.
