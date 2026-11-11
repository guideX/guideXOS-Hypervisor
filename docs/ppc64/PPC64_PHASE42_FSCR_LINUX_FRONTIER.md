# PPC64 Phase 42 — POWER9 FSCR and Linux continuation

## Outcome and repository state

**Outcome A — PPC64-42 complete.** The bounded PPC64 profile now recognizes the authentic POWER9 FSCR access, enforces privileged SPR access, persists nonzero FSCR state, and retires the unchanged Linux read/modify/write sequence through `VirtualMachine::step()`. Linux continues to a deterministic unsupported SPR 786 instruction. This is not a claim of complete Linux boot.

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `f42db806dcc3b44a36e32692898748647f569ffb`; starting worktree was clean and branch divergence was ahead 3 / behind 0.
- Starting marker was `last_completed=PPC64-41`, `next_expected=PPC64-42`. The marker is advanced to PPC64-42 in the closeout commit.
- The ignored `build/` tree and Phase 41 artifacts were preserved. The shared IA-64 `.phase` was not modified. No push, branch switch, reset, rebase, merge, stash, clean, or cherry-pick was used.

## Register identity, decode, and architecture

The original Phase 41 frontier was NIP `0xC00000000003AD04`, raw word `0x7D3922A6`, after 2,693,007 retired instructions. The unchanged kernel ELF fetch confirms:

```text
primary opcode = 31
RT             = 9
SPR low5       = 25
SPR high5      = 4
SPR            = low5 | (high5 << 5) = 153 (0x099)
XO             = 339
reserved bit   = 0
instruction    = mfspr r9,153
```

The independent ELF inspector verifies the word at file offset `0x4AD04`. Its enclosing symbol is `__setup_cpu_power9`, address `0xC00000000003AD00`, size `0xDC`; the access is at symbol offset `+4`. The previous instruction at `0x...3ACFC` is the function return, followed by `nop` at the symbol entry. Both live replays also captured the transfer into this symbol: `bcctrl 20,0`, raw `0x4E800421`, at `0xC000000001EA739C`. Independent field extraction gives primary opcode 19, BO 20, BI 0, XO 528, and LK 1. The branch was taken with CTR=`0xC00000000003AD00`, reached the ELF symbol entry, and linked LR to `0xC000000001EA73A0` (LR before the call was `0xC000000001EA7440`). A bounded 64-instruction predecessor context is stored in the trace. The sequence after the FSCR read is `mfspr; ori ...,0x1000; mtspr; mfspr; ori ...,0x180; mtspr`.

POWER9 User's Manual v2.1 Table 4-8 identifies SPR 153 as a 64-bit FSCR replicated per processor thread, with privileged reads and writes. Linux v7.2 `reg.h` defines `SPRN_FSCR=0x099` and separately defines `SPRN_HFSCR=0x0BE`; these are not aliases. `SPRG1` (SPR 273) remains separate. The XFX split-SPR reconstruction and bounded supported-SPR list are retained; unsupported SPR numbers remain unsupported.

Power ISA v3.1C Book III §7.2.12 defines FSCR as interruption-cause bits 0:7 plus facility-enable bits 8:63. In raw `uint64_t` masks, the defined FSCR enable fields are `PREFIX=0x2000`, `SCV=0x1000`, `TAR=0x100`, `EBB=0x80`, and `DSCR=0x04`; the upper byte is the interruption cause (`0xFF00000000000000`). The remaining enable-field positions are reserved for FSCR in this ISA version. Linux v7.2's header shares bit-number definitions between FSCR and HFSCR; the additional `MSGP`, `TM`, `BHRB`, `PM`, `VECVSX`, and `FP` enables are HFSCR fields, not defined FSCR enables here. On POWER9's boot path Linux sets SCV first, then TAR and EBB.

Power ISA Book I §1.3.3 permits writes of any value to reserved System Register bits. A later problem-state read returns zero; a privileged-state read after writing 1 is architecturally undefined as 0 or 1. The emulator keeps a deterministic raw 64-bit value in privileged state, a permitted choice for reserved bits, and never uses reserved bits to activate a facility. The FSCR interruption-cause byte is stored and read as state, but Phase 42 does not synthesize its exception-driven update because facility-unavailable exception machinery is out of scope. No software-visible behavior in the accepted Linux sequence depends on reserved-bit or cause-byte writes. The pinned generic QEMU source is only a raw-storage cross-check, not the basis of the reserved-bit rule.

## State, reset, privilege, and checkpoint

FSCR is explicit per-vCPU architectural state, initialized to zero, copied by CPU clone, and reset to zero. The cited FSCR definition does not prescribe one universal reset value; zero is the guideXOS POWER9 profile's reset policy, consistent with pinned QEMU source inspection and the authentic Linux value observed before the first read. It does not reuse SPRG1 or a host-global scratch register and does not add an LPID-indexed bank. Separate VMs remain isolated.

Supervisor reads and writes are supported for SF=1 and SF=0, and hypervisor reads/writes work in the tested supported state. Problem-state MFSPR and MTSPR enter the existing guest privileged-instruction Program exception path; tests verify faulting NIP, SRR0/SRR1 and MSR transition, unchanged destination FSCR/GPR state before fault, and recovery through the guest handler. MFSPR transfers the full 64-bit FSCR to any GPR, including r0, without modifying FSCR or the surrounding architectural state. MTSPR reads its source GPR before writing FSCR and leaves unrelated CPU and guest-memory state unchanged.

The CPU payload version advances from v6 to v7 by appending the 64-bit FSCR value in little-endian form. Automatic serialization retains the smallest compatible older payload when FSCR is zero and emits v7 for nonzero FSCR. Explicit v7 round trips work; v6 VM checkpoints still load with FSCR zero; v1–v5 CPU readers remain valid and default FSCR to zero; corruption is detected. Checkpoint tests keep SPRG1 intact.

Test vectors independently cover zero, all five FSCR enable fields defined by the cited ISA version, assigned interruption-cause examples `0x02`, `0x07`, `0x0C`, and `0x0D`, and mixed upper/lower 64-bit patterns. Every GPR is exercised as an MFSPR destination and MTSPR source. Raw bit 0 is reserved in FSCR in this version; setting it does not enable FP execution. All-one and reserved-bit patterns check this emulator's selected privileged-state behavior and payload integrity; they do not constrain other conforming implementations' unspecified reserved-bit read values.

## Authentic Linux execution

The artifact verifier passed without modifying the guest kernel, configuration, FDT, entry point, ELF placement, memory protections, or partition translation setup:

| Input | SHA-256 |
| --- | --- |
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

Both fresh production VM lifecycles reproduce the previous 2,693,007-instruction prefix and arrive at the authenticated read with POWER9 DD2.0 (`PVR=0x004E1200`), LPID 1, and MSR `0x8000000000000002` (`PR=0`, `HV=0`). The actual pre-read FSCR is zero. R9 before the read is `0xC0000000017AAE10`; after the genuine `mfspr` retires through `VirtualMachine::step()`, R9 is zero and NIP is `0xC00000000003AD08`. FSCR, CR, XER, MSR, SPRG1, and guest RAM are unchanged by the read; cycles and timebase each advance once.

Linux then reads FSCR, sets SCV, and writes it: at `0xC00000000003AD0C`, source R9 is `0x1000`, FSCR changes from `0` to `0x1000`. It next reads FSCR and writes `0x1180` at `0xC00000000003AD18`, setting TAR and EBB while preserving SCV. Both writes retire in the unchanged guest. The model stores those raw FSCR bits but does not implement SCV, TAR, EBB, FP, VSX, TM, or other facility execution.

The replay stops at translated, executable LPID 1 NIP `0xC00000000003AD20`, raw `0x7D32C3A6`, decoded `mtspr 786,r9` (unsupported SPR). Total retirement is 2,693,014; last successful NIP is `0xC00000000003AD1C`. FSCR at the endpoint is `0x1180`; CR=`0x84000848`, XER=`0x20040000`, MSR=`0x8000000000000002`, SPRG1=`0xC000000001F42380`. Both runs agree on kernel/FDT hashes, prefix length, FSCR access sequence, trace digest `0xBCD844D6DFF0481F`, final CPU state, RAM fingerprint `0xBCC1EC135BA83EE9`, and next frontier.

The ELF symbol, live branch target, and binary sequence attribute the FSCR accesses to `__setup_cpu_power9`. Linux v7.2's `init_FSCR_power9()` source reads FSCR, ORs `FSCR_SCV`, writes it, then calls `init_FSCR()`, which reads FSCR, ORs `FSCR_TAR|FSCR_EBB`, and writes. The binary/source correspondence is supported by the observed `bcctrl`, exact target symbol, and runtime FSCR register sequence; no runtime QEMU comparison was used.

## Focused tests and regressions

The final Debug Phase 42 console reports `checks=4146`, `focused=4141`, `linux_replay=78`, and `failures=0`. Its `focused-checks.txt` snapshot records `total_checks=4144` and `phase42_checks=4142`; the two-count difference is from the final artifact-file write checks performed after the snapshot. Replay checks are included in the focused total. Programs A–N all pass: exact Linux decode; general SPR fields; FSCR/HFSCR/SPRG1 identity; full-width reads and every GPR; zero reset; supervisor/hypervisor and problem exception behavior; paired full-width writes and readback; facility-support boundary; register/RAM preservation; VM isolation; v1–v7 checkpoint compatibility; production VM step; observed branch into `__setup_cpu_power9` and authentic Linux retirement; and bounded two-run continuation.

The accepted distinct-phase baseline through Phase 41 was 1,385,700 checks. Adding Phase 42's 4,142 phase checks gives **1,389,842 distinct-phase PPC64 checks through Phase 42**; repeated Debug/Release executions are not added again.

Full Debug CTest (`ctest --test-dir build --output-on-failure`) exited 8 with 51 passed, 6 not run, and 0 executed failures (57 registered, 995.29 seconds). All registered PPC64 phase tests, including Phases 31–42, passed. The six not-run tests are the historical missing executables: Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation.

Focused Release build succeeded and Release CTest for Phases 39–42 passed 4/4. Direct IA-64 emulator, SAL, ACPI, interrupt-controller, processor-interrupt-block, and VM-checkpoint regressions each exited 0. The full Debug build exited 1 while linking unrelated `test_cpu_execution.exe` and `test_mov_imm.exe`; MinGW reported unresolved WinSock imports such as `__imp_WSAStartup`. Focused Phase 33–42 Debug builds and all registered PPC64 tests succeeded, so the unrelated link issue was left untouched.

## Artifacts, references, and limitations

Phase 41 evidence remains under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase41\`. Final accepted Debug replay evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase42\debug-verified-final\`; the passing Phase 42 CTest and Release CTest artifacts are under `debug-ctest-run\` and `release-ctest-run\`. The initial exploratory run, earlier accepted run, and intermediate branch-capture run remain preserved and are labelled in `run-history.txt`. Regression logs, reference inspections, and the Phase 42 root SHA-256 manifest are under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase42\`.

The root manifest is `artifact-sha256-manifest.txt` (161 files; SHA-256 `9ba6dd6acb9d15e64b503ca91446fc580ea612cc977dde892a8c07896da0842c`).

The accepted two-run replay agrees on the entry transfer as well as the later state: both took raw `0x4E800421` from `0xC000000001EA739C`, with CTR before/after and target equal to `0xC00000000003AD00`; LR after the linked transfer is `0xC000000001EA73A0`. Both logs report the same `__setup_cpu_power9` entry and pre-read execution context.

References inspected:

- [Power ISA v3.1C](https://openpowerfoundation.org/specifications/isa/) — normative instruction and SPR architecture; inspected the OpenPOWER PDF linked from this page, whose extracted text identifies itself as v3.1C-rc3 (2023-06-22).
- [POWER9 Processor User's Manual v2.1, Table 4-8](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf) — SPR 153 name, width, replication, and privilege table.
- [Linux v7.2 `reg.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h) — FSCR/HFSCR identifiers, facility masks, and interruption-cause mask.
- [Linux v7.2 `cpu_setup_power.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/cpu_setup_power.c) — FSCR read/modify/write setup order.
- [Linux v7.2 `dt_cpu_ftrs.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/dt_cpu_ftrs.c) and [`prom.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/prom.c) — inspected for CPU-feature and early-device-tree setup context.
- [Pinned QEMU `cpu.h`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu.h) and [`cpu_init.c`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu_init.c) — source-only reset/SPR registration cross-check.

The inspected OpenPOWER PDF is preserved at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase42\references\PowerISA_v3.1C_OpenPOWER.pdf` (SHA-256 `56372d23ece7e9e2c1b381a639443982a3e16e38109df1c141d655b779b61fdb`). The same directory contains the FSCR, Linux, and QEMU source-inspection notes.

QEMU status is `QEMU_REFERENCE_PENDING`. No QEMU runtime was launched and no QEMU/QAC parity is claimed. Facility-unavailable exception generation, cause-byte updates from such exceptions, and facility execution remain unsupported. Phase 43 should independently attribute the `mtspr 786,r9` frontier at `0xC00000000003AD20` and qualify only that exact Linux-required SPR operation before continuing the unchanged guest.
